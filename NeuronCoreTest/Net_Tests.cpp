#include "CppUnitTest.h"
#include "net/SequenceMath.h"
#include "net/ReplayWindow.h"
#include "net/Reliability.h"
#include "net/Fragmentation.h"
#include "net/PacketCodec.h"
#include "net/Protocol.h"

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(SequenceMathTests)
{
public:
    TEST_METHOD(BasicGreater)
    {
        Assert::IsTrue (Neuron::Net::SeqGreater(5, 3));
        Assert::IsFalse(Neuron::Net::SeqGreater(3, 5));
        Assert::IsFalse(Neuron::Net::SeqGreater(5, 5));
    }

    TEST_METHOD(Wraparound)
    {
        Assert::IsTrue (Neuron::Net::SeqGreater(1, 65535));
        Assert::IsFalse(Neuron::Net::SeqGreater(65535, 1));
        Assert::IsTrue (Neuron::Net::SeqGreater(0, 65535));
    }

    TEST_METHOD(Diff)
    {
        Assert::AreEqual(3,  Neuron::Net::SeqDiff(10, 7));
        Assert::AreEqual(1,  Neuron::Net::SeqDiff(0, 65535));
        Assert::AreEqual(-1, Neuron::Net::SeqDiff(65535, 0));
    }
};

TEST_CLASS(ReplayWindowTests)
{
public:
    TEST_METHOD(FirstPacketAccepted)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue(w.CheckAndUpdate(100));
        Assert::AreEqual(uint64_t(100), w.Highest());
    }

    TEST_METHOD(DuplicateRejected)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue (w.CheckAndUpdate(100));
        Assert::IsFalse(w.CheckAndUpdate(100));
    }

    TEST_METHOD(ForwardProgress)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue (w.CheckAndUpdate(1));
        Assert::IsTrue (w.CheckAndUpdate(2));
        Assert::IsTrue (w.CheckAndUpdate(3));
        Assert::IsFalse(w.CheckAndUpdate(2));
    }

    TEST_METHOD(OutOfOrderWithinWindow)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue (w.CheckAndUpdate(10));
        Assert::IsTrue (w.CheckAndUpdate(5));
        Assert::IsFalse(w.CheckAndUpdate(5));
        Assert::IsTrue (w.CheckAndUpdate(8));
    }

    TEST_METHOD(TooOldRejected)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue (w.CheckAndUpdate(2000));
        Assert::IsFalse(w.CheckAndUpdate(900));
        Assert::IsTrue (w.CheckAndUpdate(1500));
    }

    TEST_METHOD(LargeJump)
    {
        Neuron::Net::ReplayWindow w;
        Assert::IsTrue (w.CheckAndUpdate(1));
        Assert::IsTrue (w.CheckAndUpdate(100000));
        Assert::IsTrue (w.CheckAndUpdate(99999));
        Assert::IsFalse(w.CheckAndUpdate(1));
    }
};

