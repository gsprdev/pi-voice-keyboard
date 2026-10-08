// Minimal HTTP/1.1 client on lwIP's raw TCP API (cyw43 poll mode: every
// callback runs from net_task in the main loop, so no locking is needed).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "pico/time.h"

// Body bytes waiting for the TCP send buffer: ~2s of 16kHz audio, enough to
// ride out Wi-Fi hiccups without buffering whole recordings
#define TX_RING_SIZE   65536 // power of two
#define RESP_MAX       16384
#define CHUNK_MIN      512   // batch small writes into larger chunks
#define CHUNK_MAX      2048
#define CHUNK_OVERHEAD 12    // "XXXX\r\n" + "\r\n" with room to spare

typedef enum {
    STEP_IDLE,
    STEP_RESOLVING,
    STEP_CONNECTING,
    STEP_SENDING,
    STEP_AWAITING,
    STEP_DONE,
    STEP_FAILED,
} step_t;

static step_t step = STEP_IDLE;
static uint32_t generation; // invalidates DNS callbacks from abandoned requests
static struct tcp_pcb *pcb;
static ip_addr_t addr;
static uint16_t port;
static uint32_t response_timeout_ms;
static absolute_time_t deadline;

static char header[256];
static bool header_sent;
static bool has_body;
static bool finishing;

static uint8_t tx_ring[TX_RING_SIZE];
static uint32_t tx_head, tx_tail;

static char resp[RESP_MAX + 1];
static size_t resp_len;
static int status_code;
static const char *body = "";

static void fail(const char *why) {
    printf("HTTP: %s\n", why);
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        pcb = NULL;
    }
    step = STEP_FAILED;
}

// Split the response into status code and body, decoding a chunked body in place
static bool parse_response(void) {
    resp[resp_len] = '\0';
    if (strncmp(resp, "HTTP/1.", 7) != 0 || resp_len < 12) {
        return false;
    }
    status_code = atoi(resp + 9);

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
        char *src = start, *dst = start, *end = resp + resp_len;
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

    body = start;
    return true;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        // Server closed the connection: the response is complete
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        pcb = NULL;
        if (tcp_close(tpcb) != ERR_OK) {
            tcp_abort(tpcb);
            step = parse_response() ? STEP_DONE : STEP_FAILED;
            return ERR_ABRT;
        }
        if (parse_response()) {
            step = STEP_DONE;
        } else {
            printf("HTTP: malformed response\n");
            step = STEP_FAILED;
        }
        return ERR_OK;
    }

    if (err == ERR_OK) {
        if (resp_len + p->tot_len > RESP_MAX) {
            pbuf_free(p);
            fail("response too large");
            return ERR_ABRT;
        }
        resp_len += pbuf_copy_partial(p, resp + resp_len, p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    pcb = NULL; // already freed by lwIP
    if (step != STEP_DONE) {
        printf("HTTP: connection error %d\n", err);
        step = STEP_FAILED;
    }
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg; (void)tpcb;
    if (err != ERR_OK) {
        fail("connect failed");
        return ERR_ABRT;
    }
    step = STEP_SENDING;
    return ERR_OK;
}

static void connect_to_addr(void) {
    pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (!pcb) {
        fail("out of memory");
        return;
    }
    tcp_nagle_disable(pcb);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    step = STEP_CONNECTING;
    if (tcp_connect(pcb, &addr, port, on_connected) != ERR_OK) {
        fail("connect failed");
    }
}

static void on_dns(const char *name, const ip_addr_t *found, void *arg) {
    if ((uint32_t)(uintptr_t)arg != generation || step != STEP_RESOLVING) {
        return;
    }
    if (!found) {
        printf("HTTP: could not resolve %s\n", name);
        step = STEP_FAILED;
        return;
    }
    addr = *found;
    connect_to_addr();
}

void http_reset(void) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        pcb = NULL;
    }
    generation++;
    step = STEP_IDLE;
}

