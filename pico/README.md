# Keyboard (Raspberry Pi Pico 2 W)

Firmware replacing the Pi Zero 2 W keyboard with a Pico 2 W microcontroller.
The transcription service is unchanged: audio is streamed to `POST /transcribe` and the response is typed.

## Roadmap

1. **USB keyboard** *(current)*: text written to the USB serial console is typed on the USB keyboard
2. **Microphone**: ICS-43434 via PIO I2S; raw audio dumped over the serial console to verify capture
3. **Wi-Fi streaming**: chunked upload to `/transcribe` while the button is held
4. **Multiple servers**: priority list with background health checks (from the `multi-backend` branch)
5. **USB networking**: USB host as a transcription server, CDC-NCM alongside Wi-Fi (from the `usb-networking` branch)
6. **Polish**: buzzer, LEDs, watchdog, configuration

## USB interfaces

The Pico appears as a composite device:

- **HID boot keyboard**: the keyboard itself, same report format as the Pi gadget
- **CDC serial console** (`/dev/ttyACM0` on Linux): logs, and text input for stage 1

## Build

```sh
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
./build.sh                        # Output: build/voice-keyboard.uf2
```

pico-sdk is a git submodule.
`build.sh` fetches it and the libraries it needs automatically.

## Flash

First time: hold BOOTSEL while plugging in the Pico, then copy `build/voice-keyboard.uf2` to the `RP2350` drive that appears.

Afterwards, the firmware can be put back into BOOTSEL mode without touching the button:

```sh
stty -F /dev/ttyACM0 1200         # Reboots into BOOTSEL; the RP2350 drive reappears
```

## Try it (stage 1)

Anything written to the serial console is typed on the keyboard, into whichever window has focus.
Delay the write so there's time to focus a text editor:

```sh
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0 &                # Watch logs
sleep 3; printf 'Hello, world!\n' > /dev/ttyACM0
```

Supported characters match `pi/type-ascii.py`: printable ASCII, tab, newline, and backspace.
Anything else is skipped and logged.
