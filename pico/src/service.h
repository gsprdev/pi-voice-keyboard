#ifndef SERVICE_H
#define SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One transcription at a time. Starting it is the readiness check: servers
// are tried in priority order until one accepts the upload.

typedef enum {
    SESSION_IDLE,
    SESSION_CONNECTING, // finding a server that accepts the upload
    SESSION_RECORDING,  // server ready: stream audio with service_send
    SESSION_PROCESSING, // recording ended, waiting for the transcription
    SESSION_DONE,       // transcription available from service_text
    SESSION_FAILED,
} session_state_t;

void service_init(void);

// Fail through servers, collect the result. Call from the main loop.
void service_task(void);

// Log the configured servers
void service_report(void);

// Start a transcription. Returns false if there's no network or one is
// already in progress.
bool service_start(void);

session_state_t service_state(void);

// Queue 16kHz mono samples. Returns false if the network can't keep up.
bool service_send(const int16_t *samples, size_t count);

// End the recording; the transcription follows
void service_end(void);

// The transcription, once SESSION_DONE. Valid until service_release.
const char *service_text(void);

// Abandon or finish with the transcription, returning to SESSION_IDLE
void service_release(void);

#endif
