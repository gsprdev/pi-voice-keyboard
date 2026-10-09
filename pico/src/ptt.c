// Push-to-talk. Port of pi/ptt.py, with readiness tied to the upload itself.
//
// A press opens a transcription session. Only once a server is accepting the
// upload do the recording LED and beep come on, and audio starts streaming
// from that moment: the beep is recorded too, and the service removes it.
//
// Earlier sessions keep going while the next is recorded. Their results are
// typed in the order they were recorded; the processing LED stays on until
// everything has been typed.

#include <stdio.h>

#include "feedback.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pins.h"
#include "ptt.h"
#include "service.h"
#include "typer.h"

#define DEBOUNCE_MS 30
#define MAX_PENDING 4

static bool pressed;
static bool raw_last;
static absolute_time_t raw_changed;

// The session for the press in progress (connecting or recording)
static session_t *current;
static bool announced; // recording LED and beep given for current

// Finished recordings awaiting their transcription, oldest first
static session_t *pending[MAX_PENDING];
static int num_pending;

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

static void drop_current(void) {
    service_release(current);
    current = NULL;
    if (announced) {
        feedback_recording(false);
        announced = false;
    }
}

static void on_press(void) {
    printf("Button pressed - connecting\n");
    if (num_pending == MAX_PENDING) {
        printf("Too many transcriptions in progress\n");
        feedback_error();
        return;
    }
    current = service_start();
    if (!current) {
        feedback_error();
    }
}

static void on_release(void) {
    if (!current) {
        return;
    }
    if (!announced) {
        // Released before the server was ready: nothing was recorded
        printf("Button released before recording started\n");
        drop_current();
        return;
    }
    printf("Button released - transcribing\n");
    service_end(current);
    pending[num_pending++] = current;
    current = NULL;
    announced = false;
    feedback_recording(false);
}

static void track_current(void) {
    if (!current) {
        return;
    }
    switch (service_state(current)) {
    case SESSION_RECORDING:
        if (!announced) {
            feedback_recording(true);
            feedback_beep();
            announced = true;
        }
        break;
    case SESSION_FAILED:
        printf("%s\n", announced ? "Recording failed" : "No transcription service available");
        drop_current();
        feedback_error();
        break;
    default:
        break;
    }
}

// Type finished transcriptions in order; a slow one holds back later ones
static void deliver_results(void) {
    while (num_pending > 0) {
        session_t *s = pending[0];
        session_state_t state = service_state(s);
        if (state == SESSION_DONE) {
            const char *text = service_text(s); // already cleaned by the service
            printf("Transcription: %s\n", text);
            for (const char *c = text; *c; c++) {
                if (!typer_putc(*c)) {
                    printf("Typing queue full, transcription truncated\n");
                    break;
                }
            }
        } else if (state == SESSION_FAILED) {
            printf("Transcription failed\n");
            feedback_error();
        } else {
            return;
        }
        service_release(s);
        num_pending--;
        for (int i = 0; i < num_pending; i++) {
            pending[i] = pending[i + 1];
        }
    }
}

void ptt_task(void) {
    int edge = button_edge();
    if (edge > 0) {
        on_press();
    } else if (edge < 0) {
        on_release();
    }

    track_current();
    deliver_results();
    feedback_processing(num_pending > 0 || typer_busy());
}

void ptt_audio(const int16_t *samples, size_t count) {
    if (!current || !announced) {
        return;
    }
    if (!service_send(current, samples, count)) {
        printf("Network not keeping up, abandoning recording\n");
        drop_current();
        feedback_error();
    }
}
