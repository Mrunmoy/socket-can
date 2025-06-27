#pragma once

// SocketCAN C++17 wrapper (header-only).
//
// Follows the Linux kernel SocketCAN documentation:
//   https://docs.kernel.org/networking/can.html
// Comments name the section of that page they implement, e.g.
// (docs: "RAW Socket Option CAN_RAW_FD_FRAMES").
//
// Layout of this header, one responsibility per component:
//   CanError                 error type for every failure
//   Frame / FdFrame          plain value types for Classic CAN and CAN FD
//   FrameCodec               validation + conversion to/from kernel structs
//   ISocketApi               the system calls the bus needs (injectable)
//   PosixSocketApi           the real implementation of ISocketApi
//   I*FrameListener          observer interfaces for received frames
//   ListenerRegistry         fixed-capacity (zero heap) observer list
//   ICanSender/ICanFdSender  abstractions application code can depend on
//   BasicCanbus<EnableFd>    the RAII socket that ties it all together

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// C runtime
#include <cerrno>
#include <cstring>

// POSIX / Linux
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// SocketCAN
#include <linux/can.h>
#include <linux/can/raw.h>

namespace SocketCAN
{

// ===========================================================================
// Errors
// ===========================================================================

class CanError : public std::runtime_error
{
public:
    explicit CanError(const std::string& what)
        : std::runtime_error(what)
    {}
};

// Throw a CanError carrying the current errno text.
[[noreturn]] inline void throwErrno(const std::string& where)
{
    throw CanError(where + ": " + std::strerror(errno));
}

// ===========================================================================
// Frame types
// ===========================================================================

// Classic CAN frame: up to 8 bytes of payload (docs: "How to use SocketCAN").
struct Frame
{
    uint32_t id       = 0;              // 11-bit or 29-bit ID
    bool     extended = false;          // Standard vs extended frame
    bool     rtr      = false;          // Remote transmission request
    uint8_t  dlc      = 0;              // 0..8 bytes
    std::array<uint8_t, CAN_MAX_DLEN> data{};
};

// CAN FD frame: up to 64 bytes of payload (docs: "How to use SocketCAN").
struct FdFrame
{
    uint32_t id       = 0;              // 11-bit or 29-bit ID
    bool     extended = false;          // Standard vs extended frame
    bool     brs      = false;          // Bit-rate switch
    bool     esi      = false;          // Error state indicator
    uint8_t  len      = 0;              // 0..8, 12, 16, 20, 24, 32, 48, 64
    std::array<uint8_t, CANFD_MAX_DLEN> data{};
};

static_assert(CAN_MAX_DLEN == 8, "Classic CAN payload must be 8 bytes");
static_assert(CANFD_MAX_DLEN == 64, "CAN FD payload must be 64 bytes");

// ===========================================================================
// FrameCodec: validation and kernel struct conversion (pure, no I/O)
// ===========================================================================

struct FrameCodec
{
    static constexpr uint32_t kMaxStandardId = CAN_SFF_MASK;   // 0x7FF
    static constexpr uint32_t kMaxExtendedId = CAN_EFF_MASK;   // 0x1FFFFFFF

    // canfd_frame.len is a plain 0..64 byte count, but drivers map it to the
    // 4-bit bus DLC with can_fd_len2dlc(), which rounds any other length up
    // (docs: "CAN FD (Flexible Data Rate) Driver Support"). Rejecting those
    // lengths keeps what the receiver sees equal to what was sent.
    static constexpr bool isValidFdLength(uint8_t len)
    {
        if (len <= 8)
            return true;
        switch (len)
        {
            case 12: case 16: case 20: case 24: case 32: case 48: case 64:
                return true;
            default:
                return false;
        }
    }

