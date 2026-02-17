# Consumer (Raspberry Pi)

Scripts and services for running a Raspberry Pi Zero 2 W as a USB HID keyboard gadget with push-to-talk dictation.

## Hardware Setup

Enable USB OTG and mic support in `/boot/config.txt`. Key settings:

```
# Allows function as USB gadget
otg_mode=1

# MEMS I2C Card (microphone)
dtparam=i2s=on
dtparam=audio=on
dtoverlay=googlevoicehat-soundcard
```

Wire GPIO components.
For simplicity, all components lead from a single GPIO to GND.

- Button: GPIO 24 (pull-up, active low)
- Recording LED: GPIO 17
- Processing LED: GPIO 22
- Status buzzer: GPIO 27

## Installation

```bash
# One-time setup (installs to /usr/sbin and /etc/systemd/system)
cd consumer
sudo ./gadget-install.sh

# Enable and start services
sudo systemctl enable --now kb-serve.socket
sudo systemctl enable --now type-ascii.service
sudo systemctl enable --now ptt.service
```

## Services

### type-ascii.service

Python service that converts text to USB HID keyboard reports via `/dev/hidg0`.
Uses systemd socket activation to listen on `/run/kb-serve/kb.sock`.

### kb-serve.socket

Systemd socket unit managing the Unix domain socket for keyboard input.

### ptt.service

Push-to-talk service monitoring GPIO button.
Records audio on press, sends to transcription service, types the result.

## Configuration

**REQUIRED:** Create `/etc/default/ptt` from the example template:

```bash
sudo cp ptt.env.example /etc/default/ptt
sudo nano /etc/default/ptt
```

Set this environment variable:
- `PTT_SERVICE_URL` - Base URL to transcription service (e.g., `http://gpu-host.local:8080`)

The service will check `/health` endpoint at startup and use `/transcribe` for audio processing.

## Removal

```bash
sudo gadget-uninstall
```

## Testing

```bash
# Type text directly via the socket
echo "Hello from Pi!" | socat - UNIX-CONNECT:/run/kb-serve/kb.sock
```
