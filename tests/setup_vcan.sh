#!/usr/bin/env bash
set -euo pipefail

# Use sudo only when not already root (e.g. on a dev host). Inside the
# devcontainer this runs as root, so sudo is unnecessary and may be absent.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

# Load vcan module if not loaded
if ! lsmod | grep -q "^vcan"; then
    echo "Loading vcan kernel module..."
    $SUDO modprobe vcan
fi

# Create vcan0 if it doesn't exist
if ! ip link show vcan0 >/dev/null 2>&1; then
    echo "Creating vcan0 interface..."
    $SUDO ip link add dev vcan0 type vcan
fi

# Ensure it is up (idempotent)
$SUDO ip link set vcan0 up

echo "vcan0 is ready."