bool http_begin(const char *host, uint16_t port_, const char *method, const char *path,
                const char *content_type, bool has_body_,
                uint32_t connect_timeout_ms, uint32_t response_timeout_ms_) {
    http_reset();

    port = port_;
    has_body = has_body_;
    response_timeout_ms = response_timeout_ms_;
    header_sent = false;
    finishing = !has_body;
    tx_head = tx_tail = 0;
    resp_len = 0;
    status_code = 0;
    body = "";

    int n = snprintf(header, sizeof(header),
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n%s%s%s%s\r\n",
                     method, path, host, port,
                     content_type ? "Content-Type: " : "", content_type ? content_type : "",
                     content_type ? "\r\n" : "",
                     has_body ? "Transfer-Encoding: chunked\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(header)) {
        step = STEP_FAILED;
        return false;
    }

    deadline = make_timeout_time_ms(connect_timeout_ms);
    step = STEP_RESOLVING;
    err_t err = dns_gethostbyname(host, &addr, on_dns, (void *)(uintptr_t)generation);
    if (err == ERR_OK) {
        connect_to_addr(); // numeric address, or already cached
    } else if (err != ERR_INPROGRESS) {
        printf("HTTP: could not resolve %s\n", host);
        step = STEP_FAILED;
    }
    return step != STEP_FAILED;
}

bool http_write(const void *data, size_t len) {
    if (TX_RING_SIZE - (tx_head - tx_tail) < len) {
        return false;
    }
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        tx_ring[tx_head++ % TX_RING_SIZE] = bytes[i];
    }
    return true;
}

void http_finish(void) {
    finishing = true;
}

// Room for a write without lwIP refusing it midway through a chunk
static bool can_queue(size_t bytes) {
    return tcp_sndbuf(pcb) >= bytes && tcp_sndqueuelen(pcb) + 8 <= TCP_SND_QUEUELEN;
}

static bool queue(const void *data, size_t len, bool more) {
    if (tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY | (more ? TCP_WRITE_FLAG_MORE : 0)) != ERR_OK) {
        fail("send failed");
        return false;
    }
    return true;
}

// Send one chunk of buffered body, if there's enough to bother and room for it
static bool send_chunk(void) {
    uint32_t pending = tx_head - tx_tail;
    if (pending == 0 || (pending < CHUNK_MIN && !finishing)) {
        return false;
    }
    uint32_t room = tcp_sndbuf(pcb);
    if (room <= CHUNK_OVERHEAD || !can_queue(CHUNK_OVERHEAD + 1)) {
        return false;
    }
    uint32_t n = pending;
    if (n > CHUNK_MAX) n = CHUNK_MAX;
    if (n > room - CHUNK_OVERHEAD) n = room - CHUNK_OVERHEAD;

    char size_line[8];
    int size_len = snprintf(size_line, sizeof(size_line), "%lX\r\n", (unsigned long)n);
    if (!queue(size_line, size_len, true)) {
        return false;
    }

    // The data may wrap around the end of the ring
    uint32_t start = tx_tail % TX_RING_SIZE;
    uint32_t first = TX_RING_SIZE - start < n ? TX_RING_SIZE - start : n;
    if (!queue(&tx_ring[start], first, true)) {
        return false;
    }
    if (first < n && !queue(tx_ring, n - first, true)) {
        return false;
    }
    if (!queue("\r\n", 2, false)) {
        return false;
    }
    tx_tail += n;
    return true;
}

void http_task(void) {
    switch (step) {
    case STEP_RESOLVING:
    case STEP_CONNECTING:
        if (time_reached(deadline)) {
            fail("timed out connecting");
        }
        break;

    case STEP_SENDING:
        if (!header_sent) {
            if (!can_queue(strlen(header))) {
                break;
            }
            if (!queue(header, strlen(header), has_body)) {
                break;
            }
            header_sent = true;
        }
        while (send_chunk()) {
        }
        if (step != STEP_SENDING) {
            break; // a send failed
        }
        if (finishing && tx_head == tx_tail) {
            if (has_body) {
                if (!can_queue(5) || !queue("0\r\n\r\n", 5, false)) {
                    break;
                }
            }
            step = STEP_AWAITING;
            deadline = make_timeout_time_ms(response_timeout_ms);
        }
        tcp_output(pcb);
        break;

    case STEP_AWAITING:
        if (time_reached(deadline)) {
            fail("timed out waiting for response");
        }
        break;

    default:
        break;
    }
}

http_state_t http_state(void) {
    switch (step) {
    case STEP_IDLE:   return HTTP_IDLE;
    case STEP_DONE:   return HTTP_DONE;
    case STEP_FAILED: return HTTP_FAILED;
    default:          return HTTP_BUSY;
    }
}

int http_status_code(void) {
    return status_code;
}

const char *http_body(void) {
    return body;
}
