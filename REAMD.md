# SocketCAN C++ Library 
Based on https://docs.kernel.org/networking/can.html

A modern, safe, and test-driven C++17 wrapper for Linux SocketCAN.

This library provides:

- A RAII-safe `SocketCAN::Canbus` class
- Strongly-typed `SocketCAN::Frame` structure
- Blocking and non-blocking receive (with timeout support)
- Support for
  - Standard (11-bit) CAN IDs
  - Extended (29-bit) CAN IDs
  - RTR frames
  - Optional loopback suppression
  - CAN filters
- Clean error handling via `SocketCAN::CanError`
- Full unit + integration test suite (18 tests)
- VCAN0 virtual CAN interface fixture for integration tests
- Complete Nix flake support (dev shell + builds)

## Features

### C++17 API
Encapsulated CAN handling with no raw file descriptors in user code.

### RAII
Sockets are opened in the constructor and safely closed in the destructor.

### Exceptions on errors
All system errors produce a `SocketCAN::CanError`.

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

## Basic Usage Example

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
