// Transcription sessions over streamed uploads to POST /transcribe.
//
// There are no background health checks: opening the upload is the check.
// Each session tries the configured servers in priority order and settles on
// the first that answers "100 Continue", i.e. whose handler is reading the body.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

#if VOICEKB_NETWORK

#include "http.h"
#include "net.h"
#include "secrets.h"

// Older secrets.h files name a single server
#ifndef SERVICE_URLS
#define SERVICE_URLS SERVICE_URL
#endif

#define MAX_SERVERS      4
#define READY_TIMEOUT_MS 1000  // per server: resolve, connect, 100 Continue
#define RESPONSE_TIMEOUT_MS 60000

struct session {
    bool in_use;
    session_state_t state;
    http_conn_t *conn;
    int server;
};

static struct {
    char host[64];
    uint16_t port;
} servers[MAX_SERVERS];
static int num_servers;

static session_t sessions[HTTP_MAX_CONNS];

// Parse "http://host[:port][/]" entries separated by commas
static void parse_urls(const char *urls) {
    const char *prefix = "http://";
    const char *p = urls;
    while (*p && num_servers < MAX_SERVERS) {
        p += strspn(p, ", ");
        size_t len = strcspn(p, ",");
        if (len == 0) {
            break;
        }
        if (strncmp(p, prefix, strlen(prefix)) != 0) {
            printf("Service: URL must start with http://: %.*s\n", (int)len, p);
        } else {
            const char *start = p + strlen(prefix);
            size_t host_len = strcspn(start, ":/,");
            if (host_len == 0 || host_len >= sizeof(servers[0].host)) {
                printf("Service: bad host in %.*s\n", (int)len, p);
            } else {
                memcpy(servers[num_servers].host, start, host_len);
                servers[num_servers].host[host_len] = '\0';
                uint16_t port = start[host_len] == ':' ? (uint16_t)atoi(start + host_len + 1) : 80;
                if (port != 0) {
                    servers[num_servers++].port = port;
                }
            }
        }
        p += len;
    }
}

void service_init(void) {
    parse_urls(SERVICE_URLS);
}

void service_report(void) {
    if (num_servers == 0) {
        printf("Service: no valid servers in SERVICE_URLS\n");
    }
    for (int i = 0; i < num_servers; i++) {
        printf("Service %d: %s:%u\n", i + 1, servers[i].host, servers[i].port);
    }
}

// Open the upload on the session's current server, or the next ones after it
static void try_servers(session_t *s) {
    for (; s->server < num_servers; s->server++) {
        s->conn = http_open(servers[s->server].host, servers[s->server].port, "/transcribe",
                            "audio/wav", READY_TIMEOUT_MS, RESPONSE_TIMEOUT_MS);
        if (s->conn) {
            return;
        }
    }
    s->state = SESSION_FAILED;
}

static void session_task(session_t *s) {
    if (!s->in_use || !s->conn) {
        return;
    }
    http_state_t state = http_state(s->conn);

    switch (s->state) {
    case SESSION_CONNECTING:
        if (http_ready(s->conn)) {
            printf("Service: recording to %s:%u\n", servers[s->server].host, servers[s->server].port);
            s->state = SESSION_RECORDING;

            // The service skips the 44-byte header and reads samples to the end
            // of the body, so the sizes (unknown while streaming) are left at max.
            static const uint8_t wav_header[44] = {
                'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0xff, 'W', 'A', 'V', 'E',
                'f', 'm', 't', ' ', 16, 0, 0, 0,
                1, 0,                    // PCM
                1, 0,                    // mono
                0x80, 0x3e, 0, 0,        // 16000 Hz
                0x00, 0x7d, 0, 0,        // 32000 bytes/s
                2, 0,                    // block align
                16, 0,                   // bits per sample
                'd', 'a', 't', 'a', 0xff, 0xff, 0xff, 0xff,
            };
            http_write(s->conn, wav_header, sizeof(wav_header));
        } else if (state != HTTP_BUSY) {
            // Refused, unreachable, too slow, or rejected the request: next server
            printf("Service: %s:%u not ready\n", servers[s->server].host, servers[s->server].port);
            http_close(s->conn);
            s->conn = NULL;
            s->server++;
            try_servers(s);
        }
        break;

    case SESSION_RECORDING:
        if (state != HTTP_BUSY) {
            printf("Service: upload ended early\n");
            s->state = SESSION_FAILED;
        }
        break;

    case SESSION_PROCESSING:
        if (state == HTTP_DONE && http_status_code(s->conn) == 200) {
            s->state = SESSION_DONE;
        } else if (state == HTTP_DONE) {
            printf("Service: /transcribe returned %d: %s\n", http_status_code(s->conn), http_body(s->conn));
            s->state = SESSION_FAILED;
        } else if (state == HTTP_FAILED) {
            s->state = SESSION_FAILED;
        }
        break;

    default:
        break;
    }
}

void service_task(void) {
    http_task();
    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
        session_task(&sessions[i]);
    }
}

session_t *service_start(void) {
    if (!net_up()) {
        printf("Service: Wi-Fi not connected\n");
        return NULL;
    }
    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
        session_t *s = &sessions[i];
        if (!s->in_use) {
            s->in_use = true;
            s->state = SESSION_CONNECTING;
            s->conn = NULL;
            s->server = 0;
            try_servers(s);
            return s;
        }
    }
    printf("Service: too many transcriptions in progress\n");
    return NULL;
}

session_state_t service_state(session_t *s) {
    return s->state;
}

bool service_send(session_t *s, const int16_t *samples, size_t count) {
    return http_write(s->conn, samples, count * sizeof(samples[0]));
}

void service_end(session_t *s) {
    http_finish(s->conn);
    s->state = SESSION_PROCESSING;
}

const char *service_text(session_t *s) {
    // The response buffer belongs to the connection, which the session holds
    return http_body(s->conn);
}

void service_release(session_t *s) {
    if (s->conn) {
        http_close(s->conn);
        s->conn = NULL;
    }
    s->in_use = false;
}

#else // Board without Wi-Fi: no sessions

void service_init(void) {}
void service_task(void) {}
void service_report(void) { printf("Service: no Wi-Fi on this board\n"); }
session_t *service_start(void) { return NULL; }
session_state_t service_state(session_t *s) { (void)s; return SESSION_FAILED; }
bool service_send(session_t *s, const int16_t *samples, size_t count) { (void)s; (void)samples; (void)count; return false; }
void service_end(session_t *s) { (void)s; }
const char *service_text(session_t *s) { (void)s; return ""; }
void service_release(session_t *s) { (void)s; }

#endif
