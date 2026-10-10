#!/bin/bash
#
# Build the Voice Keyboard firmware for Raspberry Pi Pico 2 W.
#
# Prerequisites:
#   sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
#
# Usage:
#   ./build.sh
#   PICO_BOARD=pico2 ./build.sh       # Plain Pico 2 (no Wi-Fi), output in build-pico2/
#   MIC_TIMING=standard ./build.sh    # ICS-43434 and other standard I2S mics
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

# Each board gets its own build directory, since the board can't change after configuring
BUILD_DIR=build${PICO_BOARD:+-$PICO_BOARD}

cmake -B "$BUILD_DIR" ${PICO_BOARD:+-DPICO_BOARD=$PICO_BOARD} ${MIC_TIMING:+-DMIC_TIMING=$MIC_TIMING}
cmake --build "$BUILD_DIR" -j "$(nproc)"

echo
echo "Built: $SCRIPT_DIR/$BUILD_DIR/voice-keyboard.uf2"
