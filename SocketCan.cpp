#include "SocketCan.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>

// Helper: convert errno to exception
static void throwErrno(const std::string& where)
{
    throw SocketCAN::CanError(where + ": " + std::strerror(errno));
}

namespace SocketCAN
{

Canbus::Canbus(const std::string& ifname,
               bool loopback,
               bool blocking,
               bool enableFd)
{
    // Create socket: PF_CAN, raw CAN frames
    m_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (m_fd < 0)
    {
        throwErrno("socket(PF_CAN, SOCK_RAW, CAN_RAW)");
    }

    // Optionally enable CAN FD frames so the socket accepts both Classic and FD
    // frames. CAN_RAW_FD_FRAMES is off by default in the kernel; when requested
    // but unsupported by the interface/kernel, fall back to classic-only.
    if (enableFd)
    {
        int fdOn = 1;
        if (::setsockopt(m_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                         &fdOn, sizeof(fdOn)) == 0)
        {
            m_fdEnabled = true;
        }
        else
        {
            const int err = errno;
            if (err != ENOPROTOOPT && err != EOPNOTSUPP && err != EINVAL)
            {
                closeIfOpen();
                throwErrno("setsockopt(CAN_RAW_FD_FRAMES)");
            }
            // Otherwise: interface/kernel lacks CAN FD support — stay classic-only.
        }
    }

    // Optional: disable local loopback
    int loopbackVal = loopback ? 1 : 0;
    if (::setsockopt(m_fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK,
                     &loopbackVal, sizeof(loopbackVal)) < 0)
    {
        closeIfOpen();
        throwErrno("setsockopt(CAN_RAW_LOOPBACK)");
    }

    // Make non-blocking if requested
    if (!blocking)
    {
        int flags = ::fcntl(m_fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            closeIfOpen();
            throwErrno("fcntl(O_NONBLOCK)");
        }
    }

    // Resolve interface index
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    if (ifname.size() >= sizeof(ifr.ifr_name))
    {
        closeIfOpen();
        throw CanError("Interface name too long: " + ifname);
    }
    std::strncpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name) - 1);

    if (::ioctl(m_fd, SIOCGIFINDEX, &ifr) < 0)
    {
        closeIfOpen();
        throwErrno("ioctl(SIOCGIFINDEX)");
    }

    // Bind to CAN interface
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (::bind(m_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0)
    {
        closeIfOpen();
        throwErrno("bind(AF_CAN)");
    }
}

Canbus::~Canbus()
{
    closeIfOpen();
}

Canbus::Canbus(Canbus&& other) noexcept
{
    m_fd              = other.m_fd;
    other.m_fd        = -1;
    m_fdEnabled       = other.m_fdEnabled;
    m_classicListeners = other.m_classicListeners;
    m_classicCount     = other.m_classicCount;
    m_fdListeners      = other.m_fdListeners;
    m_fdCount          = other.m_fdCount;
    other.m_fdEnabled    = false;
    other.m_classicCount = 0;
    other.m_fdCount      = 0;
}

Canbus& Canbus::operator=(Canbus&& other) noexcept
{
    if (this != &other)
    {
        closeIfOpen();
        m_fd               = other.m_fd;
        other.m_fd         = -1;
        m_fdEnabled        = other.m_fdEnabled;
        m_classicListeners = other.m_classicListeners;
        m_classicCount     = other.m_classicCount;
        m_fdListeners      = other.m_fdListeners;
        m_fdCount          = other.m_fdCount;
        other.m_fdEnabled    = false;
        other.m_classicCount = 0;
        other.m_fdCount      = 0;
    }
    return *this;
}

void Canbus::closeIfOpen() noexcept
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool Canbus::waitReadable(int timeoutMs)
{
    struct pollfd pfd;
    pfd.fd      = m_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int ret = ::poll(&pfd, 1, timeoutMs);
    if (ret < 0)
        throwErrno("poll()");
    if (ret == 0)
        return false;
    if (!(pfd.revents & POLLIN))
        throw CanError("poll(): unexpected revents");
    return true;
}

