#include <gtest/gtest.h>
#include "SocketCan.hpp"
#include <linux/can/raw.h>
#include <cstring>
#include <chrono>
#include <thread>

// enable 20ms, 5s, etc.
using namespace std::chrono_literals;


using namespace SocketCAN;

TEST(CanbusIntegration, SendAndReceiveOnVcan0)
{
    // These will throw SocketCAN::CanError if something is wrong
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    Frame tx;
    tx.id       = 0x123;
    tx.extended = false;
    tx.rtr      = false;
    tx.dlc      = 4;
    tx.data[0]  = 0xDE;
    tx.data[1]  = 0xAD;
    tx.data[2]  = 0xBE;
    tx.data[3]  = 0xEF;

    // Send – will throw on error
    writer.send(tx);

    // Receive with a reasonable timeout
    Frame rx;
    ASSERT_TRUE(reader.receive(rx, std::chrono::milliseconds{100}))
        << "Timed out waiting for CAN frame on vcan0";

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.extended, tx.extended);
    EXPECT_EQ(rx.rtr, tx.rtr);
    EXPECT_EQ(rx.dlc, tx.dlc);

    for (int i = 0; i < rx.dlc; ++i)
    {
        EXPECT_EQ(rx.data[i], tx.data[i]);
    }
}

TEST(CanbusIntegration, SendAndReceiveExtendedId)
{
    // Ensure vcan0 exists
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    Frame tx;
    tx.id       = 0x1ABCDE;     // Large 29-bit ID
    tx.extended = true;         // MUST specify extended ID
    tx.dlc      = 3;
    tx.data[0]  = 0x11;
    tx.data[1]  = 0x22;
    tx.data[2]  = 0x33;

    writer.send(tx);

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, std::chrono::milliseconds{10}))
        << "No frame received on vcan0";

    // Validate
    EXPECT_TRUE(rx.extended);
    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.dlc, tx.dlc);
    for (int i = 0; i < tx.dlc; ++i)
        EXPECT_EQ(rx.data[i], tx.data[i]);
}

TEST(CanbusIntegration, SendAndReceiveZeroDlc)
{
    // Ensure vcan0 exists (set up by fixture)
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    Frame tx;
    tx.id       = 0x42;
    tx.extended = false;
    tx.dlc      = 0;      // IMPORTANT: zero-length frame

    writer.send(tx);

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, std::chrono::milliseconds{10}))
        << "No frame received on vcan0";

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_FALSE(rx.extended);
    EXPECT_EQ(rx.dlc, 0);

    // All rx.data bytes must be untouched defaults (zero)
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(rx.data[i], 0);
}

TEST(CanbusIntegration, BurstSendAndReceiveTenFrames)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    // Send 10 frames with simple incrementing payload
    for (uint32_t i = 0; i < 10; ++i)
    {
        Frame tx;
        tx.id = 0x100 + i;
        tx.dlc = 1;
        tx.data[0] = static_cast<uint8_t>(i);

        ASSERT_NO_THROW(writer.send(tx));
    }

    // Now receive the 10 frames and verify ordering
    for (uint32_t i = 0; i < 10; ++i)
    {
        Frame rx;
        bool got = reader.receive(rx, std::chrono::milliseconds{20});
        ASSERT_TRUE(got) << "Failed to receive frame index " << i;

        EXPECT_EQ(rx.id, 0x100 + i);
        EXPECT_EQ(rx.dlc, 1);
        EXPECT_EQ(rx.data[0], static_cast<uint8_t>(i));
    }
}

TEST(CanbusIntegration, NonBlockingReceiveDuringTraffic)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0", true, false}; // blocking = false => fd is non-blocking

    // First ensure no frame is pending
    {
        Frame rx;
        bool got = reader.receive(rx, std::chrono::milliseconds{0});
        EXPECT_FALSE(got);
    }

    // Send 3 frames quickly
    for (int i = 0; i < 3; ++i)
    {
        Frame tx;
        tx.id = 0x200 + i;
        tx.dlc = 1;
        tx.data[0] = i;
        ASSERT_NO_THROW(writer.send(tx));
    }

    // Now, non-blocking receive should immediately get all 3 frames
    for (int i = 0; i < 3; ++i)
    {
        Frame rx;
        bool got = reader.receive(rx, std::chrono::milliseconds{0});
        ASSERT_TRUE(got) << "Expected pending frame " << i;

        EXPECT_EQ(rx.id, 0x200 + i);
        EXPECT_EQ(rx.dlc, 1);
        EXPECT_EQ(rx.data[0], i);
    }

    // And now, buffer should again be empty
    {
        Frame rx;
        bool got = reader.receive(rx, std::chrono::milliseconds{0});
        EXPECT_FALSE(got);
    }
}

