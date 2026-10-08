#include <stdio.h>
#include <string.h>

#include "feedback.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pins.h"
#include "ptt.h"
#include "service.h"
#include "text.h"
#include "typer.h"

#define DEBOUNCE_MS 30

// The start beep (100ms) is picked up by the mic, so audio during it isn't
// sent. The margin covers capture latency (DMA blocks are 8ms).
#define SKIP_AFTER_PRESS_MS 150

static enum { IDLE, RECORDING, PROCESSING } state = IDLE;
static bool pressed;
static bool raw_last;
static absolute_time_t raw_changed;
static absolute_time_t send_from;

void ptt_init(void) {
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN); // button to ground: low when pressed
}

// Returns +1 on press, -1 on release, 0 otherwise
static int button_edge(void) {
    bool raw = !gpio_get(BUTTON_PIN);
    if (raw != raw_last) {
        raw_last = raw;
        raw_changed = get_absolute_time();
        return 0;
    }
    if (raw != pressed && absolute_time_diff_us(raw_changed, get_absolute_time()) >= DEBOUNCE_MS * 1000) {
        pressed = raw;
        return pressed ? 1 : -1;
    }
    return 0;
}

static void start_recording(void) {
    printf("Button pressed - starting recording\n");
    if (!service_ready()) {
        printf("No transcription service available\n");
        feedback_error();
        return;
    }
    if (!service_begin()) {
        feedback_error();
        return;
    }
    state = RECORDING;
    send_from = make_timeout_time_ms(SKIP_AFTER_PRESS_MS);
    feedback_recording(true);
    feedback_beep();
}

static void stop_recording(void) {
    printf("Button released - transcribing\n");
    feedback_recording(false);
    feedback_processing(true);
    service_end();
    state = PROCESSING;
}

static void finish(transcribe_result_t result, char *text) {
    feedback_processing(false);
    state = IDLE;

    if (result != TRANSCRIBE_OK) {
        printf("Transcription failed\n");
        feedback_error();
        return;
    }

    printf("Transcription: %s\n", text);
    text_clean(text);
    for (const char *c = text; *c; c++) {
        if (!typer_putc(*c)) {
            printf("Typing queue full, transcription truncated\n");
            break;
        }
    }
}

void ptt_task(void) {
    int edge = button_edge();

    switch (state) {
    case IDLE:
        if (edge > 0) {
            start_recording();
        }
        break;

    case RECORDING:
        if (edge < 0) {
            stop_recording();
            break;
        }
        {
            // Server rejected the upload or the connection dropped mid-recording
            const char *unused;
            if (service_result(&unused) != TRANSCRIBE_PENDING) {
                printf("Upload ended early\n");
                feedback_recording(false);
                finish(TRANSCRIBE_FAILED, NULL);
            }
        }
        break;

    case PROCESSING: {
        const char *text;
        transcribe_result_t result = service_result(&text);
        if (result != TRANSCRIBE_PENDING) {
            finish(result, (char *)text);
        }
        break;
    }
    }
}

void ptt_audio(const int16_t *samples, size_t count) {
    if (state != RECORDING || !time_reached(send_from)) {
        return;
    }
    if (!service_send(samples, count)) {
        printf("Network not keeping up, abandoning recording\n");
        service_cancel();
        feedback_recording(false);
        finish(TRANSCRIBE_FAILED, NULL);
    }
}
