# Plan: Multi-Service Health Monitoring for Push-to-Talk

## Goal

Replace the single-server configuration with an ordered list of transcription servers.
A background health monitor continuously checks all servers and maintains a "currently selected" server — always the highest-preference one that is healthy.
Button-press event handling consults the monitor to decide which server to use, falling back through the preference list if needed.

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
Exit with a clear error message if the variable is missing or contains no valid URLs.
Invalid individual entries (malformed URLs) should log a warning and be skipped rather than cause a hard exit.

---

## Architecture

### New: `HealthMonitor` class (background thread)

Responsibilities:
- Maintain `servers: list[str]` — the parsed, ordered list of base URLs.
- Maintain `_healthy: set[str]` — the set of currently-healthy servers.
- Maintain `_selected: str | None` — the highest-preference URL that is currently healthy.
- Periodically re-check all servers and update `_healthy` and `_selected`.

Threading:
- Runs as a `threading.Thread` with `daemon=True` (dies automatically when the main process exits).
- `_healthy` and `_selected` protected by a `threading.Lock`.

Health check logic per round:
1. For each server (in any order — they're independent):
   - `GET {server}/health` with timeout = `PTT_HEALTH_CHECK_TIMEOUT` ms.
   - Server is healthy if HTTP 200 and body is `"OK"`.
2. After all checks complete, update `_healthy`.
3. Re-derive `_selected`: first server in preference order that is in `_healthy`.

Public API:
- `get_selected() -> str | None` — returns the current selected base URL (thread-safe).
- `get_healthy_in_order() -> list[str]` — returns all currently healthy base URLs in preference order (thread-safe).
  Used by `stop_recording()` to build the fallback list.
- `start()` — starts the background daemon thread and returns immediately.

### Startup behavior

The monitor starts immediately in the background with `_selected = None`.
Service startup does not block waiting for a healthy server — unavailability at boot is treated identically to a server going down later.
Startup log: `"Health monitor started (checking {N} servers every {interval}s)"`.

### Removal of inline health check

The current `check_service_health()` function and its call in `start_recording()` are deleted.
The monitor is the sole arbiter of server availability.

### Error signaling conventions

All error/alert patterns use a 100 ms gap between each blink/beep step.

| Condition | Pattern |
|---|---|
| No server available at button press | 3 pulses (blink + beep each) |
| All transcription attempts failed | 2 pulses (blink + beep each) |
| Recording start / normal acknowledgement | 1 pulse (existing behavior, unchanged) |

### Changes to `start_recording()`

1. Call `monitor.get_selected()`.
2. If no server is selected (monitor has not yet found a healthy server, or all are down):
   - Play the 3-pulse no-server error pattern (with 100 ms gaps).
   - Return early — do **not** start recording.
   This fixes the existing bug where recording proceeded past a failed health check.
3. Store the selected server URL in a module-level variable (e.g., `active_server`).
   Capturing it at press time avoids a race where the selected server changes between press and release.
4. Continue with `arecord` startup and the single acknowledgement pulse as before.

### Changes to `stop_recording()`

1. Build a candidate list: `[active_server] + [s for s in monitor.get_healthy_in_order() if s != active_server]`.
   This puts the server chosen at press-time first, then any other currently-healthy servers in preference order.
2. Attempt transcription against each candidate in order until one succeeds.
3. If all candidates fail, log an error and play the 2-pulse all-failed error pattern.
4. Clear `active_server` after the attempt (success or total failure).

---

## File changes

### `pi/ptt.py`

- Add `HealthMonitor` class (see above).
- Replace `SERVICE_URL` / `HEALTH_URL` / `TRANSCRIBE_URL` globals with a parsed `servers` list and a `monitor` instance.
- Add `active_server: str | None = None` module-level variable to capture the server at press time.
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
