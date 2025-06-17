// Unit tests for BasicCanbus (CanFD / CanClassic).
//
// The bus takes its system calls through an injected ISocketApi, so these
// tests use FakeSocketApi and need no CAN interface or kernel support. The
// real-socket behaviour is covered by CanbusIntegrationTests.cpp.

#include <gtest/gtest.h>
#include "SocketCan.hpp"
#include "FakeSocketApi.hpp"

#include <type_traits>
#include <utility>

using namespace SocketCAN;
using SocketCAN::Testing::FakeSocketApi;
using namespace std::chrono_literals;

namespace
{
    struct RecordingClassicListener : IClassicFrameListener
    {
        int   calls = 0;
        Frame last;
        void onClassicFrame(const Frame& f) override { ++calls; last = f; }
    };

    struct RecordingFdListener : IFdFrameListener
    {
        int     calls = 0;
        FdFrame last;
        void onFdFrame(const FdFrame& f) override { ++calls; last = f; }
    };

    can_frame kernelClassic(uint32_t canId, uint8_t dlc)
    {
        can_frame cf{};
        cf.can_id  = canId;
        cf.can_dlc = dlc;
        for (uint8_t i = 0; i < dlc && i < CAN_MAX_DLEN; ++i)
            cf.data[i] = static_cast<uint8_t>(0xA0 + i);
        return cf;
    }

    canfd_frame kernelFd(uint32_t canId, uint8_t len, uint8_t flags = 0)
    {
        canfd_frame cf{};
        cf.can_id = canId;
        cf.len    = len;
        cf.flags  = flags;
        for (uint8_t i = 0; i < len; ++i)
            cf.data[i] = i;
        return cf;
    }
}

class CanbusUnit : public ::testing::Test
{
protected:
    FakeSocketApi api;
};

// ---------------------------------------------------------------------------
// Construction: what the bus asks the kernel for
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, FdBusEnablesFdFramesAndBindsToInterface)
{
    CanFD bus{"can0", SocketOptions{}, api};

    EXPECT_EQ(api.socketCalls, 1);
    EXPECT_EQ(api.options.at(CAN_RAW_FD_FRAMES), 1);
    EXPECT_EQ(api.options.at(CAN_RAW_LOOPBACK), 1);
    EXPECT_EQ(api.boundIfindex, 3);
    EXPECT_EQ(bus.fd(), FakeSocketApi::kFd);
    EXPECT_TRUE(bus.fdEnabled());
}

TEST_F(CanbusUnit, ClassicBusNeverRequestsFdFrames)
{
    CanClassic bus{"can0", SocketOptions{}, api};

    EXPECT_EQ(api.options.count(CAN_RAW_FD_FRAMES), 0u);
    EXPECT_FALSE(bus.fdEnabled());
}

TEST_F(CanbusUnit, OptionsControlLoopbackAndBlocking)
{
    SocketOptions opts;
    opts.loopback = false;
    opts.blocking = false;

    CanFD bus{"can0", opts, api};

    EXPECT_EQ(api.options.at(CAN_RAW_LOOPBACK), 0);
    EXPECT_TRUE(api.flags & O_NONBLOCK);
}

TEST_F(CanbusUnit, FdFallsBackToClassicWhenKernelLacksSupport)
{
    api.failures["setsockopt:" + std::to_string(CAN_RAW_FD_FRAMES)] = ENOPROTOOPT;

    CanFD bus{"can0", SocketOptions{}, api};

    EXPECT_FALSE(bus.fdEnabled());
    EXPECT_THROW(bus.send(FdFrame{}), CanError) << "FD send needs FD enabled";
}

// ---------------------------------------------------------------------------
// Construction: failures throw and never leak the descriptor
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, ThrowsWhenSocketCannotBeCreated)
{
    api.failures["socket"] = EAFNOSUPPORT;
    EXPECT_THROW((CanFD{"can0", SocketOptions{}, api}), CanError);
    EXPECT_EQ(api.closeCalls, 0) << "nothing was opened, nothing to close";
}

TEST_F(CanbusUnit, ThrowsAndClosesOnUnknownInterface)
{
    EXPECT_THROW((CanFD{"does_not_exist0", SocketOptions{}, api}), CanError);
    EXPECT_EQ(api.closeCalls, 1);
}

