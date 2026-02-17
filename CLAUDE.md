# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pi Voice Keyboard is a speech-to-text system using Whisper with NVIDIA GPU acceleration.
It enables hands-free typing by using a Raspberry Pi Zero 2 W as a USB HID keyboard gadget.

## Build Commands

### Whisper Library (required first)
```sh
./build-whisper-cuda.sh           # Build whisper.cpp with CUDA support
./download-model.sh medium.en     # Download a Whisper model
```

whisper.cpp is a git submodule.
The build script handles `git submodule update --init` automatically.

### Transcription Service
```sh
cd transcribe-whisper
./build.sh                        # Build Go service (requires whisper.cpp built first)
./run.sh                          # Run with default port 8080
PORT=9000 ./run.sh                # Run on custom port
```

The service is a Go module (Go 1.25+) using CGO to link against whisper.cpp.
`build.sh` sets the required `CGO_CFLAGS`, `CGO_LDFLAGS`, and `CGO_CXXFLAGS` pointing to the whisper.cpp build output.
`run.sh` sets `LD_LIBRARY_PATH` at runtime.

### Keyboard (Raspberry Pi)
```sh
# On the Pi:
cd pi
sudo ./gadget-install.sh          # One-time: installs scripts/services to system
sudo systemctl enable --now type-ascii.service ptt.service
```

### Testing the Service
```sh
cd transcribe-whisper
./test-client.sh                  # Records 5s audio with arecord, posts to /transcribe
```

There are no automated tests or linting configured in this project.

## Environment Variables

Service configuration (GPU host):
- `PORT` - HTTP port (default: 8080)
- `MODEL_PATH` - Path to Whisper model (default: `../speech-models/en_whisper_medium.ggml`)
- `MODEL_LANGUAGE` - Transcription language (default: `en`)

Push-to-Talk configuration (`/etc/default/ptt` on Pi):
- `PTT_SERVICE_URL` - Base URL to transcription service (required, e.g., `http://gpu-host.local:8080`)

Service will use `/health` for startup checks and `/transcribe` for audio processing.

## Architecture

```
Button press (GPIO) → arecord → 16kHz WAV → HTTP POST /transcribe
→ Whisper (GPU) → text → local Unix socket
→ type-ascii.py → /dev/hidg0 → USB keyboard
```

**Two components:**

1. **transcribe-whisper/** - Go HTTP server running on GPU host
   - `POST /transcribe` - Accepts 16kHz WAV (`Content-Type: audio/wav`), returns plain text
   - `GET /health` - Returns "OK" when ready
   - Loads Whisper model once at startup, reuses for all requests

2. **pi/** - Python services on Raspberry Pi Zero 2 W
   - `ptt.py` - Push-to-talk GPIO handler: records audio, sends to service, receives text
   - `type-ascii.py` - Converts text to USB HID keyboard reports via `/dev/hidg0`
   - Unix socket server: binds `/run/kb-serve/kb.sock` (managed via `RuntimeDirectory=`)
   - `gadget-*.sh` - USB HID gadget setup/teardown via Linux configfs

## Key Technical Details

- Audio format: 16kHz mono 16-bit PCM WAV (standard Whisper input)
- USB HID: Standard boot keyboard descriptor, 8-byte reports `[modifier, reserved, key1-6]`
- Socket path: `/run/kb-serve/kb.sock` (created by `type-ascii.py`, directory managed by systemd `RuntimeDirectory=`)
- GPIO pins: Button on GPIO 24, LEDs on GPIO 17/22, buzzer on GPIO 27

## Dependencies

GPU host:
```sh
sudo apt install nvidia-cuda-toolkit
```

Raspberry Pi:
```sh
sudo apt install python3 python3-gpiozero alsa-utils
# Configure USB OTG: add dtoverlay=dwc2 to /boot/config.txt
```
