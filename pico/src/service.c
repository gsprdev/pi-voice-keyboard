// Transcription over streamed uploads to POST /transcribe.
//
// There are no background health checks: opening the upload is the check.
// Servers are tried in priority order; the first to answer "100 Continue"
// (its handler is reading the body) within READY_TIMEOUT_MS gets the recording.

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

#define MAX_SERVERS         4
#define READY_TIMEOUT_MS    300   // per server: resolve, connect, 100 Continue
#define RESPONSE_TIMEOUT_MS 60000

static struct {
    char host[64];
    uint16_t port;
} servers[MAX_SERVERS];
static int num_servers;

static session_state_t state = SESSION_IDLE;
static int server; // index of the server being tried or used

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

// Open the upload on the current server
static void try_server(void) {
    if (server >= num_servers) {
        state = SESSION_FAILED;
        return;
    }
    http_open(servers[server].host, servers[server].port, "/transcribe", "audio/wav",
              READY_TIMEOUT_MS, RESPONSE_TIMEOUT_MS);
}

void service_task(void) {
    http_task();
    http_state_t http = http_state();

    switch (state) {
    case SESSION_CONNECTING:
        if (http_ready()) {
            printf("Service: recording to %s:%u\n", servers[server].host, servers[server].port);
            state = SESSION_RECORDING;

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
            http_write(wav_header, sizeof(wav_header));
        } else if (http != HTTP_BUSY) {
            // Refused, unreachable, too slow, or rejected the upload: next server
            printf("Service: %s:%u not ready\n", servers[server].host, servers[server].port);
            http_close();
            server++;
            try_server();
        }
        break;

    case SESSION_RECORDING:
        if (http != HTTP_BUSY) {
            printf("Service: upload ended early\n");
            state = SESSION_FAILED;
        }
        break;

    case SESSION_PROCESSING:
        if (http == HTTP_DONE && http_status_code() == 200) {
            state = SESSION_DONE;
        } else if (http == HTTP_DONE) {
            printf("Service: /transcribe returned %d: %s\n", http_status_code(), http_body());
            state = SESSION_FAILED;
        } else if (http == HTTP_FAILED) {
            state = SESSION_FAILED;
        }
        break;

    default:
        break;
    }
}

bool service_start(void) {
    if (state != SESSION_IDLE) {
        printf("Service: previous transcription still in progress\n");
        return false;
    }
    if (!net_up()) {
        printf("Service: Wi-Fi not connected\n");
        return false;
    }
    state = SESSION_CONNECTING;
    server = 0;
    try_server();
    return true;
}

session_state_t service_state(void) {
    return state;
}

bool service_send(const int16_t *samples, size_t count) {
    return http_write(samples, count * sizeof(samples[0]));
}

void service_end(void) {
    http_finish();
    state = SESSION_PROCESSING;
}

const char *service_text(void) {
    return http_body();
}

void service_release(void) {
    http_close();
    state = SESSION_IDLE;
}

#else // Board without Wi-Fi: transcription never starts

void service_init(void) {}
void service_task(void) {}
void service_report(void) { printf("Service: no Wi-Fi on this board\n"); }
bool service_start(void) { return false; }
session_state_t service_state(void) { return SESSION_IDLE; }
bool service_send(const int16_t *samples, size_t count) { (void)samples; (void)count; return false; }
void service_end(void) {}
const char *service_text(void) { return ""; }
void service_release(void) {}

#endif
