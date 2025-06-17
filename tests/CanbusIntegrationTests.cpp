#include <gtest/gtest.h>
#include "SocketCan.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include <net/if.h>

using namespace std::chrono_literals;
using namespace SocketCAN;

// Integration tests talk to a real kernel CAN interface through the default
// (POSIX) socket API. They use can0 unless SOCKETCAN_TEST_IFACE names another
// interface. The dev container brings can0 up as a virtual (vcan) device.
// When the interface is missing (or the kernel has no CAN support) the tests
// are skipped rather than failed, so the unit tests still gate every build.

class CanbusIntegration : public ::testing::Test
{
protected:
    static std::string iface()
    {
        const char* env = std::getenv("SOCKETCAN_TEST_IFACE");
        return (env && *env) ? env : "can0";
    }

    void SetUp() override
    {
        if (::if_nametoindex(iface().c_str()) == 0)
            GTEST_SKIP() << iface() << " not present; see README to bring it up";
    }

    const std::string ifname = iface();
};

TEST_F(CanbusIntegration, ConstructorThrowsOnInvalidInterface)
{
    EXPECT_THROW({ CanFD bus{"does_not_exist0"}; }, CanError);
}

TEST_F(CanbusIntegration, FdEnabledReflectsSocketType)
{
    CanFD      fdBus{ifname};
    CanClassic classicBus{ifname};

    EXPECT_TRUE(fdBus.fdEnabled())
        << ifname << " should be FD-capable (vcan is; real hardware needs 'fd on')";
    EXPECT_FALSE(classicBus.fdEnabled());
}

// ---------------------------------------------------------------------------
// Classic CAN round-trips
// ---------------------------------------------------------------------------

TEST_F(CanbusIntegration, SendAndReceiveClassic)
{
    CanFD writer{ifname};
    CanFD reader{ifname};

    Frame tx;
    tx.id      = 0x123;
    tx.dlc     = 4;
    tx.data[0] = 0xDE;
    tx.data[1] = 0xAD;
    tx.data[2] = 0xBE;
    tx.data[3] = 0xEF;

    writer.send(tx);

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, 100ms)) << "Timed out waiting for CAN frame";

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_FALSE(rx.extended);
    EXPECT_FALSE(rx.rtr);
    EXPECT_EQ(rx.dlc, tx.dlc);
    for (int i = 0; i < rx.dlc; ++i)
        EXPECT_EQ(rx.data[i], tx.data[i]);
}

TEST_F(CanbusIntegration, SendAndReceiveExtendedId)
{
    // Exercises the EFF flag/mask encode+decode path, which the standard-ID
    // round-trip never touches.
    CanFD writer{ifname};
    CanFD reader{ifname};

    Frame tx;
    tx.id       = 0x1ABCDE;   // 29-bit ID
    tx.extended = true;
    tx.dlc      = 3;
    tx.data[0]  = 0x11;
    tx.data[1]  = 0x22;
    tx.data[2]  = 0x33;

    writer.send(tx);

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, 100ms));
    EXPECT_TRUE(rx.extended);
    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.dlc, tx.dlc);
    for (int i = 0; i < tx.dlc; ++i)
        EXPECT_EQ(rx.data[i], tx.data[i]);
}

TEST_F(CanbusIntegration, RtrFrameSendAndReceive)
{
    // RTR flag must round-trip (set on send, decoded on receive). Complements
    // the unit-level RTR *rejection* test with the valid path.
    CanFD writer{ifname};
    CanFD reader{ifname};

    Frame tx;
    tx.id  = 0x300;
    tx.rtr = true;
    tx.dlc = 4;   // requesting 4 bytes; no payload for RTR

    writer.send(tx);

    Frame rx;
    ASSERT_TRUE(reader.receive(rx, 100ms));
    EXPECT_EQ(rx.id, tx.id);
    EXPECT_TRUE(rx.rtr);
    EXPECT_EQ(rx.dlc, 4);
}

TEST_F(CanbusIntegration, NonBlockingReceiveTimesOutThenReads)
{
    // Only test of the blocking=false constructor path (fcntl O_NONBLOCK)
    // combined with the empty -> data -> empty poll cycle.
    CanFD writer{ifname};
    CanFD reader{ifname, true, false};

    Frame rx;
    EXPECT_FALSE(reader.receive(rx, 0ms)) << "buffer should start empty";

    Frame tx;
    tx.id      = 0x200;
    tx.dlc     = 1;
    tx.data[0] = 0x5A;
    writer.send(tx);

    ASSERT_TRUE(reader.receive(rx, 0ms)) << "pending frame should read immediately";
    EXPECT_EQ(rx.id, 0x200u);
    EXPECT_EQ(rx.data[0], 0x5Au);

    EXPECT_FALSE(reader.receive(rx, 0ms)) << "buffer should be drained again";
}

// ---------------------------------------------------------------------------
// Mixed-mode behaviour on the FD socket
// ---------------------------------------------------------------------------