TEST_F(CanbusUnit, RejectsTooLongInterfaceNameBeforeOpening)
{
    const std::string name(IFNAMSIZ, 'x');
    EXPECT_THROW((CanFD{name, SocketOptions{}, api}), CanError);
    EXPECT_EQ(api.socketCalls, 0);
    EXPECT_EQ(api.closeCalls, 0);
}

TEST_F(CanbusUnit, ThrowsAndClosesOnUnexpectedFdOptionError)
{
    api.failures["setsockopt:" + std::to_string(CAN_RAW_FD_FRAMES)] = EPERM;
    EXPECT_THROW((CanFD{"can0", SocketOptions{}, api}), CanError);
    EXPECT_EQ(api.closeCalls, 1);
}

TEST_F(CanbusUnit, ThrowsAndClosesOnBindFailure)
{
    api.failures["bind"] = EADDRINUSE;
    EXPECT_THROW((CanFD{"can0", SocketOptions{}, api}), CanError);
    EXPECT_EQ(api.closeCalls, 1);
}

// ---------------------------------------------------------------------------
// RAII / move semantics
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, DestructorClosesOnce)
{
    {
        CanFD bus{"can0", SocketOptions{}, api};
    }
    EXPECT_EQ(api.closeCalls, 1);
}

TEST_F(CanbusUnit, MoveConstructorTransfersOwnership)
{
    {
        CanFD original{"can0", SocketOptions{}, api};
        CanFD moved{std::move(original)};

        EXPECT_EQ(original.fd(), -1);
        EXPECT_EQ(moved.fd(), FakeSocketApi::kFd);
        EXPECT_TRUE(moved.fdEnabled());
    }
    EXPECT_EQ(api.closeCalls, 1) << "only the owner closes the descriptor";
}

TEST_F(CanbusUnit, MoveAssignmentClosesTargetsOldSocket)
{
    {
        CanFD a{"can0", SocketOptions{}, api};
        CanFD b{"can0", SocketOptions{}, api};
        b = std::move(a);

        EXPECT_EQ(api.closeCalls, 1) << "b's previous socket is released";
        EXPECT_EQ(a.fd(), -1);
        EXPECT_EQ(b.fd(), FakeSocketApi::kFd);
    }
    EXPECT_EQ(api.closeCalls, 2);
}

TEST_F(CanbusUnit, OperationsThrowOnMovedFromBus)
{
    CanFD bus{"can0", SocketOptions{}, api};
    CanFD sink{std::move(bus)};

    Frame   classic;
    FdFrame fd;
    RecordingClassicListener listener;

    EXPECT_THROW(bus.send(classic), CanError);
    EXPECT_THROW(bus.send(fd), CanError);
    EXPECT_THROW(bus.receive(classic, 0ms), CanError);
    EXPECT_THROW(bus.receive(0ms), CanError);
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, ClassicSendWritesOneKernelFrame)
{
    CanClassic bus{"can0", SocketOptions{}, api};

    Frame f;
    f.id      = 0x123;
    f.dlc     = 1;
    f.data[0] = 0x55;
    bus.send(f);

    ASSERT_EQ(api.written.size(), 1u);
    ASSERT_EQ(api.written[0].size(), sizeof(can_frame));
    const auto cf = api.lastWrittenAs<can_frame>();
    EXPECT_EQ(cf.can_id, 0x123u);
    EXPECT_EQ(cf.can_dlc, 1);
    EXPECT_EQ(cf.data[0], 0x55);
}

TEST_F(CanbusUnit, FdSendWritesOneKernelFdFrame)
{
    CanFD bus{"can0", SocketOptions{}, api};

    FdFrame f;
    f.id  = 0x456;
    f.len = 16;
    f.brs = true;
    bus.send(f);

    ASSERT_EQ(api.written.size(), 1u);
    ASSERT_EQ(api.written[0].size(), sizeof(canfd_frame));
    const auto cf = api.lastWrittenAs<canfd_frame>();
    EXPECT_EQ(cf.len, 16);
    EXPECT_TRUE(cf.flags & CANFD_BRS);
}