    static void validate(const Frame& f)
    {
        validateId(f.id, f.extended);
        if (f.dlc > CAN_MAX_DLEN)
            throw CanError("Invalid DLC (>8) for CAN classic frame");
        if (f.rtr)
        {
            for (std::size_t i = 0; i < f.dlc; ++i)
            {
                if (f.data[i] != 0)
                    throw CanError("RTR frame cannot contain data bytes");
            }
        }
    }

    static void validate(const FdFrame& f)
    {
        validateId(f.id, f.extended);
        if (f.len > CANFD_MAX_DLEN)
            throw CanError("CAN FD frame len exceeds CANFD_MAX_DLEN (64)");
        if (!isValidFdLength(f.len))
            throw CanError("CAN FD frame len " + std::to_string(f.len) +
                           " is not a valid CAN FD length (0..8,12,16,20,24,32,48,64)");
    }

    // Validate and convert to the kernel's struct can_frame.
    static can_frame encode(const Frame& f)
    {
        validate(f);
        can_frame cf{};
        cf.can_id = encodeId(f.id, f.extended);
        if (f.rtr)
            cf.can_id |= CAN_RTR_FLAG;
        cf.can_dlc = f.dlc;
        std::copy_n(f.data.begin(), f.dlc, cf.data);
        return cf;
    }

    // Validate and convert to the kernel's struct canfd_frame.
    static canfd_frame encode(const FdFrame& f)
    {
        validate(f);
        canfd_frame cf{};
        cf.can_id = encodeId(f.id, f.extended);
        cf.len    = f.len;
        if (f.brs) cf.flags |= CANFD_BRS;
        if (f.esi) cf.flags |= CANFD_ESI;
        std::copy_n(f.data.begin(), f.len, cf.data);
        return cf;
    }

    static Frame decode(const can_frame& cf)
    {
        Frame f;
        f.extended = (cf.can_id & CAN_EFF_FLAG) != 0;
        f.rtr      = (cf.can_id & CAN_RTR_FLAG) != 0;
        f.id       = decodeId(cf.can_id);
        f.dlc      = std::min<uint8_t>(cf.can_dlc, CAN_MAX_DLEN);
        std::copy_n(cf.data, f.dlc, f.data.begin());
        return f;
    }

    static FdFrame decode(const canfd_frame& cf)
    {
        FdFrame f;
        f.extended = (cf.can_id & CAN_EFF_FLAG) != 0;
        f.id       = decodeId(cf.can_id);
        f.len      = std::min<uint8_t>(cf.len, CANFD_MAX_DLEN);
        f.brs      = (cf.flags & CANFD_BRS) != 0;
        f.esi      = (cf.flags & CANFD_ESI) != 0;
        std::copy_n(cf.data, f.len, f.data.begin());
        return f;
    }

private:
    static void validateId(uint32_t id, bool extended)
    {
        if (!extended && id > kMaxStandardId)
            throw CanError("Standard CAN ID out of range (>0x7FF)");
        if (extended && id > kMaxExtendedId)
            throw CanError("Extended CAN ID out of range (>29-bit)");
    }

    static canid_t encodeId(uint32_t id, bool extended)
    {
        return extended ? ((id & CAN_EFF_MASK) | CAN_EFF_FLAG)
                        : (id & CAN_SFF_MASK);
    }

    static uint32_t decodeId(canid_t raw)
    {
        return (raw & CAN_EFF_FLAG) ? (raw & CAN_EFF_MASK) : (raw & CAN_SFF_MASK);
    }
};

// ===========================================================================
// ISocketApi: the system calls BasicCanbus depends on
// ===========================================================================
//
// BasicCanbus never calls the OS directly; it goes through this interface so
// a different implementation (a test fake, a tracing wrapper, a different
// platform shim) can be injected. Every method follows POSIX conventions:
// return -1 and set errno on failure.

class ISocketApi
{
public:
    virtual ~ISocketApi() = default;

