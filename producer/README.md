# Producer Scripts

Scripts for capturing audio, transcribing it via the service, and forwarding to the Pi.

## Scripts

### dictation-service.sh (Recommended for testing)
Interactive script with Enter-to-start/stop recording.

**Usage:**

```sh
./dictation-service.sh
```

Press Enter to start recording, press Enter again to stop and transcribe.

### dictation-toggle.sh (Recommended for daily use)
Toggle script designed to be bound to a keyboard shortcut.

**Setup:**

1. **GNOME/Ubuntu:** Settings -> Keyboard -> Keyboard Shortcuts -> Custom Shortcuts
2. **KDE Plasma:** System Settings -> Shortcuts -> Custom Shortcuts

**Usage:**
- Press your hotkey once to start recording
- Press it again to stop, transcribe, and type the result
- Notifications show status (requires `libnotify-bin`)
- GPU acceleration makes transcription essentially instantaneous

## Configuration

All scripts support environment variables:

```sh
export SERVICE_URL="http://localhost:8080/transcribe"
export REMOTE_HOST="pi02w.local"
export REMOTE_PORT="1234"
```

## Requirements

```sh
# Core dependencies
sudo apt install pulseaudio-utils curl openssh-client netcat-openbsd

# For notifications (optional but recommended)
sudo apt install libnotify-bin
```

## Workflow

1. Start transcription service: `cd ../service && ./run.sh`
2. Ensure Pi is running kb-serve.sh
3. Run one of the dictation scripts
4. Speak into your microphone
5. Text appears on the connected system (via Pi as USB keyboard)

## Troubleshooting

**No audio recording**
- Test with: `parecord --format=s16le --rate=16000 --channels=1 test.wav`
- Check microphone input levels: `pavucontrol`
