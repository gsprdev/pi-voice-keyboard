#!/bin/bash
# Run the transcription service with proper library paths

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Path to whisper.cpp libraries
WHISPER_CPP_DIR="$SCRIPT_DIR/../whisper.cpp"

# Set library path so the service can find whisper libraries at runtime
export LD_LIBRARY_PATH="${WHISPER_CPP_DIR}/build/src:${WHISPER_CPP_DIR}/build/ggml/src:${LD_LIBRARY_PATH}"

# Set port (override with environment variable)
export PORT="${PORT:-8080}"

echo "Starting transcription service on port $PORT..."
echo ""

# Run the service
./bin/transcription-service
