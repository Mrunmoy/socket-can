#include "SocketCan.hpp"

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
                     bool blocking)
{
    // Create socket: PF_CAN, raw CAN frames
    m_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (m_fd < 0)
    {
        throwErrno("socket(PF_CAN, SOCK_RAW, CAN_RAW)");
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
    m_fd = other.m_fd;
    other.m_fd = -1;
}

Canbus& Canbus::operator=(Canbus&& other) noexcept
{
    if (this != &other)
    {
        closeIfOpen();
        m_fd       = other.m_fd;
        other.m_fd = -1;
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

void Canbus::send(const Frame& frame)
{
    if (m_fd < 0)
    {
        throw CanError("send() on closed Canbus");
    }

    struct can_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    // Encode CAN ID + flags
    if (frame.extended)
    {
        cf.can_id = (frame.id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    }
    else
    {
        cf.can_id = (frame.id & CAN_SFF_MASK); // 11-bit
    }

    if (frame.rtr)
    {
        cf.can_id |= CAN_RTR_FLAG;
    }

    cf.can_dlc = (frame.dlc <= 8) ? frame.dlc : 8;

    for (uint8_t i = 0; i < cf.can_dlc; ++i)
    {
        cf.data[i] = frame.data[i];
    }

    ssize_t n = ::write(m_fd, &cf, sizeof(cf));
    if (n < 0)
    {
        throwErrno("write(can_frame)");
    }
    if (n != static_cast<ssize_t>(sizeof(cf)))
    {
        throw CanError("Partial CAN frame write");
    }
}

bool Canbus::receive(Frame& frame,
                        std::chrono::milliseconds timeout)
{
    if (m_fd < 0)
    {
        throw CanError("receive() on closed Canbus");
    }

    // If timeout < 0: block forever, else use poll()
    int timeoutMs = -1;
    if (timeout.count() >= 0)
    {
        timeoutMs = static_cast<int>(timeout.count());
    }

    struct pollfd pfd;
    pfd.fd     = m_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = ::poll(&pfd, 1, timeoutMs);
    if (ret < 0)
    {
        throwErrno("poll()");
    }
    if (ret == 0)
    {
        // Timeout
        return false;
    }

    if (!(pfd.revents & POLLIN))
    {
        // Something strange (error/hangup)
        throw CanError("poll(): unexpected revents");
    }

    struct can_frame cf;
    std::memset(&cf, 0, sizeof(cf));

    ssize_t n = ::read(m_fd, &cf, sizeof(cf));
    if (n < 0)
    {
        throwErrno("read(can_frame)");
    }
    if (n != static_cast<ssize_t>(sizeof(cf)))
    {
        throw CanError("Partial CAN frame read");
    }

    // Decode into our Frame type
    frame.rtr = (cf.can_id & CAN_RTR_FLAG) != 0;
    bool eff  = (cf.can_id & CAN_EFF_FLAG) != 0;
    frame.extended = eff;

    if (eff)
    {
        frame.id = cf.can_id & CAN_EFF_MASK;
    }
    else
    {
        frame.id = cf.can_id & CAN_SFF_MASK;
    }

    frame.dlc = cf.can_dlc;
    for (uint8_t i = 0; i < frame.dlc && i < 8; ++i)
    {
        frame.data[i] = cf.data[i];
    }

    return true;
}

} // namespace SocketCAN
