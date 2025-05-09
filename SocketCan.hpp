#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// Classic CAN frame — up to 8 bytes of payload
// ---------------------------------------------------------------------------
struct Frame
{
    uint32_t id       = 0;              // 11-bit or 29-bit ID
    bool     extended = false;          // Standard vs extended frame
    bool     rtr      = false;          // Remote transmission request
    uint8_t  dlc      = 0;              // 0..8 bytes
    std::array<uint8_t, 8> data{};     // Classic CAN payload
};

// ---------------------------------------------------------------------------
// CAN FD frame — up to 64 bytes of payload
// ---------------------------------------------------------------------------
struct FdFrame
{
    uint32_t id       = 0;                          // 11-bit or 29-bit ID
    bool     extended = false;                      // Standard vs extended frame
    bool     brs      = false;                      // Bit-rate switch
    bool     esi      = false;                      // Error state indicator
    uint8_t  len      = 0;                          // 0..64 bytes
    std::array<uint8_t, CANFD_MAX_DLEN> data{};    // CAN FD payload (64 bytes)
};

// ---------------------------------------------------------------------------
// Listener interfaces — implement to receive dispatched frames
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Canbus — single socket supporting both Classic CAN and CAN FD
// ---------------------------------------------------------------------------
class Canbus
{
public:
    // Maximum number of listeners of each type (zero-heap, fixed-size arrays).
    static constexpr std::size_t MaxListeners = 8;

    // Open and bind to a CAN interface (e.g. "vcan0", "can0").
    // loopback=false if you don't want to see your own frames.
    // enableFd selects the socket mode:
    //   true  => CAN_RAW_FD_FRAMES is enabled; the socket sends/receives both
    //            Classic and CAN FD frames (matching the PR's mixed-traffic API).
    //   false => classic-only socket (the kernel default); CAN FD frames on the
    //            bus are not delivered and send(const FdFrame&) throws.
    // If enableFd is true but the interface/kernel cannot support CAN FD, the
    // socket falls back to classic-only and fdEnabled() reports false.
    explicit Canbus(const std::string& ifname,
                    bool loopback = true,
                    bool blocking = true,
                    bool enableFd = true);

    ~Canbus();

    Canbus(const Canbus&) = delete;
    Canbus& operator=(const Canbus&) = delete;

    Canbus(Canbus&& other) noexcept;
    Canbus& operator=(Canbus&& other) noexcept;

    // -----------------------------------------------------------------------
    // Send
    // -----------------------------------------------------------------------

    // Send a Classic CAN frame (blocking).
    void send(const Frame& frame);

    // Send a CAN FD frame (blocking).
    void send(const FdFrame& frame);

    // -----------------------------------------------------------------------
    // Receive — direct (backward-compatible)
    // -----------------------------------------------------------------------

    // Receive a Classic CAN frame directly into 'frame'.
    // Returns true if a frame was received before timeout, false on timeout.
    // Throws CanError if a CAN FD frame arrives — use the listener-based
    // receive() overload for mixed-type traffic.
    bool receive(Frame& frame,
                 std::chrono::milliseconds timeout =
                     std::chrono::milliseconds{-1}); // -1 => block forever

    // -----------------------------------------------------------------------
    // Receive — listener-dispatching
    // -----------------------------------------------------------------------

    // Read one frame and dispatch it to all registered listeners of the
    // matching type. Returns true if a frame was read, false on timeout.
    bool receive(std::chrono::milliseconds timeout =
                     std::chrono::milliseconds{-1}); // -1 => block forever

    // -----------------------------------------------------------------------
    // Listener registration
    // -----------------------------------------------------------------------

    // Register a Classic CAN frame listener (up to MaxListeners). Throws if full.
    void addClassicListener(IClassicFrameListener* listener);

    // Register a CAN FD frame listener (up to MaxListeners). Throws if full.
    void addFdListener(IFdFrameListener* listener);

    // Get underlying file descriptor if you want to use your own poll/epoll.
    int fd() const { return m_fd; }

    // True if CAN FD mode is active on this socket (requested and supported).
    bool fdEnabled() const { return m_fdEnabled; }

private:
    int  m_fd        = -1;
    bool m_fdEnabled = false;

    std::array<IClassicFrameListener*, MaxListeners> m_classicListeners{};
    std::size_t m_classicCount = 0;

    std::array<IFdFrameListener*, MaxListeners> m_fdListeners{};
    std::size_t m_fdCount = 0;

    void closeIfOpen() noexcept;

    // Wait until the socket has data to read. Returns false on timeout.
    bool waitReadable(int timeoutMs);
};

} // namespace SocketCAN
