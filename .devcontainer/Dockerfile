# Dev container for the SocketCAN C++ library.
# Provides the full build/test toolchain (GCC, CMake, GoogleTest) plus the
# tooling needed to bring up a virtual CAN interface (vcan0) for the
# integration tests. The entrypoint creates vcan0 on container start.
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

# Bring up vcan0 on container start, then run the requested command.
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /workspace

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
