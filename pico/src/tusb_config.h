#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO

#define CFG_TUD_ENDPOINT0_SIZE  64

// Composite device: HID keyboard + two CDC serial ports
// (console: logs and text input; audio: raw microphone capture)
#define CFG_TUD_HID             1
#define CFG_TUD_CDC             2
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_HID_EP_BUFSIZE  8

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  4096 // ~125ms of 16kHz audio
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif
