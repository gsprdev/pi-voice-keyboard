# Testing the Web Interface

Quick guide for testing the web-based dictation interface.

## Prerequisites

1. **Transcription service running** (GPU system)
   ```bash
   cd service
   ./run.sh
   ```

2. **SSH tunnel active** (if GPU service is remote)
   ```bash
   ssh -L 8080:localhost:8080 gpu-system
   ```

## Pi Setup

### Terminal 1: Keyboard Service
```bash
cd consumer
./kb-serve.sh
```

Keep this running. You should see:
```
[+] Listening on /tmp/kb.sock
```

### Terminal 2: Web Server
```bash
cd consumer
./web-server
```

On first run, you'll see something like:
```
Generated new API key and saved to web-server-config.txt
Your API key: alpha-bravo-charlie-delta-42
Starting web server on port 8081
Server ready at http://localhost:8081
```

**Save the API key** - you'll need it for authentication.

## Testing from Browser

### Local Testing (on Pi)
1. Open browser to `http://localhost:8081`
2. Enter the API key
3. Click "Connect"
4. Grant microphone permission when prompted
5. Tap "TAP TO RECORD" to start recording
6. Speak: "This is a test"
7. Tap again to stop
8. Watch for "Processing..." then "Success!"
9. Text should appear where the Pi is connected

### Mobile Testing
1. Find Pi's IP address:
   ```bash
   hostname -I
   ```

2. On mobile device:
   - Open browser to `http://<pi-ip>:8081`
   - Enter API key
   - Tap "Connect"
   - Grant microphone permission
   - Test recording

## Troubleshooting

### "Failed to connect to socket"
**Cause:** `kb-serve.sh` is not running

**Fix:**
```bash
cd consumer
./kb-serve.sh
```

### "Transcription failed"
**Cause:** GPU service not accessible

**Check:**
```bash
curl -X POST \
  --data-binary "@test.wav" \
  http://localhost:8080/transcribe
```

If this fails:
- Verify service is running: `cd service && ./run.sh`
- Check SSH tunnel (if remote)
- Verify `TRANSCRIPTION_URL` in `web-server-config.txt`

### "Authentication failed"
**Cause:** API key mismatch

**Fix:**
1. Check key in server output or `web-server-config.txt`
2. Re-enter correct key in browser
3. Or generate new key: `./web-server -generate-key`

### "Microphone access denied"
**Cause:** Browser blocked microphone permission

**Fix:**
- Click the microphone icon in address bar
- Allow microphone access
- Refresh page

**Note:** Some browsers require HTTPS for microphone access. Exception: `localhost` works over HTTP.

### Text not appearing
**Cause:** HID gadget not configured or not connected

**Check:**
```bash
# Verify HID device exists
ls -l /dev/hidg0

# Test directly
echo "test" | socat - UNIX-CONNECT:/tmp/kb.sock
```

If `/dev/hidg0` doesn't exist:
```bash
sudo ./gadget-bind.sh
```

## Testing Without HID (Development)

To test transcription without the HID gadget:

**Modify `web-server.go` temporarily:**
```go
// Comment out the typeText call in handleTranscribe
// if err := s.typeText(transcription); err != nil {

// Add logging instead
log.Printf("Would type: %s", transcription)
```

Rebuild and run:
```bash
./build-web-server.sh
./web-server
```

The transcription will be logged to console instead of typed.

## Performance Testing

Watch the server logs for timing information:

```
[1733356789123] Starting transcription request
[1733356789123] Received audio: 160044 bytes
[1733356789123] Transcription complete: 45 chars
[1733356789123] === SUMMARY === Total: 1.2s | Transcription successful
```

**Expected timings:**
- Upload: 100-500ms (depends on network)
- Transcription: 100-500ms (GPU: 0.02x-0.19x realtime)
- Total: 1-3s for 5s audio

## Security Testing

### API Key Rotation
```bash
# Generate new key
./web-server -generate-key

# Copy output to web-server-config.txt
nano web-server-config.txt

# Restart server
# Ctrl+C in Terminal 2
./web-server
```

All browser clients will need the new key.

### Unauthorized Access Test
Try accessing without API key:
```bash
curl -X POST http://<pi-ip>:8081/transcribe --data-binary "@test.wav"
```

Should return: `401 Unauthorized`

## Load Testing

Test multiple concurrent requests:
```bash
# Terminal 3
for i in {1..5}; do
  curl -X POST \
    -H "X-API-Key: your-key-here" \
    --data-binary "@test.wav" \
    http://localhost:8081/transcribe &
done
```

Server processes requests sequentially. Watch for errors in logs.
