# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pi Voice Keyboard is a speech-to-text system using Whisper with NVIDIA GPU acceleration. It enables hands-free typing by using a Raspberry Pi Zero 2 W as a USB HID keyboard gadget.

## Build Commands

### Whisper Library (required first)
```sh
./build-whisper-cuda.sh           # Build whisper.cpp with CUDA support
./download-model.sh medium.en     # Download a Whisper model
```

### Transcription Service
```sh
cd service
./build.sh                        # Build Go service (requires whisper.cpp built first)
./run.sh                          # Run with default port 8080
PORT=9000 ./run.sh                # Run on custom port
```

### Consumer (Raspberry Pi)
```sh
# On the Pi:
./gadget-install.sh               # One-time USB HID gadget setup
./kb-serve.sh                     # Start keyboard server (listens on port 1234)
```

## Environment Variables

Service configuration:
- `PORT` - HTTP port (default: 8080)
- `MODEL_PATH` - Path to Whisper model (default: `../speech-models/en_whisper_medium.ggml`)
- `MODEL_LANGUAGE` - Transcription language (default: `en`)

Producer configuration:
- `SERVICE_URL` - Transcription endpoint (default: `http://localhost:8080/transcribe`)
- `REMOTE_HOST` - Pi hostname (default: `pi02w.local`)
- `REMOTE_PORT` - Pi keyboard server port (default: `1234`)

## Architecture

```
Microphone → parecord → 16kHz WAV → POST /transcribe
→ Whisper (GPU) → text → SSH tunnel → netcat
→ kb-serve.sh → type-ascii.py → /dev/hidg0 → USB keyboard
```

**Three components:**

1. **service/** - Go HTTP server running on GPU host
   - `POST /transcribe` - Accepts 16kHz WAV, returns plain text
   - `GET /health` - Returns "OK" when ready
   - Loads Whisper model at startup, reuses for all requests

2. **producer/** - Bash scripts on the workstation
   - `dictation-toggle.sh` - Bind to keyboard shortcut for daily use
   - `dictation-service.sh` - Interactive Enter-to-start/stop for testing
   - Captures audio with `parecord`, sends to service, forwards result to Pi

3. **consumer/** - Python/Bash on Raspberry Pi Zero 2 W
   - `type-ascii.py` - Types text via USB HID gadget (`/dev/hidg0`)
   - `kb-serve.sh` - Bridges TCP port to the typing script
   - `gadget-*.sh` - USB HID gadget setup utilities

## Key Technical Details

- Audio format: 16kHz mono 16-bit PCM WAV (standard Whisper input)
- USB HID: Standard boot keyboard descriptor for maximum OS compatibility
- 8-byte HID reports: `[modifier, reserved, key1-6]`

## Dependencies

GPU host:
```sh
sudo apt install nvidia-cuda-toolkit pulseaudio-utils curl openssh-client netcat-openbsd libnotify-bin
```

Raspberry Pi: Python 3, configured with `otg_mode=1` in config.txt