TEST_CLASS(ReliabilityTests)
{
public:
    TEST_METHOD(SequenceAssignment)
    {
        Neuron::Net::ReliableSender s;
        Assert::AreEqual(uint16_t(0), s.Send({1, 2, 3}, 0.0));
        Assert::AreEqual(uint16_t(1), s.Send({4, 5},    0.0));
        Assert::AreEqual(size_t(2),   s.InFlightCount());
    }

    TEST_METHOD(AckRetiresMessage)
    {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);
        s.Send({2}, 0.0);
        s.OnAck(1, 0x1, 0.05);
        Assert::AreEqual(size_t(0), s.InFlightCount());
    }

    TEST_METHOD(RttEstimated)
    {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);
        s.OnAck(0, 0, 0.1);
        Assert::IsTrue(s.SmoothedRtt() > 0.0);
        Assert::IsTrue(s.CurrentRto() >= 0.05);
    }

    TEST_METHOD(TimeoutCollectsForResend)
    {
        Neuron::Net::ReliableSender s;
        s.Send({1}, 0.0);
        auto early = s.CollectTimedOut(0.01);
        Assert::AreEqual(size_t(0), early.size());
        auto late = s.CollectTimedOut(0.5);
        Assert::AreEqual(size_t(1), late.size());
        Assert::AreEqual(uint32_t(2), late[0]->sendCount);
    }

    TEST_METHOD(ReceiverDupDetection)
    {
        Neuron::Net::ReliableReceiver r;
        Assert::IsTrue (r.OnReceive(5));
        Assert::IsFalse(r.OnReceive(5));
        Assert::IsTrue (r.OnReceive(6));
    }

    TEST_METHOD(AckBitsReflectHistory)
    {
        Neuron::Net::ReliableReceiver r;
        r.OnReceive(10);
        r.OnReceive(9);
        r.OnReceive(7);
        Assert::AreEqual(uint16_t(10), r.Ack());
        const uint32_t bits = r.AckBits();
        Assert::IsTrue((bits & 0x1) != 0);
        Assert::IsTrue((bits & 0x2) == 0);
        Assert::IsTrue((bits & 0x4) != 0);
    }

    TEST_METHOD(ReorderedDelivery)
    {
        Neuron::Net::ReliableReceiver r;
        Assert::IsTrue (r.OnReceive(3));
        Assert::IsTrue (r.OnReceive(1));
        Assert::IsTrue (r.OnReceive(2));
        Assert::IsFalse(r.OnReceive(2));
        Assert::AreEqual(uint16_t(3), r.Ack());
    }

    TEST_METHOD(LossThenResendAcked)
    {
        Neuron::Net::ReliableSender   s;
        Neuron::Net::ReliableReceiver r;
        s.Send({0}, 0.0);
        s.Send({1}, 0.0);
        s.Send({2}, 0.0);
        r.OnReceive(0);
        r.OnReceive(2);
        s.OnAck(r.Ack(), r.AckBits(), 0.05);
        Assert::AreEqual(size_t(1), s.InFlightCount());
        auto resend = s.CollectTimedOut(0.5);
        Assert::AreEqual(size_t(1),     resend.size());
        Assert::AreEqual(uint16_t(1),   resend[0]->sequence);
    }
};

TEST_CLASS(FragmentationTests)
{
public:
    TEST_METHOD(SmallMessageOneFragment)
    {
        std::vector<uint8_t> msg{1, 2, 3, 4};
        auto frags = Neuron::Net::Fragmentize(7, msg, 1000);
        Assert::AreEqual(size_t(1),      frags.size());
        Assert::AreEqual(uint16_t(1),    frags[0].header.fragmentCount);
    }

    TEST_METHOD(SplitAndReassemble)
    {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 2500; ++i) msg.push_back(static_cast<uint8_t>(i & 0xFF));
        auto frags = Neuron::Net::Fragmentize(42, msg, 1000);
        Assert::AreEqual(size_t(3), frags.size());

        Neuron::Net::Reassembler ra;
        std::optional<std::vector<uint8_t>> done;
        for (auto& f : frags) done = ra.Add(f);
        Assert::IsTrue(done.has_value());
        Assert::IsTrue(*done == msg);
    }

    TEST_METHOD(ReassembleReordered)
    {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 2500; ++i) msg.push_back(static_cast<uint8_t>((i * 7) & 0xFF));
        auto frags = Neuron::Net::Fragmentize(1, msg, 1000);
        Assert::IsTrue(frags.size() == 3);

        Neuron::Net::Reassembler ra;
        ra.Add(frags[2]);
        ra.Add(frags[0]);
        auto done = ra.Add(frags[1]);
        Assert::IsTrue(done.has_value());
        Assert::IsTrue(*done == msg);
    }

    TEST_METHOD(DuplicateFragmentsIgnored)
    {
        std::vector<uint8_t> msg;
        for (int i = 0; i < 1500; ++i) msg.push_back(static_cast<uint8_t>(i));
        auto frags = Neuron::Net::Fragmentize(9, msg, 1000);
        Assert::IsTrue(frags.size() == 2);

        Neuron::Net::Reassembler ra;
        ra.Add(frags[0]);
        ra.Add(frags[0]);
        auto done = ra.Add(frags[1]);
        Assert::IsTrue(done.has_value());
        Assert::IsTrue(*done == msg);
    }
};

