#include "CppUnitTest.h"
#include "serde/BitStream.h"
#include "serde/Serde.h"

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(SerdeTests)
{
public:
    TEST_METHOD(BitWriter_ReadBack_uint8)
    {
        Neuron::Serde::BitWriter w;
        w.WriteUint8(0xAB);
        w.WriteUint8(0xCD);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        Assert::AreEqual(uint8_t(0xAB), r.ReadUint8());
        Assert::AreEqual(uint8_t(0xCD), r.ReadUint8());
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitWriter_ReadBack_uint32)
    {
        Neuron::Serde::BitWriter w;
        w.WriteUint32(0xDEADBEEFu);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        Assert::AreEqual(uint32_t(0xDEADBEEFu), r.ReadUint32());
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitWriter_ReadBack_uint64)
    {
        Neuron::Serde::BitWriter w;
        w.WriteUint64(0xCAFEBABEDEADBEEFull);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        Assert::AreEqual(uint64_t(0xCAFEBABEDEADBEEFull), r.ReadUint64());
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitWriter_ReadBack_float)
    {
        Neuron::Serde::BitWriter w;
        w.WriteFloat(3.14159f);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        const float v = r.ReadFloat();
        Assert::IsTrue(std::fabs(v - 3.14159f) < 1e-6f);
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitWriter_ReadBack_bool)
    {
        Neuron::Serde::BitWriter w;
        w.WriteBool(true);
        w.WriteBool(false);
        w.WriteBool(true);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        Assert::AreEqual(true,  r.ReadBool());
        Assert::AreEqual(false, r.ReadBool());
        Assert::AreEqual(true,  r.ReadBool());
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitWriter_SubByteFields)
    {
        Neuron::Serde::BitWriter w;
        w.Write(0b101u, 3);
        w.Write(0b10110u, 5);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        Assert::AreEqual(uint64_t(5),  r.Read(3));
        Assert::AreEqual(uint64_t(22), r.Read(5));
        Assert::IsFalse(r.HasError());
    }

    TEST_METHOD(BitReader_Underflow)
    {
        Neuron::Serde::BitWriter w;
        w.WriteUint8(0xFF);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        r.ReadUint8();
        r.ReadUint8(); // underflow
        Assert::IsTrue(r.HasError());
    }

    TEST_METHOD(WriteBuffer_ReadBuffer_roundtrip)
    {
        Neuron::Serde::WriteBuffer wb;
        wb.WriteUint8(42);
        wb.WriteUint16(1234);
        wb.WriteInt64(INT64_MIN);
        wb.WriteFloat(2.718f);
        wb.WriteBool(true);
        wb.Finalise();

        Neuron::Serde::ReadBuffer rb(wb.Data());
        Assert::IsTrue(rb.IsGood());
        Assert::AreEqual(uint8_t(42),    rb.ReadUint8());
        Assert::AreEqual(uint16_t(1234), rb.ReadUint16());
        Assert::AreEqual(INT64_MIN,      rb.ReadInt64());
        Assert::IsTrue(std::fabs(rb.ReadFloat() - 2.718f) < 1e-5f);
        Assert::AreEqual(true, rb.ReadBool());
        Assert::IsTrue(rb.IsGood());
    }

    TEST_METHOD(WriteBuffer_VersionMismatch)
    {
        Neuron::Serde::BitWriter w;
        w.WriteUint32(0xFFFFFFFFu);
        w.WriteUint8(0);
        w.Flush();

        Neuron::Serde::ReadBuffer rb(w.Data());
        Assert::IsTrue(rb.HasVersionMismatch());
        Assert::IsFalse(rb.IsGood());
    }
};
