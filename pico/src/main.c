// Voice Keyboard firmware for Raspberry Pi Pico 2 W.
//
// Stage 1: characters written to the USB serial console are typed on the
// USB keyboard. Later stages replace the console input with transcriptions.
//
// Stage 2: the microphone runs continuously. While the audio serial port is
// open, 16kHz mono 16-bit PCM is streamed to it for verifying capture.

#include <stdio.h>

#include "mic.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "typer.h"

#define CDC_AUDIO 1

static void audio_task(void) {
    static int16_t samples[512];
    static uint32_t dropped, last_dropped, last_overruns;

    // Always drain the mic so the filters stay continuous
    size_t n = mic_read(samples, TU_ARRAY_SIZE(samples));
    if (n > 0 && tud_cdc_n_connected(CDC_AUDIO)) {
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

int main(void) {
    // TinyUSB must be up before stdio_usb, since we provide the descriptors
    tusb_init();
    stdio_init_all();
    mic_init();

    printf("Voice Keyboard ready\n");

    while (true) {
        tud_task();

        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            if (!typer_putc((char)c)) {
                printf("Typing queue full, dropping input\n");
                break;
            }
        }

        typer_task();
        audio_task();
    }
}
