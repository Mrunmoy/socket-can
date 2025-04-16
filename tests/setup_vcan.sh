#!/usr/bin/env bash
set -e

# Load vcan module if not loaded
if ! lsmod | grep -q "^vcan"; then
    echo "Loading vcan kernel module..."
    sudo modprobe vcan
fi

# Create vcan0 if it doesn't exist
if ! ip link show vcan0 >/dev/null 2>&1; then
    echo "Creating vcan0 interface..."
    sudo ip link add dev vcan0 type vcan
    sudo ip link set vcan0 up
fi

echo "vcan0 is ready."
