#!/usr/bin/env bash
# .devcontainer/setup.sh
#
# Runs once when the codespace container is first created (onCreateCommand).
# Installs everything needed to compile the C++23 terminal and let GLFW
# open a real GL window against the desktop-lite feature's Xvfb display.
set -euo pipefail

echo ">>> Installing GCC 13, CMake, and GL/X11 dev headers..."
sudo apt-get update -qq

sudo apt-get install -y -qq \
    gcc-13 g++-13 \
    cmake ninja-build \
    libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    pkg-config

# Point the default gcc/g++ at 13 explicitly (base image may ship an older
# default alongside 13; devcontainers/cpp images generally include 13 on
# Ubuntu 24.04, this just makes the selection unambiguous and reproducible).
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 130
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 130

echo ">>> g++ version in use:"
g++ --version | head -1

echo ">>> Setup complete. Run 'cmake --build build -j' then './run_terminal.sh' to launch."
