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

### Backward compatibility

[TODO: CLARIFY] Should `PTT_SERVICE_URL` (singular, the current variable) still be accepted as a single-entry fallback for users who haven't migrated their config?
**Options:**
- Support both: if `PTT_SERVICE_URLS` is not set, fall back to `PTT_SERVICE_URL`.
- Hard rename: require users to update their env file.
- Recommendation: support both, with a deprecation warning logged at startup.

### Validation at startup

Parse `PTT_SERVICE_URLS` into an ordered list.
Exit with a clear error message if the variable is missing or contains no valid URLs.
Invalid individual entries (malformed URLs) should log a warning and be skipped, not cause a hard exit.

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
- `is_any_healthy() -> bool` — convenience check.
- `start()` — starts the background thread and optionally blocks until first round completes (see startup behavior below).

### Startup behavior

[TODO: CLARIFY] Should `ptt.py` wait for the first health check round before declaring itself ready?

**Options:**
- **Block until ready**: `HealthMonitor.start()` does an initial synchronous check round, then starts the background loop.
  Benefit: "Service ready" is only printed once a server is known.
  Risk: if no server is reachable at boot, the service hangs (needs a timeout/give-up).
- **Non-blocking start**: the monitor starts immediately in the background with `_selected = None`.
  The button handler handles the "no server yet" case gracefully (same as "all servers unhealthy").
  Recommendation: non-blocking, as it is more robust and the Pi may boot before the network is ready.

[TODO: CLARIFY] If non-blocking: print a startup notice like `"Health monitor started; waiting for a server to become available"` rather than `"Service ready"` until a server is selected?

### Removal of inline health check

The current `check_service_health()` call in `start_recording()` is removed.
Its role is replaced by consulting `monitor.get_selected()`.

### Changes to `start_recording()`

1. Call `monitor.get_selected()`.
2. [TODO: CLARIFY] If no server is selected, should recording be skipped entirely, or should we record anyway and fail at transcription time?
   **Current behavior**: recording proceeds even if the health check fails (just beeps).
   **Proposed behavior**: if no server is healthy, skip recording, play error tones, and return early.
   This makes more sense given the goal is to avoid recording audio that cannot be transcribed.
3. Store the selected server URL at the moment the button is pressed.
   This avoids a race where the selected server changes between press and release.

### Changes to `stop_recording()`

1. Use the server URL captured at button-press time as the primary target.
2. If the transcription request fails (network error, timeout, non-200 response):
   - Try the remaining healthy servers in preference order (excluding the failed one).
   - [TODO: CLARIFY] Should this retry all remaining healthy servers (greedy), or only try once with the next one?
     Recommendation: greedy — try all healthy servers before giving up, since transcription is a one-shot high-value event.
3. If all attempts fail, log an error and play error tones (same UX as current behavior).

---

## File changes

### `pi/ptt.py`

- Add `HealthMonitor` class.
- Replace `SERVICE_URL` / `HEALTH_URL` / `TRANSCRIBE_URL` globals with parsed config + monitor instance.
- Remove `check_service_health()` function.
- Update `start_recording()` and `stop_recording()` as described above.

### `pi/ptt.env.example`

- Replace `PTT_SERVICE_URL` with `PTT_SERVICE_URLS` (with a comment explaining comma-separation and preference order).
- Add `PTT_HEALTH_CHECK_INTERVAL` and `PTT_HEALTH_CHECK_TIMEOUT` with comments.

### `CLAUDE.md` / `pi/README.md`

- Update env var documentation to reflect new names and format.

---

## Non-goals / out of scope

- No changes to `transcribe-whisper/` (the Go service).
- No changes to `type-ascii.py`.
- No persistent state across restarts (monitor re-learns server health from scratch on startup).
- No weighted load balancing — strict preference order only.

---

## Open questions (TODOs requiring clarification)

1. **Backward compat for `PTT_SERVICE_URL`** — rename or support both?
2. **Startup blocking** — block until first healthy server found, or start immediately with `_selected = None`?
3. **No-server behavior in `start_recording()`** — skip recording, or record and fail later?
4. **Transcription fallback granularity** — try all remaining healthy servers, or just one next?