TEST_F(CanbusUnit, InvalidFramesAreRejectedBeforeWriting)
{
    CanFD bus{"can0", SocketOptions{}, api};

    Frame classic;
    classic.dlc = 9;
    EXPECT_THROW(bus.send(classic), CanError);

    FdFrame fd;
    fd.len = 65;
    EXPECT_THROW(bus.send(fd), CanError);

    EXPECT_TRUE(api.written.empty());
}

TEST_F(CanbusUnit, SendThrowsOnWriteErrorOrShortWrite)
{
    CanFD bus{"can0", SocketOptions{}, api};

    api.failures["write"] = ENOBUFS;
    EXPECT_THROW(bus.send(Frame{}), CanError);

    api.failures.clear();
    api.shortWrite = true;
    EXPECT_THROW(bus.send(Frame{}), CanError);
    EXPECT_THROW(bus.send(FdFrame{}), CanError);
}

// ---------------------------------------------------------------------------
// Receive: direct
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, ReceivePassesTimeoutToPoll)
{
    CanFD bus{"can0", SocketOptions{}, api};

    Frame f;
    EXPECT_FALSE(bus.receive(f, 25ms));
    EXPECT_FALSE(bus.receive(25ms));

    ASSERT_EQ(api.pollTimeouts.size(), 2u);
    EXPECT_EQ(api.pollTimeouts[0], 25);
    EXPECT_EQ(api.pollTimeouts[1], 25);
}

TEST_F(CanbusUnit, ReceiveDecodesClassicFrame)
{
    CanClassic bus{"can0", SocketOptions{}, api};
    api.queueRead(kernelClassic(0x1ABCDE | CAN_EFF_FLAG, 2));

    Frame f;
    ASSERT_TRUE(bus.receive(f, 0ms));
    EXPECT_TRUE(f.extended);
    EXPECT_EQ(f.id, 0x1ABCDEu);
    EXPECT_EQ(f.dlc, 2);
    EXPECT_EQ(f.data[1], 0xA1);
}

TEST_F(CanbusUnit, DirectReceiveThrowsOnFdFrame)
{
    CanFD bus{"can0", SocketOptions{}, api};
    api.queueRead(kernelFd(0x10, 12));

    Frame f;
    EXPECT_THROW(bus.receive(f, 0ms), CanError);
}

TEST_F(CanbusUnit, ReceiveThrowsOnReadError)
{
    CanFD bus{"can0", SocketOptions{}, api};
    api.queueRead(kernelClassic(0x10, 0));
    api.failures["read"] = EIO;

    Frame f;
    EXPECT_THROW(bus.receive(f, 0ms), CanError);
}

TEST_F(CanbusUnit, ReceiveThrowsOnPollError)
{
    CanFD bus{"can0", SocketOptions{}, api};
    api.failures["poll"] = EINTR;

    EXPECT_THROW(bus.receive(0ms), CanError);
}

// ---------------------------------------------------------------------------
// Receive: listener dispatch
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, DispatchRoutesEachFrameTypeToMatchingListeners)
{
    CanFD bus{"can0", SocketOptions{}, api};
    RecordingClassicListener classic;
    RecordingFdListener      fd;
    bus.addClassicListener(&classic);
    bus.addFdListener(&fd);

    api.queueRead(kernelClassic(0x111, 2));
    ASSERT_TRUE(bus.receive(0ms));
    EXPECT_EQ(classic.calls, 1);
    EXPECT_EQ(fd.calls, 0);
    EXPECT_EQ(classic.last.id, 0x111u);

    api.queueRead(kernelFd(0x222, 16, CANFD_BRS));
    ASSERT_TRUE(bus.receive(0ms));
    EXPECT_EQ(classic.calls, 1);
    EXPECT_EQ(fd.calls, 1);
    EXPECT_EQ(fd.last.id, 0x222u);
    EXPECT_EQ(fd.last.len, 16);
    EXPECT_TRUE(fd.last.brs);
}

