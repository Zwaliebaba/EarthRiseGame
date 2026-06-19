#pragma once
// M1a loopback-network tests — in-memory wire with loss/reorder/dup (R10).
//
// These exercise LoopbackNetwork + LoopbackSocket, the deterministic impairment
// harness the acceptance tests rely on. We assert on aggregate behavior (counts,
// at-least-twice, run-to-run equality) rather than exact internal scheduling, so
// they stay robust to the impairment implementation details.

#include "TestRunner.h"
#include "net/LoopbackNetwork.h"

#include <cstdint>
#include <vector>

TEST_SUITE(Loopback)
{
    using namespace Neuron::Net;

    TEST_CASE(BasicSendRecv) {
        LoopbackNetwork net; // default: no impairments
        LoopbackSocket a(&net), b(&net);
        REQUIRE(a.Open(5000));
        REQUIRE(b.Open(6000));
        CHECK_EQ(a.LocalPort(), uint16_t(5000));
        CHECK_EQ(b.LocalPort(), uint16_t(6000));

        const std::vector<uint8_t> payload{ 10, 20, 30, 40, 50 };
        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const int sent = a.SendTo(dst, payload);
        CHECK_EQ(sent, int(payload.size()));

        Endpoint from;
        std::vector<uint8_t> buf(64);
        const int got = b.RecvFrom(from, buf);
        REQUIRE(got == int(payload.size()));
        buf.resize(static_cast<size_t>(got));
        CHECK(buf == payload);
        CHECK_EQ(from.port, uint16_t(5000)); // sender's port
    });

    TEST_CASE(NoDataReturnsZero) {
        LoopbackNetwork net;
        LoopbackSocket a(&net);
        REQUIRE(a.Open(7000));
        Endpoint from;
        std::vector<uint8_t> buf(64);
        CHECK_EQ(a.RecvFrom(from, buf), 0); // empty inbound queue
    });

    TEST_CASE(LossDropsPackets) {
        LoopbackNetwork net;
        Impairments imp; imp.lossProbability = 1.0; // drop everything
        net.Configure(imp);

        LoopbackSocket a(&net), b(&net);
        REQUIRE(a.Open(5000));
        REQUIRE(b.Open(6000));

        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const std::vector<uint8_t> payload{ 1, 2, 3 };
        for (int i = 0; i < 200; ++i)
            a.SendTo(dst, payload);
        net.Step();

        Endpoint from;
        std::vector<uint8_t> buf(64);
        int received = 0;
        while (b.RecvFrom(from, buf) > 0)
            ++received;
        CHECK_EQ(received, 0); // 100% loss => nothing arrives
    });

    TEST_CASE(Duplication) {
        LoopbackNetwork net;
        Impairments imp; imp.dupProbability = 1.0; // always duplicate
        net.Configure(imp);

        LoopbackSocket a(&net), b(&net);
        REQUIRE(a.Open(5000));
        REQUIRE(b.Open(6000));

        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const int kSends = 25;
        for (int i = 0; i < kSends; ++i) {
            const std::vector<uint8_t> payload{ static_cast<uint8_t>(i) };
            a.SendTo(dst, payload);
        }
        net.Step();

        Endpoint from;
        std::vector<uint8_t> buf(64);
        int received = 0;
        while (true) {
            const int n = b.RecvFrom(from, buf);
            if (n <= 0) break;
            ++received;
        }
        // Each datagram delivered at least twice.
        CHECK_GE(received, kSends * 2);
    });

    TEST_CASE(DeterministicWithSeed) {
        // Same seed + same send sequence must produce identical delivered output
        // across two independent runs (loss + dup + reorder all active).
        auto run = [](uint32_t seed) {
            LoopbackNetwork net;
            Impairments imp;
            imp.lossProbability = 0.3;
            imp.dupProbability  = 0.3;
            imp.reorderDepth    = 4;
            imp.seed            = seed;
            net.Configure(imp);

            LoopbackSocket a(&net), b(&net);
            a.Open(5000);
            b.Open(6000);

            Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
            for (int i = 0; i < 100; ++i) {
                const std::vector<uint8_t> payload{
                    static_cast<uint8_t>(i),
                    static_cast<uint8_t>(i * 3)
                };
                a.SendTo(dst, payload);
            }
            net.Step();

            std::vector<std::vector<uint8_t>> delivered;
            Endpoint from;
            std::vector<uint8_t> buf(64);
            while (true) {
                const int n = b.RecvFrom(from, buf);
                if (n <= 0) break;
                delivered.emplace_back(buf.begin(), buf.begin() + n);
            }
            return delivered;
        };

        const auto run1 = run(98765);
        const auto run2 = run(98765);
        CHECK(run1 == run2); // identical delivered sequence (order + contents)
        CHECK(!run1.empty()); // sanity: some packets did get through

        // A different seed should (extremely likely) diverge.
        const auto run3 = run(11111);
        CHECK(run3 != run1);
    });
}
