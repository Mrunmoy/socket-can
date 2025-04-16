#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

// POSIX / Linux
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

class CanError : public std::runtime_error
{
public:
    explicit CanError(const std::string& what)
        : std::runtime_error(what)
    {}
};

struct Frame
{
    uint32_t id = 0;                     // 11-bit or 29-bit ID
    bool     extended = false;           // Standard vs extended frame
    bool     rtr      = false;           // Remote transmission request
    uint8_t  dlc      = 0;               // 0..8 bytes
    std::array<uint8_t, 8> data{};      // CAN classic payload
};

class Canbus
{
public:
    // Open and bind to a CAN interface (e.g. "vcan0", "can0").
    // loopback=false if you don’t want to see your own frames.
    explicit Canbus(const std::string& ifname,
                       bool loopback = true,
                       bool blocking = true);

    ~Canbus();

    Canbus(const Canbus&) = delete;
    Canbus& operator=(const Canbus&) = delete;

    Canbus(Canbus&& other) noexcept;
    Canbus& operator=(Canbus&& other) noexcept;

    // Send a CAN frame (blocking).
    void send(const Frame& frame);

    // Receive a CAN frame.
    // Returns true if a frame was received before timeout.
    // Returns false on timeout.
    // Throws on errors.
    bool receive(Frame& frame,
                 std::chrono::milliseconds timeout =
                     std::chrono::milliseconds{-1}); // -1 => block forever

    // Get underlying file descriptor if you want to use your own poll/epoll.
    int fd() const { return m_fd; }

private:
    int m_fd = -1;

    void closeIfOpen() noexcept;
};

} // namespace SocketCAN
