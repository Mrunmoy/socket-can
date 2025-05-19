# SocketCAN C++ Library 
Based on https://docs.kernel.org/networking/can.html

A modern, safe, and test-driven C++17 wrapper for Linux SocketCAN.

This library provides:

- RAII-safe socket types: `SocketCAN::CanFD` (Classic + CAN FD) and `SocketCAN::CanClassic` (classic-only)
- Strongly-typed `SocketCAN::Frame` (Classic CAN) and `SocketCAN::FdFrame` (CAN FD) structures
- Support for both **Classic CAN** (up to 8 bytes) and **CAN FD** (up to 64 bytes) — the mode is a compile-time property of the socket type
- Separate listener interfaces (`IClassicFrameListener`, `IFdFrameListener`) — register as many as you need; each frame type dispatches only to matching listeners
- Blocking and non-blocking receive (with timeout support)
- Support for
  - Standard (11-bit) CAN IDs
  - Extended (29-bit) CAN IDs
  - RTR frames (Classic CAN only)
  - CAN FD bit-rate switch (BRS) and error state indicator (ESI) flags
  - Optional loopback suppression
  - CAN filters
- Zero heap allocation — listener slots are fixed-size arrays (`MaxListeners = 8` per type)
- Clean error handling via `SocketCAN::CanError`
- Full unit + integration test suite
- VCAN0 virtual CAN interface fixture for integration tests
- Docker dev container with the full toolchain + vcan tooling

## Features

### C++17 API
Encapsulated CAN handling with no raw file descriptors in user code.

### RAII
Sockets are opened in the constructor and safely closed in the destructor.

### Exceptions on errors
All system errors produce a `SocketCAN::CanError`.

### Compile-time CAN FD vs Classic
CAN FD is a compile-time property of the socket type, not a runtime flag. The
class is the template `BasicCanbus<bool EnableFd>`, with two aliases:

```cpp
using CanClassic = BasicCanbus<false>;  // Classic-only (kernel default)
using CanFD      = BasicCanbus<true>;   // Classic + CAN FD
```

```cpp
CanFD      fdBus{"can0"};       // CAN_RAW_FD_FRAMES enabled
CanClassic classicBus{"can0"};  // classic-only socket
```

The FD-only API (`send(const FdFrame&)`, `addFdListener`) exists **only** on the
FD type — calling it on a `CanClassic` is a *compile error*, not a runtime
one. A `CanClassic` never receives CAN FD frames (the kernel filters them).
If the interface/kernel cannot support CAN FD, an FD socket falls back to
classic-only and `fdEnabled()` reports `false`.

### Integration tests using VCAN
Tests automatically create `vcan0` using a test fixture.

## Include the library

```cpp
#include "SocketCan.hpp"
using namespace SocketCAN;
```

## Classic CAN usage

```cpp
#include "SocketCan.hpp"
#include <iostream>

using namespace SocketCAN;

int main() {
    CanClassic bus{"vcan0"};

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

## CAN FD usage

```cpp
#include "SocketCan.hpp"
#include <iostream>

using namespace SocketCAN;

int main() {
    CanFD bus{"vcan0"};

    FdFrame f{};
    f.id  = 0x456;
    f.len = 12;
    f.brs = true;   // enable bit-rate switch
    for (uint8_t i = 0; i < f.len; ++i)
        f.data[i] = i;

    bus.send(f);
}
```

## Listener-based receive (Classic + FD on one socket)

Implement `IClassicFrameListener` and/or `IFdFrameListener`, register them,
then call `receive()` in your loop. Each frame is dispatched only to listeners
of the matching type — Classic frames never reach FD listeners and vice versa.

```cpp
#include "SocketCan.hpp"
#include <iostream>

using namespace SocketCAN;

struct MyClassicHandler : IClassicFrameListener {
    void onClassicFrame(const Frame& f) override {
        std::cout << "Classic  ID=0x" << std::hex << f.id
                  << " DLC=" << std::dec << int(f.dlc) << "\n";
    }
};

struct MyFdHandler : IFdFrameListener {
    void onFdFrame(const FdFrame& f) override {
        std::cout << "CAN FD   ID=0x" << std::hex << f.id
                  << " len=" << std::dec << int(f.len)
                  << (f.brs ? " BRS" : "") << "\n";
    }
};

int main() {
    CanFD bus{"vcan0"};

    MyClassicHandler classicHandler;
    MyFdHandler      fdHandler;

    bus.addClassicListener(&classicHandler);
    bus.addFdListener(&fdHandler);

    // Dispatch loop — each call reads one frame and notifies the right listener
    while (true) {
        bus.receive(std::chrono::milliseconds{100});
    }
}
```

Up to `CanFD::MaxListeners` (8) listeners of each type can be registered.
All registered listeners of a given type are notified for every matching frame.

## Developer Environment (Docker Dev Container)

The repository ships a `.devcontainer/` with the full toolchain (GCC, CMake,
GoogleTest, pkg-config) plus `iproute2`/`kmod`/`can-utils` for the virtual CAN
interface.

### VS Code

Open the folder and choose **Reopen in Container**. The container is granted
`NET_ADMIN` + `SYS_MODULE` and mounts `/lib/modules` read-only so it can bring
up `vcan0`; `tests/setup_vcan.sh` runs automatically on create.

> The host kernel must provide the `vcan` module (standard on Ubuntu). The
> container shares the host kernel, so `modprobe vcan` loads it there.

### Plain Docker (CI-style)

```bash
docker build -f .devcontainer/Dockerfile -t socketcan-dev .devcontainer

docker run --rm \
    --cap-add=NET_ADMIN --cap-add=SYS_MODULE \
    -v /lib/modules:/lib/modules:ro \
    -v "$PWD":/workspace -w /workspace \
    socketcan-dev \
    bash -c 'tests/setup_vcan.sh && \
             cmake -S . -B build -DBUILD_TESTING=ON && \
             cmake --build build -j && \
             (cd build && ctest --output-on-failure)'
```

## Building and Running Tests with CMake

```bash
./tests/setup_vcan.sh        # create vcan0 (uses sudo on a dev host)
mkdir build
cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .
ctest --output-on-failure
```

## Project Structure

```
.
├── .devcontainer/
│   ├── Dockerfile
│   └── devcontainer.json
├── CMakeLists.txt
├── SocketCan.hpp          # header-only: BasicCanbus<EnableFd> + aliases
└── tests/
    ├── setup_vcan.sh
    ├── UnitTests.cpp
    ├── CanbusUnitTests.cpp
    └── CanbusIntegrationTests.cpp
```
