#include "feedback.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "pins.h"

// Passive buzzer driven with a square wave, as gpiozero's TonalBuzzer did
#define BUZZER_HZ 400
#define BEEP_MS   100

static uint buzzer_slice;
static uint16_t buzzer_wrap;

// Beep pattern: remaining on/off steps, each BEEP_MS long
static int steps_left;
static bool pattern_flashes_led;
static absolute_time_t next_step;
static bool recording_led;

static void buzzer(bool on) {
    pwm_set_gpio_level(BUZZER_PIN, on ? buzzer_wrap / 2 : 0);
}

void feedback_init(void) {
    gpio_init(LED_RECORDING_PIN);
    gpio_set_dir(LED_RECORDING_PIN, GPIO_OUT);
    gpio_init(LED_PROCESSING_PIN);
    gpio_set_dir(LED_PROCESSING_PIN, GPIO_OUT);

    // Divide the system clock so BUZZER_HZ fits the 16-bit counter
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    buzzer_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    float div = 64.0f;
    buzzer_wrap = (uint16_t)(clock_get_hz(clk_sys) / div / BUZZER_HZ - 1);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, div);
    pwm_config_set_wrap(&cfg, buzzer_wrap);
    pwm_init(buzzer_slice, &cfg, true);
    buzzer(false);
}

static void start_pattern(int beeps, bool flash_led) {
    steps_left = beeps * 2;
    pattern_flashes_led = flash_led;
    next_step = get_absolute_time();
}

void feedback_task(void) {
    if (steps_left == 0 || !time_reached(next_step)) {
        return;
    }
    steps_left--;
    bool on = steps_left % 2 == 1; // odd steps remaining: beep; even: gap
    buzzer(on);
    if (pattern_flashes_led) {
        gpio_put(LED_RECORDING_PIN, on || recording_led);
    }
    next_step = make_timeout_time_ms(BEEP_MS);
}

void feedback_recording(bool on) {
    recording_led = on;
    gpio_put(LED_RECORDING_PIN, on);
}

void feedback_processing(bool on) {
    gpio_put(LED_PROCESSING_PIN, on);
}

void feedback_beep(void) {
    start_pattern(1, false);
}

void feedback_error(void) {
    start_pattern(3, true);
}
