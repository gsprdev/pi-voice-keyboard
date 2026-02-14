#!/bin/bash
set -e

# Path to whisper.cpp (use absolute path)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHISPER_CPP_DIR="$(cd "$SCRIPT_DIR/../whisper.cpp" && pwd)"

# Create bin directory if it doesn't exist
mkdir -p bin

# Set CGO flags to point to whisper.cpp
export CGO_CFLAGS="-I${WHISPER_CPP_DIR}/include -I${WHISPER_CPP_DIR}/ggml/include"
export CGO_LDFLAGS="-L${WHISPER_CPP_DIR}/build/src -lwhisper -L${WHISPER_CPP_DIR}/build/ggml/src -lggml -lggml-base -lggml-cpu"

# Build the service
echo "Building transcription service..."
go build -o bin/transcription-service

echo "Build complete: bin/transcription-service"
