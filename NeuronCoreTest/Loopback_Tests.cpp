#include "CppUnitTest.h"
#include "net/LoopbackNetwork.h"

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Net;

TEST_CLASS(LoopbackTests)
{
public:
    TEST_METHOD(BasicSendRecv)
    {
        LoopbackNetwork net;
        LoopbackSocket a(&net), b(&net);
        Assert::IsTrue(a.Open(5000));
        Assert::IsTrue(b.Open(6000));
        Assert::AreEqual(uint16_t(5000), a.LocalPort());
        Assert::AreEqual(uint16_t(6000), b.LocalPort());

        const std::vector<uint8_t> payload{10, 20, 30, 40, 50};
        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const int sent = a.SendTo(dst, payload);
        Assert::AreEqual(int(payload.size()), sent);

        Endpoint from;
        std::vector<uint8_t> buf(64);
        const int got = b.RecvFrom(from, buf);
        Assert::IsTrue(got == int(payload.size()));
        buf.resize(static_cast<size_t>(got));
        Assert::IsTrue(buf == payload);
        Assert::AreEqual(uint16_t(5000), from.port);
    }

    TEST_METHOD(NoDataReturnsZero)
    {
        LoopbackNetwork net;
        LoopbackSocket a(&net);
        Assert::IsTrue(a.Open(7000));
        Endpoint from;
        std::vector<uint8_t> buf(64);
        Assert::AreEqual(0, a.RecvFrom(from, buf));
    }

    TEST_METHOD(LossDropsPackets)
    {
        LoopbackNetwork net;
        Impairments imp; imp.lossProbability = 1.0;
        net.Configure(imp);

        LoopbackSocket a(&net), b(&net);
        Assert::IsTrue(a.Open(5000));
        Assert::IsTrue(b.Open(6000));

        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const std::vector<uint8_t> payload{1, 2, 3};
        for (int i = 0; i < 200; ++i)
            a.SendTo(dst, payload);
        net.Step();

        Endpoint from;
        std::vector<uint8_t> buf(64);
        int received = 0;
        while (b.RecvFrom(from, buf) > 0)
            ++received;
        Assert::AreEqual(0, received);
    }

    TEST_METHOD(Duplication)
    {
        LoopbackNetwork net;
        Impairments imp; imp.dupProbability = 1.0;
        net.Configure(imp);

        LoopbackSocket a(&net), b(&net);
        Assert::IsTrue(a.Open(5000));
        Assert::IsTrue(b.Open(6000));

        Endpoint dst; dst.ip = "127.0.0.1"; dst.port = 6000;
        const int kSends = 25;
        for (int i = 0; i < kSends; ++i) {
            const std::vector<uint8_t> payload{static_cast<uint8_t>(i)};
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
        Assert::IsTrue(received >= kSends * 2);
    }

    TEST_METHOD(DeterministicWithSeed)
    {
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
        Assert::IsTrue(run1 == run2);
        Assert::IsFalse(run1.empty());

        const auto run3 = run(11111);
        Assert::IsTrue(run3 != run1);
    }
};
