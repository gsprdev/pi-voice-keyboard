# Consumer Scripts (Raspberry Pi)

Copy all of these files to the Raspberry Pi Zero 2 W.

Scripts for setting up and running a Raspberry Pi Zero 2 W as a USB HID keyboard gadget when connected via USB OTG cable.

## config.txt

```
# OTG mode required to function as a USB gadget (Keyboard)
[cm4]
otg_mode=1
```

Run the ./gadget-install.sh script for setup.

### Installing the Keyboard Service

The installation script installs kb-serve.service and ptt.service on the Raspberry Pi as systemd services.

```bash
# Install the USB gadget (one-time setup)
sudo ./gadget-install.sh
systemctl enable --now kb-serve.service
```

### Removal

The installation process adds `gadget-uninstall` to sbin, for complete reset.
As this is for Pi, designed primarily for this sole purpose, the uninstallation script serves primarily to facilitate
testing of reinstallation.

```bash
sudo gadget-uninstall
```

## Keyboard Service

The keyboard service listens on a Unix socket and types any text it receives as HID keyboard input.

### Components

- **type-ascii.py**: Python script that converts text to HID keyboard reports
- **kb-serve.sh**: Wrapper that exposes the Unix socket over TCP (port 1234)

### Testing Locally

```bash
# Type text directly via the Unix socket
echo "Hello from Pi!" | socat - UNIX-CONNECT:/tmp/kb.sock

# Type text via TCP (from another machine)
echo "Hello from remote!" | nc <pi-ip> 1234
```
