// Push-to-talk. Port of pi/ptt.py, with readiness tied to the upload itself.
//
// A press starts a transcription. Only once a server is accepting the upload
// do the recording LED and beep come on, and audio starts streaming from that
// moment: the beep is recorded too, and the service removes it.
//
// One transcription at a time. A press isn't acted on until the previous one
// has been handed to the typing queue: holding the button simply waits until
// then, and the beep says when to speak. Typing continues in the background,
// so the next recording can overlap it. The processing LED covers both
// waiting for the transcription and typing it.

#include <stdio.h>

#include "feedback.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pins.h"
#include "ptt.h"
#include "service.h"
#include "typer.h"

#define DEBOUNCE_MS 30

static bool pressed;
static bool raw_last;
static absolute_time_t raw_changed;
static bool armed = true; // released since the last press was acted on

static bool holding;   // this press started a transcription, still in its recording phase
static bool announced; // recording LED and beep given

void ptt_init(void) {
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN); // button to ground: low when pressed
}

// Debounced button state
static void button_update(void) {
    bool raw = !gpio_get(BUTTON_PIN);
    if (raw != raw_last) {
        raw_last = raw;
        raw_changed = get_absolute_time();
    } else if (absolute_time_diff_us(raw_changed, get_absolute_time()) >= DEBOUNCE_MS * 1000) {
        pressed = raw;
    }
}

static void abandon(void) {
    service_release();
    holding = false;
    if (announced) {
        feedback_recording(false);
        announced = false;
    }
}

static void start(void) {
    printf("Button pressed - connecting\n");
    if (service_start()) {
        holding = true;
    } else {
        feedback_error();
    }
}

static void release(void) {
    if (!announced) {
        // Released before a server was ready: nothing was recorded
        printf("Button released before recording started\n");
        abandon();
        return;
    }
    printf("Button released - transcribing\n");
    service_end();
    holding = false;
    announced = false;
    feedback_recording(false);
}

// React to the transcription's progress
static void track_session(void) {
    switch (service_state()) {
    case SESSION_RECORDING:
        if (!announced) {
            feedback_recording(true);
            feedback_beep();
            announced = true;
        }
        break;

    case SESSION_DONE: {
        // Hand off to the typing queue, freeing the way for the next press
        const char *text = service_text(); // already cleaned by the service
        printf("Transcription: %s\n", text);
        for (const char *c = text; *c; c++) {
            if (!typer_putc(*c)) {
                printf("Typing queue full, transcription truncated\n");
                break;
            }
        }
        service_release();
        break;
    }

    case SESSION_FAILED:
        printf("%s\n", holding && !announced ? "No transcription service available" : "Transcription failed");
        abandon();
        feedback_error();
        break;

    default:
        break;
    }
}

void ptt_task(void) {
    track_session();

    button_update();
    if (!pressed) {
        armed = true;
        if (holding) {
            release();
        }
    } else if (armed && service_state() == SESSION_IDLE) {
        // A press is noticed only once the previous transcription is out of
        // the way. One attempt per press, even if it fails while still held.
        armed = false;
        start();
    }

    feedback_processing(service_state() == SESSION_PROCESSING || typer_busy());
}

void ptt_audio(const int16_t *samples, size_t count) {
    if (!holding || !announced) {
        return;
    }
    if (!service_send(samples, count)) {
        printf("Network not keeping up, abandoning recording\n");
        abandon();
        feedback_error();
    }
}
