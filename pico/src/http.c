// Minimal HTTP/1.1 client on lwIP's raw TCP API (lwIP is polled: every
// callback runs from net_task in the main loop, so no locking is needed).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "net.h"
#include "pico/time.h"

// Body bytes waiting for the TCP send buffer: ~2s of 16kHz audio, enough to
// ride out Wi-Fi hiccups without buffering whole recordings
#define TX_RING_SIZE   65536 // power of two
#define RESP_MAX       16384
#define CHUNK_MIN      512   // batch small writes into larger chunks
#define CHUNK_MAX      2048
#define CHUNK_OVERHEAD 12    // "XXXX\r\n" + "\r\n" with room to spare

typedef enum {
    STEP_FREE,
    STEP_RESOLVING,
    STEP_CONNECTING,
    STEP_CONTINUE,  // request headers sent, waiting for 100 Continue
    STEP_STREAMING,
    STEP_AWAITING,  // body finished, waiting for the response
    STEP_DONE,
    STEP_FAILED,
} step_t;

typedef struct {
    step_t step;
    struct tcp_pcb *pcb;
    ip_addr_t addr;
    char host[64];
    uint16_t port;
    uint32_t response_timeout_ms;
    absolute_time_t deadline;

    char header[320];
    bool header_sent;
    bool finishing;

    uint8_t tx_ring[TX_RING_SIZE];
    uint32_t tx_head, tx_tail;

    char resp[RESP_MAX + 1];
    size_t resp_len;
    int status_code;
    const char *body;
} http_conn_t;

static http_conn_t conn;

static void detach(http_conn_t *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        tcp_abort(c->pcb);
        c->pcb = NULL;
    }
}

static void fail(http_conn_t *c, const char *why) {
    printf("HTTP %s:%u: %s\n", c->host, c->port, why);
    detach(c);
    c->step = STEP_FAILED;
}

// Length of a complete interim (1xx) response at the start of resp, or 0
static size_t interim_response_len(http_conn_t *c) {
    c->resp[c->resp_len] = '\0';
    if (c->resp_len < 12 || strncmp(c->resp, "HTTP/1.", 7) != 0 || c->resp[9] != '1') {
        return 0;
    }
    char *end = strstr(c->resp, "\r\n\r\n");
    return end ? (size_t)(end + 4 - c->resp) : 0;
}

static void drop_interim_responses(http_conn_t *c) {
    size_t len;
    while ((len = interim_response_len(c)) > 0) {
        memmove(c->resp, c->resp + len, c->resp_len - len);
        c->resp_len -= len;
        if (c->step == STEP_CONTINUE) {
            c->step = STEP_STREAMING;
        }
    }
}

// Split the final response into status code and body, decoding a chunked body in place
static bool parse_response(http_conn_t *c) {
    drop_interim_responses(c);
    char *resp = c->resp;
    resp[c->resp_len] = '\0';
    if (strncmp(resp, "HTTP/1.", 7) != 0 || c->resp_len < 12) {
        return false;
    }
    c->status_code = atoi(resp + 9);

    char *headers_end = strstr(resp, "\r\n\r\n");
    if (!headers_end) {
        return false;
    }
    *headers_end = '\0';
    char *start = headers_end + 4;

    bool chunked = false;
    for (char *line = strstr(resp, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "transfer-encoding:", 18) == 0 && strstr(line + 20, "chunked")) {
            chunked = true;
        }
    }

    if (chunked) {
        char *src = start, *dst = start, *end = resp + c->resp_len;
        while (true) {
            char *after;
            unsigned long size = strtoul(src, &after, 16);
            char *data = strstr(after, "\r\n");
            if (after == src || !data || data + 2 + size > end) {
                return false;
            }
            data += 2;
            if (size == 0) {
                break;
            }
            memmove(dst, data, size);
            dst += size;
            src = data + size + 2;
        }
        *dst = '\0';
    }

    c->body = start;
    return true;
}

