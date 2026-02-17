#!/usr/bin/env python3

import os
import re
import socket
import subprocess
import sys
import tempfile
from signal import pause
from time import sleep
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from gpiozero import LED, Button, TonalBuzzer
from gpiozero.tones import Tone

# Configuration - REQUIRED: Edit these before use
SERVICE_URL = os.environ.get("PTT_SERVICE_URL")

if not SERVICE_URL:
    print("ERROR: Required configuration missing!", file=sys.stderr)
    print("Set environment variable:", file=sys.stderr)
    print("  PTT_SERVICE_URL - Base URL to transcription service", file=sys.stderr)
    print("  Example: http://gpu-host.local:8080", file=sys.stderr)
    sys.exit(1)

# Remove trailing slash for consistency
SERVICE_URL = SERVICE_URL.rstrip('/')

# Service endpoints
HEALTH_URL = f"{SERVICE_URL}/health"
TRANSCRIBE_URL = f"{SERVICE_URL}/transcribe"

ledRecording = LED(17)
ledProcessing = LED(22)
btn = Button(24, bounce_time=0.2)
buzzer_freq = 400
buzzer = TonalBuzzer(27, mid_tone=buzzer_freq)

# Global state
recording_process = None
temp_file = None


def start_recording():
    """Start recording when button is pressed"""
    global recording_process, temp_file

    print("Button pressed - starting recording")

    # Perform health check
    if not check_service_health():
        print("ERROR: Transcription service is not reachable", file=sys.stderr)
        print(f"Verify {SERVICE_URL} is accessible and service is running", file=sys.stderr)
        for _ in range(3):
            ledRecording.on()
            buzzer.play(Tone(frequency=buzzer_freq))
            sleep(.1)
            buzzer.stop()
            ledRecording.off()

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

    # Send to transcription service
    transcription = ""
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
            TRANSCRIBE_URL,
            data=audio_data,
            headers={'Content-Type': 'application/octet-stream'}
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

def check_service_health():
    """Check if transcription service is reachable before starting"""
    try:
        print(f"Checking service health at {HEALTH_URL}...")
        with urlopen(HEALTH_URL, timeout=5) as response:
            status = response.read().decode('utf-8').strip()
            if status == "OK":
                print("Service health check passed")
                return True
            else:
                print(f"Service health check returned unexpected status: {status}")
                return False
    except Exception as e:
        print(f"Service health check failed: {e}", file=sys.stderr)
        return False

btn.when_pressed = start_recording
btn.when_released = stop_recording

print("Push-to-talk service ready")
pause()
