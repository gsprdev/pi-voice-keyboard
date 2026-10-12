#ifndef TYPER_H
#define TYPER_H

#include <stdbool.h>

// Queue one character to be typed. Returns false if the queue is full.
bool typer_putc(char ch);

// Text queued or a key still being pressed
bool typer_busy(void);

// Advance the key press/release state machine. Call from the main loop.
void typer_task(void);

#endif
