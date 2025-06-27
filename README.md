# SocketCAN C++ Library

A modern, safe, test-driven, header-only C++17 wrapper for Linux SocketCAN.

This library is written to follow the official Linux kernel SocketCAN
documentation, **[SocketCAN - Controller Area Network](https://docs.kernel.org/networking/can.html)**
(`Documentation/networking/can.rst` in the kernel tree). Every socket call,
option and frame layout used here comes from that document, and the code
comments name the section they implement (for example
`docs: "RAW Socket Option CAN_RAW_FD_FRAMES"`). If anything here disagrees with the kernel docs, the kernel docs win
and it is a bug in this library.

| Kernel docs section | What this library does with it |
| --- | --- |
| How to use SocketCAN | `socket(PF_CAN, SOCK_RAW, CAN_RAW)`, `SIOCGIFINDEX`, `bind()`; `struct can_frame` / `struct canfd_frame` behind `Frame` / `FdFrame` |
| RAW Socket Option CAN_RAW_LOOPBACK | `SocketOptions::loopback` |
| RAW Socket Option CAN_RAW_FD_FRAMES | `CanFD`; Classic vs FD frames told apart by `CAN_MTU` / `CANFD_MTU`; `ENOPROTOOPT` fallback to Classic |
| CAN FD (Flexible Data Rate) Driver Support | `FdFrame::len` validation, `bitrate` / `dbitrate` / `fd on` setup |
| The Virtual CAN Driver (vcan) | `can0` backed by `vcan` in the dev container and CI |

## Features

- RAII socket types: `SocketCAN::CanFD` (Classic + CAN FD) and `SocketCAN::CanClassic` (Classic only)
- Strongly typed `Frame` (Classic CAN) and `FdFrame` (CAN FD) value types
- CAN FD vs Classic is a **compile-time** property of the socket type
- Separate listener interfaces (`IClassicFrameListener`, `IFdFrameListener`); each frame goes only to listeners of its type
- Blocking and non-blocking receive, with timeouts
- Standard (11-bit) and extended (29-bit) IDs, RTR frames, CAN FD BRS/ESI flags, optional loopback suppression
- Frame validation before anything reaches the bus, including the CAN FD length rule (0..8, 12, 16, 20, 24, 32, 48, 64)
- Zero heap allocation: listener slots are fixed-size arrays (`MaxListeners = 8` per type)
- Errors reported as `SocketCAN::CanError`
- Injectable system-call layer (`ISocketApi`) so the bus, and code built on it, can be unit tested without CAN hardware
- Unit tests (no hardware) + integration tests on `can0`, run in CI

## Quick start

```cpp
#include "SocketCan.hpp"
using namespace SocketCAN;
```

### Classic CAN

```cpp
#include "SocketCan.hpp"
#include <iostream>

using namespace SocketCAN;

int main() {
    CanClassic bus{"can0"};

    Frame f{};
    f.id = 0x123;
    f.dlc = 2;
    f.data[0] = 0xDE;
    f.data[1] = 0xAD;

    bus.send(f);

    Frame r{};
    if (bus.receive(r, std::chrono::milliseconds{100})) {
        std::cout << "ID=" << std::hex << r.id
                  << " DLC=" << std::dec << int(r.dlc)
                  << std::endl;
    }
}
```

### CAN FD

```cpp
#include "SocketCan.hpp"

using namespace SocketCAN;

int main() {
    CanFD bus{"can0"};

    FdFrame f{};
    f.id  = 0x456;
    f.len = 12;     // must be a valid CAN FD length
    f.brs = true;   // bit-rate switch
    for (uint8_t i = 0; i < f.len; ++i)
        f.data[i] = i;

    bus.send(f);
}
```

### Listener-based receive (Classic + FD on one socket)

```cpp
#include "SocketCan.hpp"
#include <iostream>

using namespace SocketCAN;

struct MyClassicHandler : IClassicFrameListener {
    void onClassicFrame(const Frame& f) override {
        std::cout << "Classic  ID=0x" << std::hex << f.id << "\n";
    }
};

struct MyFdHandler : IFdFrameListener {
    void onFdFrame(const FdFrame& f) override {
        std::cout << "CAN FD   ID=0x" << std::hex << f.id
                  << " len=" << std::dec << int(f.len) << "\n";
    }
};

int main() {
    CanFD bus{"can0"};

    MyClassicHandler classicHandler;
    MyFdHandler      fdHandler;
    bus.addClassicListener(&classicHandler);
    bus.addFdListener(&fdHandler);

    while (true) {
        bus.receive(std::chrono::milliseconds{100});   // reads one frame, notifies listeners
    }
}
```

Listeners are not owned by the bus and must outlive it.

### Socket options

```cpp
SocketOptions opts;
opts.loopback = false;   // CAN_RAW_LOOPBACK off: don't see frames from other local sockets
opts.blocking = false;   // O_NONBLOCK

CanFD bus{"can0", opts};
```

The older `CanFD bus{"can0", /*loopback*/ true, /*blocking*/ false};` form still works.

## Compile-time CAN FD vs Classic

The bus is the class template `BasicCanbus<bool EnableFd>` with two aliases:

```cpp
using CanClassic = BasicCanbus<false>;  // Classic only (kernel default)
using CanFD      = BasicCanbus<true>;   // Classic + CAN FD (CAN_RAW_FD_FRAMES)
```

The FD-only API (`send(const FdFrame&)`, `addFdListener`) exists **only** on
`CanFD`; calling it on a `CanClassic` is a compile error. A `CanClassic`
never receives CAN FD frames because the kernel only delivers `CAN_MTU`
frames to it (docs: "RAW Socket Option CAN_RAW_FD_FRAMES").
If the interface or kernel cannot do CAN FD, a `CanFD` socket falls back to
Classic only and `fdEnabled()` returns `false`.

## Design

The header is split into small parts, each with one job, following the SOLID
principles and using dependency injection where it pays off.

| Component | Responsibility |
| --- | --- |
| `Frame`, `FdFrame` | Plain value types, no behaviour |
| `FrameCodec` | Validates frames and converts to/from `struct can_frame` / `struct canfd_frame`. Pure functions, no I/O |
| `ISocketApi` | The system calls the bus needs (`socket`, `setsockopt`, `bind`, `poll`, `read`, `write`, ...) |
| `PosixSocketApi` | The real Linux implementation of `ISocketApi` |
| `ListenerRegistry<L, N>` | Fixed-capacity, non-owning observer list |
| `IClassicFrameListener`, `IFdFrameListener` | What receivers implement (segregated per frame type) |
| `ICanSender`, `ICanFdSender` | What senders depend on |
| `BasicCanbus<EnableFd>` | RAII socket that composes the above |

- **Single responsibility:** validation/encoding, system calls, listener
  bookkeeping and socket lifetime live in separate types.
- **Open/closed:** new behaviour (tracing, a different platform, a fake) is
  added by implementing `ISocketApi` or a listener, not by editing the bus.
- **Liskov substitution:** `CanFD` can be used anywhere an `ICanSender` or
  `ICanFdSender` is expected; `CanClassic` only where an `ICanSender` is.
- **Interface segregation:** Classic and FD listeners are separate
  interfaces, and so are Classic and FD senders.
- **Dependency inversion:** the bus depends on `ISocketApi`, not on the OS;
  your code can depend on `ICanSender` / listener interfaces, not on the bus.

### Injecting the socket API

```cpp
MySocketApi api;                         // implements SocketCAN::ISocketApi
CanFD bus{"can0", SocketOptions{}, api}; // api must outlive bus
```

Without an argument the bus uses `defaultSocketApi()`, a shared
`PosixSocketApi`.

### Depending on an abstraction in your own code

```cpp
class Heartbeat {
public:
    explicit Heartbeat(SocketCAN::ICanSender& out) : m_out(out) {}
    void tick() {
        SocketCAN::Frame f;
        f.id = 0x700; f.dlc = 1; f.data[0] = m_counter++;
        m_out.send(f);
    }
private:
    SocketCAN::ICanSender& m_out;
    uint8_t m_counter = 0;
};

SocketCAN::CanClassic bus{"can0"};
Heartbeat hb{bus};       // production
// Heartbeat hb{fake};   // unit test, with your own ICanSender fake
```

## Setting up can0

### Real hardware

Configure the bit rate before bringing the interface up (kernel docs:
"Setting the CAN Bit-Timing" and "CAN FD (Flexible Data Rate) Driver
Support"; also `ip link help can`):

```bash
# Classic CAN at 500 kbit/s
sudo ip link set can0 type can bitrate 500000
sudo ip link set up can0

# CAN FD: 500 kbit/s arbitration, 2 Mbit/s data phase
sudo ip link set can0 type can bitrate 500000 dbitrate 2000000 fd on
sudo ip link set up can0
```

### Virtual can0 (no hardware)

For development and the integration tests, `can0` can be a virtual (`vcan`)
interface:

```bash
sudo modprobe vcan
sudo ip link add dev can0 type vcan
sudo ip link set up can0
```

`vcan` is CAN FD capable out of the box. The kernel docs name virtual
interfaces `vcanX` by convention; this project names it `can0` so the same
code and examples run unchanged on real hardware.

## Tests

The project is developed test first. There are three groups:

| Suite | Needs can0? | Covers |
| --- | --- | --- |
| `FrameCodec.*` | No | ID/DLC/length validation, encode/decode round trips |
| `ListenerRegistry.*` | No | Capacity, null handling, fan-out |
| `CanbusUnit.*`, `CanbusCompileTime.*` | No | The bus with an injected `FakeSocketApi`: socket setup, error paths and fd cleanup, move semantics, send framing, receive and dispatch, compile-time FD gating |
| `CanbusIntegration.*` | Yes | Real round trips over `can0` through the kernel |

Integration tests use `can0`, or the interface named by
`SOCKETCAN_TEST_IFACE`. When that interface does not exist they are reported
as skipped, so the unit tests still run anywhere.

### Build and run

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
cd build && ctest --output-on-failure
```

CI (GitHub Actions) brings up a virtual `can0` and runs the full suite on
every push and pull request.

## Developer environment (Docker dev container)

`.devcontainer/` has the full toolchain (GCC, CMake, GoogleTest) plus
`iproute2`/`kmod`/`can-utils`. Its entrypoint creates `can0` as a `vcan`
interface on start.

### `./build.py`

```bash
./build.py test      # build + run the test suite (default)
./build.py build     # configure + build only
./build.py shell     # interactive shell, can0 already up
./build.py image     # (re)build the Docker image
./build.py clean     # remove build/
```

### VS Code

Open the folder and choose **Reopen in Container**. The container gets
`NET_ADMIN` + `SYS_MODULE` and a read-only `/lib/modules` mount so the
entrypoint can load `vcan` and create `can0`.

> The host kernel must provide the `vcan` module (standard on Ubuntu). The
> container shares the host kernel, so `modprobe vcan` loads it there.

### Plain Docker

```bash
docker build -f .devcontainer/Dockerfile -t socketcan-dev .devcontainer

docker run --rm \
    --cap-add=NET_ADMIN --cap-add=SYS_MODULE \
    -v /lib/modules:/lib/modules:ro \
    -v "$PWD":/workspace -w /workspace \
    socketcan-dev \
    bash -c 'cmake -S . -B build -DBUILD_TESTING=ON && \
             cmake --build build -j && \
             (cd build && ctest --output-on-failure)'
```

## Project structure

```
.
├── .devcontainer/
│   ├── Dockerfile
│   ├── devcontainer.json
│   └── entrypoint.sh            # brings up can0 (vcan) on container start
├── .github/workflows/ci.yml     # build + unit + integration tests
├── build.py                     # Docker build/test helper
├── CMakeLists.txt
├── SocketCan.hpp                # the whole library (header-only)
└── tests/
    ├── FakeSocketApi.hpp        # ISocketApi test double
    ├── FrameCodecTests.cpp
    ├── ListenerRegistryTests.cpp
    ├── CanbusUnitTests.cpp
    └── CanbusIntegrationTests.cpp
```

## Reference

- Linux kernel documentation, *SocketCAN - Controller Area Network*:
  https://docs.kernel.org/networking/can.html
- Kernel headers used: `<linux/can.h>`, `<linux/can/raw.h>`

## License

MIT, see [LICENSE](LICENSE).