TEST_F(CanbusIntegration, DirectReceiveThrowsOnFdFrame)
{
    // The direct receive(Frame&) overload cannot represent an FD frame, so it
    // must throw rather than silently truncate — steering users to the
    // listener-based receive() for mixed traffic.
    CanFD writer{ifname};
    CanFD reader{ifname};

    FdFrame fd;
    fd.id  = 0x321;
    fd.len = 16;
    fd.brs = true;
    for (uint8_t i = 0; i < fd.len; ++i)
        fd.data[i] = i;
    writer.send(fd);

    Frame rx;
    EXPECT_THROW(reader.receive(rx, 100ms), CanError);
}

TEST_F(CanbusIntegration, FdSendAndReceiveFullPayload)
{
    CanFD writer{ifname};
    CanFD reader{ifname};

    FdFrame tx;
    tx.id  = 0x456;
    tx.len = 64;
    tx.brs = true;
    for (uint8_t i = 0; i < 64; ++i)
        tx.data[i] = i;

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
    ASSERT_TRUE(reader.receive(100ms)) << "Timed out waiting for CAN FD frame";
    ASSERT_TRUE(got) << "FD listener was not called";

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.len, tx.len);
    EXPECT_TRUE(rx.brs);
    for (uint8_t i = 0; i < 64; ++i)
        EXPECT_EQ(rx.data[i], tx.data[i]);
}

TEST_F(CanbusIntegration, MixedClassicAndFdDispatchToSeparateListeners)
{
    // The core of the design: the read-size discriminator must route a classic
    // frame only to classic listeners and an FD frame only to FD listeners.
    CanFD writer{ifname};
    CanFD reader{ifname};

    Frame   rxClassic;
    FdFrame rxFd;
    bool    gotClassic = false;
    bool    gotFd      = false;

    struct ClassicSink : IClassicFrameListener
    {
        Frame& out; bool& flag;
        ClassicSink(Frame& o, bool& f) : out(o), flag(f) {}
        void onClassicFrame(const Frame& frame) override { out = frame; flag = true; }
    } classicSink{rxClassic, gotClassic};

    struct FdSink : IFdFrameListener
    {
        FdFrame& out; bool& flag;
        FdSink(FdFrame& o, bool& f) : out(o), flag(f) {}
        void onFdFrame(const FdFrame& frame) override { out = frame; flag = true; }
    } fdSink{rxFd, gotFd};

    reader.addClassicListener(&classicSink);
    reader.addFdListener(&fdSink);

    // ---- classic frame ----
    Frame classic;
    classic.id      = 0x111;
    classic.dlc     = 2;
    classic.data[0] = 0xAA;
    classic.data[1] = 0xBB;
    writer.send(classic);

    ASSERT_TRUE(reader.receive(100ms));
    EXPECT_TRUE(gotClassic) << "Classic listener should have fired";
    EXPECT_FALSE(gotFd)     << "FD listener must not fire on a classic frame";
    EXPECT_EQ(rxClassic.id, classic.id);
    EXPECT_EQ(rxClassic.data[0], 0xAAu);

    gotClassic = false;

    // ---- FD frame ----
    FdFrame fd;
    fd.id  = 0x222;
    fd.len = 16;
    fd.brs = true;
    for (uint8_t i = 0; i < 16; ++i)
        fd.data[i] = static_cast<uint8_t>(0x10 + i);
    writer.send(fd);

    ASSERT_TRUE(reader.receive(100ms));
    EXPECT_TRUE(gotFd)        << "FD listener should have fired";
    EXPECT_FALSE(gotClassic)  << "Classic listener must not fire on an FD frame";
    EXPECT_EQ(rxFd.id, fd.id);
    EXPECT_EQ(rxFd.len, fd.len);
    EXPECT_TRUE(rxFd.brs);
    for (uint8_t i = 0; i < 16; ++i)
        EXPECT_EQ(rxFd.data[i], static_cast<uint8_t>(0x10 + i));
}

TEST_F(CanbusIntegration, MultipleClassicListenersAllReceive)
{
    // The dispatch loop must fan out to every registered listener, not just one.
    CanFD writer{ifname};
    CanFD reader{ifname};

    int callCount = 0;
    struct CountingSink : IClassicFrameListener
    {
        int& count;
        explicit CountingSink(int& c) : count(c) {}
        void onClassicFrame(const Frame&) override { ++count; }
    };

    CountingSink s1{callCount}, s2{callCount}, s3{callCount};
    reader.addClassicListener(&s1);
    reader.addClassicListener(&s2);
    reader.addClassicListener(&s3);

    Frame tx;
    tx.id  = 0x400;
    tx.dlc = 1;
    writer.send(tx);

    ASSERT_TRUE(reader.receive(100ms));
    EXPECT_EQ(callCount, 3) << "All three Classic listeners must be called";
}
