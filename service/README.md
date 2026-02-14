# Transcription Service

This transcription service converts audio to transcribed text to be typed.

## Contract

A transcription service for use with the voice keyboard has a simple but strict contract.

### /health

```http
GET /health
```

returns

```http
Content-Type: text/plain

OK
```

**Purpose:** A response of "OK" that the service is ready to process requests.
Any other response indicates otherwise.

### /transcribe

```http
POST /transcribe
Content-Type: audio/wav
```

```http
Content-Type: text/plain; charset=utf-8

The quick brown fox jumps over the lazy dog.
```

Requests must provide exactly 16kHz `audio/wav` content.

Responses interpret this audio to provide `text/plain; charset=utf-8`

Content-Length nor any other headers are required.

**Purpose:** Transcription of speech to text, for the purposes of voice-typing.

## Prerequisites

- NVIDIA GPU with CUDA support
- Ubuntu-based Linux (for the `nvidia-cuda-toolkit` multiverse package)

Drivers through `nvidia-cuda-toolkit` Ubuntu multiverse package are the only ones tested by the other, but other distros can be *theoretically* be used, through alternative drivers directly from NVIDIA.

## Installation

On the GPU-bearing Linux host:

1. `sudo apt install nvidia-cuda-toolkit`
2. `../build-whisper-cuda.sh`
3. Download a Whisper model using `../download-model.sh`
4. Compile using `./build.sh`
5. Edit `./transcription.service` for your paths, then install as a systemd service

This sytem will need to exist on the same local network as the Pi-based keyboard.
