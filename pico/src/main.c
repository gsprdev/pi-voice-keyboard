// Voice Keyboard firmware for Raspberry Pi Pico 2 W and Pico 2.
//
// Hold the button to stream audio to the transcription service, over USB to
// the host or over Wi-Fi; release to have the transcription typed on the USB
// keyboard.
//
// Development aids on the USB serial ports: text written to the console is
// typed directly, and the audio port streams raw microphone PCM while open.

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
    // Summarize state whenever a console is opened, since earlier logs are lost
    static bool was_connected;
    bool connected = stdio_usb_connected();
    if (connected && !was_connected) {
        printf("Voice Keyboard ready\n");
        net_report();
        service_report();
    }
    was_connected = connected;

    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (!typer_putc((char)c)) {
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
