// Types queued ASCII text as USB HID boot keyboard reports.
// Port of pi/type-ascii.py, driven by the main loop instead of sleeping.

#include <math.h>
#include <stdio.h>

#include "hardware/adc.h"
#include "pico/time.h"
#include "pins.h"
#include "tusb.h"
#include "typer.h"

// Each key is held for the delay, then released for the delay. Without the
// speed knob it's a fixed 10ms, safe for slow hosts and apps. The knob covers
// KNOB_SLOW_MS down to KNOB_FAST_MS in KNOB_STEPS steps (build options),
// logarithmic so the fast end has finer control; 1ms typed cleanly into a
// terminal and Kate on Linux.
#define FIXED_DELAY_US 10000
#if TYPING_KNOB
#define KNOB_STEP_RAW (4096 / KNOB_STEPS) // the ADC has 12 bits
static uint32_t knob_delays_us[KNOB_STEPS];
#endif
static uint32_t delay_us;

#define QUEUE_SIZE 4096 // power of two

static const uint8_t ascii_to_hid[128][2] = { HID_ASCII_TO_KEYCODE };

static char queue[QUEUE_SIZE];
static uint32_t head, tail;

static enum { IDLE, PRESSED, RELEASED } state = IDLE;
static absolute_time_t next_at;

bool typer_has_room(void) {
    return head - tail < QUEUE_SIZE;
}

bool typer_putc(char ch) {
    if (!typer_has_room()) {
        return false;
    }
    queue[head++ % QUEUE_SIZE] = ch;
    return true;
}

// Same character set as type-ascii.py: printable ASCII, tab, newline, backspace
static bool lookup(char ch, uint8_t *mod, uint8_t *keycode) {
    unsigned char c = (unsigned char)ch;
    if (!((c >= 0x20 && c <= 0x7e) || c == '\t' || c == '\n' || c == '\b')) {
        return false;
    }
    *mod = ascii_to_hid[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    *keycode = ascii_to_hid[c][1];
    return true;
}

void typer_init(void) {
#if TYPING_KNOB
    adc_init();
    adc_gpio_init(SPEED_KNOB_PIN);
    adc_select_input(SPEED_KNOB_PIN - 26); // ADC inputs 0-3 are GPIO 26-29
    float slow = KNOB_SLOW_MS * 1000.0f, fast = KNOB_FAST_MS * 1000.0f;
    for (int i = 0; i < KNOB_STEPS; i++) {
        float t = KNOB_STEPS > 1 ? (float)i / (KNOB_STEPS - 1) : 0;
        knob_delays_us[i] = (uint32_t)(slow * powf(fast / slow, t) + 0.5f);
    }
#endif
}

// Read per key, so turning the knob takes effect mid-sentence
static uint32_t key_delay_us(void) {
#if TYPING_KNOB
    static int step = -1;
    int raw = adc_read();
    int slack = KNOB_STEP_RAW / 8; // hysteresis, so a knob between steps doesn't flicker
    if (step < 0 || raw < step * KNOB_STEP_RAW - slack || raw >= (step + 1) * KNOB_STEP_RAW + slack) {
        step = raw / KNOB_STEP_RAW;
        if (step >= KNOB_STEPS) {
            step = KNOB_STEPS - 1; // the remainder when STEPS doesn't divide 4096
        }
        printf("Typing delay: %uus\n", (unsigned)knob_delays_us[step]);
    }
    return knob_delays_us[step];
#else
    return FIXED_DELAY_US;
#endif
}

bool typer_busy(void) {
    return head != tail || state != IDLE;
}

void typer_task(void) {
    if (!tud_mounted()) {
        return;
    }

    switch (state) {
    case IDLE:
        while (tail != head && tud_hid_ready()) {
            char ch = queue[tail++ % QUEUE_SIZE];
            uint8_t mod, keycode[6] = {0};
            if (!lookup(ch, &mod, &keycode[0])) {
                printf("Skipping byte %u: unsupported character\n", (unsigned char)ch);
                continue;
            }
            tud_hid_keyboard_report(0, mod, keycode);
            state = PRESSED;
            delay_us = key_delay_us();
            next_at = make_timeout_time_us(delay_us);
            break;
        }
        break;

    case PRESSED:
        if (time_reached(next_at) && tud_hid_ready()) {
            tud_hid_keyboard_report(0, 0, NULL); // key up
            state = RELEASED;
            next_at = make_timeout_time_us(delay_us);
        }
        break;

    case RELEASED:
        if (time_reached(next_at)) {
            state = IDLE;
        }
        break;
    }
}

// Required TinyUSB HID callbacks. Keyboard LED state (caps lock etc.) is ignored.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
