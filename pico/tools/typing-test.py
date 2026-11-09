#!/usr/bin/env python3
"""Find the fastest typing rate a host accepts from the Pico keyboard.

For each step, sets the key hold and gap delays over the console, has the Pico
type a test line, and checks what arrives. Steps are HOLD/GAP in milliseconds.

By default the typed lines come back to this terminal, so run it in the
terminal you want to test, on the computer the Pico is plugged into, and leave
it focused. With --blind, focus another app during the countdown and compare
the lines by eye. The firmware defaults (2/2) are restored at the end.

Usage:
  ./typing-test.py                       # Default sweep, checked in this terminal
  ./typing-test.py 3/3 2/2 2/1 -n 20     # Chosen steps, 20 lines each
  ./typing-test.py --blind 5/5 2/2 1/1   # Type into another app
"""

import argparse
import os
import select
import sys
import termios
import time
import tty

TEXT = ("The quick brown fox jumps over the lazy dog. THE QUICK BROWN FOX! "
        "aA bB Mississippi bookkeeper 1100 1!2@3#4$5%6^7&8*9(0)-_=+[{]}\\|;:'\",<.>/?`~")
DEFAULT_STEPS = ["10/10", "5/5", "3/3", "2/2", "1/1", "1/0", "0/0"]


def parse_step(s):
    hold, gap = (float(x) for x in s.split("/"))
    return hold, gap


class Console:
    def __init__(self, device):
        self.fd = os.open(device, os.O_WRONLY | os.O_NOCTTY)
        tty.setraw(self.fd)  # No \n -> \r\n translation

    def delays(self, hold_ms, gap_ms):
        os.write(self.fd, f"\x01{round(hold_ms * 1000)} {round(gap_ms * 1000)}\n".encode())

    def type(self, line):
        os.write(self.fd, (line + "\n").encode())


def read_line(timeout):
    if select.select([sys.stdin], [], [], timeout)[0]:
        return sys.stdin.readline().rstrip("\n")
    return None  # Newline never arrived (dropped or still typing)


def checked(console, steps, lines):
    print(f"Typing {lines} line(s) per step; keep this terminal focused.\n")
    for hold, gap in steps:
        console.delays(hold, gap)
        # Generous: the delays plus ~1ms USB polling per report, doubled
        timeout = 2 * len(TEXT) * (hold + gap + 2) / 1000 + 1
        bad, chars, elapsed = [], 0, 0.0
        for _ in range(lines):
            termios.tcflush(sys.stdin, termios.TCIFLUSH)
            start = time.monotonic()
            console.type(TEXT)
            got = read_line(timeout)
            elapsed += time.monotonic() - start
            if got != TEXT:
                bad.append(got)
                termios.tcflush(sys.stdin, termios.TCIFLUSH)
            chars += len(TEXT) + 1
        print(f"\n== {hold:g}/{gap:g} ms: {lines - len(bad)}/{lines} lines ok, "
              f"{chars / elapsed:.0f} chars/s ==")
        for got in bad[:3]:
            print("   got: " + ("(timed out, no newline)" if got is None else got))
        if bad:
            print("   want: " + TEXT)
        print()


def blind(console, steps, lines):
    for n in range(5, 0, -1):
        print(f"Focus the target app: typing in {n}...", end="\r", flush=True)
        time.sleep(1)
    print()
    for hold, gap in steps:
        console.delays(hold, gap)
        for _ in range(lines):
            console.type(f"{hold:g}/{gap:g}: {TEXT}")
        # Wait for it to be typed before changing the delays
        time.sleep(lines * (len(TEXT) + 12) * (hold + gap + 2) / 1000 + 0.5)
    print("Done; compare each line with:\n" + TEXT)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", nargs="*", default=DEFAULT_STEPS, help="HOLD/GAP in ms (default: %(default)s)")
    ap.add_argument("-n", "--lines", type=int, default=5, help="lines per step (default: %(default)s)")
    ap.add_argument("-d", "--device", default="/dev/ttyACM0", help="Pico console port (default: %(default)s)")
    ap.add_argument("--blind", action="store_true", help="type into another app instead of checking here")
    args = ap.parse_args()

    steps = [parse_step(s) for s in args.steps]
    if not args.blind and not sys.stdin.isatty():
        sys.exit("stdin must be the terminal the Pico types into (or use --blind)")

    console = Console(args.device)
    try:
        (blind if args.blind else checked)(console, steps, args.lines)
    except KeyboardInterrupt:
        pass
    finally:
        console.delays(2, 2)


if __name__ == "__main__":
    main()
