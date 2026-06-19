#pragma once
// M1a unit tests — platform-independent networking core.
//   SequenceMath  — 16-bit wraparound comparison
//   ReplayWindow  — anti-replay sliding window (R13)
//   Reliability   — ack bitfield, RTT/RTO, dup detection, loss/reorder (R10)
//   Fragmentation — split/reassemble incl. reorder + dup fragments
//   PacketCodec   — header + message framing round-trip
//   Protocol      — version gate

#include "TestRunner.h"
#include "net/SequenceMath.h"
#include "net/ReplayWindow.h"
#include "net/Reliability.h"
#include "net/Fragmentation.h"
#include "net/PacketCodec.h"
#include "net/Protocol.h"

#include <cstdint>
#include <vector>

TEST_SUITE(SequenceMath)
{
    TEST_CASE(BasicGreater) {
        CHECK(Neuron::Net::SeqGreater(5, 3));
        CHECK(!Neuron::Net::SeqGreater(3, 5));
        CHECK(!Neuron::Net::SeqGreater(5, 5));
    });

    TEST_CASE(Wraparound) {
        // 1 is newer than 65535 (just wrapped)
        CHECK(Neuron::Net::SeqGreater(1, 65535));
        CHECK(!Neuron::Net::SeqGreater(65535, 1));
        // 0 newer than 65535
        CHECK(Neuron::Net::SeqGreater(0, 65535));
    });

    TEST_CASE(Diff) {
        CHECK_EQ(Neuron::Net::SeqDiff(10, 7), 3);
        CHECK_EQ(Neuron::Net::SeqDiff(0, 65535), 1);   // wrapped forward by 1
        CHECK_EQ(Neuron::Net::SeqDiff(65535, 0), -1);
    });
}

TEST_SUITE(ReplayWindow)
{
    TEST_CASE(FirstPacketAccepted) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(100));
        CHECK_EQ(w.Highest(), uint64_t(100));
    });

    TEST_CASE(DuplicateRejected) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(100));
        CHECK(!w.CheckAndUpdate(100)); // exact duplicate
    });

    TEST_CASE(ForwardProgress) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(1));
        CHECK(w.CheckAndUpdate(2));
        CHECK(w.CheckAndUpdate(3));
        CHECK(!w.CheckAndUpdate(2)); // replay of 2
    });

    TEST_CASE(OutOfOrderWithinWindow) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(10));
        CHECK(w.CheckAndUpdate(5));  // older but within window — fresh, accept
        CHECK(!w.CheckAndUpdate(5)); // now a duplicate
        CHECK(w.CheckAndUpdate(8));  // still fresh
    });

    TEST_CASE(TooOldRejected) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(2000));
        // 2000 - offset >= 1024 window → reject anything <= 976
        CHECK(!w.CheckAndUpdate(900));
        CHECK(w.CheckAndUpdate(1500)); // within window
    });

    TEST_CASE(LargeJump) {
        Neuron::Net::ReplayWindow w;
        CHECK(w.CheckAndUpdate(1));
        CHECK(w.CheckAndUpdate(100000)); // huge jump clears window
        CHECK(w.CheckAndUpdate(99999));  // within new window
        CHECK(!w.CheckAndUpdate(1));     // ancient — reject
    });
}

