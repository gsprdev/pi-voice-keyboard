#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal HTTP/1.1 client on lwIP's raw API for streamed uploads: a POST
// whose body is sent with chunked transfer encoding while it's produced.
//
// Requests carry "Expect: 100-continue". The server answers "100 Continue"
// only once its handler starts reading the body, so http_ready() means a
// responsive server is accepting the upload, not just that TCP connected.
// The response is collected until the server closes the connection.

#define HTTP_MAX_CONNS 3

typedef struct http_conn http_conn_t;

typedef enum {
    HTTP_BUSY,   // connecting, streaming, or awaiting the response
    HTTP_DONE,   // final response received: see http_status_code / http_body
    HTTP_FAILED,
} http_state_t;

// Start a streamed POST. Returns NULL if all connections are in use.
// ready_timeout_ms bounds resolving, connecting and the 100 Continue;
// response_timeout_ms bounds the wait for the response once the body ends.
http_conn_t *http_open(const char *host, uint16_t port, const char *path, const char *content_type,
                       uint32_t ready_timeout_ms, uint32_t response_timeout_ms);

// The server has agreed to receive the body
bool http_ready(http_conn_t *conn);

// Queue body bytes. Returns false if the send buffer is full.
bool http_write(http_conn_t *conn, const void *data, size_t len);

// End the body; the response follows
void http_finish(http_conn_t *conn);

// Abort if still active, and free the connection
void http_close(http_conn_t *conn);

http_state_t http_state(http_conn_t *conn);
int http_status_code(http_conn_t *conn);
const char *http_body(http_conn_t *conn); // NUL-terminated, valid until http_close

// Send queued data, enforce timeouts. Call from the main loop.
void http_task(void);

#endif
