# Keyboard (Raspberry Pi Pico 2 W)

Firmware replacing the Pi Zero 2 W keyboard with a Pico 2 W microcontroller.
The transcription service is unchanged: audio is streamed to `POST /transcribe` and the response is typed.

See [PLAN.md](PLAN.md) for design decisions, pin assignments, and the staged roadmap.

## Status

1. **USB keyboard**: done
2. **Microphone**: done
3. **Wi-Fi streaming**: done
4. **Multiple servers and readiness**: built, awaiting hardware verification
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
mise run pico:build                     # Pico 2 W: build/voice-keyboard.uf2
PICO_BOARD=pico2 mise run pico:build    # Plain Pico 2, no networking: build-pico2/voice-keyboard.uf2
MIC_TIMING=standard mise run pico:build # Standard I2S mic (ICS-43434 etc.) instead of SPH0645
```

pico-sdk is a git submodule.
`mise run pico:build` fetches it and the libraries it needs automatically.

## Serial permissions

`/dev/ttyACM*` is `root:dialout` by default, so `stty` and `tools/capture.py` fail as a normal user.
Install the udev rule once to grant the logged-in user access, with no root or group change needed:

```sh
sudo cp 70-pico-keyboard.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

Then replug the Pico.
Alternatively, `sudo usermod -aG dialout $USER` and log in again.

## Flash

First time: hold BOOTSEL while plugging in the Pico, then copy the `.uf2` to the `RP2350` drive that appears.

Afterwards, `mise run pico:flash` builds, reboots the running firmware into BOOTSEL (opening `/dev/ttyACM0` at 1200 baud), and copies the `.uf2` once the drive appears, mounting it if the desktop hasn't.
It also takes `PICO_BOARD`.

## Wiring

See the pin table in [PLAN.md](PLAN.md#pins).

- Microphone: SCK → GP10, WS → GP11, SD → GP12, SEL → GND, plus 3V3 and GND
- Button: GP14 to GND (internal pull-up)
- Recording LED: GP16, processing LED: GP17 (each through a resistor to GND)
- Buzzer (passive): GP18 to GND

## Use

Hold the button, wait for the beep, and speak; release to have the transcription typed.

- **Recording LED + beep:** a server is accepting the recording; speak now.
  The servers in `SERVICE_URLS` are tried in order, up to 300 ms each, until one is ready.
- **Processing LED:** waiting for the transcription, or typing it.
  You can press for the next one any time: it starts once the previous transcription is back, and can overlap the typing.
- **Three beeps with the recording LED:** no server ready (or Wi-Fi down), or the upload failed

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
