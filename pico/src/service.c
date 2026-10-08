// Transcription service client: proactive health checks so a button press can
// start recording immediately, and streamed uploads to POST /transcribe.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"
#include "service.h"

#if VOICEKB_NETWORK

#include "http.h"
#include "net.h"
#include "secrets.h"

#define HEALTH_INTERVAL_MS   10000
#define HEALTH_TIMEOUT_MS    1000
#define CONNECT_TIMEOUT_MS   3000
#define TRANSCRIBE_TIMEOUT_MS 60000

static char host[64];
static uint16_t port;
static bool url_valid;

static bool healthy;
static bool checking;
static bool transcribing;
static absolute_time_t next_check;

static void parse_url(const char *url) {
    const char *prefix = "http://";
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        printf("Service: SERVICE_URL must start with http://\n");
        return;
    }
    const char *start = url + strlen(prefix);
    size_t len = strcspn(start, ":/");
    if (len == 0 || len >= sizeof(host)) {
        printf("Service: bad host in SERVICE_URL\n");
        return;
    }
    memcpy(host, start, len);
    host[len] = '\0';
    port = start[len] == ':' ? (uint16_t)atoi(start + len + 1) : 80;
    url_valid = port != 0;
}

static void set_healthy(bool now_healthy) {
    if (now_healthy != healthy) {
        printf("Service: %s:%u %s\n", host, port, now_healthy ? "ready" : "unavailable");
    }
    healthy = now_healthy;
}

void service_init(void) {
    parse_url(SERVICE_URL);
}

void service_task(void) {
    http_task();

    if (!net_up()) {
        set_healthy(false);
        if (checking) {
            http_reset();
            checking = false;
        }
        return;
    }
    if (transcribing || !url_valid) {
        return;
    }

    if (checking) {
        http_state_t state = http_state();
        if (state == HTTP_BUSY) {
            return;
        }
        // Trim the trailing newline the service sends
        const char *body = http_body();
        set_healthy(state == HTTP_DONE && http_status_code() == 200 && strncmp(body, "OK", 2) == 0
                    && strspn(body + 2, "\r\n") == strlen(body + 2));
        http_reset();
        checking = false;
        next_check = make_timeout_time_ms(HEALTH_INTERVAL_MS);
    } else if (time_reached(next_check)) {
        checking = http_begin(host, port, "GET", "/health", NULL, false,
                              HEALTH_TIMEOUT_MS, HEALTH_TIMEOUT_MS);
        if (!checking) {
            set_healthy(false);
            next_check = make_timeout_time_ms(HEALTH_INTERVAL_MS);
        }
    }
}

bool service_ready(void) {
    return healthy;
}

void service_report(void) {
    if (!url_valid) {
        printf("Service: invalid SERVICE_URL\n");
    } else {
        printf("Service: %s:%u %s\n", host, port, healthy ? "ready" : "unavailable");
    }
}

bool service_begin(void) {
    checking = false; // http_begin abandons any health check in flight
    if (!http_begin(host, port, "POST", "/transcribe", "audio/wav", true,
                    CONNECT_TIMEOUT_MS, TRANSCRIBE_TIMEOUT_MS)) {
        return false;
    }
    transcribing = true;

    // The service skips the 44-byte header and reads samples to the end of the
    // body, so the sizes (unknown while streaming) are left at their maximum.
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
    return http_write(wav_header, sizeof(wav_header));
}

bool service_send(const int16_t *samples, size_t count) {
    return http_write(samples, count * sizeof(samples[0]));
}

void service_end(void) {
    http_finish();
}

void service_cancel(void) {
    http_reset();
    transcribing = false;
    next_check = get_absolute_time(); // the network was struggling: re-check
}

transcribe_result_t service_result(const char **text) {
    http_state_t state = http_state();
    if (state == HTTP_BUSY) {
        return TRANSCRIBE_PENDING;
    }

    transcribe_result_t result = TRANSCRIBE_FAILED;
    if (state == HTTP_DONE && http_status_code() == 200) {
        *text = http_body();
        result = TRANSCRIBE_OK;
    } else if (state == HTTP_DONE) {
        printf("Service: /transcribe returned %d: %s\n", http_status_code(), http_body());
    }

    // The body stays valid until the next request, which service_task starts
    transcribing = false;
    if (result == TRANSCRIBE_FAILED) {
        set_healthy(false);
        next_check = get_absolute_time(); // re-check right away
    } else {
        next_check = make_timeout_time_ms(HEALTH_INTERVAL_MS);
    }
    return result;
}

#else // Board without Wi-Fi: never ready

void service_init(void) {}
void service_task(void) {}
bool service_ready(void) { return false; }
void service_report(void) { printf("Service: no Wi-Fi on this board\n"); }
bool service_begin(void) { return false; }
bool service_send(const int16_t *samples, size_t count) { (void)samples; (void)count; return false; }
void service_end(void) {}
void service_cancel(void) {}
transcribe_result_t service_result(const char **text) { (void)text; return TRANSCRIBE_FAILED; }

#endif
