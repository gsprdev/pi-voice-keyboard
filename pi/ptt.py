#!/usr/bin/env python3

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
from signal import pause
from time import sleep, time
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from gpiozero import LED, Button, TonalBuzzer
from gpiozero.tones import Tone

# Configuration - REQUIRED: Edit these before use
SERVICE_URLS_RAW = os.environ.get("PTT_SERVICE_URLS")

if not SERVICE_URLS_RAW:
    print("ERROR: Required configuration missing!", file=sys.stderr)
    print("Set environment variable:", file=sys.stderr)
    print("  PTT_SERVICE_URLS - Comma-separated list of transcription service URLs (priority order)", file=sys.stderr)
    print("  Example: http://gpu1.local:8080,http://gpu2.local:8080", file=sys.stderr)
    sys.exit(1)

# Parse comma-separated URLs: strip whitespace and trailing slashes, filter empty
SERVICE_URLS = [url.strip().rstrip('/') for url in SERVICE_URLS_RAW.split(',')]
SERVICE_URLS = [url for url in SERVICE_URLS if url]

if not SERVICE_URLS:
    print("ERROR: PTT_SERVICE_URLS contains no valid URLs", file=sys.stderr)
    sys.exit(1)

# Health check configuration
HEALTH_INTERVAL = int(os.environ.get("PTT_HEALTH_INTERVAL", "10"))
HEALTH_TIMEOUT = int(os.environ.get("PTT_HEALTH_TIMEOUT", "200")) / 1000.0  # convert ms to seconds


class HealthMonitor:
    """Background health monitor for multiple transcription servers."""

    def __init__(self, urls, interval, timeout):
        self._urls = urls
        self._interval = interval
        self._timeout = timeout
        self._lock = threading.Lock()
        self._status = {url: {"healthy": False, "last_check": 0} for url in urls}

    def check_all(self):
        """Perform a health check on all servers."""
        for url in self._urls:
            healthy = False
            try:
                health_url = f"{url}/health"
                with urlopen(health_url, timeout=self._timeout) as response:
                    status = response.read().decode('utf-8').strip()
                    healthy = (response.status == 200 and status == "OK")
            except Exception:
                healthy = False

            with self._lock:
                self._status[url] = {"healthy": healthy, "last_check": time()}

    def get_best_server(self):
        """Return the URL of the highest-preference healthy server, or None."""
        with self._lock:
            for url in self._urls:
                if self._status[url]["healthy"]:
                    return url
        return None

    def start(self):
        """Start the background polling thread."""
        t = threading.Thread(target=self._poll_loop, daemon=True)
        t.start()

    def _poll_loop(self):
        while True:
            sleep(self._interval)
            self.check_all()


# Initialize health monitor
health_monitor = HealthMonitor(SERVICE_URLS, HEALTH_INTERVAL, HEALTH_TIMEOUT)
health_monitor.check_all()  # Initial synchronous check
health_monitor.start()

ledRecording = LED(17)
ledProcessing = LED(22)
btn = Button(24, bounce_time=0.2)
buzzer_freq = 400
buzzer = TonalBuzzer(27, mid_tone=buzzer_freq)

# Global state
recording_process = None
temp_file = None
active_server_url = None


def start_recording():
    """Start recording when button is pressed"""
    global recording_process, temp_file, active_server_url

    print("Button pressed - starting recording")

    # Select best available server
    active_server_url = health_monitor.get_best_server()
    if active_server_url is None:
        print("ERROR: No transcription server is reachable", file=sys.stderr)
        for _ in range(3):
            ledRecording.on()
            buzzer.play(Tone(frequency=buzzer_freq))
            sleep(.1)
            buzzer.stop()
            ledRecording.off()
        return

    print(f"Using server: {active_server_url}")

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
    """Stop recording and send to transcription service"""
    global recording_process, temp_file, active_server_url

    print("Button released - stopping recording")
    ledRecording.off()

    if recording_process is None:
        print("No recording in progress")
        return

    # Stop the recording process
    recording_process.terminate()
    recording_process.wait()
    recording_process = None

    # Send to transcription service
    transcription = ""
    transcribe_url = f"{active_server_url}/transcribe"
    try:
        ledProcessing.on()
        with open(temp_file.name, 'rb') as f:
            audio_data = f.read()

        # Abort if no audio data
        if not audio_data:
            print("No audio data recorded")
            return

        print(f"Sending {len(audio_data)} bytes to transcription service...")

        request = Request(
            transcribe_url,
            data=audio_data,
            headers={'Content-Type': 'audio/wav'}
        )

        with urlopen(request) as response:
            transcription = response.read().decode('utf-8')

        print(f"Transcription: {transcription}")

    except Exception as e:
        print(f"Error during transcription: {e}")
    finally:
        # Clean up temp file
        if temp_file and os.path.exists(temp_file.name):
            os.unlink(temp_file.name)
        temp_file = None
        active_server_url = None
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

btn.when_pressed = start_recording
btn.when_released = stop_recording

print("Push-to-talk service ready")
pause()
