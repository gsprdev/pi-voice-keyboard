#!/bin/bash
#
# Build whisper.cpp with Vulkan GPU Acceleration
#
# This script clones whisper.cpp (if needed) and builds it with Vulkan support.
# Targets non-NVIDIA hardware (e.g., Intel Mesa integrated graphics).
#
# Prerequisites:
# - Vulkan SDK: sudo apt install libvulkan-dev vulkan-tools
# - GLSL compiler: sudo apt install glslc
# - CMake and build tools installed
#
# Usage:
#   ./build-whisper-vulkan.sh

set -e

echo "========================================="
echo "Building whisper.cpp with Vulkan Support"
echo "========================================="
echo

# Look for directories relative to this script's directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check required packages are installed
MISSING_PKGS=()
for pkg in libvulkan-dev vulkan-tools glslc; do
    if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "install ok installed"; then
        MISSING_PKGS+=("$pkg")
    fi
done

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    echo "Error: Missing required packages: ${MISSING_PKGS[*]}"
    echo
    echo "Install with:"
    echo "  sudo apt install ${MISSING_PKGS[*]}"
    echo
    exit 1
fi

echo "Vulkan runtime detected:"
vulkaninfo --summary 2>/dev/null | grep -E "apiVersion|deviceName" | head -2
echo

echo "GLSL compiler detected:"
glslc --version 2>&1 | head -1
echo

# Clone whisper.cpp if it doesn't exist
if [ ! -d "whisper.cpp" ]; then
    echo "whisper.cpp not found. Cloning from upstream..."
    git submodule update --init whisper.cpp
    echo
fi

# Navigate to whisper.cpp directory
cd whisper.cpp

# Clean previous build
if [ -d "build" ]; then
    echo "Cleaning previous build..."
    rm -rf build
    echo
fi

echo "Configuring CMake with Vulkan support (-DGGML_VULKAN=1)..."
cmake -B build -DGGML_VULKAN=1

echo
echo "Building whisper.cpp (using up to 6 parallel jobs)..."
cmake --build build -j $(( $(nproc) < 6 ? $(nproc) : 6 )) --config Release

echo
echo "========================================="
echo "Build Complete!"
echo "========================================="
echo

# Verify the build - check for libwhisper.so
if [ -f "build/src/libwhisper.so" ]; then
    echo "✓ whisper library built successfully"
else
    echo "✗ Warning: whisper library not found (expected build/src/libwhisper.so)"
fi

# Verify the build - find libggml-vulkan.so anywhere under build/
VULKAN_LIB="$(find build -name "libggml-vulkan.so" 2>/dev/null | head -1)"
if [ -n "$VULKAN_LIB" ]; then
    echo "✓ Vulkan backend library built successfully ($VULKAN_LIB)"
else
    echo "✗ Warning: Vulkan backend library not found (libggml-vulkan.so)"
fi

# Verify Vulkan enablement in CMake cache
if grep -q "^GGML_VULKAN:BOOL=1" build/CMakeCache.txt 2>/dev/null; then
    echo "✓ Vulkan support enabled"
else
    echo "✗ Warning: Vulkan support may not be enabled"
fi

echo
echo "Next steps:"
echo "  1. Rebuild the Go service:"
echo "     cd ../transcribe-whisper && ./build.sh"
echo "  2. Run the service:"
echo "     cd ../transcribe-whisper && ./run.sh"
echo "  3. GPU will be used automatically!"
echo