    virtual int     socket(int domain, int type, int protocol) = 0;
    virtual int     setsockopt(int fd, int level, int optname,
                               const void* optval, socklen_t optlen) = 0;
    virtual int     getFlags(int fd) = 0;                    // fcntl(F_GETFL)
    virtual int     setFlags(int fd, int flags) = 0;         // fcntl(F_SETFL)
    virtual int     interfaceIndex(int fd, const std::string& ifname) = 0; // SIOCGIFINDEX
    virtual int     bind(int fd, int ifindex) = 0;           // bind(sockaddr_can)
    virtual ssize_t read(int fd, void* buf, std::size_t len) = 0;
    virtual ssize_t write(int fd, const void* buf, std::size_t len) = 0;
    virtual int     poll(int fd, short events, int timeoutMs, short& revents) = 0;
    virtual int     close(int fd) = 0;
};

// The real Linux implementation (docs: "How to use SocketCAN").
class PosixSocketApi final : public ISocketApi
{
public:
    int socket(int domain, int type, int protocol) override
    {
        return ::socket(domain, type, protocol);
    }

    int setsockopt(int fd, int level, int optname,
                   const void* optval, socklen_t optlen) override
    {
        return ::setsockopt(fd, level, optname, optval, optlen);
    }

    int getFlags(int fd) override { return ::fcntl(fd, F_GETFL, 0); }

    int setFlags(int fd, int flags) override { return ::fcntl(fd, F_SETFL, flags); }

    int interfaceIndex(int fd, const std::string& ifname) override
    {
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name) - 1);
        if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
            return -1;
        return ifr.ifr_ifindex;
    }

    int bind(int fd, int ifindex) override
    {
        struct sockaddr_can addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.can_family  = AF_CAN;
        addr.can_ifindex = ifindex;
        return ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    }

    ssize_t read(int fd, void* buf, std::size_t len) override
    {
        return ::read(fd, buf, len);
    }

    ssize_t write(int fd, const void* buf, std::size_t len) override
    {
        return ::write(fd, buf, len);
    }

    int poll(int fd, short events, int timeoutMs, short& revents) override
    {
        struct pollfd pfd{fd, events, 0};
        const int ret = ::poll(&pfd, 1, timeoutMs);
        revents = pfd.revents;
        return ret;
    }

    int close(int fd) override { return ::close(fd); }
};

// Process-wide default used when no ISocketApi is injected. Stateless, so
// sharing one instance is safe.
inline ISocketApi& defaultSocketApi()
{
    static PosixSocketApi api;
    return api;
}

// ===========================================================================
// Listener interfaces (observer) and the fixed-capacity registry
// ===========================================================================

class IClassicFrameListener
{
public:
    virtual ~IClassicFrameListener() = default;
    virtual void onClassicFrame(const Frame& frame) = 0;
};

class IFdFrameListener
{
public:
    virtual ~IFdFrameListener() = default;
    virtual void onFdFrame(const FdFrame& frame) = 0;
};

// Non-owning list of up to Capacity listeners. No heap allocation.
template <typename Listener, std::size_t Capacity>
class ListenerRegistry
{
public:
    static constexpr std::size_t capacity() { return Capacity; }

    void add(Listener* listener)
    {
        if (listener == nullptr)
            throw CanError("Listener cannot be null");
        if (m_count >= Capacity)
            throw CanError("Maximum number of listeners (" +
                           std::to_string(Capacity) + ") reached");
        m_items[m_count++] = listener;
    }

    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (std::size_t i = 0; i < m_count; ++i)
            fn(*m_items[i]);
    }

    std::size_t size() const { return m_count; }

    void clear()
    {
        m_items.fill(nullptr);
        m_count = 0;
    }

private:
    std::array<Listener*, Capacity> m_items{};
    std::size_t                     m_count = 0;
};

// ===========================================================================
// Sender abstractions
// ===========================================================================
//
// Application code that only needs to transmit can depend on these instead of
// a concrete bus, and be unit tested with its own fake sender.

