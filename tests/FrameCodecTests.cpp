// Unit tests for FrameCodec: validation and conversion between the library's
// Frame/FdFrame types and the kernel's struct can_frame / struct canfd_frame.
// Pure functions, so no socket or CAN interface is involved.

#include <gtest/gtest.h>
#include "SocketCan.hpp"

using namespace SocketCAN;

// ---------------------------------------------------------------------------
// Classic CAN: validation
// ---------------------------------------------------------------------------

TEST(FrameCodec, ClassicAcceptsBoundaryValues)
{
    Frame f;
    f.id  = 0x7FF;
    f.dlc = 8;
    EXPECT_NO_THROW(FrameCodec::validate(f));

    f.extended = true;
    f.id       = 0x1FFFFFFF;
    EXPECT_NO_THROW(FrameCodec::validate(f));
}

TEST(FrameCodec, ClassicRejectsDlcAboveEight)
{
    Frame f;
    f.dlc = 9;
    EXPECT_THROW(FrameCodec::validate(f), CanError);
}

TEST(FrameCodec, ClassicRejectsStandardIdAbove11Bits)
{
    Frame f;
    f.id = 0x800;
    EXPECT_THROW(FrameCodec::validate(f), CanError);
}

TEST(FrameCodec, ClassicRejectsExtendedIdAbove29Bits)
{
    Frame f;
    f.extended = true;
    f.id       = 0x20000000;
    EXPECT_THROW(FrameCodec::validate(f), CanError);
}

TEST(FrameCodec, ClassicRejectsRtrWithData)
{
    Frame f;
    f.rtr     = true;
    f.dlc     = 2;
    f.data[1] = 0x01;
    EXPECT_THROW(FrameCodec::validate(f), CanError);
}

// ---------------------------------------------------------------------------
// Classic CAN: encode / decode
// ---------------------------------------------------------------------------

TEST(FrameCodec, ClassicEncodeSetsKernelFlags)
{
    Frame f;
    f.id       = 0x1ABCDE;
    f.extended = true;
    f.dlc      = 2;
    f.data[0]  = 0xDE;
    f.data[1]  = 0xAD;

    const can_frame cf = FrameCodec::encode(f);

    EXPECT_EQ(cf.can_id, 0x1ABCDEu | CAN_EFF_FLAG);
    EXPECT_EQ(cf.can_dlc, 2);
    EXPECT_EQ(cf.data[0], 0xDE);
    EXPECT_EQ(cf.data[1], 0xAD);
    EXPECT_EQ(cf.data[2], 0x00) << "bytes beyond dlc must be zeroed";
}

TEST(FrameCodec, ClassicEncodeSetsRtrFlag)
{
    Frame f;
    f.id  = 0x300;
    f.rtr = true;
    f.dlc = 4;

    const can_frame cf = FrameCodec::encode(f);

    EXPECT_EQ(cf.can_id, 0x300u | CAN_RTR_FLAG);
    EXPECT_EQ(cf.can_dlc, 4);
}

TEST(FrameCodec, ClassicEncodeValidates)
{
    Frame f;
    f.dlc = 9;
    EXPECT_THROW(FrameCodec::encode(f), CanError);
}

TEST(FrameCodec, ClassicRoundTrip)
{
    Frame tx;
    tx.id       = 0x123;
    tx.dlc      = 3;
    tx.data[0]  = 1;
    tx.data[1]  = 2;
    tx.data[2]  = 3;

    const Frame rx = FrameCodec::decode(FrameCodec::encode(tx));

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_FALSE(rx.extended);
    EXPECT_FALSE(rx.rtr);
    EXPECT_EQ(rx.dlc, tx.dlc);
    EXPECT_EQ(rx.data, tx.data);
}

TEST(FrameCodec, ClassicDecodeClampsOversizedDlc)
{
    // The kernel allows can_dlc 9..15 on some controllers (len8_dlc); never
    // read past the 8-byte payload.
    can_frame cf{};
    cf.can_id  = 0x10;
    cf.can_dlc = 15;

    const Frame f = FrameCodec::decode(cf);
    EXPECT_EQ(f.dlc, 8);
}

// ---------------------------------------------------------------------------
// CAN FD: validation
// ---------------------------------------------------------------------------

TEST(FrameCodec, FdAcceptsEveryValidLength)
{
    // Valid CAN FD payload lengths per the kernel documentation.
    for (uint8_t len : {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64})
    {
        FdFrame f;
        f.len = len;
        EXPECT_NO_THROW(FrameCodec::validate(f)) << "len=" << int(len);
    }
}

TEST(FrameCodec, FdRejectsLengthsWithNoDlcCode)
{
    // These would be silently padded by the kernel, so the receiver would see
    // a different length from the one sent.
    for (uint8_t len : {9, 13, 33, 63, 65})
    {
        FdFrame f;
        f.len = len;
        EXPECT_THROW(FrameCodec::validate(f), CanError) << "len=" << int(len);
    }
}

TEST(FrameCodec, FdRejectsOutOfRangeIds)
{
    FdFrame f;
    f.id = 0x800;
    EXPECT_THROW(FrameCodec::validate(f), CanError);

    f.extended = true;
    f.id       = 0x20000000;
    EXPECT_THROW(FrameCodec::validate(f), CanError);
}

// ---------------------------------------------------------------------------
// CAN FD: encode / decode
// ---------------------------------------------------------------------------

TEST(FrameCodec, FdEncodeSetsFlags)
{
    FdFrame f;
    f.id  = 0x456;
    f.len = 12;
    f.brs = true;
    f.esi = true;

    const canfd_frame cf = FrameCodec::encode(f);

    EXPECT_EQ(cf.can_id, 0x456u);
    EXPECT_EQ(cf.len, 12);
    EXPECT_TRUE(cf.flags & CANFD_BRS);
    EXPECT_TRUE(cf.flags & CANFD_ESI);
}

TEST(FrameCodec, FdRoundTripFullPayload)
{
    FdFrame tx;
    tx.id       = 0x1FFFFFFF;
    tx.extended = true;
    tx.len      = 64;
    tx.brs      = true;
    for (uint8_t i = 0; i < 64; ++i)
        tx.data[i] = i;

    const FdFrame rx = FrameCodec::decode(FrameCodec::encode(tx));

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_TRUE(rx.extended);
    EXPECT_EQ(rx.len, 64);
    EXPECT_TRUE(rx.brs);
    EXPECT_FALSE(rx.esi);
    EXPECT_EQ(rx.data, tx.data);
}
