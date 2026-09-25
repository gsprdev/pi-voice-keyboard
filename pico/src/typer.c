// Types queued ASCII text as USB HID boot keyboard reports.
// Port of pi/type-ascii.py, driven by the main loop instead of sleeping.

#include <stdio.h>

#include "pico/time.h"
#include "tusb.h"
#include "typer.h"

// Matches the 10ms press and 10ms release delays in type-ascii.py
#define KEY_HOLD_US 10000
#define KEY_GAP_US  10000

#define QUEUE_SIZE 4096 // power of two

static const uint8_t ascii_to_hid[128][2] = { HID_ASCII_TO_KEYCODE };

static char queue[QUEUE_SIZE];
static uint32_t head, tail;

static enum { IDLE, PRESSED, RELEASED } state = IDLE;
static absolute_time_t next_at;

bool typer_putc(char ch) {
    if (head - tail == QUEUE_SIZE) {
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
            next_at = make_timeout_time_us(KEY_HOLD_US);
            break;
        }
        break;

    case PRESSED:
        if (time_reached(next_at) && tud_hid_ready()) {
            tud_hid_keyboard_report(0, 0, NULL); // key up
            state = RELEASED;
            next_at = make_timeout_time_us(KEY_GAP_US);
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
