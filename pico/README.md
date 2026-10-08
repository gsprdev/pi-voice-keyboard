# Keyboard (Raspberry Pi Pico 2 W)

Firmware replacing the Pi Zero 2 W keyboard with a Pico 2 W microcontroller.
The transcription service is unchanged: audio is streamed to `POST /transcribe` and the response is typed.

See [PLAN.md](PLAN.md) for design decisions, pin assignments, and the staged roadmap.

## Status

1. **USB keyboard**: done
2. **Microphone**: done
3. **Wi-Fi streaming**: built, awaiting hardware verification
4. Multiple servers
5. USB networking
6. Polish

## USB interfaces

The Pico appears as a composite device:

- **HID boot keyboard**: the keyboard itself, same report format as the Pi gadget
- **Console** (`/dev/ttyACM0` on Linux): logs, and text typed directly for testing
- **Audio** (`/dev/ttyACM1`): raw 16 kHz mono 16-bit PCM while the port is open

## Build

Wi-Fi settings are compiled in. Copy the example and fill it in (it's git-ignored):

```sh
cp src/secrets.h.example src/secrets.h
```

```sh
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
./build.sh                             # Pico 2 W: build/voice-keyboard.uf2
PICO_BOARD=pico2 ./build.sh            # Plain Pico 2, no networking: build-pico2/voice-keyboard.uf2
MIC_TIMING=standard ./build.sh         # Standard I2S mic (ICS-43434 etc.) instead of SPH0645
```

pico-sdk is a git submodule.
`build.sh` fetches it and the libraries it needs automatically.

## Flash

First time: hold BOOTSEL while plugging in the Pico, then copy the `.uf2` to the `RP2350` drive that appears.

Afterwards, the firmware can be put back into BOOTSEL mode without touching the button:

```sh
stty -F /dev/ttyACM0 1200         # Reboots into BOOTSEL; the RP2350 drive reappears
```

## Wiring

See the pin table in [PLAN.md](PLAN.md#pins).

- Microphone: SCK → GP10, WS → GP11, SD → GP12, SEL → GND, plus 3V3 and GND
- Button: GP14 to GND (internal pull-up)
- Recording LED: GP16, processing LED: GP17 (each through a resistor to GND)
- Buzzer (passive): GP18 to GND

## Use

Hold the button and speak; release to have the transcription typed.

- **Recording LED + beep:** recording, audio streaming to the service
- **Processing LED:** waiting for the transcription
- **Three beeps with the recording LED:** not ready (Wi-Fi down or service unavailable), or the upload failed

Open the console (`/dev/ttyACM0`) to see why: it prints Wi-Fi and service status when opened, and logs each step.

## Try it

### Stage 1: keyboard

Anything written to the console is typed on the keyboard, into whichever window has focus.
Delay the write so there's time to focus a text editor:

```sh
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0 &                # Watch logs
sleep 3; printf 'Hello, world!\n' > /dev/ttyACM0
```

Supported characters match `pi/type-ascii.py`: printable ASCII, tab, newline, and backspace.
Anything else is skipped and logged.

### Stage 2: microphone

Record from the audio port, then listen to the result:

```sh
tools/capture.py test.wav                                      # 5 seconds, prints levels
aplay test.wav
tools/capture.py test.wav --transcribe http://gpu-host:8080   # Also transcribe it
```

The levels line (peak, RMS, DC offset, clipped samples) helps diagnose wiring and timing problems.
See [Verifying stage 2](PLAN.md#verifying-stage-2).