TEST_CLASS(PacketCodecTests)
{
public:
    TEST_METHOD(HeaderRoundTrip)
    {
        Neuron::Net::PacketHeader h;
        h.protocolId      = Neuron::Net::kProtocolId;
        h.connectionToken = 0xCAFEBABEDEADBEEFull;
        h.packetNumber    = 0x0123456789ABCDEFull;

        std::vector<uint8_t> buf;
        Neuron::Net::WritePacketHeader(buf, h);
        Assert::AreEqual(Neuron::Net::PacketHeader::kWireSize, buf.size());

        Neuron::Net::PacketHeader out;
        size_t off = 0;
        Assert::IsTrue(Neuron::Net::ReadPacketHeader(buf, out, off));
        Assert::AreEqual(h.protocolId,      out.protocolId);
        Assert::AreEqual(h.connectionToken, out.connectionToken);
        Assert::AreEqual(h.packetNumber,    out.packetNumber);
    }

    TEST_METHOD(HeaderTruncated)
    {
        std::vector<uint8_t> buf{1, 2, 3};
        Neuron::Net::PacketHeader out;
        size_t off = 0;
        Assert::IsFalse(Neuron::Net::ReadPacketHeader(buf, out, off));
    }

    TEST_METHOD(MessageFramingRoundTrip)
    {
        std::vector<uint8_t> payload;
        const uint8_t bodyA[] = {10, 20, 30};
        const uint8_t bodyB[] = {99};
        Neuron::Net::WriteMessage(payload, Neuron::Net::Channel::ReliableOrdered,
                                  Neuron::Net::MsgType::Command, bodyA);
        Neuron::Net::WriteMessage(payload, Neuron::Net::Channel::Unreliable,
                                  Neuron::Net::MsgType::Snapshot, bodyB);

        std::vector<Neuron::Net::DecodedMessage> msgs;
        Assert::IsTrue(Neuron::Net::ReadMessages(payload, msgs));
        Assert::IsTrue(msgs.size() == 2);
        Assert::IsTrue(msgs[0].type == Neuron::Net::MsgType::Command);
        Assert::AreEqual(size_t(3),    msgs[0].body.size());
        Assert::AreEqual(uint8_t(20),  msgs[0].body[1]);
        Assert::IsTrue(msgs[1].type == Neuron::Net::MsgType::Snapshot);
        Assert::AreEqual(uint8_t(99),  msgs[1].body[0]);
    }

    TEST_METHOD(MalformedFramingRejected)
    {
        std::vector<uint8_t> payload{
            static_cast<uint8_t>(Neuron::Net::Channel::ReliableOrdered),
            static_cast<uint8_t>(Neuron::Net::MsgType::Command),
            0xFF, 0x00
        };
        std::vector<Neuron::Net::DecodedMessage> msgs;
        Assert::IsFalse(Neuron::Net::ReadMessages(payload, msgs));
    }
};

TEST_CLASS(ProtocolTests)
{
public:
    TEST_METHOD(VersionGate)
    {
        Assert::IsTrue (Neuron::Net::IsProtocolCompatible(Neuron::Net::kProtocolId));
        Assert::IsFalse(Neuron::Net::IsProtocolCompatible(0xDEADBEEF));
        const uint32_t wrongVer = (static_cast<uint32_t>(Neuron::Net::kProtocolMagic) << 16) | 0xFFFF;
        Assert::IsFalse(Neuron::Net::IsProtocolCompatible(wrongVer));
    }
};
