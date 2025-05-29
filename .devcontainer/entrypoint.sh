#!/usr/bin/env bash
# Container entrypoint: bring up the virtual CAN interface (vcan0) that the
# integration tests use, then exec the requested command. Runs as root inside
# the container, so no sudo is needed.
#
# Requires the container to be started with --cap-add=NET_ADMIN (create the
# link) and, if the host hasn't already loaded the module, --cap-add=SYS_MODULE
# plus a read-only /lib/modules mount (load vcan). Failures are non-fatal so the
# container still starts for non-CAN work; they are reported on stderr.

if ! lsmod | grep -q "^vcan"; then
    modprobe vcan 2>/dev/null \
        || echo "entrypoint: could not load vcan module (need --cap-add=SYS_MODULE and a /lib/modules mount)" >&2
fi

if ! ip link show vcan0 >/dev/null 2>&1; then
    ip link add dev vcan0 type vcan 2>/dev/null \
        || echo "entrypoint: could not create vcan0 (need --cap-add=NET_ADMIN)" >&2
fi

ip link set vcan0 up 2>/dev/null || true

exec "$@"
