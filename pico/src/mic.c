// Continuous I2S microphone capture.
//
// PIO clocks the mic at 32kHz (64 SCK per frame = 2.048MHz), which keeps both
// the SPH0645 (1.024-4.096MHz) and ICS-43434 (high-performance mode needs
// >= 23kHz) comfortably in range. Two chained DMA channels fill a ring of
// blocks with raw 32-bit slots, and mic_read() converts them to 16kHz mono
// 16-bit PCM: pick one channel, half-band low-pass, decimate by 2, remove DC.

#include <math.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "mic.h"
#include "mic_i2s.pio.h"
#include "pins.h"

#define CAPTURE_RATE  (MIC_OUTPUT_RATE * 2)
#define SCK_PER_FRAME 64
#define PIO_CYCLES_PER_SCK 2

// Channel the mic is configured for with its SEL / L/R pin (0 = left, 1 = right)
#ifndef MIC_CHANNEL
#define MIC_CHANNEL 0
#endif

// Ring of DMA blocks. Each block is 256 stereo frames (8ms); 16 blocks give
// mic_read() ~110ms of slack before audio is dropped.
#define FRAMES_PER_BLOCK 256
#define WORDS_PER_BLOCK  (FRAMES_PER_BLOCK * 2)
#define NUM_BLOCKS       16

static uint32_t blocks[NUM_BLOCKS][WORDS_PER_BLOCK];
static int dma_ch[2];

// Blocks completed by DMA (written by IRQ) and blocks processed by mic_read
static volatile uint32_t produced;
static uint32_t consumed;
static uint32_t overruns;

// Half-band FIR low-pass for 2:1 decimation: Blackman-windowed sinc, cutoff at
// a quarter of the capture rate. Flat to ~7kHz, -76dB by 10kHz.
#define FIR_TAPS 63
static float fir_coeffs[FIR_TAPS];
static float fir_history[FIR_TAPS * 2]; // doubled so each window is contiguous
static unsigned fir_pos;
static bool fir_odd;

// DC blocker state: y[n] = x[n] - x[n-1] + R * y[n-1]
#define DC_R 0.995f
static float dc_x, dc_y;

static void fir_init(void) {
    const int mid = (FIR_TAPS - 1) / 2;
    float sum = 0;
    for (int i = 0; i < FIR_TAPS; i++) {
        int n = i - mid;
        float sinc = n == 0 ? 0.5f : sinf((float)M_PI * n / 2) / ((float)M_PI * n);
        float window = 0.42f - 0.5f * cosf(2 * (float)M_PI * i / (FIR_TAPS - 1))
                     + 0.08f * cosf(4 * (float)M_PI * i / (FIR_TAPS - 1));
        fir_coeffs[i] = sinc * window;
        sum += fir_coeffs[i];
    }
    for (int i = 0; i < FIR_TAPS; i++) {
        fir_coeffs[i] /= sum; // unity gain at DC
    }
}

// Feed one 32kHz sample; returns true and sets *out every second sample
static bool decimate(float in, float *out) {
    fir_history[fir_pos] = in;
    fir_history[fir_pos + FIR_TAPS] = in;
    fir_pos = (fir_pos + 1) % FIR_TAPS;

    fir_odd = !fir_odd;
    if (fir_odd) {
        return false;
    }

    // Oldest sample is at fir_pos; the filter is symmetric so order doesn't matter
    const float *window = &fir_history[fir_pos];
    float acc = 0;
    for (int i = 0; i < FIR_TAPS; i++) {
        acc += window[i] * fir_coeffs[i];
    }
    *out = acc;
    return true;
}

static int16_t to_pcm16(float x) {
    // DC blocker
    float y = x - dc_x + DC_R * dc_y;
    dc_x = x;
    dc_y = y;

    float s = y * 32768.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

static void __isr dma_irq_handler(void) {
    for (int i = 0; i < 2; i++) {
        if (dma_channel_get_irq0_status(dma_ch[i])) {
            dma_channel_acknowledge_irq0(dma_ch[i]);
            produced++;
            // The other channel is now filling the next block; queue this one
            // for the block after that. It starts when the other one finishes.
            dma_channel_set_write_addr(dma_ch[i], blocks[(produced + 1) % NUM_BLOCKS], false);
        }
    }
}

void mic_init(void) {
    fir_init();

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);

#if MIC_SAMPLE_ON_FALLING_EDGE
    uint offset = pio_add_program(pio, &i2s_in_falling_program);
    pio_sm_config cfg = i2s_in_falling_program_get_default_config(offset);
#else
    uint offset = pio_add_program(pio, &i2s_in_rising_program);
    pio_sm_config cfg = i2s_in_rising_program_get_default_config(offset);
#endif

    sm_config_set_sideset_pins(&cfg, MIC_SCK_PIN);
    sm_config_set_in_pins(&cfg, MIC_SD_PIN);
    sm_config_set_in_shift(&cfg, false, true, 32); // MSB first, autopush per slot
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys)
                                   / (CAPTURE_RATE * SCK_PER_FRAME * PIO_CYCLES_PER_SCK));

    pio_gpio_init(pio, MIC_SCK_PIN);
    pio_gpio_init(pio, MIC_WS_PIN);
    pio_gpio_init(pio, MIC_SD_PIN);
    gpio_pull_down(MIC_SD_PIN); // the mic tri-states SD during the other channel
    pio_sm_set_consecutive_pindirs(pio, sm, MIC_SCK_PIN, 2, true);
    pio_sm_set_consecutive_pindirs(pio, sm, MIC_SD_PIN, 1, false);

    pio_sm_init(pio, sm, offset, &cfg);

    // Two DMA channels, each chained to the other, alternate through the ring
    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
        channel_config_set_chain_to(&c, dma_ch[1 - i]);
        dma_channel_configure(dma_ch[i], &c, blocks[i], &pio->rxf[sm], WORDS_PER_BLOCK, false);
        dma_channel_set_irq0_enabled(dma_ch[i], true);
    }
    irq_add_shared_handler(DMA_IRQ_0, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(dma_ch[0]);
    pio_sm_set_enabled(pio, sm, true);
}

size_t mic_read(int16_t *out, size_t max) {
    size_t n = 0;

    // Two blocks are owned by DMA at any time: the one being written and the
    // one queued next. Anything older than that has been overwritten.
    uint32_t done = produced;
    if (done - consumed > NUM_BLOCKS - 2) {
        uint32_t skip = done - consumed - (NUM_BLOCKS - 2);
        overruns += skip;
        consumed += skip;
    }

    while (consumed != done && max - n >= FRAMES_PER_BLOCK / 2) {
        const uint32_t *block = blocks[consumed % NUM_BLOCKS];
        for (int i = 0; i < FRAMES_PER_BLOCK; i++) {
            // Samples are left-justified in the 32-bit slot
            float x = (float)(int32_t)block[i * 2 + MIC_CHANNEL] / 2147483648.0f;
            float y;
            if (decimate(x, &y)) {
                out[n++] = to_pcm16(y);
            }
        }
        consumed++;
    }
    return n;
}

uint32_t mic_overruns(void) {
    return overruns;
}
