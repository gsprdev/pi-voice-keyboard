#ifndef SERVICE_H
#define SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The transcription service: background health checks, and one streamed
// transcription at a time.

typedef enum {
    TRANSCRIBE_PENDING,
    TRANSCRIBE_OK,
    TRANSCRIBE_FAILED,
} transcribe_result_t;

void service_init(void);

// Health checks. Call from the main loop.
void service_task(void);

// Wi-Fi is up and the last health check passed
bool service_ready(void);

// Log the service's state
void service_report(void);

// Start streaming a recording to /transcribe
bool service_begin(void);

// Queue 16kHz mono samples. Returns false if the network can't keep up.
bool service_send(const int16_t *samples, size_t count);

// End the recording; the transcription follows
void service_end(void);

// Abandon the transcription in progress
void service_cancel(void);

// Poll for the outcome. On TRANSCRIBE_OK, *text is the transcription, valid
// until service_task next runs. Any final result ends the transcription.
transcribe_result_t service_result(const char **text);

#endif
