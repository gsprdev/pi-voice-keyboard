#ifndef TYPER_H
#define TYPER_H

#include <stdbool.h>

// Set up the speed knob, when built with it
void typer_init(void);

// Queue one character to be typed. Returns false if the queue is full.
bool typer_putc(char ch);

// Room for another character. Console input is read only while this holds,
// so a full queue backs up into USB flow control instead of dropping text.
bool typer_has_room(void);

// Text queued or a key still being pressed
bool typer_busy(void);

// Advance the key press/release state machine. Call from the main loop.
void typer_task(void);

#endif
