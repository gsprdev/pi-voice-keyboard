#!/bin/bash
#
# Build the Voice Keyboard firmware for Raspberry Pi Pico 2 W.
#
# Prerequisites:
#   sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
#
# Usage:
#   ./build.sh
#
# Output: build/voice-keyboard.uf2

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v arm-none-eabi-gcc &> /dev/null; then
    echo "Error: arm-none-eabi-gcc not found."
    echo
    echo "Install with:"
    echo "  sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib"
    exit 1
fi

# Fetch pico-sdk and only the libraries this firmware uses
if [ ! -f "pico-sdk/pico_sdk_init.cmake" ]; then
    echo "pico-sdk not found. Fetching submodule..."
    git submodule update --init pico-sdk
fi
git -C pico-sdk submodule update --init lib/tinyusb lib/cyw43-driver lib/lwip

cmake -B build
cmake --build build -j "$(nproc)"

echo
echo "Built: $SCRIPT_DIR/build/voice-keyboard.uf2"
