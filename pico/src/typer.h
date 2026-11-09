#ifndef TYPER_H
#define TYPER_H

#include <stdbool.h>
#include <stdint.h>

// Queue one character to be typed. Returns false if the queue is full.
bool typer_putc(char ch);

// Key press duration and pause after release, in microseconds. Takes effect
// immediately, including for text already queued.
void typer_set_delays(uint32_t hold_us, uint32_t gap_us);

// Text queued or a key still being pressed
bool typer_busy(void);

// Advance the key press/release state machine. Call from the main loop.
void typer_task(void);

#endif
