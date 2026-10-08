#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stdbool.h>

// LEDs and buzzer, without blocking the main loop

void feedback_init(void);

// Advance beep patterns. Call from the main loop.
void feedback_task(void);

void feedback_recording(bool on);
void feedback_processing(bool on);

// Short beep: recording started
void feedback_beep(void);

// Three beeps with the recording LED: something went wrong
void feedback_error(void);

#endif