// ---------------------------------------------------------------------------
// Send — Classic CAN
// ---------------------------------------------------------------------------
void Canbus::send(const Frame& frame)
{
    if (m_fd < 0)
        throw CanError("send() on closed Canbus");

    // Validate DLC (CAN classic supports only 0..8)
    if (frame.dlc > 8)
        throw CanError("Invalid DLC (>8) for CAN classic frame");

    if (frame.rtr)
    {
        // RTR frames must NOT contain data bytes
        for (std::size_t i = 0; i < frame.dlc; ++i)
        {
            if (frame.data[i] != 0)
                throw CanError("RTR frame cannot contain data bytes");
        }
    }

    // Validate CAN ID ranges
    if (!frame.extended)
    {
        if (frame.id > 0x7FFu)
            throw CanError("Standard CAN ID out of range (>0x7FF)");
    }
    else
    {
        if (frame.id > 0x1FFFFFFFu)
            throw CanError("Extended CAN ID out of range (>29-bit)");
    }

    struct can_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    // Encode CAN ID + flags
    if (frame.extended)
        cf.can_id = (frame.id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    else
        cf.can_id = (frame.id & CAN_SFF_MASK);

    if (frame.rtr)
        cf.can_id |= CAN_RTR_FLAG;

    cf.can_dlc = (frame.dlc <= 8) ? frame.dlc : 8;
    for (uint8_t i = 0; i < cf.can_dlc; ++i)
        cf.data[i] = frame.data[i];

    ssize_t n = ::write(m_fd, &cf, sizeof(cf));
    if (n < 0)
        throwErrno("write(can_frame)");
    if (n != static_cast<ssize_t>(sizeof(cf)))
        throw CanError("Partial CAN frame write");
}

// ---------------------------------------------------------------------------
// Send — CAN FD
// ---------------------------------------------------------------------------
void Canbus::send(const FdFrame& frame)
{
    if (m_fd < 0)
        throw CanError("send() on closed Canbus");

    if (!m_fdEnabled)
        throw CanError("CAN FD not enabled on this socket (construct with enableFd=true on an FD-capable interface)");

    if (frame.len > CANFD_MAX_DLEN)
        throw CanError("CAN FD frame len exceeds CANFD_MAX_DLEN (64)");

    // Validate CAN ID ranges
    if (!frame.extended)
    {
        if (frame.id > 0x7FFu)
            throw CanError("Standard CAN ID out of range (>0x7FF)");
    }
    else
    {
        if (frame.id > 0x1FFFFFFFu)
            throw CanError("Extended CAN ID out of range (>29-bit)");
    }

    struct canfd_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    if (frame.extended)
        cf.can_id = (frame.id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    else
        cf.can_id = frame.id & CAN_SFF_MASK;

    cf.len   = frame.len;
    cf.flags = 0;
    if (frame.brs) cf.flags |= CANFD_BRS;
    if (frame.esi) cf.flags |= CANFD_ESI;

    std::memcpy(cf.data, frame.data.data(), frame.len);

    ssize_t n = ::write(m_fd, &cf, sizeof(cf));
    if (n < 0)
        throwErrno("write(canfd_frame)");
    if (n != static_cast<ssize_t>(sizeof(cf)))
        throw CanError("Partial CAN FD frame write");
}

// ---------------------------------------------------------------------------
// Receive — direct Classic CAN (backward-compatible)
// ---------------------------------------------------------------------------
bool Canbus::receive(Frame& frame, std::chrono::milliseconds timeout)
{
    if (m_fd < 0)
        throw CanError("receive() on closed Canbus");

    int timeoutMs = -1;
    if (timeout.count() >= 0)
        timeoutMs = static_cast<int>(timeout.count());

    if (!waitReadable(timeoutMs))
        return false;

    // Read into a canfd_frame-sized buffer; the kernel returns
    // sizeof(can_frame) for Classic frames and sizeof(canfd_frame) for FD.
    struct canfd_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    ssize_t n = ::read(m_fd, &cf, sizeof(cf));
    if (n < 0)
        throwErrno("read()");

    if (n == static_cast<ssize_t>(sizeof(struct canfd_frame)))
        throw CanError("CAN FD frame received; use listener-based receive() for mixed traffic");

    if (n != static_cast<ssize_t>(sizeof(struct can_frame)))
        throw CanError("Partial CAN frame read");

    // can_frame and canfd_frame share the same layout for the first
    // sizeof(can_frame) bytes, so a reinterpret_cast is safe here.
    const auto* classic = reinterpret_cast<const struct can_frame*>(&cf);

    frame.rtr      = (classic->can_id & CAN_RTR_FLAG) != 0;
    bool eff       = (classic->can_id & CAN_EFF_FLAG) != 0;
    frame.extended = eff;
    frame.id       = eff ? (classic->can_id & CAN_EFF_MASK)
                         : (classic->can_id & CAN_SFF_MASK);
    frame.dlc = classic->can_dlc;
    for (uint8_t i = 0; i < frame.dlc && i < 8; ++i)
        frame.data[i] = classic->data[i];

    return true;
}

// ---------------------------------------------------------------------------
// Receive — listener-dispatching (Classic + FD)
// ---------------------------------------------------------------------------
bool Canbus::receive(std::chrono::milliseconds timeout)
{
    if (m_fd < 0)
        throw CanError("receive() on closed Canbus");

    int timeoutMs = -1;
    if (timeout.count() >= 0)
        timeoutMs = static_cast<int>(timeout.count());

    if (!waitReadable(timeoutMs))
        return false;

    struct canfd_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    ssize_t n = ::read(m_fd, &cf, sizeof(cf));
    if (n < 0)
        throwErrno("read()");

    if (n == static_cast<ssize_t>(sizeof(struct can_frame)))
    {
        // Classic CAN frame
        const auto* classic = reinterpret_cast<const struct can_frame*>(&cf);

        Frame frame;
        frame.rtr      = (classic->can_id & CAN_RTR_FLAG) != 0;
        bool eff       = (classic->can_id & CAN_EFF_FLAG) != 0;
        frame.extended = eff;
        frame.id       = eff ? (classic->can_id & CAN_EFF_MASK)
                             : (classic->can_id & CAN_SFF_MASK);
        frame.dlc = classic->can_dlc;
        for (uint8_t i = 0; i < frame.dlc && i < 8; ++i)
            frame.data[i] = classic->data[i];

        for (std::size_t i = 0; i < m_classicCount; ++i)
            m_classicListeners[i]->onClassicFrame(frame);
    }
    else if (n == static_cast<ssize_t>(sizeof(struct canfd_frame)))
    {
        // CAN FD frame
        FdFrame frame;
        bool eff       = (cf.can_id & CAN_EFF_FLAG) != 0;
        frame.extended = eff;
        frame.id       = eff ? (cf.can_id & CAN_EFF_MASK)
                             : (cf.can_id & CAN_SFF_MASK);
        frame.len = cf.len;
        frame.brs = (cf.flags & CANFD_BRS) != 0;
        frame.esi = (cf.flags & CANFD_ESI) != 0;
        for (uint8_t i = 0; i < cf.len && i < CANFD_MAX_DLEN; ++i)
            frame.data[i] = cf.data[i];

        for (std::size_t i = 0; i < m_fdCount; ++i)
            m_fdListeners[i]->onFdFrame(frame);
    }
    else
    {
        throw CanError("Unexpected read size from CAN socket");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Listener registration
// ---------------------------------------------------------------------------
void Canbus::addClassicListener(IClassicFrameListener* listener)
{
    if (listener == nullptr)
        throw CanError("Classic CAN listener cannot be null");
    if (m_classicCount >= MaxListeners)
        throw CanError("Maximum number of Classic CAN listeners reached");
    m_classicListeners[m_classicCount++] = listener;
}

void Canbus::addFdListener(IFdFrameListener* listener)
{
    if (listener == nullptr)
        throw CanError("CAN FD listener cannot be null");
    if (m_fdCount >= MaxListeners)
        throw CanError("Maximum number of CAN FD listeners reached");
    m_fdListeners[m_fdCount++] = listener;
}

} // namespace SocketCAN
