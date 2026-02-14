#!/bin/bash
# Test client for transcription service (Phase 1)
# Records audio and sends it to the local transcription service

set -e

# Configuration
SERVICE_URL="${SERVICE_URL:-http://localhost:8080/transcribe}"
DURATION="${DURATION:-5}"  # seconds to record
SAMPLE_RATE=16000
AUDIO_FILE="/tmp/recording.wav"

echo "=== Transcription Service Test Client ==="
echo "Service URL: $SERVICE_URL"
echo "Recording duration: ${DURATION}s"
echo ""

# Check if arecord is available
if ! command -v arecord &> /dev/null; then
    echo "Error: arecord not found. Install with: sudo apt install alsa-utils"
    exit 1
fi

# Record audio
echo "Recording... (speak now for ${DURATION} seconds)"
arecord -f S16_LE -r $SAMPLE_RATE -c 1 -d $DURATION "$AUDIO_FILE" 2>/dev/null
echo "Recording complete"
echo ""

# Send to transcription service
echo "Sending audio to transcription service..."
response=$(curl -s -X POST \
    --data-binary "@${AUDIO_FILE}" \
    "$SERVICE_URL")

# Display result
echo "=== Transcription Result ==="
echo "$response"
echo ""

# Cleanup
rm -f "$AUDIO_FILE"
echo "Done!"
