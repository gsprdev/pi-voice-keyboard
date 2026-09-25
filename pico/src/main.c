// Voice Keyboard firmware for Raspberry Pi Pico 2 W.
//
// Stage 1: characters written to the USB serial console are typed on the
// USB keyboard. Later stages replace the console input with transcriptions.

#include <stdio.h>

#include "pico/stdlib.h"
#include "tusb.h"
#include "typer.h"

int main(void) {
    // TinyUSB must be up before stdio_usb, since we provide the descriptors
    tusb_init();
    stdio_init_all();

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
    }
}
