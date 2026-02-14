#!/bin/bash
#
# Build whisper.cpp with CUDA GPU Acceleration
#
# This script clones whisper.cpp (if needed) and builds it with CUDA support.
#
# Prerequisites:
# - CUDA Toolkit installed: sudo apt install nvidia-cuda-toolkit
# - CMake and build tools installed
#
# Usage:
#   ./build-whisper-cuda.sh

set -e

echo "========================================="
echo "Building whisper.cpp with CUDA Support"
echo "========================================="
echo

# Look for directories relative to this script's directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check if CUDA is installed
if ! command -v nvcc &> /dev/null; then
    echo "Error: CUDA Toolkit not found (nvcc command not available)."
    echo
    echo "Install CUDA with:"
    echo "  sudo apt install nvidia-cuda-toolkit"
    echo
    exit 1
fi

echo "CUDA Toolkit detected:"
nvcc --version | head -1
echo

# Check if nvidia-smi is available
if command -v nvidia-smi &> /dev/null; then
    echo "Available NVIDIA GPUs:"
    nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader
    echo
else
    echo "Warning: nvidia-smi not found. Cannot verify GPU availability."
    echo
fi

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

echo "Configuring CMake with CUDA support (-DGGML_CUDA=1)..."
cmake -B build -DGGML_CUDA=1

echo
echo "Building whisper.cpp (using up to 6 parallel jobs)..."
cmake --build build -j $(( $(nproc) < 6 ? $(nproc) : 6 )) --config Release

echo
echo "========================================="
echo "Build Complete!"
echo "========================================="
echo

# Verify the build
if [ -f "build/src/libwhisper.so" ] || [ -f "build/src/libwhisper.a" ]; then
    echo "✓ whisper library built successfully"
else
    echo "✗ Warning: whisper library not found"
fi

if grep -q "GGML_CUDA:BOOL=ON\|GGML_CUDA:STRING=1" build/CMakeCache.txt 2>/dev/null; then
    echo "✓ CUDA support enabled"
else
    echo "✗ Warning: CUDA support may not be enabled"
fi

echo
echo "Next steps:"
echo "  1. Rebuild the Go service:"
echo "     cd ../service && ./build.sh"
echo "  2. Run the service:"
echo "     cd ../service && ./run.sh"
echo "  3. GPU will be used automatically!"
echo
