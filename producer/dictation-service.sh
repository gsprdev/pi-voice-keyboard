#!/bin/bash
# Simple terminal-based loop for audio recording, delegation to transcription service, and relay to Pi for typing
# Unlike dictation-toggle.sh, this script is intended for scenarios whhen the when the system being typed on is different from the recording system

set -e

# Configuration
SERVICE_URL="${SERVICE_URL:-http://localhost:8080/transcribe}"
REMOTE_HOST="${REMOTE_HOST:-pi02w.local}"
REMOTE_PORT="${REMOTE_PORT:-1234}"
SAMPLE_RATE=16000
AUDIO_FILE="/tmp/dictation-recording.wav"
AUDIO_DEVICE="${AUDIO_DEVICE:-plughw:CARD=MIC,DEV=0}"  # USB mic, or set via environment

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo "=== Dictation Service Client ==="
echo "Service: $SERVICE_URL"
echo "Remote: $REMOTE_HOST:$REMOTE_PORT"
echo ""

# Check dependencies
for cmd in parecord curl ssh nc; do
    if ! command -v $cmd &> /dev/null; then
        echo -e "${RED}Error: $cmd not found${NC}"
        echo "Install with: sudo apt install pulseaudio-utils curl openssh-client netcat"
        exit 1
    fi
done

# Test service connection
echo -n "Testing transcription service... "
if curl -s -o /dev/null -w "%{http_code}" "$SERVICE_URL" | grep -q "401"; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
    echo "Make sure the transcription service is running (cd service && ./run.sh)"
    exit 1
fi

# Setup SSH tunnel to Pi
echo -n "Connecting to Pi... "
ssh -N -L 127.0.0.1:$REMOTE_PORT:127.0.0.1:$REMOTE_PORT $REMOTE_HOST &
SSH_PID=$!
trap "kill $SSH_PID 2>/dev/null" EXIT

# Wait for tunnel
for i in {1..30}; do
    if nc -z 127.0.0.1 $REMOTE_PORT 2>/dev/null; then
        echo -e "${GREEN}Connected${NC}"
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}FAILED${NC}"
        echo "Could not connect to $REMOTE_HOST"
        exit 1
    fi
    sleep 0.1
done

echo ""
echo -e "${GREEN}Ready for dictation!${NC}"
echo ""

# Main loop
while true; do
    echo -e "${YELLOW}Press ENTER to start recording (or Ctrl+C to quit)${NC}"
    read -r

    echo -e "${GREEN}● Recording...${NC} (Press ENTER to stop)"

    # Start recording in background using parecord with low latency
    parecord --format=s16le --rate=$SAMPLE_RATE --channels=1 --latency-msec=500 "$AUDIO_FILE" &
    RECORD_PID=$!

    # Wait for user to press Enter to stop
    read -r

    # Stop recording
    kill $RECORD_PID 2>/dev/null || true
    wait $RECORD_PID 2>/dev/null || true

    # Check if audio file was created
    if [ ! -f "$AUDIO_FILE" ]; then
        echo -e "${RED}Error: Audio file not created${NC}"
        continue
    fi

    # Check audio file size
    AUDIO_SIZE=$(stat -c%s "$AUDIO_FILE" 2>/dev/null || stat -f%z "$AUDIO_FILE" 2>/dev/null || echo "0")
    if [ -z "$AUDIO_SIZE" ] || [ "$AUDIO_SIZE" -lt 1000 ]; then
        echo -e "${YELLOW}Recording too short, skipping${NC}"
        rm -f "$AUDIO_FILE"
        echo ""
        continue
    fi

    echo "Audio captured: $AUDIO_SIZE bytes"
    echo -n "Transcribing... "

    # Send to transcription service (with better error handling)
    HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/transcription-response.txt -X POST \
        --data-binary "@${AUDIO_FILE}" \
        "$SERVICE_URL")
    CURL_EXIT=$?

    if [ $CURL_EXIT -ne 0 ]; then
        echo -e "${RED}Failed${NC}"
        echo "Curl error (exit code: $CURL_EXIT)"
        echo "Possible causes:"
        echo "  - Service not running (check: cd ../service && ./run.sh)"
        echo "  - Network issue"
        rm -f "$AUDIO_FILE" /tmp/transcription-response.txt
        echo ""
        continue
    fi

    RESPONSE=$(cat /tmp/transcription-response.txt)

    if [ "$HTTP_CODE" -eq 200 ] && [ -n "$RESPONSE" ]; then
        echo -e "${GREEN}Done${NC}"
        echo "Text: $RESPONSE"

        # Send to Pi
        echo -n "Typing on remote system... "
        printf "%s" "$RESPONSE" | nc -q 1 127.0.0.1 $REMOTE_PORT 2>&1
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}Done${NC}"
        else
            echo -e "${RED}Failed${NC}"
            echo "Could not connect to Pi (check kb-serve.sh is running)"
        fi
    else
        echo -e "${RED}Failed (HTTP $HTTP_CODE)${NC}"
        if [ "$HTTP_CODE" -eq 400 ]; then
            echo "Bad request - audio format issue?"
            echo "Response: $RESPONSE"
        else
            echo "Response: $RESPONSE"
        fi
    fi

    echo ""

    # Cleanup
    rm -f "$AUDIO_FILE" /tmp/transcription-response.txt
done
