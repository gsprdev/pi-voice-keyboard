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

## Web Interface

A web-based dictation interface that allows any device with a browser to record audio and send it for transcription.

### Architecture

```
[Browser] --HTTPS--> [Pi Web Server :8081]
                            |
                            v
                   [GPU Service :8080] (HTTP tunnel)
                            |
                            v
                   [Pi types via HID]
```

### Installing the Web Interface

The web interface should be built before deployment to the Pi, e.g.

```bash
cd consumer
./build-web-server.sh
scp build/web-server pi@<pi-ip>:/home/pi/
```

Then install the web server as a systemd service, or launch it manually.
No predefined unit files are yet included for this process.

### Configuration

On first run, the server generates a random API key and saves it to `web-server-config.txt`:

```bash
./web-server
```

Output:
```
Generated new API key and saved to web-server-config.txt
Your API key: alpha-bravo-charlie-delta-42
Starting web server on port 8081
```

**Important:** Save this API key - you'll need to enter it in the web interface.

### Generating New API Keys

```bash
# Generate a new API key
./web-server -generate-key

# Output: alpha-bravo-charlie-delta-42
```

Then update `web-server-config.txt`:

```
API_KEY=alpha-bravo-charlie-delta-42
```

### Running the Web Server

```bash
# Start the web server (default: port 8081)
./web-server

# Custom port
./web-server -port 9000

# Custom transcription service URL
./web-server -transcription-url http://192.168.1.100:8080/transcribe
```

### Using the Web Interface

1. **Start required services:**
   ```bash
   # Terminal 1: Start keyboard service
   ./kb-serve.sh

   # Terminal 2: Start web server
   ./web-server
   ```

2. **Access from mobile device:**
   - Open browser to `http://<pi-ip>:8081`
   - Enter the API key shown in the server output
   - Tap "Connect"

3. **Record audio:**
   - Tap the "TAP TO RECORD" button
   - Speak your dictation
   - Tap again to stop recording
   - Transcribed text will be typed on the connected device

### Status Indicators

- **Ready** - Server is ready for recording
- **Recording...** - Audio is being captured
- **Processing...** - Audio is being transcribed
- **Success!** - Transcription complete, text typed
- **Authentication failed** - Invalid API key
- **Transcription failed** - Server error

### Configuration Options

Edit `web-server-config.txt`:

```bash
# API key for browser authentication
API_KEY=alpha-bravo-charlie-delta-42

# Port for web server
PORT=8081

# Transcription service URL (GPU service)
TRANSCRIPTION_URL=http://localhost:8080/transcribe

# Unix socket path for type-ascii
SOCKET_PATH=/tmp/kb.sock
```

### Security Notes

- **API Key:** Simple shared secret for home network use. Change regularly using `-generate-key`.
- **HTTPS:** Not currently implemented. For local network use only.
- **Network:** Ensure web server is only accessible on trusted networks.

### Troubleshooting

**"Failed to connect to socket"**
- Ensure `kb-serve.sh` is running
- Check that `/tmp/kb.sock` exists

**"Transcription failed"**
- Verify transcription service is running on GPU system
- Check `TRANSCRIPTION_URL` in config
- Verify SSH tunnel is active (if using remote GPU)

**"Microphone access denied"**
- Browser requires HTTPS for mic access (exception: localhost)
- On Android, grant microphone permission when prompted

**"Authentication failed"**
- Check API key matches `web-server-config.txt`
- Generate new key with `-generate-key` if needed

### Development

The web interface is embedded in the Go binary using `//go:embed`:

```
consumer/
├── web-server.go          # Go server
├── web/
│   └── index.html         # Recording UI (embedded)
└── build-web-server.sh    # Build script
```

To modify the UI, edit `web/index.html` and rebuild.
