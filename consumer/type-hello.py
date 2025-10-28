#!/usr/bin/env python3
# Minimal HID boot keyboard typer for /dev/hidg0

import time

DEV = "/dev/hidg0"

# HID usage IDs for US keyboard
KEY_NONE = 0x00
KEY_ENTER = 0x28
KEY_SPACE = 0x2C
KEY_COMMA = 0x36
KEY_1 = 0x1E

# Letters a-z
KEY_A = 0x04
def key_for_letter(ch):
    o = ord(ch.lower())
    return KEY_A + (o - ord('a'))

# Build one 8-byte boot keyboard report: [mods, 0x00, k1, k2, k3, k4, k5, k6]
def report(mod, keycode):
    return bytes([mod, 0x00, keycode, 0x00, 0x00, 0x00, 0x00, 0x00])

MOD_LSHIFT = 0x02

def send_char(fd, ch):
    # Decide keycode + whether shift needed
    mod = 0
    if 'a' <= ch <= 'z':
        kc = key_for_letter(ch)
    elif 'A' <= ch <= 'Z':
        kc = key_for_letter(ch)
        mod = MOD_LSHIFT
    elif ch == ' ':
        kc = KEY_SPACE
    elif ch == ',':
        kc = KEY_COMMA
    elif ch == '!':
        kc = KEY_1
        mod = MOD_LSHIFT
    else:
        raise ValueError(f"Unsupported character: {ch}")

    fd.write(report(mod, kc))
    fd.flush()
    time.sleep(0.01)
    fd.write(report(0, KEY_NONE))  # key up
    fd.flush()
    time.sleep(0.01)

def type_string(s):
    with open(DEV, "wb", buffering=0) as fd:
        for ch in s:
            send_char(fd, ch)

if __name__ == "__main__":
    type_string("Hello, world!")

