#pragma once

// In-memory test double for SocketCAN::ISocketApi.
//
// Injected into BasicCanbus so unit tests can drive every code path (option
// setup, error handling, framing, dispatch) without a kernel CAN interface.
// Each call is recorded so tests can assert on what the bus asked the kernel
// to do, and any call can be made to fail with a chosen errno.

#include "SocketCan.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace SocketCAN::Testing
{

class FakeSocketApi : public ISocketApi
{
public:
    // ---- Knobs --------------------------------------------------------------

    static constexpr int kFd = 42;

    // Interfaces that "exist", by name -> ifindex.
    std::map<std::string, int> interfaces{{"can0", 3}};

    // Fail the named operation ("socket", "setsockopt:<optname>", "getFlags",
    // "setFlags", "ifindex", "bind", "read", "write", "poll") with this errno.
    std::map<std::string, int> failures;

    // Bytes the next read() calls return, oldest first. poll() reports the
    // socket readable while this is non-empty.
    std::deque<std::vector<uint8_t>> pendingReads;

    // Force write() to report fewer bytes than requested.
    bool shortWrite = false;

    // ---- Recorded calls -----------------------------------------------------

    int                               socketCalls = 0;
    int                               closeCalls  = 0;
    std::map<int, int>                options;     // optname -> value
    int                               flags       = 0;
    int                               boundIfindex = -1;
    std::vector<std::vector<uint8_t>> written;
    std::vector<int>                  pollTimeouts;

    // ---- Helpers ------------------------------------------------------------

    template <typename T>
    void queueRead(const T& frame)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&frame);
        pendingReads.emplace_back(p, p + sizeof(T));
    }

    template <typename T>
    T lastWrittenAs() const
    {
        T out{};
        std::memcpy(&out, written.back().data(), sizeof(T));
        return out;
    }

    // ---- ISocketApi ---------------------------------------------------------

    int socket(int, int, int) override
    {
        ++socketCalls;
        return failIf("socket") ? -1 : kFd;
    }

    int setsockopt(int, int, int optname, const void* val, socklen_t len) override
    {
        if (failIf("setsockopt:" + std::to_string(optname)))
            return -1;
        int v = 0;
        std::memcpy(&v, val, std::min<std::size_t>(len, sizeof(v)));
        options[optname] = v;
        return 0;
    }

    int getFlags(int) override
    {
        return failIf("getFlags") ? -1 : flags;
    }

    int setFlags(int, int newFlags) override
    {
        if (failIf("setFlags"))
            return -1;
        flags = newFlags;
        return 0;
    }

    int interfaceIndex(int, const std::string& ifname) override
    {
        if (failIf("ifindex"))
            return -1;
        auto it = interfaces.find(ifname);
        if (it == interfaces.end())
        {
            errno = ENODEV;
            return -1;
        }
        return it->second;
    }

    int bind(int, int ifindex) override
    {
        if (failIf("bind"))
            return -1;
        boundIfindex = ifindex;
        return 0;
    }

    ssize_t read(int, void* buf, std::size_t len) override
    {
        if (failIf("read"))
            return -1;
        if (pendingReads.empty())
        {
            errno = EAGAIN;
            return -1;
        }
        auto bytes = std::move(pendingReads.front());
        pendingReads.pop_front();
        const std::size_t n = std::min(len, bytes.size());
        std::memcpy(buf, bytes.data(), n);
        return static_cast<ssize_t>(n);
    }

    ssize_t write(int, const void* buf, std::size_t len) override
    {
        if (failIf("write"))
            return -1;
        const auto* p = static_cast<const uint8_t*>(buf);
        written.emplace_back(p, p + len);
        return shortWrite ? static_cast<ssize_t>(len / 2) : static_cast<ssize_t>(len);
    }

    int poll(int, short, int timeoutMs, short& revents) override
    {
        pollTimeouts.push_back(timeoutMs);
        if (failIf("poll"))
            return -1;
        revents = pendingReads.empty() ? 0 : POLLIN;
        return pendingReads.empty() ? 0 : 1;
    }

    int close(int) override
    {
        ++closeCalls;
        return 0;
    }

private:
    bool failIf(const std::string& op)
    {
        auto it = failures.find(op);
        if (it == failures.end())
            return false;
        errno = it->second;
        return true;
    }
};

} // namespace SocketCAN::Testing
