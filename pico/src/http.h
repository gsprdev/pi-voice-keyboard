#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal HTTP/1.1 client on lwIP's raw API, one request at a time.
// Request bodies are streamed with chunked transfer encoding; the response
// is collected until the server closes the connection.

typedef enum {
    HTTP_IDLE,
    HTTP_BUSY,   // resolving, connecting, sending, or awaiting the response
    HTTP_DONE,   // response received: see http_status_code / http_body
    HTTP_FAILED,
} http_state_t;

// Start a request. With has_body, write it with http_write and end it with
// http_finish; without, the request is sent as soon as it connects.
// The timeouts bound resolving + connecting, and waiting for the response
// once the request is sent. Streaming the body itself has no time limit.
bool http_begin(const char *host, uint16_t port, const char *method, const char *path,
                const char *content_type, bool has_body,
                uint32_t connect_timeout_ms, uint32_t response_timeout_ms);

// Queue body bytes. Returns false if the send buffer is full.
bool http_write(const void *data, size_t len);

// End the request body
void http_finish(void);

// Abort any request in progress and return to idle
void http_reset(void);

// Send queued data, enforce timeouts. Call from the main loop.
void http_task(void);

http_state_t http_state(void);
int http_status_code(void);
const char *http_body(void); // NUL-terminated, valid until the next http_begin

#endif
