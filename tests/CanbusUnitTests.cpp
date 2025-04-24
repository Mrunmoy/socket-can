#include <gtest/gtest.h>
#include "SocketCan.hpp"

using namespace SocketCAN;

TEST(CanbusUnit, ConstructorThrowsOnInvalidInterface)
{
    // We expect constructing Canbus on a non-existent
    // interface to throw a CanError exception.
    EXPECT_THROW(
        {
            Canbus bus{"does_not_exist0"};
        },
        CanError
    );
}

TEST(CanbusUnit, SendThrowsOnInvalidDlc)
{
    Canbus bus{"vcan0"};

    Frame f;
    f.id  = 0x100;
    f.dlc = 9;   // INVALID DLC

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, RtrFrameMustNotContainData)
{
    Canbus bus{"vcan0"};

    Frame f;
    f.id  = 0x200;
    f.rtr = true;
    f.dlc = 3;

    // Put illegal payload bytes (RTR frames must not send data)
    f.data[0] = 0xAA;
    f.data[1] = 0xBB;
    f.data[2] = 0xCC;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, StandardIdOutOfRangeThrows)
{
    Canbus bus{"vcan0"};

    Frame f;
    f.id       = 0x1800;  // beyond 11-bit
    f.dlc      = 1;
    f.extended = false;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, ExtendedIdOutOfRangeThrows)
{
    Canbus bus{"vcan0"};

    Frame f;
    f.id       = 0x3FFFFFFF;  // beyond 29-bit range
    f.dlc      = 1;
    f.extended = true;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, MoveConstructorTransfersFd)
{
    Canbus original{"vcan0"};
    int original_fd = original.fd();

    // Move construct
    Canbus moved{std::move(original)};

    EXPECT_EQ(original.fd(), -1) << "Original should relinquish fd after move";
    EXPECT_EQ(moved.fd(), original_fd) << "Moved object should take ownership";
}

TEST(CanbusUnit, MoveAssignmentTransfersFd)
{
    Canbus a{"vcan0"};
    int fd_a = a.fd();

    Canbus b{"vcan0"};
    (void)b.fd();  // we don't care what b.fd() is initially

    // Move assign
    b = std::move(a);

    EXPECT_EQ(a.fd(), -1) << "Source of move-assign should relinquish fd";
    EXPECT_EQ(b.fd(), fd_a) << "Destination should now own moved fd";
}

TEST(CanbusUnit, ReceiveWithZeroTimeoutReturnsFalse)
{
    Canbus reader{"vcan0"};

    Frame f;
    bool result = reader.receive(f, std::chrono::milliseconds{0});

    EXPECT_FALSE(result) << "receive(0ms) should return false when no data is available";
}

// ---------------------------------------------------------------------------
// CAN FD send validation
// ---------------------------------------------------------------------------

TEST(CanbusUnit, FdSendThrowsOnLenExceeds64)
{
    Canbus bus{"vcan0"};

    FdFrame f;
    f.id  = 0x100;
    f.len = 65;  // INVALID: max is 64

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, FdSendThrowsOnStandardIdOutOfRange)
{
    Canbus bus{"vcan0"};

    FdFrame f;
    f.id       = 0x1800;  // beyond 11-bit
    f.len      = 1;
    f.extended = false;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, FdSendThrowsOnExtendedIdOutOfRange)
{
    Canbus bus{"vcan0"};

    FdFrame f;
    f.id       = 0x3FFFFFFF;  // beyond 29-bit
    f.len      = 1;
    f.extended = true;

    EXPECT_THROW(bus.send(f), CanError);
}

// ---------------------------------------------------------------------------
// Listener registration overflow
// ---------------------------------------------------------------------------

namespace
{
    // Minimal no-op listener implementations for registration tests
    struct NoopClassicListener : IClassicFrameListener
    {
        void onClassicFrame(const Frame&) override {}
    };

    struct NoopFdListener : IFdFrameListener
    {
        void onFdFrame(const FdFrame&) override {}
    };
}

TEST(CanbusUnit, AddClassicListenerThrowsWhenFull)
{
    Canbus bus{"vcan0"};
    NoopClassicListener listeners[Canbus::MaxListeners + 1];

    for (std::size_t i = 0; i < Canbus::MaxListeners; ++i)
        ASSERT_NO_THROW(bus.addClassicListener(&listeners[i]));

    EXPECT_THROW(bus.addClassicListener(&listeners[Canbus::MaxListeners]), CanError);
}

TEST(CanbusUnit, AddFdListenerThrowsWhenFull)
{
    Canbus bus{"vcan0"};
    NoopFdListener listeners[Canbus::MaxListeners + 1];

    for (std::size_t i = 0; i < Canbus::MaxListeners; ++i)
        ASSERT_NO_THROW(bus.addFdListener(&listeners[i]));

    EXPECT_THROW(bus.addFdListener(&listeners[Canbus::MaxListeners]), CanError);
}

TEST(CanbusUnit, ListenerReceiveWithZeroTimeoutReturnsFalse)
{
    Canbus reader{"vcan0"};

    // No listeners registered — dispatch-receive should still honour timeout
    bool result = reader.receive(std::chrono::milliseconds{0});
    EXPECT_FALSE(result);
}
