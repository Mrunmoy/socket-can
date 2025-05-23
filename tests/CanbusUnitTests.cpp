#include <gtest/gtest.h>
#include "SocketCan.hpp"

#include <type_traits>
#include <utility>

using namespace SocketCAN;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(CanbusUnit, ConstructorThrowsOnInvalidInterface)
{
    // Binding to a non-existent interface must fail loudly (ioctl -> throwErrno)
    // and not leak the socket fd.
    EXPECT_THROW({ CanFD bus{"does_not_exist0"}; }, CanError);
}

// ---------------------------------------------------------------------------
// Send validation — our pre-write guards (no frame ever reaches the bus)
// ---------------------------------------------------------------------------

TEST(CanbusUnit, ClassicSendRejectsInvalidFrames)
{
    CanFD bus{"vcan0"};

    {   // DLC > 8 is not a valid Classic CAN length
        Frame f;
        f.id  = 0x100;
        f.dlc = 9;
        EXPECT_THROW(bus.send(f), CanError);
    }
    {   // RTR frames must not carry data bytes
        Frame f;
        f.id      = 0x200;
        f.rtr     = true;
        f.dlc     = 3;
        f.data[0] = 0xAA;
        EXPECT_THROW(bus.send(f), CanError);
    }
    {   // Standard ID beyond 11 bits
        Frame f;
        f.id  = 0x1800;
        f.dlc = 1;
        EXPECT_THROW(bus.send(f), CanError);
    }
    {   // Extended ID beyond 29 bits
        Frame f;
        f.id       = 0x3FFFFFFF;
        f.extended = true;
        f.dlc      = 1;
        EXPECT_THROW(bus.send(f), CanError);
    }
}

TEST(CanbusUnit, FdSendRejectsInvalidFrames)
{
    CanFD bus{"vcan0"};

    {   // len beyond CANFD_MAX_DLEN (64)
        FdFrame f;
        f.id  = 0x100;
        f.len = 65;
        EXPECT_THROW(bus.send(f), CanError);
    }
    {   // Standard ID beyond 11 bits
        FdFrame f;
        f.id  = 0x1800;
        f.len = 1;
        EXPECT_THROW(bus.send(f), CanError);
    }
    {   // Extended ID beyond 29 bits
        FdFrame f;
        f.id       = 0x3FFFFFFF;
        f.extended = true;
        f.len      = 1;
        EXPECT_THROW(bus.send(f), CanError);
    }
}

// ---------------------------------------------------------------------------
// RAII / move semantics — fd ownership
// ---------------------------------------------------------------------------

TEST(CanbusUnit, MoveConstructorTransfersFd)
{
    CanFD original{"vcan0"};
    int original_fd = original.fd();

    CanFD moved{std::move(original)};

    EXPECT_EQ(original.fd(), -1) << "Source must relinquish fd after move";
    EXPECT_EQ(moved.fd(), original_fd) << "Destination must take ownership";
}

TEST(CanbusUnit, MoveAssignmentTransfersFd)
{
    CanFD a{"vcan0"};
    int fd_a = a.fd();

    CanFD b{"vcan0"};   // b's own fd is closed by the move-assign

    b = std::move(a);

    EXPECT_EQ(a.fd(), -1) << "Source of move-assign must relinquish fd";
    EXPECT_EQ(b.fd(), fd_a) << "Destination must own the moved fd";
}

TEST(CanbusUnit, OperationsThrowOnClosedBus)
{
    CanFD bus{"vcan0"};
    CanFD sink{std::move(bus)};   // bus is now closed (fd == -1)

    Frame   classic;
    FdFrame fd;

    EXPECT_THROW(bus.send(classic), CanError);
    EXPECT_THROW(bus.send(fd), CanError);
    EXPECT_THROW(bus.receive(classic, std::chrono::milliseconds{0}), CanError);
    EXPECT_THROW(bus.receive(std::chrono::milliseconds{0}), CanError);
}

// ---------------------------------------------------------------------------
// Receive timeout — both overloads share the poll/timeout path
// ---------------------------------------------------------------------------

TEST(CanbusUnit, ReceiveReturnsFalseOnTimeout)
{
    CanFD reader{"vcan0"};

    Frame f;
    EXPECT_FALSE(reader.receive(f, std::chrono::milliseconds{0}))
        << "direct receive() must return false when no data is available";
    EXPECT_FALSE(reader.receive(std::chrono::milliseconds{0}))
        << "dispatch receive() must return false when no data is available";
}

// ---------------------------------------------------------------------------
// Listener registration
// ---------------------------------------------------------------------------

namespace
{
    struct NoopClassicListener : IClassicFrameListener
    {
        void onClassicFrame(const Frame&) override {}
    };

    struct NoopFdListener : IFdFrameListener
    {
        void onFdFrame(const FdFrame&) override {}
    };
}

TEST(CanbusUnit, AddListenerRejectsNull)
{
    CanFD bus{"vcan0"};
    EXPECT_THROW(bus.addClassicListener(nullptr), CanError);
    EXPECT_THROW(bus.addFdListener(nullptr), CanError);
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

// ---------------------------------------------------------------------------
// Compile-time CAN FD mode (CanFD vs CanClassic) — the headline contract
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
    static_assert(HasFdSend<SocketCAN::CanFD>::value,
                  "CanFD must accept send(FdFrame)");
    static_assert(!HasFdSend<SocketCAN::CanClassic>::value,
                  "CanClassic must reject send(FdFrame) at compile time");
    static_assert(HasFdListener<SocketCAN::CanFD>::value,
                  "CanFD must expose addFdListener");
    static_assert(!HasFdListener<SocketCAN::CanClassic>::value,
                  "CanClassic must not expose addFdListener");

    static_assert(CanFD::kFdMode, "CanFD::kFdMode must be true");
    static_assert(!CanClassic::kFdMode, "CanClassic::kFdMode must be false");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// FD enable state (runtime, depends on the interface)
// ---------------------------------------------------------------------------

TEST(CanbusUnit, FdEnabledReflectsSocketType)
{
    CanFD      fdBus{"vcan0"};
    CanClassic classicBus{"vcan0"};

    EXPECT_TRUE(fdBus.fdEnabled())
        << "vcan0 is FD-capable; the FD socket type must enable CAN_RAW_FD_FRAMES";
    EXPECT_FALSE(classicBus.fdEnabled())
        << "classic-only socket never enables CAN FD";
}
