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

Requests must provide exactly 16kHz `audio/wav` content, up to 64 MB (about 30 minutes), or the response is 413.

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

## Corpus

To evaluate and troubleshoot transcription, the service can record every request into a corpus: a single SQLite file of audio, transcripts, and the circumstances that produced them.
The audio can then be listened to, labeled with what was really said, and replayed against other models, parameters, builds, and backends, with each result scored against the label.
It is off unless `CORPUS_DB` is set.
`mise.toml` sets it to `$XDG_DATA_HOME/pi-voice-keyboard/corpus.db` (`~/.local/share/...` by default), so local `mise run run` records by default, as do the commands below in a mise-activated shell.
Every worktree shares that one file, so runs from builds with different backends land side by side.
The installed service and copies of `dist/` don't use mise, so they don't record.

```sh
mise run run                                 # records into ~/.local/share/pi-voice-keyboard/corpus.db
CORPUS_DB=~/corpora/scratch.db mise run run  # a separate file, e.g. for experiments
CORPUS_DB= mise run run                      # no recording
```

- `CORPUS_DB` - Corpus file, relative to the working directory (created if missing, along with its directory).
  Unset or empty disables recording.
- `CORPUS_MAX_MB` - Storage budget (default: 1024, about 8.7 hours of audio).

For the systemd unit, add `Environment=CORPUS_DB=corpus.db` under `[Service]`.

Recordings are of whatever was said, so the file is created private (mode 0600).

### What is kept

- A **sample** is the audio exactly as received, with when and where it came from (client address, headers).
It never changes.
- A **process** is a `serve` or `replay` that loaded a model: its build, backend, host, device, and model.
It is recorded before loading, with `load_error` set to `loading` until the load finishes, so a load that fails or crashes (such as running out of device memory) leaves a trace.
- A **run** is one transcription of a sample by a process: the parameters asked for and actually used, the machine's state, timings, and the text with its segments and token probabilities.
A process's first transcription includes warm-up, such as Vulkan compiling its pipelines.
Replay runs are recorded as `incomplete` before they start, so a crash leaves a trace, and failed requests are kept with their error.
- A **label** is a human-verified reference transcript.
Labeling also sets **keep**, which exempts the sample from eviction and purging and can be cleared separately.

When the corpus is over budget, the oldest samples that are not kept are deleted, with their runs.

Keep is also how to flag a sample for later, without stopping to label it: `keep last` right after a transcription worth a closer look.
Later, `sweep` plays each kept sample that has no label yet, to label it, skip it, or unkeep it.
`purge` then deletes everything not kept and compacts the file, leaving just the samples worth having.

### Commands

Besides `serve`, which runs the service, the binary has subcommands for the corpus (run it with no arguments for usage).
Each takes `-db`, defaulting to `$CORPUS_DB`, and `<id>` may be `last`:

```sh
T=dist/transcribe-whisper
$T list                     # newest samples
$T label last               # play the last one, then accept its transcript, type the correct one, or /e to edit it
$T label 42 "What I said."  # or label without listening
$T play 42                  # listen, normalized: DC offset removed, quiet audio amplified
$T play 42 > 42.wav         # save it instead; -raw for the audio exactly as received
$T keep last                # flag it for later, protected from eviction; -clear to undo
$T sweep                    # label the kept samples that have no label yet, oldest first
$T purge                    # delete every sample that isn't kept, after asking, and compact
$T replay -labeled -model large-v3 -beam 5   # transcribe again, scoring against labels
$T replay -labeled -audio-ctx 768            # a shorter encoder window: faster, but too short loops
$T show 42                  # every run of a sample, with its configuration, conditions and score
$T report                   # every configuration, with its word error rate and speed by clip length
```

`replay` uses the same code path as the service, so its results are comparable, and records each run whatever the outcome.
Word error rate (WER) is word-level edit distance against the label, ignoring case and punctuation.

`report` groups runs by everything that makes them differ besides the audio and the machine's state: model, parameters, build, backend, build and run hosts, and device.
It shows how many labeled samples each WER covers, so compare configurations over the same samples, such as `replay -labeled` for each.
Speed is the median inference time per clip length, since Whisper encodes a fixed window and a short clip costs nearly as much as a long one.
It leaves out each process's first transcription, unless given `-first`.

`mise run build` stamps the backend from `vars.gpu` (default `cuda`) and the build host into the binary, so runs from each worktree's build can be told apart.
SQLite handles several processes at once on a local disk, but not reliably over a network filesystem.
Every build refuses a corpus from another schema version, and there is no migration: convert it by hand, or move it aside and start a new one.

For anything else, the file is plain SQLite, described by the schema in `corpus.go`:

```sh
sqlite3 corpus.db "SELECT writefile('42.wav', wav) FROM audio WHERE sample_id = 42"
```