TEST(CanbusIntegration, RtrFrameSendAndReceive)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    Frame tx;
    tx.id = 0x300;
    tx.rtr = true;
    tx.dlc = 4; // requesting 4 bytes
    // payload ignored for RTR

    ASSERT_NO_THROW(writer.send(tx));

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, std::chrono::milliseconds{20}));

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_TRUE(rx.rtr);
    EXPECT_EQ(rx.dlc, 4);
}

#include <linux/can/raw.h>

TEST(CanbusIntegration, CanFilterBlocksOtherIds)
{
    CanFD writer{"vcan0"};

    // Reader A: no filter
    CanFD readerA{"vcan0"};

    // Reader B: apply filter to only see ID 0x555
    int rfd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    ASSERT_GE(rfd, 0) << "Failed to open raw CAN socket";

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, "vcan0", IFNAMSIZ);
    ASSERT_GE(::ioctl(rfd, SIOCGIFINDEX, &ifr), 0);

    struct sockaddr_can addr {};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    ASSERT_GE(::bind(rfd, (sockaddr*)&addr, sizeof(addr)), 0);

    struct can_filter filter {};
    filter.can_id = 0x555;
    filter.can_mask = CAN_SFF_MASK;
    ASSERT_GE(::setsockopt(rfd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)), 0);

    // Send two frames: one matching, one not
    {
        Frame tx;
        tx.id = 0x111;
        tx.dlc = 1;
        tx.data[0] = 0xAA;
        writer.send(tx);
    }
    {
        Frame tx;
        tx.id = 0x555;
        tx.dlc = 1;
        tx.data[0] = 0xBB;
        writer.send(tx);
    }

    // Reader A: should receive TWO frames
    {
        Frame rx1; readerA.receive(rx1, 20ms);
        Frame rx2; readerA.receive(rx2, 20ms);
    }

    // Reader B: should receive ONLY the 0x555 frame
    struct can_frame cf{};
    ssize_t n = ::read(rfd, &cf, sizeof(cf));
    ASSERT_EQ(n, sizeof(cf));
    EXPECT_EQ(cf.can_id & CAN_SFF_MASK, 0x555u);
}

// ---------------------------------------------------------------------------
// CAN FD integration tests
// ---------------------------------------------------------------------------

TEST(CanbusIntegration, FdSendAndReceiveFullPayload)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    FdFrame tx;
    tx.id  = 0x456;
    tx.len = 64;
    tx.brs = true;
    for (uint8_t i = 0; i < 64; ++i)
        tx.data[i] = i;

    writer.send(tx);

    FdFrame rx;
    // Use listener-based receive to capture the FD frame
    bool got = false;

    struct Sink : IFdFrameListener
    {
        FdFrame& out;
        bool&    flag;
        Sink(FdFrame& o, bool& f) : out(o), flag(f) {}
        void onFdFrame(const FdFrame& frame) override
        {
            out  = frame;
            flag = true;
        }
    } sink{rx, got};

    reader.addFdListener(&sink);
    ASSERT_TRUE(reader.receive(std::chrono::milliseconds{100}))
        << "Timed out waiting for CAN FD frame on vcan0";

    ASSERT_TRUE(got) << "FD listener was not called";
    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.len, tx.len);
    EXPECT_TRUE(rx.brs);
    for (uint8_t i = 0; i < 64; ++i)
        EXPECT_EQ(rx.data[i], tx.data[i]);
}

