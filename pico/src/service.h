#ifndef SERVICE_H
#define SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Transcription sessions. Starting a session is the readiness check: servers
// are tried in priority order until one accepts the upload. Several sessions
// can be in flight, e.g. recording while earlier ones are still transcribing.

typedef struct session session_t;

typedef enum {
    SESSION_CONNECTING, // finding a server that accepts the upload
    SESSION_RECORDING,  // server ready: stream audio with service_send
    SESSION_PROCESSING, // recording ended, waiting for the transcription
    SESSION_DONE,       // transcription available from service_text
    SESSION_FAILED,
} session_state_t;

void service_init(void);

// Advance sessions: fail through servers, collect results. Call from the main loop.
void service_task(void);

// Log the configured servers
void service_report(void);

// Start a session. Returns NULL if there's no network or no free session.
session_t *service_start(void);

session_state_t service_state(session_t *s);

// Queue 16kHz mono samples. Returns false if the network can't keep up.
bool service_send(session_t *s, const int16_t *samples, size_t count);

// End the recording; the transcription follows
void service_end(session_t *s);

// The transcription, once SESSION_DONE. Valid until service_release.
const char *service_text(session_t *s);

// Abandon or finish with a session
void service_release(session_t *s);

#endif
