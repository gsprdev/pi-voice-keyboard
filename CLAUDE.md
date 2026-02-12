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

whisper.cpp is a git submodule. The build script handles `git submodule update --init` automatically.

### Transcription Service
```sh
cd service
./build.sh                        # Build Go service (requires whisper.cpp built first)
./run.sh                          # Run with default port 8080
PORT=9000 ./run.sh                # Run on custom port
```

The service is a Go module (Go 1.25+) using CGO to link against whisper.cpp. `build.sh` sets the required `CGO_CFLAGS`, `CGO_LDFLAGS`, and `CGO_CXXFLAGS` pointing to the whisper.cpp build output. `run.sh` sets `LD_LIBRARY_PATH` at runtime.

### Consumer Web Server (cross-compiled for Pi)
```sh
cd consumer
./build-web-server.sh             # Cross-compile Go web server for linux/arm64
```

Output goes to `consumer/build/`. The web server requires TLS certs (`cert.pem`/`key.pem`) and auto-generates an API key in `web-server-config.txt` on first run.

### Consumer (Raspberry Pi)
```sh
# On the Pi:
./gadget-install.sh               # One-time: installs scripts to /usr/sbin and enables systemd service
./kb-serve.sh                     # Start keyboard server (listens on port 1234)
```

### Testing the Service
```sh
cd service
./test-client.sh                  # Records 5s audio with arecord, posts to /transcribe
```

There are no automated tests or linting configured in this project.

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
   - `POST /transcribe` - Accepts 16kHz WAV (`Content-Type: audio/wav`), returns plain text
   - `GET /health` - Returns "OK" when ready
   - Loads Whisper model once at startup, reuses for all requests

2. **producer/** - Bash scripts on the workstation
   - `dictation-toggle.sh` - Toggle recording on/off, bind to a keyboard shortcut for daily use
   - `dictation-service.sh` - Interactive Enter-to-start/stop loop for testing
   - Captures audio with `parecord`, sends to service, forwards result to Pi

3. **consumer/** - Python/Bash/Go on Raspberry Pi Zero 2 W
   - `type-ascii.py` - Converts text to USB HID keyboard reports via `/dev/hidg0`; listens on a Unix socket (`/tmp/kb.sock`)
   - `kb-serve.sh` - Bridges TCP port 1234 to the Unix socket using `socat`
   - `gadget-*.sh` - USB HID gadget setup/teardown via Linux configfs
   - `web-server.go` - HTTPS server (port 8081) with embedded web UI for browser-based dictation; authenticates via API key, forwards audio to the transcription service, and types via HID socket

## Key Technical Details

- Audio format: 16kHz mono 16-bit PCM WAV (standard Whisper input)
- USB HID: Standard boot keyboard descriptor, 8-byte reports `[modifier, reserved, key1-6]`
- The dictation toggle stores state in `/tmp/dictation-toggle/` (PID files and temp audio)

## Dependencies

GPU host:
```sh
sudo apt install nvidia-cuda-toolkit pulseaudio-utils curl openssh-client netcat-openbsd libnotify-bin
```

Raspberry Pi: Python 3, socat, configured with `otg_mode=1` in `/boot/config.txt`
