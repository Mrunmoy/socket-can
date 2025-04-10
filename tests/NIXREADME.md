# Nix Build & DevShell Guide for the SocketCAN C++ Library

This document explains **how the Nix-based workflow works**, how to build your library and tests using flakes, and how the development environment is structured.

It complements the main `README.md`.

---

# Project Structure (Nix-Aware)

```
socket-can/
├── flake.nix
├── flake.lock
├── SocketCan.cpp
├── SocketCan.hpp
├── CMakeLists.txt
├── tests/
│   ├── CMakeLists.txt
│   ├── setup_vcan.sh
│   ├── CanbusUnitTests.cpp
│   ├── CanbusIntegrationTests.cpp
│   └── UnitTests.cpp
```

Nix is responsible for:

- Creating a *reproducible dev environment* (`nix develop`)
- Building the library (`nix build`)
- Building tests separately (`nix build .#tests`)
- Ensuring dependencies (GCC, GTest, Linux headers, CMake) are identical everywhere

---

# Development Environment

Inside your project, run:

```
nix develop
```

This loads the environment specified in `devShells.default`:

### Includes

- GCC / G++
- CMake
- gdb
- pkg-config
- linuxHeaders
- GTest (for manual/local builds)

### Prompt

Your prompt becomes:

```
[nix-socketcan] $
```

Useful for knowing when you're inside the environment.

---

# Building the Library Using Nix

You do **not** manually run CMake when using:

```
nix build
```

This triggers the flake’s `packages.default` derivation, which:

1. Configures CMake
2. Builds the shared + static libs
3. Installs headers + libs into `$out`

After building, you get:

```
result/
├── include/SocketCan.hpp
└── lib/
    ├── libsocketcan.so
    └── libsocketcan_static.a
```

---

# Building Tests Using Nix

Tests are **separated** into their own derivation so that:

- library builds do **not** automatically pull GTest
- tests stay optional
- Nix outputs stay clean

Run:

```
nix build .#tests
```

This creates a *different result symlink*:

```
result/
└── socketcan_tests
```

### Running tests

Just execute:

```
./result/socketcan_tests
```

Because GTest is statically linked and the test binary has correct RPATHs.

> No need for LD_LIBRARY_PATH, everything is bundled correctly.

---

# Running Tests Manually (Non-Nix)

Inside build directory:

```
mkdir build
cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .
ctest --output-on-failure
```

This uses GTest from the dev shell.

---

# Virtual CAN Interface Setup (vcan0)

Tests needing integration with SocketCAN require a virtual CAN device.

CTests automatically run this:

```
tests/setup_vcan.sh
```

It performs:

```
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
```

Nix doesn't modify your system; the script does only what is needed.

---

# Summary of Nix Commands

### Enter dev environment

```
nix develop
```

### Build library only

```
nix build
```

### Build and extract the test binary

```
nix build .#tests
```

### Run tests

```
./result/socketcan_tests
```

---

# Why This Repository Uses Two Nix Packages?

### `packages.default`
- Builds normal library
- Installs only `include/` and `lib/`
- Does NOT require GTest or sudo
- Clean output for dependents

### `packages.tests`
- Builds test-only executable
- Includes GTest
- Includes proper RPATH
- Does NOT install library

This separation ensures reproducibility & modularity.

---

# GitHub CI (Future)

A typical workflow will be:

```
nix develop --command ctest
```

or

```
nix build .#tests
./result/socketcan_tests
```

---

# Notes & Best Practices

- Do **not** check in the `result` symlink — it points into `/nix/store`
- `flake.lock` **must** be checked in for reproducibility
- DevShell ensures all contributors compile with the same environment
- Library consumers (Rust, Python, C++) will benefit from consistent ABI

---

# End of Nix README

Let me know if you want:
- A Nix-based **example consumer** (Rust, C++, or Python)
- A GitHub Actions workflow using Nix
- A NixOS module to install your library system-wide