class ICanSender
{
public:
    virtual ~ICanSender() = default;
    virtual void send(const Frame& frame) = 0;
};

class ICanFdSender : public ICanSender
{
public:
    using ICanSender::send;
    virtual void send(const FdFrame& frame) = 0;
};

// Socket configuration applied at construction.
struct SocketOptions
{
    bool loopback = true;   // CAN_RAW_LOOPBACK: see frames from other local sockets
    bool blocking = true;   // false sets O_NONBLOCK on the descriptor
};

template <bool EnableFd>
class BasicCanbus;

namespace detail
{
    // Selects the sender interface for each mode and, for FD, routes the
    // virtual send(const FdFrame&) to the bus implementation. This keeps the
    // FD-only API absent from CanClassic at compile time.
    template <bool EnableFd>
    class SenderBase : public ICanSender
    {
    };

    template <>
    class SenderBase<true> : public ICanFdSender
    {
    public:
        using ICanFdSender::send;
        void send(const FdFrame& frame) override;
    };
} // namespace detail

// ===========================================================================
// BasicCanbus: a CAN_RAW socket whose FD mode is fixed at compile time
// ===========================================================================
//
//   EnableFd == true   CAN_RAW_FD_FRAMES is enabled (docs: "RAW Socket Option
//                      CAN_RAW_FD_FRAMES"); the socket
//                      sends/receives both Classic and CAN FD frames.
//                      send(const FdFrame&) and addFdListener() are available.
//   EnableFd == false  Classic-only socket (the kernel default). CAN FD frames
//                      are never delivered, and the FD-only members do not
//                      exist, so calling them is a compile error.
//
// Use the CanFD / CanClassic aliases at the bottom of this file.
template <bool EnableFd = true>
class BasicCanbus : public detail::SenderBase<EnableFd>
{
    friend class detail::SenderBase<true>;

public:
    static constexpr bool        kFdMode      = EnableFd;
    static constexpr std::size_t MaxListeners = 8;

    // Open and bind to a CAN interface (e.g. "can0") using the real OS API.
    explicit BasicCanbus(const std::string& ifname,
                         bool loopback = true,
                         bool blocking = true)
        : BasicCanbus(ifname, SocketOptions{loopback, blocking}, defaultSocketApi())
    {}

    // Open and bind with an injected socket API. The api must outlive the bus.
    BasicCanbus(const std::string& ifname,
                const SocketOptions& options,
                ISocketApi& api = defaultSocketApi())
        : m_api(&api)
    {
        open(ifname, options);
    }

    ~BasicCanbus() override { closeIfOpen(); }

    BasicCanbus(const BasicCanbus&)            = delete;
    BasicCanbus& operator=(const BasicCanbus&) = delete;

    BasicCanbus(BasicCanbus&& other) noexcept { takeFrom(other); }

    BasicCanbus& operator=(BasicCanbus&& other) noexcept
    {
        if (this != &other)
        {
            closeIfOpen();
            takeFrom(other);
        }
        return *this;
    }

    // -----------------------------------------------------------------------
    // Send
    // -----------------------------------------------------------------------

    using detail::SenderBase<EnableFd>::send;

    // Send a Classic CAN frame. Available in both modes.
    void send(const Frame& frame) override
    {
        requireOpen("send()");
        const can_frame cf = FrameCodec::encode(frame);
        writeAll(&cf, sizeof(cf), "write(can_frame)");
    }

    // -----------------------------------------------------------------------
    // Receive: direct
    // -----------------------------------------------------------------------

    // Receive one Classic CAN frame into 'frame'. Returns false on timeout.
    // A negative timeout blocks until a frame arrives. On the FD socket type
    // this throws if a CAN FD frame arrives; use the listener-based receive()
    // for mixed traffic.
    bool receive(Frame& frame,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds{-1})
    {
        requireOpen("receive()");
        canfd_frame buf{};
        const ssize_t n = readOne(buf, timeout);
        if (n == 0)
            return false;

        if (n == static_cast<ssize_t>(CANFD_MTU))
            throw CanError("CAN FD frame received; use listener-based receive() for mixed traffic");
        if (n != static_cast<ssize_t>(CAN_MTU))
            throw CanError("Partial CAN frame read");

        frame = FrameCodec::decode(asClassic(buf));
        return true;
    }

