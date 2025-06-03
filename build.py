#!/usr/bin/env python3
"""Dev helper: build/test the SocketCAN library inside the Docker dev container.

vcan0 is brought up automatically by the container entrypoint.

Usage:
    ./build.py test      build the library and run the test suite   (default)
    ./build.py build     configure + build only (no tests)
    ./build.py shell     open an interactive shell (vcan0 already up)
    ./build.py image     (re)build the Docker image
    ./build.py clean     remove the build/ directory
"""

import argparse
import os
import shutil
import subprocess
import sys

IMAGE = "socketcan-dev"
ROOT = os.path.dirname(os.path.abspath(__file__))
DOCKERFILE = os.path.join(ROOT, ".devcontainer", "Dockerfile")

# Shared docker run flags: CAN caps + module access + source mount.
DOCKER_ARGS = [
    "--rm",
    "--cap-add=NET_ADMIN",
    "--cap-add=SYS_MODULE",
    "-v", "/lib/modules:/lib/modules:ro",
    "-v", f"{ROOT}:/workspace",
    "-w", "/workspace",
]


def run(cmd, **kwargs):
    """Run a command, echoing it; return the exit code."""
    print(">>", " ".join(cmd))
    return subprocess.run(cmd, **kwargs).returncode


def build_image():
    return run(["docker", "build", "-f", DOCKERFILE, "-t", IMAGE,
                os.path.join(ROOT, ".devcontainer")])


def ensure_image():
    have = subprocess.run(
        ["docker", "image", "inspect", IMAGE],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if not have:
        print(f">> image '{IMAGE}' not found — building it")
        rc = build_image()
        if rc != 0:
            sys.exit(rc)


def run_in_container(inner):
    """Run an inner build command, then hand build/ back to the host user
    (the container runs as root, so artifacts would otherwise be root-owned)."""
    ensure_image()
    wrapped = (
        f"{inner}; status=$?; "
        f"chown -R $HOST_UID:$HOST_GID build 2>/dev/null || true; "
        f"exit $status"
    )
    return run([
        "docker", "run", *DOCKER_ARGS,
        "-e", f"HOST_UID={os.getuid()}",
        "-e", f"HOST_GID={os.getgid()}",
        IMAGE, "bash", "-c", wrapped,
    ])


def cmd_test(_):
    return run_in_container(
        "cmake -S . -B build -DBUILD_TESTING=ON "
        "&& cmake --build build -j "
        "&& (cd build && ctest --output-on-failure)"
    )


def cmd_build(_):
    return run_in_container(
        "cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build -j"
    )


def cmd_shell(_):
    ensure_image()
    return run(["docker", "run", "-it", *DOCKER_ARGS, IMAGE])


def cmd_image(_):
    return build_image()


def cmd_clean(_):
    build_dir = os.path.join(ROOT, "build")
    shutil.rmtree(build_dir, ignore_errors=True)
    print("removed build/")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")
    for name, fn in {
        "test": cmd_test,
        "build": cmd_build,
        "shell": cmd_shell,
        "image": cmd_image,
        "clean": cmd_clean,
    }.items():
        sub.add_parser(name).set_defaults(func=fn)

    args = parser.parse_args()
    func = getattr(args, "func", cmd_test)   # default to `test`
    sys.exit(func(args))


if __name__ == "__main__":
    main()