TEST_SUITE(Reliability)
{
    TEST_CASE(SequenceAssignment) {
        Neuron::Net::ReliableSender s;
        CHECK_EQ(s.Send({1, 2, 3}, 0.0), uint16_t(0));
        CHECK_EQ(s.Send({4, 5},    0.0), uint16_t(1));
        CHECK_EQ(s.InFlightCount(), size_t(2));
    });

    TEST_CASE(AckRetiresMessage) {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);  // seq 0
        s.Send({2}, 0.0);  // seq 1
        s.OnAck(1, 0x1, 0.05); // ack=1, ackBits bit0 ⇒ seq 0 also acked
        CHECK_EQ(s.InFlightCount(), size_t(0));
    });

    TEST_CASE(RttEstimated) {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);
        s.OnAck(0, 0, 0.1); // 100 ms RTT
        CHECK(s.SmoothedRtt() > 0.0);
        CHECK(s.CurrentRto() >= 0.05);
    });

    TEST_CASE(TimeoutCollectsForResend) {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);
        // Before RTO: nothing timed out
        auto early = s.CollectTimedOut(0.01);
        CHECK_EQ(early.size(), size_t(0));
        // After RTO (default 200 ms): collected
        auto late = s.CollectTimedOut(0.5);
        CHECK_EQ(late.size(), size_t(1));
        CHECK_EQ(late[0]->sendCount, uint32_t(2)); // bumped
    });

    TEST_CASE(ReceiverDupDetection) {
        Neuron::Net::ReliableReceiver r;
        CHECK(r.OnReceive(5));
        CHECK(!r.OnReceive(5)); // duplicate
        CHECK(r.OnReceive(6));
    });

    TEST_CASE(AckBitsReflectHistory) {
        Neuron::Net::ReliableReceiver r;
        r.OnReceive(10);
        r.OnReceive(9);
        r.OnReceive(7); // skipped 8
        CHECK_EQ(r.Ack(), uint16_t(10));
        const uint32_t bits = r.AckBits();
        // bit0 ⇒ seq 9 received, bit1 ⇒ seq 8 NOT, bit2 ⇒ seq 7 received
        CHECK((bits & 0x1) != 0);  // 9
        CHECK((bits & 0x2) == 0);  // 8 missing
        CHECK((bits & 0x4) != 0);  // 7
    });

    TEST_CASE(ReorderedDelivery) {
        // Simulate out-of-order receipt; all unique sequences accepted once.
        Neuron::Net::ReliableReceiver r;
        CHECK(r.OnReceive(3));
        CHECK(r.OnReceive(1));
        CHECK(r.OnReceive(2));
        CHECK(!r.OnReceive(2)); // dup
        CHECK_EQ(r.Ack(), uint16_t(3));
    });

    TEST_CASE(LossThenResendAcked) {
        // End-to-end: send 3, "lose" seq 1, receiver acks 0 and 2, sender resends 1.
        Neuron::Net::ReliableSender s;
        Neuron::Net::ReliableReceiver r;
        s.Send({0}, 0.0); // seq 0
        s.Send({1}, 0.0); // seq 1 (lost)
        s.Send({2}, 0.0); // seq 2
        r.OnReceive(0);
        r.OnReceive(2);
        s.OnAck(r.Ack(), r.AckBits(), 0.05);
        CHECK_EQ(s.InFlightCount(), size_t(1)); // only seq 1 remains
        auto resend = s.CollectTimedOut(0.5);
        CHECK_EQ(resend.size(), size_t(1));
        CHECK_EQ(resend[0]->sequence, uint16_t(1));
    });
}

TEST_SUITE(Fragmentation)
{
    TEST_CASE(SmallMessageOneFragment) {
        std::vector<uint8_t> msg{1, 2, 3, 4};
        auto frags = Neuron::Net::Fragmentize(7, msg, 1000);
        CHECK_EQ(frags.size(), size_t(1));
        CHECK_EQ(frags[0].header.fragmentCount, uint16_t(1));
    });

    TEST_CASE(SplitAndReassemble) {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 2500; ++i) msg.push_back(static_cast<uint8_t>(i & 0xFF));
        auto frags = Neuron::Net::Fragmentize(42, msg, 1000);
        CHECK_EQ(frags.size(), size_t(3)); // 1000+1000+500

        Neuron::Net::Reassembler ra;
        std::optional<std::vector<uint8_t>> done;
        for (auto& f : frags) done = ra.Add(f);
        REQUIRE(done.has_value());
        CHECK(*done == msg);
    });

    TEST_CASE(ReassembleReordered) {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 2500; ++i) msg.push_back(static_cast<uint8_t>((i * 7) & 0xFF));
        auto frags = Neuron::Net::Fragmentize(1, msg, 1000);
        REQUIRE(frags.size() == 3);

        Neuron::Net::Reassembler ra;
        ra.Add(frags[2]);
        ra.Add(frags[0]);
        auto done = ra.Add(frags[1]);
        REQUIRE(done.has_value());
        CHECK(*done == msg);
    });

    TEST_CASE(DuplicateFragmentsIgnored) {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 1500; ++i) msg.push_back(static_cast<uint8_t>(i));
        auto frags = Neuron::Net::Fragmentize(9, msg, 1000);
        REQUIRE(frags.size() == 2);

        Neuron::Net::Reassembler ra;
        ra.Add(frags[0]);
        ra.Add(frags[0]); // dup, ignored
        auto done = ra.Add(frags[1]);
        REQUIRE(done.has_value());
        CHECK(*done == msg);
    });
}

