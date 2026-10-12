#!/usr/bin/env python3
"""Record from the Pico's audio serial port to a WAV file (stage 2 mic check).

Prints level statistics, and optionally sends the recording to the
transcription service, exercising the whole audio path without Wi-Fi.

Usage:
  ./capture.py out.wav                         # 5 seconds from /dev/ttyACM1
  ./capture.py out.wav -s 10 -d /dev/ttyACM2
  ./capture.py out.wav --transcribe http://gpu-host:8080
"""

import argparse
import array
import math
import os
import select
import sys
import termios
import time
import tty
import wave
from urllib.request import Request, urlopen

RATE = 16000


def record(device, seconds):
    fd = os.open(device, os.O_RDONLY | os.O_NOCTTY)
    try:
        tty.setraw(fd)
        termios.tcflush(fd, termios.TCIFLUSH)
        data = bytearray()
        want = seconds * RATE * 2
        deadline = time.monotonic() + seconds + 2
        while len(data) < want and time.monotonic() < deadline:
            if select.select([fd], [], [], 0.5)[0]:
                data += os.read(fd, 4096)
        return bytes(data[:want - want % 2])
    finally:
        os.close(fd)


def report(samples):
    if not samples:
        print("No audio received", file=sys.stderr)
        return
    peak = max(abs(s) for s in samples)
    mean = sum(samples) / len(samples)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    clipped = sum(1 for s in samples if s in (32767, -32768))

    def dbfs(v):
        return 20 * math.log10(v / 32768) if v > 0 else float("-inf")

    print(f"{len(samples) / RATE:.2f}s  peak {dbfs(peak):.1f} dBFS  "
          f"rms {dbfs(rms):.1f} dBFS  dc {mean:+.0f}  clipped {clipped}")
    if peak == 0:
        print("All zeros: check SD wiring and power", file=sys.stderr)
    elif clipped > len(samples) // 1000:
        print("Frequent clipping: try the other MIC_TIMING", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", help="WAV file to write")
    parser.add_argument("-s", "--seconds", type=int, default=5)
    parser.add_argument("-d", "--device", default="/dev/ttyACM1", help="audio serial port")
    parser.add_argument("--transcribe", metavar="URL", help="transcription service base URL")
    args = parser.parse_args()

    print(f"Recording {args.seconds}s from {args.device}...")
    pcm = record(args.device, args.seconds)

    with wave.open(args.output, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    print(f"Wrote {args.output}")

    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    report(samples)

    if args.transcribe:
        with open(args.output, "rb") as f:
            request = Request(args.transcribe.rstrip("/") + "/transcribe", data=f.read(),
                              headers={"Content-Type": "audio/wav"})
        with urlopen(request) as response:
            print("Transcription:", response.read().decode("utf-8"))


if __name__ == "__main__":
    main()