TEST(CanbusIntegration, FdSendAndReceiveExtendedId)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    FdFrame tx;
    tx.id       = 0x1ABCDE;
    tx.extended = true;
    tx.len      = 8;
    tx.data[0]  = 0xCA;
    tx.data[1]  = 0xFE;

    writer.send(tx);

    FdFrame rx;
    bool got = false;

    struct Sink : IFdFrameListener
    {
        FdFrame& out;
        bool&    flag;
        Sink(FdFrame& o, bool& f) : out(o), flag(f) {}
        void onFdFrame(const FdFrame& frame) override { out = frame; flag = true; }
    } sink{rx, got};

    reader.addFdListener(&sink);
    ASSERT_TRUE(reader.receive(std::chrono::milliseconds{100}));
    ASSERT_TRUE(got);
    EXPECT_TRUE(rx.extended);
    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.data[0], 0xCAu);
    EXPECT_EQ(rx.data[1], 0xFEu);
}

TEST(CanbusIntegration, MixedClassicAndFdDispatchToSeparateListeners)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    // ---- listeners ----
    Frame   rxClassic;
    FdFrame rxFd;
    bool    gotClassic = false;
    bool    gotFd      = false;

    struct ClassicSink : IClassicFrameListener
    {
        Frame& out;
        bool&  flag;
        ClassicSink(Frame& o, bool& f) : out(o), flag(f) {}
        void onClassicFrame(const Frame& frame) override { out = frame; flag = true; }
    } classicSink{rxClassic, gotClassic};

    struct FdSink : IFdFrameListener
    {
        FdFrame& out;
        bool&    flag;
        FdSink(FdFrame& o, bool& f) : out(o), flag(f) {}
        void onFdFrame(const FdFrame& frame) override { out = frame; flag = true; }
    } fdSink{rxFd, gotFd};

    reader.addClassicListener(&classicSink);
    reader.addFdListener(&fdSink);

    // ---- send classic ----
    Frame classic;
    classic.id    = 0x111;
    classic.dlc   = 2;
    classic.data[0] = 0xAA;
    classic.data[1] = 0xBB;
    writer.send(classic);

    ASSERT_TRUE(reader.receive(std::chrono::milliseconds{100}));
    EXPECT_TRUE(gotClassic)  << "Classic listener should have fired";
    EXPECT_FALSE(gotFd)      << "FD listener must not fire on a classic frame";
    EXPECT_EQ(rxClassic.id, classic.id);
    EXPECT_EQ(rxClassic.data[0], 0xAAu);

    gotClassic = false;  // reset for second frame

    // ---- send FD ----
    FdFrame fd;
    fd.id    = 0x222;
    fd.len   = 16;
    fd.brs   = true;
    for (uint8_t i = 0; i < 16; ++i)
        fd.data[i] = static_cast<uint8_t>(0x10 + i);
    writer.send(fd);

    ASSERT_TRUE(reader.receive(std::chrono::milliseconds{100}));
    EXPECT_TRUE(gotFd)        << "FD listener should have fired";
    EXPECT_FALSE(gotClassic)  << "Classic listener must not fire on an FD frame";
    EXPECT_EQ(rxFd.id, fd.id);
    EXPECT_EQ(rxFd.len, fd.len);
    EXPECT_TRUE(rxFd.brs);
    for (uint8_t i = 0; i < 16; ++i)
        EXPECT_EQ(rxFd.data[i], static_cast<uint8_t>(0x10 + i));
}

TEST(CanbusIntegration, MultipleClassicListenersAllReceive)
{
    CanFD writer{"vcan0"};
    CanFD reader{"vcan0"};

    int callCount = 0;

    struct CountingSink : IClassicFrameListener
    {
        int& count;
        explicit CountingSink(int& c) : count(c) {}
        void onClassicFrame(const Frame&) override { ++count; }
    };

    CountingSink s1{callCount};
    CountingSink s2{callCount};
    CountingSink s3{callCount};

    reader.addClassicListener(&s1);
    reader.addClassicListener(&s2);
    reader.addClassicListener(&s3);

    Frame tx;
    tx.id  = 0x400;
    tx.dlc = 1;
    writer.send(tx);

    ASSERT_TRUE(reader.receive(std::chrono::milliseconds{100}));
    EXPECT_EQ(callCount, 3) << "All three Classic listeners must be called";
}