TEST_F(CanbusUnit, DispatchNotifiesEveryListener)
{
    CanClassic bus{"can0", SocketOptions{}, api};
    RecordingClassicListener a, b, c;
    bus.addClassicListener(&a);
    bus.addClassicListener(&b);
    bus.addClassicListener(&c);

    api.queueRead(kernelClassic(0x400, 1));
    ASSERT_TRUE(bus.receive(0ms));

    EXPECT_EQ(a.calls + b.calls + c.calls, 3);
}

TEST_F(CanbusUnit, DispatchThrowsOnUnexpectedReadSize)
{
    CanFD bus{"can0", SocketOptions{}, api};
    api.pendingReads.emplace_back(5, uint8_t{0});

    EXPECT_THROW(bus.receive(0ms), CanError);
}

// ---------------------------------------------------------------------------
// Listener registration
// ---------------------------------------------------------------------------

TEST_F(CanbusUnit, AddListenerRejectsNullAndOverflow)
{
    CanFD bus{"can0", SocketOptions{}, api};
    EXPECT_THROW(bus.addClassicListener(nullptr), CanError);
    EXPECT_THROW(bus.addFdListener(nullptr), CanError);

    RecordingFdListener listeners[CanFD::MaxListeners + 1];
    for (std::size_t i = 0; i < CanFD::MaxListeners; ++i)
        ASSERT_NO_THROW(bus.addFdListener(&listeners[i]));
    EXPECT_THROW(bus.addFdListener(&listeners[CanFD::MaxListeners]), CanError);
}

// ---------------------------------------------------------------------------
// Compile-time CAN FD mode (CanFD vs CanClassic)
// ---------------------------------------------------------------------------

template <typename, typename = void>
struct HasFdSend : std::false_type {};
template <typename T>
struct HasFdSend<T, std::void_t<decltype(std::declval<T&>().send(std::declval<const FdFrame&>()))>>
    : std::true_type {};

template <typename, typename = void>
struct HasFdListener : std::false_type {};
template <typename T>
struct HasFdListener<T, std::void_t<decltype(std::declval<T&>().addFdListener(std::declval<IFdFrameListener*>()))>>
    : std::true_type {};

TEST(CanbusCompileTime, FdMembersAreGatedByType)
{
    static_assert(HasFdSend<CanFD>::value, "CanFD must accept send(FdFrame)");
    static_assert(!HasFdSend<CanClassic>::value, "CanClassic must reject send(FdFrame)");
    static_assert(HasFdListener<CanFD>::value, "CanFD must expose addFdListener");
    static_assert(!HasFdListener<CanClassic>::value, "CanClassic must not expose addFdListener");
    static_assert(CanFD::kFdMode);
    static_assert(!CanClassic::kFdMode);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Sender abstractions: application code can depend on an interface
// ---------------------------------------------------------------------------

TEST(CanbusCompileTime, BusesImplementSenderInterfaces)
{
    static_assert(std::is_base_of_v<ICanSender, CanClassic>);
    static_assert(std::is_base_of_v<ICanSender, CanFD>);
    static_assert(std::is_base_of_v<ICanFdSender, CanFD>);
    static_assert(!std::is_base_of_v<ICanFdSender, CanClassic>,
                  "a classic-only bus must not be usable where FD is required");
    SUCCEED();
}

namespace
{
    // Example of application code that depends on the abstraction only.
    void sendHeartbeat(ICanSender& out, uint8_t counter)
    {
        Frame f;
        f.id      = 0x700;
        f.dlc     = 1;
        f.data[0] = counter;
        out.send(f);
    }
}

TEST_F(CanbusUnit, ApplicationCodeCanUseTheBusThroughICanSender)
{
    CanFD bus{"can0", SocketOptions{}, api};
    sendHeartbeat(bus, 7);

    ASSERT_EQ(api.written.size(), 1u);
    EXPECT_EQ(api.lastWrittenAs<can_frame>().data[0], 7);
}

TEST_F(CanbusUnit, FdSenderInterfaceDispatchesBothFrameTypes)
{
    CanFD bus{"can0", SocketOptions{}, api};
    ICanFdSender& out = bus;

    out.send(Frame{});
    out.send(FdFrame{});

    ASSERT_EQ(api.written.size(), 2u);
    EXPECT_EQ(api.written[0].size(), sizeof(can_frame));
    EXPECT_EQ(api.written[1].size(), sizeof(canfd_frame));
}
