# Plan: Multi-Service Health Monitoring for Push-to-Talk

## Goal

Replace the single-server configuration with an ordered list of transcription servers.
Background health monitors independently track each server's availability.
Button-press event handling queries for the best available server at the moment it's needed, falling back through the preference list as necessary.

---

## Configuration Changes

### New environment variables

| Variable | Default | Description |
|---|---|---|
| `PTT_SERVICE_URLS` | *(required)* | Comma-separated list of base URLs in preference order |
| `PTT_HEALTH_CHECK_INTERVAL` | `10` | Seconds between health check rounds |
| `PTT_HEALTH_CHECK_TIMEOUT` | `200` | Milliseconds per health check request |

Example `/etc/default/ptt`:

```sh
PTT_SERVICE_URLS=http://gpu-host.local:8080,http://fallback-host.local:8080,http://localhost:8080
PTT_HEALTH_CHECK_INTERVAL=10
PTT_HEALTH_CHECK_TIMEOUT=200
```

### No backward compatibility

`PTT_SERVICE_URL` (singular) is removed with no migration shim.
The env file is under our control and will be updated directly.

### Validation at startup

Parse `PTT_SERVICE_URLS` into an ordered list.
Exit with an error if the variable is missing or if **any** entry is a malformed URL.
All URLs must be well-formed for the service to start.

---

## Architecture

### Data model: `Server`

Rather than tracking health status through separate dictionaries or sets, each server is represented as a single object holding both its configuration and its current health state:

```python
class Server:
    url: str        # base URL, no trailing slash
    healthy: bool   # last known health state (default False)
```

The servers are stored in a single list, ordered by user-specified preference.
This list is the sole source of truth.
To find the best available server at any point, iterate the list and return the first one where `healthy is True`.

### Health monitoring: one thread per server

Each server gets its own dedicated daemon thread that loops independently:
1. `GET {server.url}/health` with timeout = `PTT_HEALTH_CHECK_TIMEOUT` ms.
2. Set `server.healthy = True` if HTTP 200 and body is `"OK"`, otherwise `server.healthy = False`.
3. Sleep for `PTT_HEALTH_CHECK_INTERVAL` seconds.
4. Repeat.

Benefits:
- A slow or unresponsive server cannot delay recognition of other healthy servers.
- No cross-server coordination needed — each thread manages only its own `Server.healthy` flag.
- The number of threads is fixed at startup (one per configured URL).

Thread safety:
- Each thread writes only to its own `Server.healthy` (a single boolean assignment, which is atomic in CPython due to the GIL).
- The main thread reads `server.healthy` — no lock needed for a single boolean read/write under the GIL.

### Startup behavior

The monitor threads start immediately in the background with all servers' `healthy` set to `False`.
Service startup does not block waiting for a healthy server — unavailability at boot is treated identically to a server going down later.
Startup log: `"Health monitor started ({N} servers, checking every {interval}s)"`.

### Removal of inline health check

The current `check_service_health()` function and its call in `start_recording()` are deleted.
The monitor threads are the sole arbiters of server availability.

### Accessor: `get_healthy_servers() -> list[Server]`

A module-level function (or method on a manager object — implementation detail) that iterates the server list in preference order and returns those where `healthy is True`.
Used by both `start_recording()` (to gate recording) and `stop_recording()` (to build the fallback list).

### Error signaling conventions

All error/alert patterns use a 100 ms gap between each blink/beep step.

| Condition | Pattern |
|---|---|
| No server available at button press | 3 pulses (blink + beep each) |
| All transcription attempts failed | 2 pulses (blink + beep each) |
| Recording start / normal acknowledgement | 1 pulse (existing behavior, unchanged) |

### Changes to `start_recording()`

1. Call `get_healthy_servers()`.
2. If the list is empty (no healthy server):
   - Play the 3-pulse no-server error pattern (with 100 ms gaps between steps).
   - Return early — do **not** start recording.
   This fixes the existing bug where recording proceeded past a failed health check.
3. Otherwise, proceed with `arecord` startup and the single acknowledgement pulse as before.

No server selection is stored at press time.
The server to use for transcription is determined at release time by querying `get_healthy_servers()` again, which gives the freshest view of availability.
This avoids module-level state coupling the press and release events.

### Changes to `stop_recording()`

1. Stop `arecord` and read the audio data (as before).
2. Call `get_healthy_servers()` to get the current ordered list of candidates.
3. Attempt transcription against each candidate in order until one succeeds.
4. If all candidates fail (or the list is empty), log an error and play the 2-pulse all-failed error pattern.

---

## File changes

### `pi/ptt.py`

- Add `Server` class.
- Add per-server health monitor thread spawning.
- Add `get_healthy_servers()` accessor.
- Replace `SERVICE_URL` / `HEALTH_URL` / `TRANSCRIBE_URL` globals with parsed server list.
- Remove `check_service_health()`.
- Rewrite `start_recording()` and `stop_recording()` as described above.
- Add 100 ms inter-step gaps to all error pulse sequences.

### `pi/ptt.env.example`

- Replace `PTT_SERVICE_URL` with `PTT_SERVICE_URLS` (with a comment explaining comma-separation and preference order).
- Add `PTT_HEALTH_CHECK_INTERVAL` and `PTT_HEALTH_CHECK_TIMEOUT` with comments.

### `CLAUDE.md` and `pi/README.md`

- Update env var documentation to reflect the new names and multi-URL format.

---

## Non-goals / out of scope

- No changes to `transcribe-whisper/` (the Go service).
- No changes to `type-ascii.py`.
- No persistent state across restarts (monitor re-learns server health from scratch on startup).
- No weighted load balancing — strict preference order only.
