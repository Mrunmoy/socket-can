# SocketCAN C++ Library 
Based on https://docs.kernel.org/networking/can.html

A modern, safe, and test-driven C++17 wrapper for Linux SocketCAN.

This library provides:

- A RAII-safe `SocketCAN::Canbus` class
- Strongly-typed `SocketCAN::Frame` (Classic CAN) and `SocketCAN::FdFrame` (CAN FD) structures
- Support for both **Classic CAN** (up to 8 bytes) and **CAN FD** (up to 64 bytes) on a single socket
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
- Complete Nix flake support (dev shell + builds)

## Features

### C++17 API
Encapsulated CAN handling with no raw file descriptors in user code.

### RAII
Sockets are opened in the constructor and safely closed in the destructor.

### Exceptions on errors
All system errors produce a `SocketCAN::CanError`.

### CAN FD + Classic CAN on one socket
A single `Canbus` instance enables `CAN_RAW_FD_FRAMES` automatically. You can send and receive both frame types through it without any extra configuration.

### Integration tests using VCAN
Tests automatically create `vcan0` using a test fixture.

### Nix-native build & test workflow
- `nix develop` gives a full C++ devshell
- `nix build` builds the library
- `nix build .#tests` builds test binary at `result/socketcan_tests`
   make sure to run the tests/setup_vcan.sh first to create the interface

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
    Canbus bus{"vcan0"};

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
    Canbus bus{"vcan0"};

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
    Canbus bus{"vcan0"};

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

Up to `Canbus::MaxListeners` (8) listeners of each type can be registered.
All registered listeners of a given type are notified for every matching frame.

## Developer Environment (Nix Flake)
Provides GCC, CMake, GTest, pkg-config, Linux headers.

```bash
nix develop
./tests/setup_vcan.sh
```

## Building and Running Tests with Nix

```bash
nix build
nix build .#tests
./result/socketcan_tests
```

## Building and Running Tests  with CMake

```bash
mkdir build
cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .
ctest --output-on-failure
```

## Project Structure

```
.
├── CMakeLists.txt
├── flake.nix
├── SocketCan.hpp
├── SocketCan.cpp
└── tests/
    ├── setup_vcan.sh
    ├── UnitTests.cpp
    ├── CanbusUnitTests.cpp
    └── CanbusIntegrationTests.cpp
```
