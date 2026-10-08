# Pico Firmware Plan

Replace the Raspberry Pi Zero 2 W (scarce, scalped) with a Raspberry Pi Pico 2 W.
The goal is feature parity with `pi/` in its minimal form, then the shelved branches on top.

## Why a microcontroller works

Everything the Pi does maps onto the Pico; nothing needs Linux.

| Pi | Pico |
|---|---|
| configfs HID gadget, `type-ascii.py` | TinyUSB HID boot keyboard, `src/typer.c` |
| `arecord`, googlevoicehat overlay | PIO I2S receiver + DMA, `src/mic.c` |
| gpiozero button, LEDs, `TonalBuzzer` | GPIO, hardware PWM |
| `urlopen` health check and POST | lwIP over Wi-Fi |
| Unix socket between services | Not needed: one program |
| systemd `Restart=` | Hardware watchdog |
| SSH + journalctl | USB serial console |

Memory (520 KB) rules out buffering a whole recording, so audio is streamed while the button is held.

## Decisions

- **Board:** Pico 2 W. A plain Pico 2 runs every stage except Wi-Fi (`PICO_BOARD=pico2 ./build.sh`).
- **Language:** C with pico-sdk (submodule) and TinyUSB.
- **Microphone:** SPH0645 (Adafruit 3421), the mic the Pi build uses.
  It is non-standard: data changes on the rising clock edge, so it is sampled on the falling edge (`MIC_TIMING=sph0645`, the default).
  Standard I2S mics (ICS-43434, INMP441, cheap modules) use `MIC_TIMING=standard`.
  A PDM mic is the long-term supply hedge if I2S breakouts dry up; only `src/mic.c` would change.
- **Capture rate:** the mic is clocked at 32 kHz (2.048 MHz SCK) and decimated to 16 kHz in firmware.
  16 kHz directly would put the SPH0645 at its 1.024 MHz minimum clock and the ICS-43434 in its low-power mode (high-performance mode needs at least 23 kHz).
- **Audio processing:** pick the mic's channel (SEL / L/R pin low = left), 63-tap half-band low-pass (flat to ~7 kHz), decimate by 2, DC blocker, top 16 bits.
  No gain, matching what `arecord` produced on the Pi.
- **Mic runs continuously** from boot. This avoids mic startup time on each press and keeps filters settled.
  A short pre-roll from before the press is possible later.
- **Transport:** HTTP POST to the existing `/transcribe` with `Transfer-Encoding: chunked`, sending a WAV header with placeholder sizes, then PCM as it's captured.
  No server change needed: `loadWAV` skips the 44-byte header and reads to EOF.
- **Name resolution:** DHCP-provided or manually set DNS servers via lwIP; no mDNS.
- **Server readiness, not failproofing:** health checks run in the background (every 10 s, 1 s timeout), so a press starts recording immediately instead of waiting on a check.
  No audio buffering or retry if a stream fails mid-recording, just an error beep.
  A 2 s send buffer rides out Wi-Fi hiccups; if it overflows, the recording is abandoned.
- **Not ready aborts** recording (the Pi build beeps but records anyway).
- **Start beep is not sent:** the first 150 ms after a press (the beep plus capture latency) are dropped rather than transcribed.
- **Text cleanup** (`clean_transcription` in `ptt.py`) is ported to `src/text.c`, matching the Python regex behavior exactly.
- **Configuration:** Wi-Fi credentials and service URL are compiled in from `src/secrets.h` (git-ignored, from `secrets.h.example`); configuration over the serial console later.
- **Wi-Fi:** WPA2 (also joins WPA2/WPA3 mixed networks, not WPA3-only), power saving off since the device is USB powered.
  lwIP runs in poll mode from the main loop, so there are no threads or locks.

## USB interfaces

One composite device:

1. **HID boot keyboard**: same report format and VID/PID as the Pi gadget
2. **CDC console** (`/dev/ttyACM0`): logs, stage 1 text input, 1200-baud reboot into BOOTSEL
3. **CDC audio** (`/dev/ttyACM1`): raw 16 kHz mono PCM while the port is open (stage 2 verification)

The CDC interfaces are development aids and may be compiled out of production builds later.

## Pins

GPIO 23-25 and 29 are avoided (Wi-Fi chip on the Pico 2 W; VBUS/SMPS/LED/VSYS on the Pico 2), so wiring is the same on both boards.
PIO side-set requires SCK and WS to be consecutive.

| Function | GPIO | Pico pin |
|---|---|---|
| Mic SCK (BCLK) | 10 | 14 |
| Mic WS (LRCL) | 11 | 15 |
| Mic SD (DOUT) | 12 | 16 |
| Mic SEL | GND (left channel) | |
| Button (to GND) | 14 | 19 |
| Recording LED | 16 | 21 |
| Processing LED | 17 | 22 |
| Buzzer | 18 | 24 |

Mic power: 3V3 (pin 36) and GND.

## Stages

1. **USB keyboard** *(done, verified on hardware)*: console text is typed on the keyboard.
2. **Microphone** *(done, verified on hardware)*: capture to the audio port; `tools/capture.py` saves a WAV, reports levels, and can POST it to `/transcribe`.
3. **Wi-Fi streaming** *(built, awaiting hardware)*: button, LEDs, buzzer; background health checks; chunked upload while held; type the response.
4. **Multiple servers**: priority list with background health checks, ported from the `multi-backend` branch.
   Polling pauses while streaming.
5. **USB networking**: USB host as a transcription server, from the `usb-networking` branch.
   CDC-NCM instead of ECM (native Windows support), host-side MAC `aa:bb:cc:dd:ee:01` so `host/host-setup.sh` works unchanged.
   Two lwIP interfaces (Wi-Fi + USB) sharing one stack; USB link-up triggers an immediate health check.
6. **Polish**: watchdog, configuration over serial, optional production build without CDC.

The `multi-backend` and `usb-networking` branches are shelved until stage 3 is working.

## Verifying stage 2

Signs of trouble in `tools/capture.py` output:

- **All zeros:** SD not connected, mic unpowered, or SEL selecting the other channel.
- **Loud, harsh, frequent clipping:** sampling on the wrong edge (one-bit shift); try the other `MIC_TIMING`.
- **Very quiet but clean:** possibly shifted the other way; also try the other `MIC_TIMING`.
- **Large DC offset:** should be removed by the DC blocker; report if not.
