#pragma once
// M0 unit tests — serialization (BitWriter/BitReader, WriteBuffer/ReadBuffer).

#include "TestRunner.h"
#include "serde/BitStream.h"
#include "serde/Serde.h"

#include <cmath>

TEST_SUITE(Serde)
{
    TEST_CASE(BitWriter_ReadBack_uint8) {
        Neuron::Serde::BitWriter w;
        w.WriteUint8(0xAB);
        w.WriteUint8(0xCD);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        CHECK_EQ(r.ReadUint8(), uint8_t(0xAB));
        CHECK_EQ(r.ReadUint8(), uint8_t(0xCD));
        CHECK(!r.HasError());
    });

    TEST_CASE(BitWriter_ReadBack_uint32) {
        Neuron::Serde::BitWriter w;
        w.WriteUint32(0xDEADBEEFu);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        CHECK_EQ(r.ReadUint32(), uint32_t(0xDEADBEEFu));
        CHECK(!r.HasError());
    });

    TEST_CASE(BitWriter_ReadBack_uint64) {
        Neuron::Serde::BitWriter w;
        w.WriteUint64(0xCAFEBABEDEADBEEFull);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        CHECK_EQ(r.ReadUint64(), uint64_t(0xCAFEBABEDEADBEEFull));
        CHECK(!r.HasError());
    });

    TEST_CASE(BitWriter_ReadBack_float) {
        Neuron::Serde::BitWriter w;
        w.WriteFloat(3.14159f);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        const float v = r.ReadFloat();
        CHECK(std::fabsf(v - 3.14159f) < 1e-6f);
        CHECK(!r.HasError());
    });

    TEST_CASE(BitWriter_ReadBack_bool) {
        Neuron::Serde::BitWriter w;
        w.WriteBool(true);
        w.WriteBool(false);
        w.WriteBool(true);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        CHECK_EQ(r.ReadBool(), true);
        CHECK_EQ(r.ReadBool(), false);
        CHECK_EQ(r.ReadBool(), true);
        CHECK(!r.HasError());
    });

    TEST_CASE(BitWriter_SubByteFields) {
        // Write 3-bit and 5-bit fields — total 8 bits = 1 byte.
        Neuron::Serde::BitWriter w;
        w.Write(0b101u, 3);  // 5
        w.Write(0b10110u, 5); // 22
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        CHECK_EQ(r.Read(3), uint64_t(5));
        CHECK_EQ(r.Read(5), uint64_t(22));
        CHECK(!r.HasError());
    });

    TEST_CASE(BitReader_Underflow) {
        // Reading past end of buffer should set error flag, not crash.
        Neuron::Serde::BitWriter w;
        w.WriteUint8(0xFF);
        w.Flush();

        Neuron::Serde::BitReader r(w.Data());
        r.ReadUint8();  // consumes the one byte
        r.ReadUint8();  // underflow
        CHECK(r.HasError());
    });

    TEST_CASE(WriteBuffer_ReadBuffer_roundtrip) {
        Neuron::Serde::WriteBuffer wb;
        wb.WriteUint8(42);
        wb.WriteUint16(1234);
        wb.WriteInt64(INT64_MIN);
        wb.WriteFloat(2.718f);
        wb.WriteBool(true);
        wb.Finalise();

        Neuron::Serde::ReadBuffer rb(wb.Data());
        REQUIRE(rb.IsGood());
        CHECK_EQ(rb.ReadUint8(),  uint8_t(42));
        CHECK_EQ(rb.ReadUint16(), uint16_t(1234));
        CHECK_EQ(rb.ReadInt64(),  INT64_MIN);
        CHECK(std::fabsf(rb.ReadFloat() - 2.718f) < 1e-5f);
        CHECK_EQ(rb.ReadBool(), true);
        CHECK(rb.IsGood());
    });

    TEST_CASE(WriteBuffer_VersionMismatch) {
        // Manually forge a buffer with a wrong version header.
        Neuron::Serde::BitWriter w;
        w.WriteUint32(0xFFFFFFFFu); // wrong version
        w.WriteUint8(0);
        w.Flush();

        Neuron::Serde::ReadBuffer rb(w.Data());
        CHECK(rb.HasVersionMismatch());
        CHECK(!rb.IsGood());
    });
}
