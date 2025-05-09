# Dev container for the SocketCAN C++ library.
# Provides the full build/test toolchain (GCC, CMake, GoogleTest) plus the
# tooling needed to bring up a virtual CAN interface (vcan0) for the
# integration tests.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        g++ \
        gdb \
        git \
        iproute2 \
        kmod \
        can-utils \
        libgtest-dev \
        pkg-config \
        sudo \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04's libgtest-dev ships prebuilt static libraries and a CMake
# config package, so no manual GoogleTest build step is required.

WORKDIR /workspace
