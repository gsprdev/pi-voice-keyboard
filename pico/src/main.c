// Voice Keyboard firmware for Raspberry Pi Pico 2 W and Pico 2.
//
// Hold the button to stream audio to the transcription service, over USB to
// the host or over Wi-Fi; release to have the transcription typed on the USB
// keyboard.
//
// Development aids on the USB serial ports: text written to the console is
// typed directly (or sets the typing delays, see console_task), and the audio
// port streams raw microphone PCM while open.

#include <stdio.h>

#include "feedback.h"
#include "mic.h"
#include "net.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "ptt.h"
#include "service.h"
#include "tusb.h"
#include "typer.h"

#define CDC_AUDIO 1

static void audio_task(void) {
    static int16_t samples[512];
    static uint32_t dropped, last_dropped, last_overruns;

    // Always drain the mic so the filters stay continuous
    size_t n = mic_read(samples, TU_ARRAY_SIZE(samples));
    if (n == 0) {
        return;
    }

    ptt_audio(samples, n);

    if (tud_cdc_n_connected(CDC_AUDIO)) {
        uint32_t bytes = n * sizeof(samples[0]);
        if (tud_cdc_n_write_available(CDC_AUDIO) >= bytes) {
            tud_cdc_n_write(CDC_AUDIO, samples, bytes);
            tud_cdc_n_write_flush(CDC_AUDIO);
        } else {
            dropped += n;
        }
    }

    if (dropped != last_dropped) {
        printf("Audio port not keeping up, %lu samples dropped\n", (unsigned long)dropped);
        last_dropped = dropped;
    }
    if (mic_overruns() != last_overruns) {
        last_overruns = mic_overruns();
        printf("Mic overrun, %lu blocks dropped\n", (unsigned long)last_overruns);
    }
}

static void console_task(void) {
    // Summarize state whenever a console is opened, since earlier logs are lost.
    // Wait a moment first: a freshly enumerated port echoes until the program
    // that opened it sets raw mode, and echoed logs would be typed.
    // ponytail: fixed delay, a host slower than this still gets the echo
    static bool was_connected;
    static absolute_time_t greet_at = 0; // 0: nothing pending
    bool connected = stdio_usb_connected();
    if (connected && !was_connected) {
        greet_at = make_timeout_time_ms(250);
    }
    if (!connected) {
        greet_at = 0;
    } else if (greet_at && time_reached(greet_at)) {
        greet_at = 0;
        printf("Voice Keyboard ready\n");
        net_report();
        service_report();
    }
    was_connected = connected;

    // A line starting with SOH (^A) is a command instead of text:
    // "\x01<hold_us> <gap_us>\n" sets the typing delays (tools/typing-test.py)
    static char cmd[32];
    static int cmd_len = -1; // -1: not in a command

    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\x01') {
            cmd_len = 0;
        } else if (cmd_len >= 0) {
            if (c != '\n') {
                if (cmd_len < (int)sizeof(cmd) - 1) {
                    cmd[cmd_len++] = (char)c;
                }
                continue;
            }
            cmd[cmd_len] = '\0';
            cmd_len = -1;
            unsigned long hold, gap;
            if (sscanf(cmd, "%lu %lu", &hold, &gap) == 2) {
                typer_set_delays(hold, gap);
            } else {
                printf("Bad command: %s\n", cmd);
            }
        } else if (!typer_putc((char)c)) {
            printf("Typing queue full, dropping input\n");
            break;
        }
    }
}

int main(void) {
    // Loads the Wi-Fi chip's firmware; done first so USB enumeration isn't stalled
    net_init();
    // TinyUSB must be up before stdio_usb, since we provide the descriptors
    tusb_init();
    stdio_init_all();
    mic_init();
    feedback_init();
    ptt_init();
    service_init();

    while (true) {
        tud_task();
        net_task();
        service_task();
        console_task();
        audio_task();
        ptt_task();
        typer_task();
        feedback_task();
    }
}
