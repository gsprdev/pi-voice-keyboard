#ifndef MIC_H
#define MIC_H

#include <stddef.h>
#include <stdint.h>

// Output format: 16kHz mono signed 16-bit PCM, the transcription service's input
#define MIC_OUTPUT_RATE 16000

// Start continuous capture from the I2S microphone.
void mic_init(void);

// Process captured audio into out (up to max samples). Returns the number of
// samples written. Call regularly: audio not read within ~100ms is dropped.
size_t mic_read(int16_t *out, size_t max);

// Number of capture blocks dropped because mic_read wasn't called in time
uint32_t mic_overruns(void);

#endif
