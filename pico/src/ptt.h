#ifndef PTT_H
#define PTT_H

#include <stddef.h>
#include <stdint.h>

// Push-to-talk: hold the button to record, release to transcribe and type.
// Port of pi/ptt.py.

void ptt_init(void);

// Handle the button and transcription results. Call from the main loop.
void ptt_task(void);

// Hand captured 16kHz audio to the recording in progress, if any
void ptt_audio(const int16_t *samples, size_t count);

#endif
