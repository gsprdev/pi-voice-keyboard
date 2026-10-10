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

The response is meant to be typed as-is, so the service cleans it first rather than leaving that to the keyboard:

- An empty body means nothing was said, and nothing should be typed.
This is returned when the transcription is only Whisper's "nothing here" sentinels: `[BLANK_AUDIO]`, `[silence]`, or `(inaudible)`.
These words are kept when they appear within real speech.
- Background noise annotations are removed wherever they appear: `(beep)` (the Pi's record-start buzzer), `(click)`/`(clicks)`, `(typing)`, `[Music]`/`(music playing)`.
- Pause filler (`...`, `…`) is removed.
- Whitespace is collapsed and trimmed, and spaces left before punctuation are dropped.

## Prerequisites

- NVIDIA GPU with CUDA support
- Ubuntu-based Linux (for the `nvidia-cuda-toolkit` multiverse package)

Drivers through `nvidia-cuda-toolkit` Ubuntu multiverse package are the only ones tested by the other, but other distros can be *theoretically* be used, through alternative drivers directly from NVIDIA.

## Installation

On the GPU-bearing Linux host:

1. `sudo apt install nvidia-cuda-toolkit`
2. `mise run whisper`
3. Download a Whisper model using `mise run model` (`MODEL=<name>` for others)
4. Compile using `mise run build`
5. Edit `./transcription.service` for your paths, then install as a systemd service

This sytem will need to exist on the same local network as the Pi-based keyboard.

## Debug capture

To investigate mistranscriptions, the service can keep the most recent requests on disk.
It is off by default; set `DEBUG_CAPTURE_DIR` to turn it on:

```sh
DEBUG_CAPTURE_DIR=~/transcribe-captures DEBUG_CAPTURE_KEEP=50 ./run.sh
```

- `DEBUG_CAPTURE_DIR` - Directory for captures (created if missing). Unset disables capture.
- `DEBUG_CAPTURE_KEEP` - Number of most recent captures to keep (default: 50). Older ones are deleted after each request.

Each request gets its own directory, named so they sort by time:

```
<DEBUG_CAPTURE_DIR>/20261010-045829.020-1791607109020791115/
  audio.wav        exactly as uploaded
  transcript.txt   text returned to the client (successful requests only)
  meta.json        client, headers, WAV format, model, language, timings, segments with token probabilities, status and response
```

Failed requests (for example, unreadable WAV) are captured too, without `transcript.txt`.
Pruning only touches directories with that naming pattern, so other files in the directory are left alone.

Captures are recordings of whatever was said, so keep the directory private (it is created with mode 0700) and turn capture off when done.

To try a capture against another model or parameters, use whisper.cpp's CLI directly:

```sh
../whisper.cpp/build/bin/whisper-cli -m ../speech-models/en_whisper_large.ggml -f <capture>/audio.wav
```

or replay it against a running service:

```sh
curl -H 'Content-Type: audio/wav' --data-binary @<capture>/audio.wav http://localhost:8080/transcribe
```
