#include <gtest/gtest.h>
#include "SocketCan.hpp"

using namespace SocketCAN;

TEST(CanbusUnit, ConstructorThrowsOnInvalidInterface)
{
    // We expect constructing CanFD on a non-existent
    // interface to throw a CanError exception.
    EXPECT_THROW(
        {
            CanFD bus{"does_not_exist0"};
        },
        CanError
    );
}

TEST(CanbusUnit, SendThrowsOnInvalidDlc)
{
    CanFD bus{"vcan0"};

    Frame f;
    f.id  = 0x100;
    f.dlc = 9;   // INVALID DLC

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, RtrFrameMustNotContainData)
{
    CanFD bus{"vcan0"};

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
    CanFD bus{"vcan0"};

    Frame f;
    f.id       = 0x1800;  // beyond 11-bit
    f.dlc      = 1;
    f.extended = false;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, ExtendedIdOutOfRangeThrows)
{
    CanFD bus{"vcan0"};

    Frame f;
    f.id       = 0x3FFFFFFF;  // beyond 29-bit range
    f.dlc      = 1;
    f.extended = true;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, MoveConstructorTransfersFd)
{
    CanFD original{"vcan0"};
    int original_fd = original.fd();

    // Move construct
    CanFD moved{std::move(original)};

    EXPECT_EQ(original.fd(), -1) << "Original should relinquish fd after move";
    EXPECT_EQ(moved.fd(), original_fd) << "Moved object should take ownership";
}

TEST(CanbusUnit, MoveAssignmentTransfersFd)
{
    CanFD a{"vcan0"};
    int fd_a = a.fd();

    CanFD b{"vcan0"};
    (void)b.fd();  // we don't care what b.fd() is initially

    // Move assign
    b = std::move(a);

    EXPECT_EQ(a.fd(), -1) << "Source of move-assign should relinquish fd";
    EXPECT_EQ(b.fd(), fd_a) << "Destination should now own moved fd";
}

TEST(CanbusUnit, ReceiveWithZeroTimeoutReturnsFalse)
{
    CanFD reader{"vcan0"};

    Frame f;
    bool result = reader.receive(f, std::chrono::milliseconds{0});

    EXPECT_FALSE(result) << "receive(0ms) should return false when no data is available";
}

// ---------------------------------------------------------------------------
// CAN FD send validation
// ---------------------------------------------------------------------------

TEST(CanbusUnit, FdSendThrowsOnLenExceeds64)
{
    CanFD bus{"vcan0"};

    FdFrame f;
    f.id  = 0x100;
    f.len = 65;  // INVALID: max is 64

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, FdSendThrowsOnStandardIdOutOfRange)
{
    CanFD bus{"vcan0"};

    FdFrame f;
    f.id       = 0x1800;  // beyond 11-bit
    f.len      = 1;
    f.extended = false;

    EXPECT_THROW(bus.send(f), CanError);
}

TEST(CanbusUnit, FdSendThrowsOnExtendedIdOutOfRange)
{
    CanFD bus{"vcan0"};

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
    CanFD bus{"vcan0"};
    NoopClassicListener listeners[CanFD::MaxListeners + 1];

    for (std::size_t i = 0; i < CanFD::MaxListeners; ++i)
        ASSERT_NO_THROW(bus.addClassicListener(&listeners[i]));

    EXPECT_THROW(bus.addClassicListener(&listeners[CanFD::MaxListeners]), CanError);
}

TEST(CanbusUnit, AddFdListenerThrowsWhenFull)
{
    CanFD bus{"vcan0"};
    NoopFdListener listeners[CanFD::MaxListeners + 1];

    for (std::size_t i = 0; i < CanFD::MaxListeners; ++i)
        ASSERT_NO_THROW(bus.addFdListener(&listeners[i]));

    EXPECT_THROW(bus.addFdListener(&listeners[CanFD::MaxListeners]), CanError);
}

TEST(CanbusUnit, ListenerReceiveWithZeroTimeoutReturnsFalse)
{
    CanFD reader{"vcan0"};

    // No listeners registered — dispatch-receive should still honour timeout
    bool result = reader.receive(std::chrono::milliseconds{0});
    EXPECT_FALSE(result);
}

// ---------------------------------------------------------------------------
// Compile-time CAN FD mode (CanFD vs CanClassic)
// ---------------------------------------------------------------------------

// Detection idiom: does T accept send(const FdFrame&) / addFdListener(...)?
template <typename, typename = void>
struct HasFdSend : std::false_type {};
template <typename T>
struct HasFdSend<T, std::void_t<decltype(std::declval<T&>().send(std::declval<const SocketCAN::FdFrame&>()))>>
    : std::true_type {};

template <typename, typename = void>
struct HasFdListener : std::false_type {};
template <typename T>
struct HasFdListener<T, std::void_t<decltype(std::declval<T&>().addFdListener(std::declval<SocketCAN::IFdFrameListener*>()))>>
    : std::true_type {};

TEST(CanbusUnit, FdMembersAreCompileTimeGatedByType)
{
    // The FD socket type exposes the FD API; the classic type does not.
    static_assert(HasFdSend<SocketCAN::CanFD>::value,
                  "CanFD (FD) must accept send(FdFrame)");
    static_assert(!HasFdSend<SocketCAN::CanClassic>::value,
                  "CanClassic must reject send(FdFrame) at compile time");
    static_assert(HasFdListener<SocketCAN::CanFD>::value,
                  "CanFD (FD) must expose addFdListener");
    static_assert(!HasFdListener<SocketCAN::CanClassic>::value,
                  "CanClassic must not expose addFdListener");

    static_assert(CanFD::kFdMode, "CanFD::kFdMode must be true");
    static_assert(!CanClassic::kFdMode, "CanClassic::kFdMode must be false");
    SUCCEED();
}

TEST(CanbusUnit, FdEnabledByDefaultOnFdCapableInterface)
{
    CanFD bus{"vcan0"};
    EXPECT_TRUE(bus.fdEnabled()) << "vcan0 is FD-capable; the FD socket type enables FD";
}

TEST(CanbusUnit, ClassicOnlySocketReportsFdDisabled)
{
    CanClassic bus{"vcan0"};
    EXPECT_FALSE(bus.fdEnabled());
}
