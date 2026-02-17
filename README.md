# Raspberry Pi Voice Keyboard

A speech-to-text transcription system using Whisper with NVIDIA GPU acceleration, designed to reduce hand strain from extended typing.
Uses a Raspberry Pi Zero 2 W as a USB HID keyboard gadget with physical push-to-talk button.

## Features

- **GPU-accelerated transcription** - 0.02x-0.19x realtime with medium.en Whisper model using NVIDIA CUDA (essentially instantaneous)
- **Raspberry Pi integration** - Types transcribed text as a USB keyboard, no-fuss compatibility
- **Physical push-to-talk** - Hardware button on Pi for recording control
- **Production ready** - In daily use by author

## Prerequisites

- NVIDIA GPU with CUDA support
- Raspberry Pi Zero 2 W with GPIO components (button, LEDs, buzzer)
- Micro-USB OTG cable
- Ubuntu-based Linux (for the `nvidia-cuda-toolkit` multiverse package)

Non-Ubuntu systems can theoretically be used, through alternative drivers directly from NVIDIA.

## Quick Start

### GPU Host Setup

```bash
sudo apt install nvidia-cuda-toolkit
./build-whisper-cuda.sh
./download-model.sh medium.en
cd transcribe-whisper && ./build.sh && ./run.sh
```

### Raspberry Pi Setup

```bash
# Configure /boot/config.txt with dtoverlay=dwc2
# Wire GPIO button to pin 24, LEDs to pins 17/22, buzzer to pin 27
cd pi
sudo ./gadget-install.sh
sudo systemctl enable --now kb-serve.socket type-ascii.service ptt.service
```

See [CLAUDE.md](CLAUDE.md) for detailed instructions.

## Architecture

- **transcribe-whisper/** - Go HTTP service for GPU-accelerated transcription
- **pi/** - Raspberry Pi services for USB keyboard emulation and push-to-talk
- **whisper.cpp/** - Whisper inference library (git submodule)

### Push-to-Talk Flow

```
Button press (GPIO) → arecord → HTTP POST → Whisper (GPU) → text
→ Unix socket → type-ascii → /dev/hidg0 → USB keyboard
```

## Documentation

- [CLAUDE.md](CLAUDE.md) - Complete setup and usage guide
- [pi/README.md](pi/README.md) - Raspberry Pi setup documentation

## License

See [LICENSE](LICENSE)