TEST_SUITE(PacketCodec)
{
    TEST_CASE(HeaderRoundTrip) {
        Neuron::Net::PacketHeader h;
        h.protocolId      = Neuron::Net::kProtocolId;
        h.connectionToken = 0xCAFEBABEDEADBEEFull;
        h.packetNumber    = 0x0123456789ABCDEFull;

        std::vector<uint8_t> buf;
        Neuron::Net::WritePacketHeader(buf, h);
        CHECK_EQ(buf.size(), Neuron::Net::PacketHeader::kWireSize);

        Neuron::Net::PacketHeader out;
        size_t off = 0;
        REQUIRE(Neuron::Net::ReadPacketHeader(buf, out, off));
        CHECK_EQ(out.protocolId, h.protocolId);
        CHECK_EQ(out.connectionToken, h.connectionToken);
        CHECK_EQ(out.packetNumber, h.packetNumber);
    });

    TEST_CASE(HeaderTruncated) {
        std::vector<uint8_t> buf{1, 2, 3}; // too short
        Neuron::Net::PacketHeader out;
        size_t off = 0;
        CHECK(!Neuron::Net::ReadPacketHeader(buf, out, off));
    });

    TEST_CASE(MessageFramingRoundTrip) {
        std::vector<uint8_t> payload;
        const uint8_t bodyA[] = {10, 20, 30};
        const uint8_t bodyB[] = {99};
        Neuron::Net::WriteMessage(payload, Neuron::Net::Channel::ReliableOrdered,
                                  Neuron::Net::MsgType::Command, bodyA);
        Neuron::Net::WriteMessage(payload, Neuron::Net::Channel::Unreliable,
                                  Neuron::Net::MsgType::Snapshot, bodyB);

        std::vector<Neuron::Net::DecodedMessage> msgs;
        REQUIRE(Neuron::Net::ReadMessages(payload, msgs));
        REQUIRE(msgs.size() == 2);
        CHECK(msgs[0].type == Neuron::Net::MsgType::Command);
        CHECK_EQ(msgs[0].body.size(), size_t(3));
        CHECK_EQ(msgs[0].body[1], uint8_t(20));
        CHECK(msgs[1].type == Neuron::Net::MsgType::Snapshot);
        CHECK_EQ(msgs[1].body[0], uint8_t(99));
    });

    TEST_CASE(MalformedFramingRejected) {
        // length claims more bytes than present
        std::vector<uint8_t> payload{
            static_cast<uint8_t>(Neuron::Net::Channel::ReliableOrdered),
            static_cast<uint8_t>(Neuron::Net::MsgType::Command),
            0xFF, 0x00 // length = 255, but no body
        };
        std::vector<Neuron::Net::DecodedMessage> msgs;
        CHECK(!Neuron::Net::ReadMessages(payload, msgs));
    });
}

TEST_SUITE(Protocol)
{
    TEST_CASE(VersionGate) {
        CHECK(Neuron::Net::IsProtocolCompatible(Neuron::Net::kProtocolId));
        CHECK(!Neuron::Net::IsProtocolCompatible(0xDEADBEEF));
        // Wrong version, right magic → still incompatible
        const uint32_t wrongVer = (static_cast<uint32_t>(Neuron::Net::kProtocolMagic) << 16) | 0xFFFF;
        CHECK(!Neuron::Net::IsProtocolCompatible(wrongVer));
    });
}