    // -----------------------------------------------------------------------
    // Receive: listener dispatch
    // -----------------------------------------------------------------------

    // Read one frame and notify every listener of the matching type.
    // Returns false on timeout. The read size tells Classic (CAN_MTU) and FD
    // (CANFD_MTU) frames apart (docs: "RAW Socket Option CAN_RAW_FD_FRAMES").
    bool receive(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1})
    {
        requireOpen("receive()");
        canfd_frame buf{};
        const ssize_t n = readOne(buf, timeout);
        if (n == 0)
            return false;

        if (n == static_cast<ssize_t>(CAN_MTU))
        {
            const Frame frame = FrameCodec::decode(asClassic(buf));
            m_classicListeners.forEach(
                [&](IClassicFrameListener& l) { l.onClassicFrame(frame); });
            return true;
        }

        if constexpr (EnableFd)
        {
            if (n == static_cast<ssize_t>(CANFD_MTU))
            {
                const FdFrame frame = FrameCodec::decode(buf);
                m_fdListeners.forEach(
                    [&](IFdFrameListener& l) { l.onFdFrame(frame); });
                return true;
            }
        }

        throw CanError("Unexpected read size from CAN socket");
    }

    // -----------------------------------------------------------------------
    // Listener registration (non-owning; listeners must outlive the bus)
    // -----------------------------------------------------------------------

    void addClassicListener(IClassicFrameListener* listener)
    {
        m_classicListeners.add(listener);
    }

    // Only exists on the FD socket type.
    template <bool E = EnableFd, std::enable_if_t<E, int> = 0>
    void addFdListener(IFdFrameListener* listener)
    {
        m_fdListeners.add(listener);
    }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    // Underlying descriptor, for use with your own poll/epoll loop.
    int fd() const { return m_fd; }

    // True if CAN FD is active (requested and supported by interface/kernel).
    // Always false for the classic socket type.
    bool fdEnabled() const { return m_fdEnabled; }

