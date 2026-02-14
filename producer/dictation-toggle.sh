#!/bin/bash
# Toggle-based dictation for keyboard shortcut binding
# Bind this script to a keyboard shortcut (e.g., Super+D)
# Press once to start, press again to stop and transcribe

set -e

# Configuration
SERVICE_URL="${SERVICE_URL:-http://localhost:8080/transcribe}"
REMOTE_HOST="${REMOTE_HOST:-pi02w.local}"
REMOTE_PORT="${REMOTE_PORT:-1234}"
SAMPLE_RATE=16000

# State files
STATE_DIR="/tmp/dictation-toggle"
PID_FILE="$STATE_DIR/recording.pid"
AUDIO_FILE="$STATE_DIR/recording.wav"
TUNNEL_PID_FILE="$STATE_DIR/tunnel.pid"

mkdir -p "$STATE_DIR"

# Check if currently recording
if [ -f "$PID_FILE" ]; then
    # Stop recording
    RECORD_PID=$(cat "$PID_FILE")

    if ps -p $RECORD_PID > /dev/null 2>&1; then
        # Send SIGINT (Ctrl+C) for clean arecord shutdown
        kill -INT $RECORD_PID 2>/dev/null || true
        wait $RECORD_PID 2>/dev/null || true
        # Give arecord time to flush WAV file and write headers
        sleep 0.3
    fi

    rm -f "$PID_FILE"

    notify-send "Dictation" "Transcribing..." -t 2000 2>/dev/null || echo "Transcribing..."

    # Check if we got audio
    if [ ! -s "$AUDIO_FILE" ]; then
        notify-send "Dictation" "No recording captured" -t 2000 2>/dev/null || echo "No recording captured"
        rm -f "$AUDIO_FILE"
        exit 0
    fi

    # Verify file is large enough to be valid WAV (at least 1KB)
    FILE_SIZE=$(stat -c%s "$AUDIO_FILE" 2>/dev/null || stat -f%z "$AUDIO_FILE" 2>/dev/null || echo "0")
    if [ "$FILE_SIZE" -lt 1000 ]; then
        notify-send "Dictation" "Recording too short" -t 2000 2>/dev/null || echo "Recording too short"
        rm -f "$AUDIO_FILE"
        exit 0
    fi

    # Ensure SSH tunnel is up - check if port is accessible instead of relying on PID
    if ! nc -z 127.0.0.1 $REMOTE_PORT 2>/dev/null; then
        # Tunnel not accessible, try to create it
        # Clean up stale PID file
        rm -f "$TUNNEL_PID_FILE"

        # Start tunnel and capture its PID properly
        ssh -N -f -L 127.0.0.1:$REMOTE_PORT:127.0.0.1:$REMOTE_PORT $REMOTE_HOST 2>/dev/null || true

        # Wait a bit for tunnel to establish
        sleep 0.5

        # Verify tunnel is working
        if nc -z 127.0.0.1 $REMOTE_PORT 2>/dev/null; then
            # Find and save the actual SSH PID
            SSH_PID=$(pgrep -f "ssh.*127.0.0.1:$REMOTE_PORT.*$REMOTE_HOST" | head -1)
            if [ -n "$SSH_PID" ]; then
                echo "$SSH_PID" > "$TUNNEL_PID_FILE"
            fi
        else
            notify-send "Dictation" "Could not connect to Pi" -u critical -t 3000 2>/dev/null || echo "Could not connect to Pi"
            rm -f "$AUDIO_FILE"
            exit 1
        fi
    fi

    # Transcribe
    RESPONSE=$(curl -s -X POST \
        --data-binary "@${AUDIO_FILE}" \
        "$SERVICE_URL" 2>/dev/null)

    if [ $? -eq 0 ] && [ -n "$RESPONSE" ]; then
        # Send to Pi
        printf "%s" "$RESPONSE" | nc -q 1 127.0.0.1 $REMOTE_PORT 2>/dev/null

        # Show notification with transcribed text
        PREVIEW=$(echo "$RESPONSE" | head -c 50)
        if [ ${#RESPONSE} -gt 50 ]; then
            PREVIEW="${PREVIEW}..."
        fi
        notify-send "Dictation" "$PREVIEW" -t 3000 2>/dev/null || echo "Typed: $RESPONSE"
    else
        notify-send "Dictation" "Transcription failed" -u critical -t 3000 2>/dev/null || echo "Transcription failed"
    fi

    rm -f "$AUDIO_FILE"
else
    # Start recording
    notify-send "Dictation" "● Recording..." -t 2000 2>/dev/null || echo "● Recording..."

    # Start parecord (PulseAudio/PipeWire native) to use the system default device
    parecord --format=s16le --rate=$SAMPLE_RATE --channels=1 --latency-msec=500 "$AUDIO_FILE" 2>/dev/null &
    RECORD_PID=$!
    echo $RECORD_PID > "$PID_FILE"

    # Immediately check if parecord is still running
    sleep 0.2
    if ! ps -p $RECORD_PID > /dev/null 2>&1; then
        notify-send "Dictation" "Recording failed" -u critical -t 5000 2>/dev/null
        rm -f "$PID_FILE"
        exit 1
    fi
fi
