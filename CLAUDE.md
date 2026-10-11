# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pi Voice Keyboard is a speech-to-text system using Whisper with NVIDIA GPU acceleration.
It enables hands-free typing by using a Raspberry Pi Zero 2 W as a USB HID keyboard gadget.

## Build Commands

### Whisper Library (required first)
```sh
mise run whisper                  # Build whisper.cpp with CUDA support (incremental)
mise run model                    # Download the Whisper model (MODEL=large-v3 for others)
```

whisper.cpp is a git submodule.
`mise run whisper` runs `git submodule update --init` and the CMake configure automatically.

### Transcription Service
```sh
mise run build                    # Build Go service (requires whisper.cpp built first)
mise run run                      # Build and run with default port 8080
PORT=9000 mise run run            # Run on custom port
mise run test                     # Run Go tests
```

The service is a Go module (Go 1.25+) using CGO to link against whisper.cpp.
`mise run build` installs the whisper.cpp libraries into `dist/lib` and builds `dist/transcribe-whisper` with an `$ORIGIN/lib` rpath.
`dist/` is self-contained, so the systemd unit runs `dist/transcribe-whisper` directly, without mise or the submodule.
`mise.toml` sets `CGO_CFLAGS`, `CGO_LDFLAGS`, and `LD_LIBRARY_PATH` (for `go test`).

### Keyboard (Raspberry Pi)
```sh
# On the Pi:
cd pi
sudo ./gadget-install.sh          # One-time: installs scripts/services to system
sudo systemctl enable --now type-ascii.service ptt.service
```

### Keyboard (Raspberry Pi Pico 2 W, in progress)
```sh
cp pico/src/secrets.h.example pico/src/secrets.h   # Wi-Fi + service URL (git-ignored), required for Pico 2 W
mise run pico:build               # Build firmware: pico/build/voice-keyboard.uf2
PICO_BOARD=pico2 mise run pico:build      # Plain Pico 2 (no Wi-Fi): pico/build-pico2/
MIC_TIMING=standard mise run pico:build   # Standard I2S mic instead of SPH0645
mise run pico:flash               # Build, reboot into BOOTSEL, copy to the RP2350 drive
mise run pico:tail                # Follow the console log
mise run pico:capture             # Record from the audio serial port and transcribe (stage 2 mic check)
```

C firmware using pico-sdk (git submodule at `pico/pico-sdk`) and TinyUSB.
`mise run pico:build` initializes the submodule and the SDK libraries it needs.
See `pico/PLAN.md` for design decisions, pin assignments, and the staged roadmap.

### Testing the Service
```sh
cd transcribe-whisper
./test-client.sh                  # Records 5s audio with arecord, posts to /transcribe
```

`transcribe-whisper/clean_test.go` covers transcription cleanup.
Run it with `mise run test`.
There is no linting configured.

## Environment Variables

Service configuration (GPU host):
- `PORT` - HTTP port (default: 8080)
- `MODEL` - Upstream Whisper model name, loaded from `speech-models/ggml-<MODEL>.bin` relative to the working directory (default: `medium.en`)
- `MODEL_LANGUAGE` - Transcription language (default: `en`)

Pico configuration is compiled in from `pico/src/secrets.h`: `WIFI_SSID`, `WIFI_PASSWORD`, `SERVICE_URLS` (comma-separated, priority order).

Push-to-Talk configuration (`/etc/default/ptt` on Pi):
- `PTT_SERVICE_URLS` - Comma-separated list of transcription service URLs in priority order (required, e.g., `http://gpu1.local:8080,http://gpu2.local:8080`)
- `PTT_HEALTH_INTERVAL` - Background health check interval in seconds (default: `10`)
- `PTT_HEALTH_TIMEOUT` - Per-server health check timeout in milliseconds (default: `200`)

Service will use `/health` for startup checks and `/transcribe` for audio processing.

## Architecture

```
Button press (GPIO) → arecord → 16kHz WAV → HTTP POST /transcribe
→ Whisper (GPU) → text → local Unix socket
→ type-ascii.py → /dev/hidg0 → USB keyboard
```

**Components:**

1. **transcribe-whisper/** - Go HTTP server running on GPU host
   - `POST /transcribe` - Accepts 16kHz WAV (`Content-Type: audio/wav`), returns plain text
   - `GET /health` - Returns "OK" when ready
   - Loads Whisper model once at startup, reuses for all requests

2. **pi/** - Python services on Raspberry Pi Zero 2 W
   - `ptt.py` - Push-to-talk GPIO handler: records audio, sends to service, receives text
   - `type-ascii.py` - Converts text to USB HID keyboard reports via `/dev/hidg0`
   - Unix socket server: binds `/run/kb-serve/kb.sock` (managed via `RuntimeDirectory=`)
   - `gadget-*.sh` - USB HID gadget setup/teardown via Linux configfs

3. **pico/** - Replacement firmware for the Pi, on a Pico 2 W (in progress)
   - Composite USB device: HID boot keyboard + CDC console (logs) + CDC audio (raw PCM)
   - `src/typer.c` - Port of `type-ascii.py`, driven from the main loop
   - `src/mic.c`, `src/mic_i2s.pio` - I2S mic via PIO + DMA at 32kHz, decimated to 16kHz mono PCM
   - `src/ptt.c` - Push-to-talk state machine (port of `ptt.py`), `src/feedback.c` LEDs/buzzer
   - `src/net.c`, `src/http.c`, `src/service.c` - Wi-Fi, minimal lwIP HTTP client, transcription sessions: chunked streaming to `/transcribe`, readiness via `Expect: 100-continue`, fail-through across servers
   - `src/pins.h` - GPIO assignments

## Key Technical Details

- Audio format: 16kHz mono 16-bit PCM WAV (standard Whisper input)
- USB HID: Standard boot keyboard descriptor, 8-byte reports `[modifier, reserved, key1-6]`
- Socket path: `/run/kb-serve/kb.sock` (created by `type-ascii.py`, directory managed by systemd `RuntimeDirectory=`)
- GPIO pins (Pi): Button on GPIO 24, LEDs on GPIO 17/22, buzzer on GPIO 27
- GPIO pins (Pico): see `pico/src/pins.h`; GPIO 23-25 and 29 are reserved on the Pico 2 W

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