private:
    ISocketApi* m_api       = nullptr;
    int         m_fd        = -1;
    bool        m_fdEnabled = false;

    ListenerRegistry<IClassicFrameListener, MaxListeners> m_classicListeners;
    ListenerRegistry<IFdFrameListener, MaxListeners>      m_fdListeners;

    // socket(), setsockopt(), bind() to an ifindex (docs: "How to use SocketCAN").
    void open(const std::string& ifname, const SocketOptions& options)
    {
        if (ifname.size() >= IFNAMSIZ)
            throw CanError("Interface name too long: " + ifname);

        m_fd = m_api->socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (m_fd < 0)
            throwErrno("socket(PF_CAN, SOCK_RAW, CAN_RAW)");

        try
        {
            if constexpr (EnableFd)
                enableFdFrames();

            setIntOption(CAN_RAW_LOOPBACK, options.loopback ? 1 : 0,
                         "setsockopt(CAN_RAW_LOOPBACK)");

            if (!options.blocking)
                setNonBlocking();

            const int ifindex = m_api->interfaceIndex(m_fd, ifname);
            if (ifindex < 0)
                throwErrno("ioctl(SIOCGIFINDEX, " + ifname + ")");

            if (m_api->bind(m_fd, ifindex) < 0)
                throwErrno("bind(sockaddr_can)");
        }
        catch (...)
        {
            closeIfOpen();
            throw;
        }
    }

    // CAN_RAW_FD_FRAMES is off by default; older kernels return ENOPROTOOPT
    // (docs: "RAW Socket Option CAN_RAW_FD_FRAMES"). Fall back to classic-only
    // and report fdEnabled()=false in that case.
    void enableFdFrames()
    {
        const int on = 1;
        if (m_api->setsockopt(m_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) == 0)
        {
            m_fdEnabled = true;
            return;
        }
        const int err = errno;
        if (err != ENOPROTOOPT && err != EOPNOTSUPP && err != EINVAL)
            throwErrno("setsockopt(CAN_RAW_FD_FRAMES)");
    }

    void setIntOption(int optname, int value, const char* what)
    {
        if (m_api->setsockopt(m_fd, SOL_CAN_RAW, optname, &value, sizeof(value)) < 0)
            throwErrno(what);
    }

    void setNonBlocking()
    {
        const int flags = m_api->getFlags(m_fd);
        if (flags < 0 || m_api->setFlags(m_fd, flags | O_NONBLOCK) < 0)
            throwErrno("fcntl(O_NONBLOCK)");
    }

    void sendFd(const FdFrame& frame)
    {
        requireOpen("send()");
        if (!m_fdEnabled)
            throw CanError("CAN FD not enabled on this socket (interface/kernel lacks CAN FD support)");
        const canfd_frame cf = FrameCodec::encode(frame);
        writeAll(&cf, sizeof(cf), "write(canfd_frame)");
    }

    void writeAll(const void* buf, std::size_t len, const char* what)
    {
        const ssize_t n = m_api->write(m_fd, buf, len);
        if (n < 0)
            throwErrno(what);
        if (n != static_cast<ssize_t>(len))
            throw CanError(std::string("Partial write: ") + what);
    }

    // Wait for and read one frame. Returns 0 on timeout, else bytes read.
    ssize_t readOne(canfd_frame& buf, std::chrono::milliseconds timeout)
    {
        const int timeoutMs = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());

        short revents = 0;
        const int ready = m_api->poll(m_fd, POLLIN, timeoutMs, revents);
        if (ready < 0)
            throwErrno("poll()");
        if (ready == 0)
            return 0;
        if (!(revents & POLLIN))
            throw CanError("poll(): unexpected revents");

        const ssize_t n = m_api->read(m_fd, &buf, sizeof(buf));
        if (n < 0)
            throwErrno("read()");
        return n;
    }

    // can_frame and canfd_frame share the same layout for the first CAN_MTU
    // bytes (docs: "RAW Socket Option CAN_RAW_FD_FRAMES"), so a classic frame
    // read into a canfd_frame buffer
    // can be viewed as a can_frame.
    static can_frame asClassic(const canfd_frame& buf)
    {
        can_frame cf;
        std::memcpy(&cf, &buf, sizeof(cf));
        return cf;
    }

    void requireOpen(const char* op) const
    {
        if (m_fd < 0)
            throw CanError(std::string(op) + " on closed Canbus");
    }

    void takeFrom(BasicCanbus& other) noexcept
    {
        m_api              = other.m_api;
        m_fd               = std::exchange(other.m_fd, -1);
        m_fdEnabled        = std::exchange(other.m_fdEnabled, false);
        m_classicListeners = other.m_classicListeners;
        m_fdListeners      = other.m_fdListeners;
        other.m_classicListeners.clear();
        other.m_fdListeners.clear();
    }

    void closeIfOpen() noexcept
    {
        if (m_fd >= 0)
        {
            m_api->close(m_fd);
            m_fd = -1;
        }
    }
};

namespace detail
{
    inline void SenderBase<true>::send(const FdFrame& frame)
    {
        static_cast<BasicCanbus<true>&>(*this).sendFd(frame);
    }
} // namespace detail

// ===========================================================================
// Convenience aliases: the mode is part of the type
// ===========================================================================
using CanClassic = BasicCanbus<false>;  // Classic only
using CanFD      = BasicCanbus<true>;   // Classic + CAN FD

} // namespace SocketCAN