static void complete(http_conn_t *c) {
    if (parse_response(c)) {
        c->step = STEP_DONE;
    } else {
        printf("HTTP %s:%u: malformed response\n", c->host, c->port);
        c->step = STEP_FAILED;
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    http_conn_t *c = arg;
    if (!p) {
        // Server closed the connection: the response is complete
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        c->pcb = NULL;
        err_t result = ERR_OK;
        if (tcp_close(tpcb) != ERR_OK) {
            tcp_abort(tpcb);
            result = ERR_ABRT;
        }
        complete(c);
        return result;
    }

    if (err == ERR_OK) {
        if (c->resp_len + p->tot_len > RESP_MAX) {
            pbuf_free(p);
            fail(c, "response too large");
            return ERR_ABRT;
        }
        c->resp_len += pbuf_copy_partial(p, c->resp + c->resp_len, p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
        drop_interim_responses(c);

        // A final response instead of 100 Continue: the upload was refused
        if (c->step == STEP_CONTINUE && strstr(c->resp, "\r\n\r\n")) {
            pbuf_free(p);
            char why[64];
            int status_len = (int)strcspn(c->resp, "\r");
            snprintf(why, sizeof(why), "refused upload: %.*s", status_len, c->resp);
            fail(c, why);
            return ERR_ABRT;
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    http_conn_t *c = arg;
    c->pcb = NULL; // already freed by lwIP
    if (c->step != STEP_DONE) {
        printf("HTTP %s:%u: connection error %d\n", c->host, c->port, err);
        c->step = STEP_FAILED;
    }
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    http_conn_t *c = arg;
    (void)tpcb;
    if (err != ERR_OK) {
        fail(c, "connect failed");
        return ERR_ABRT;
    }
    c->step = STEP_CONTINUE;
    return ERR_OK;
}

static void connect_to_addr(http_conn_t *c) {
    // Fail at once rather than at the timeout, e.g. the USB host while USB is down
    if (!net_routable(&c->addr)) {
        fail(c, "network down");
        return;
    }
    c->pcb = tcp_new_ip_type(IP_GET_TYPE(&c->addr));
    if (!c->pcb) {
        fail(c, "out of memory");
        return;
    }
    tcp_arg(c->pcb, c);
    tcp_nagle_disable(c->pcb);
    tcp_recv(c->pcb, on_recv);
    tcp_err(c->pcb, on_err);
    c->step = STEP_CONNECTING;
    if (tcp_connect(c->pcb, &c->addr, c->port, on_connected) != ERR_OK) {
        fail(c, "connect failed");
    }
}

static void on_dns(const char *name, const ip_addr_t *found, void *arg) {
    http_conn_t *c = arg;
    // Ignore lookups for a request this connection no longer serves
    if (c->step != STEP_RESOLVING || strcmp(name, c->host) != 0) {
        return;
    }
    if (!found) {
        fail(c, "could not resolve host");
        return;
    }
    c->addr = *found;
    connect_to_addr(c);
}

bool http_open(const char *host, uint16_t port, const char *path, const char *content_type,
               uint32_t ready_timeout_ms, uint32_t response_timeout_ms) {
    http_conn_t *c = &conn;
    if (c->step != STEP_FREE) {
        return false;
    }

    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->pcb = NULL;
    c->response_timeout_ms = response_timeout_ms;
    c->header_sent = false;
    c->finishing = false;
    c->tx_head = c->tx_tail = 0;
    c->resp_len = 0;
    c->status_code = 0;
    c->body = "";

    int n = snprintf(c->header, sizeof(c->header),
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Content-Type: %s\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "Expect: 100-continue\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host, port, content_type);
    if (n < 0 || (size_t)n >= sizeof(c->header)) {
        c->step = STEP_FAILED;
        return true;
    }

    c->deadline = make_timeout_time_ms(ready_timeout_ms);
    c->step = STEP_RESOLVING;
    err_t err = dns_gethostbyname(c->host, &c->addr, on_dns, c);
    if (err == ERR_OK) {
        connect_to_addr(c); // numeric address, or already cached
    } else if (err != ERR_INPROGRESS) {
        fail(c, "could not resolve host");
    }
    return true;
}

bool http_ready(void) {
    http_conn_t *c = &conn;
    return c->step == STEP_STREAMING || c->step == STEP_AWAITING;
}

bool http_write(const void *data, size_t len) {
    http_conn_t *c = &conn;
    if (TX_RING_SIZE - (c->tx_head - c->tx_tail) < len) {
        return false;
    }
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        c->tx_ring[c->tx_head++ % TX_RING_SIZE] = bytes[i];
    }
    return true;
}

void http_finish(void) {
    http_conn_t *c = &conn;
    c->finishing = true;
}

void http_close(void) {
    http_conn_t *c = &conn;
    detach(c);
    c->step = STEP_FREE;
}

// Room for a write without lwIP refusing it midway through a chunk
static bool can_queue(http_conn_t *c, size_t bytes) {
    return tcp_sndbuf(c->pcb) >= bytes && tcp_sndqueuelen(c->pcb) + 8 <= TCP_SND_QUEUELEN;
}

static bool queue(http_conn_t *c, const void *data, size_t len, bool more) {
    if (tcp_write(c->pcb, data, len, TCP_WRITE_FLAG_COPY | (more ? TCP_WRITE_FLAG_MORE : 0)) != ERR_OK) {
        fail(c, "send failed");
        return false;
    }
    return true;
}

// Send one chunk of buffered body, if there's enough to bother and room for it
static bool send_chunk(http_conn_t *c) {
    uint32_t pending = c->tx_head - c->tx_tail;
    if (pending == 0 || (pending < CHUNK_MIN && !c->finishing)) {
        return false;
    }
    uint32_t room = tcp_sndbuf(c->pcb);
    if (room <= CHUNK_OVERHEAD || !can_queue(c, CHUNK_OVERHEAD + 1)) {
        return false;
    }
    uint32_t n = pending;
    if (n > CHUNK_MAX) n = CHUNK_MAX;
    if (n > room - CHUNK_OVERHEAD) n = room - CHUNK_OVERHEAD;

    char size_line[8];
    int size_len = snprintf(size_line, sizeof(size_line), "%lX\r\n", (unsigned long)n);
    if (!queue(c, size_line, size_len, true)) {
        return false;
    }

    // The data may wrap around the end of the ring
    uint32_t start = c->tx_tail % TX_RING_SIZE;
    uint32_t first = TX_RING_SIZE - start < n ? TX_RING_SIZE - start : n;
    if (!queue(c, &c->tx_ring[start], first, true)) {
        return false;
    }
    if (first < n && !queue(c, c->tx_ring, n - first, true)) {
        return false;
    }
    if (!queue(c, "\r\n", 2, false)) {
        return false;
    }
    c->tx_tail += n;
    return true;
}

static void conn_task(http_conn_t *c) {
    switch (c->step) {
    case STEP_RESOLVING:
    case STEP_CONNECTING:
        if (time_reached(c->deadline)) {
            fail(c, "timed out connecting");
        }
        break;

    case STEP_CONTINUE:
        if (!c->header_sent && can_queue(c, strlen(c->header))) {
            if (!queue(c, c->header, strlen(c->header), false)) {
                break;
            }
            c->header_sent = true;
            tcp_output(c->pcb);
        }
        if (time_reached(c->deadline)) {
            fail(c, "no 100 Continue from server");
        }
        break;

    case STEP_STREAMING:
        while (send_chunk(c)) {
        }
        if (c->step != STEP_STREAMING) {
            break; // a send failed
        }
        if (c->finishing && c->tx_head == c->tx_tail) {
            if (!can_queue(c, 5) || !queue(c, "0\r\n\r\n", 5, false)) {
                break;
            }
            c->step = STEP_AWAITING;
            c->deadline = make_timeout_time_ms(c->response_timeout_ms);
        }
        tcp_output(c->pcb);
        break;

    case STEP_AWAITING:
        if (time_reached(c->deadline)) {
            fail(c, "timed out waiting for response");
        }
        break;

    default:
        break;
    }
}

void http_task(void) {
    conn_task(&conn);
}

http_state_t http_state(void) {
    http_conn_t *c = &conn;
    switch (c->step) {
    case STEP_DONE:   return HTTP_DONE;
    case STEP_FAILED: return HTTP_FAILED;
    default:          return HTTP_BUSY;
    }
}

int http_status_code(void) {
    http_conn_t *c = &conn;
    return c->status_code;
}

const char *http_body(void) {
    http_conn_t *c = &conn;
    return c->body;
}
