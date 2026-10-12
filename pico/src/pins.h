#ifndef PINS_H
#define PINS_H

// GPIO assignments. GPIO 23-25 and 29 are avoided: on the Pico 2 W they are
// wired to the Wi-Fi chip, and on the Pico 2 to VBUS sense, SMPS mode, LED
// and VSYS sense. The same wiring therefore works on both boards.

// I2S microphone. The PIO program drives SCK and WS as side-set pins, which
// must be consecutive: WS is always MIC_SCK_PIN + 1.
#define MIC_SCK_PIN 10
#define MIC_WS_PIN  (MIC_SCK_PIN + 1)
#define MIC_SD_PIN  12

// Push-to-talk and feedback (used from stage 3 onward)
#define BUTTON_PIN         14
#define LED_RECORDING_PIN  16
#define LED_PROCESSING_PIN 17
#define BUZZER_PIN         18

#endif
