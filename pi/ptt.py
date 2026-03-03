#!/usr/bin/env python3

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
from signal import pause
from time import sleep
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from gpiozero import LED, Button, TonalBuzzer
from gpiozero.tones import Tone


class Server:
    def __init__(self, url):
        self.url = url
        self.healthy = False


# Configuration
raw_urls = os.environ.get("PTT_SERVICE_URLS")

if not raw_urls:
    print("ERROR: Required configuration missing!", file=sys.stderr)
    print("Set environment variable:", file=sys.stderr)
    print("  PTT_SERVICE_URLS - Comma-separated list of transcription service URLs", file=sys.stderr)
    print("  Example: http://gpu-host.local:8080,http://fallback.local:8080", file=sys.stderr)
    sys.exit(1)

servers = []
for raw in raw_urls.split(","):
    url = raw.strip().rstrip("/")
    parsed = urlparse(url)
    if not parsed.scheme or not parsed.netloc:
        print(f"ERROR: Malformed URL: {raw.strip()}", file=sys.stderr)
        sys.exit(1)
    servers.append(Server(url))

health_check_interval = int(os.environ.get("PTT_HEALTH_CHECK_INTERVAL", "10"))
health_check_timeout = int(os.environ.get("PTT_HEALTH_CHECK_TIMEOUT", "200")) / 1000

# Hardware
ledRecording = LED(17)
ledProcessing = LED(22)
btn = Button(24, bounce_time=0.2)
buzzer_freq = 400
buzzer = TonalBuzzer(27, mid_tone=buzzer_freq)

# Global state
recording_process = None
temp_file = None


def health_check_loop(server, interval, timeout):
    """Background health monitor for a single server."""
    while True:
        was_healthy = server.healthy
        try:
            with urlopen(f"{server.url}/health", timeout=timeout) as response:
                body = response.read().decode("utf-8").strip()
                server.healthy = response.status == 200 and body == "OK"
        except Exception as e:
            server.healthy = False
            if was_healthy:
                print(f"Health check failed for {server.url}: {e}")
        if server.healthy != was_healthy:
            status = "healthy" if server.healthy else "unhealthy"
            print(f"Server {server.url} is now {status}")
        sleep(interval)


def get_healthy_servers():
    """Return servers with healthy=True in preference order."""
    return [s for s in servers if s.healthy]


def error_signal(count):
    """Play count blink+beep pulses with 100ms gaps."""
    for _ in range(count):
        ledRecording.on()
        buzzer.play(Tone(frequency=buzzer_freq))
        sleep(0.1)
        buzzer.stop()
        ledRecording.off()
        sleep(0.1)


def start_recording():
    """Start recording when button is pressed."""
    global recording_process, temp_file

    print("Button pressed - starting recording")

    if not get_healthy_servers():
        print("ERROR: No healthy transcription servers available", file=sys.stderr)
        error_signal(3)
        return

    # Touch a temp file for arecord to use
    temp_file = tempfile.NamedTemporaryFile(suffix='.wav', delete=False)
    temp_file.close()

    # Start arecord subprocess
    recording_process = subprocess.Popen([
        'arecord',
        '-D', 'plughw',
        '-c1',
        '-r', '16000',
        '-f', 'S16_LE',
        '-t', 'wav',
        temp_file.name
    ])

    ledRecording.on()
    buzzer.play(Tone(frequency=buzzer_freq))
    sleep(.1)
    buzzer.stop()


def stop_recording():
    """Stop recording and send to transcription service."""
    global recording_process, temp_file

    print("Button released - stopping recording")
    ledRecording.off()

    if recording_process is None:
        print("No recording in progress")
        return

    # Stop the recording process
    recording_process.terminate()
    recording_process.wait()
    recording_process = None

    # Read audio data
    transcription = ""
    try:
        ledProcessing.on()
        with open(temp_file.name, 'rb') as f:
            audio_data = f.read()

        if not audio_data:
            print("No audio data recorded")
            return

        print(f"Sending {len(audio_data)} bytes to transcription service...")

        # Try each healthy server in preference order
        candidates = get_healthy_servers()
        for server in candidates:
            try:
                request = Request(
                    f"{server.url}/transcribe",
                    data=audio_data,
                    headers={'Content-Type': 'application/octet-stream'}
                )
                with urlopen(request) as response:
                    transcription = response.read().decode('utf-8')
                print(f"Transcription from {server.url}: {transcription}")
                break
            except Exception as e:
                print(f"Transcription failed on {server.url}: {e}")
        else:
            print("ERROR: All transcription attempts failed", file=sys.stderr)
            error_signal(2)

    except Exception as e:
        print(f"Error during transcription: {e}")
    finally:
        if temp_file and os.path.exists(temp_file.name):
            os.unlink(temp_file.name)
        temp_file = None
        ledProcessing.off()

    transcription = clean_transcription(transcription)

    if transcription:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.connect('/run/kb-serve/kb.sock')
                sock.sendall(transcription.encode('utf-8'))
        except Exception as e:
            print(f"Error sending to keyboard service: {e}")


def clean_transcription(transcription):
    """Trims default or error output from transcription. Sometimes this results in a blank string, which is intentional.
    Can also clean up noise from a transcription, including small incidental things not meant for actual dictation."""

    # Define noise keywords (without brackets - those are handled automatically)
    noise_keywords = [
        'BLANK_AUDIO', # Generally indicates an empty recording, in which case we shouldn't type anything
        'silence',  # Generally indicates an empty recording, in which case we shouldn't type anything
        'beep', # Likely to be the built-in buzzer to notify user of recording commencement
        'inaudible', # Suggests either a bad recording or unintended pickup before or after intended verbalization
    ]

    # Non-transcription keywords and feedback are generally surrounded by () or []
    patterns = [rf'[\[\(]?{re.escape(keyword)}[\]\)]?' for keyword in noise_keywords]
    combined_pattern = '|'.join(patterns)

    # Remove all noise patterns (case-insensitive)
    cleaned = re.sub(combined_pattern, '', transcription, flags=re.IGNORECASE)

    # Clean up whitespace left over after noise stripping
    cleaned = re.sub(r'\s{2,}', ' ', cleaned)
    return cleaned.strip()


# Start health monitor threads
for server in servers:
    t = threading.Thread(
        target=health_check_loop,
        args=(server, health_check_interval, health_check_timeout),
        daemon=True,
    )
    t.start()

print(f"Health monitor started ({len(servers)} servers, checking every {health_check_interval}s)")

btn.when_pressed = start_recording
btn.when_released = stop_recording

print("Push-to-talk service ready")
pause()
