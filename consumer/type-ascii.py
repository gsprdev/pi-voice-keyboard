#!/usr/bin/env python3
# Extended HID boot keyboard typer for /dev/hidg0 - Full ASCII support

import socket
import sys
import time

DEV = "/dev/hidg0"

# HID usage IDs for US keyboard
KEY_NONE = 0x00
KEY_ENTER = 0x28
KEY_SPACE = 0x2C
KEY_TAB = 0x2B
KEY_BACKSPACE = 0x2A

# Numbers 0-9
KEY_0 = 0x27
KEY_1 = 0x1E
KEY_2 = 0x1F
KEY_3 = 0x20
KEY_4 = 0x21
KEY_5 = 0x22
KEY_6 = 0x23
KEY_7 = 0x24
KEY_8 = 0x25
KEY_9 = 0x26

# Letters a-z
KEY_A = 0x04
def key_for_letter(ch):
    o = ord(ch.lower())
    return KEY_A + (o - ord('a'))

# Special characters
KEY_MINUS = 0x2D
KEY_EQUALS = 0x2E
KEY_LEFT_BRACKET = 0x2F
KEY_RIGHT_BRACKET = 0x30
KEY_BACKSLASH = 0x31
KEY_SEMICOLON = 0x33
KEY_QUOTE = 0x34
KEY_GRAVE = 0x35
KEY_COMMA = 0x36
KEY_PERIOD = 0x37
KEY_SLASH = 0x38

# Modifier keys
MOD_LSHIFT = 0x02
MOD_RSHIFT = 0x20
MOD_LCTRL = 0x01
MOD_LALT = 0x04
MOD_RALT = 0x40

# Build one 8-byte boot keyboard report: [mods, 0x00, k1, k2, k3, k4, k5, k6]
def report(mod, keycode):
    return bytes([mod, 0x00, keycode, 0x00, 0x00, 0x00, 0x00, 0x00])

def send_char(fd, ch):
    """Send a single character with full ASCII support"""
    mod = 0
    kc = 0

    # Letters a-z
    if 'a' <= ch <= 'z':
        kc = key_for_letter(ch)

    # Letters A-Z (with shift)
    elif 'A' <= ch <= 'Z':
        kc = key_for_letter(ch)
        mod = MOD_LSHIFT

    # Numbers 0-9
    elif ch == '0':
        kc = KEY_0
    elif ch == '1':
        kc = KEY_1
    elif ch == '2':
        kc = KEY_2
    elif ch == '3':
        kc = KEY_3
    elif ch == '4':
        kc = KEY_4
    elif ch == '5':
        kc = KEY_5
    elif ch == '6':
        kc = KEY_6
    elif ch == '7':
        kc = KEY_7
    elif ch == '8':
        kc = KEY_8
    elif ch == '9':
        kc = KEY_9

    # Special characters (unshifted)
    elif ch == ' ':
        kc = KEY_SPACE
    elif ch == '\t':
        kc = KEY_TAB
    elif ch == '\n':
        kc = KEY_ENTER
    elif ch == '\b':
        kc = KEY_BACKSPACE
    elif ch == '-':
        kc = KEY_MINUS
    elif ch == '=':
        kc = KEY_EQUALS
    elif ch == '[':
        kc = KEY_LEFT_BRACKET
    elif ch == ']':
        kc = KEY_RIGHT_BRACKET
    elif ch == '\\':
        kc = KEY_BACKSLASH
    elif ch == ';':
        kc = KEY_SEMICOLON
    elif ch == "'":
        kc = KEY_QUOTE
    elif ch == '`':
        kc = KEY_GRAVE
    elif ch == ',':
        kc = KEY_COMMA
    elif ch == '.':
        kc = KEY_PERIOD
    elif ch == '/':
        kc = KEY_SLASH

    # Special characters (shifted)
    elif ch == '_':
        kc = KEY_MINUS
        mod = MOD_LSHIFT
    elif ch == '+':
        kc = KEY_EQUALS
        mod = MOD_LSHIFT
    elif ch == '{':
        kc = KEY_LEFT_BRACKET
        mod = MOD_LSHIFT
    elif ch == '}':
        kc = KEY_RIGHT_BRACKET
        mod = MOD_LSHIFT
    elif ch == '|':
        kc = KEY_BACKSLASH
        mod = MOD_LSHIFT
    elif ch == ':':
        kc = KEY_SEMICOLON
        mod = MOD_LSHIFT
    elif ch == '"':
        kc = KEY_QUOTE
        mod = MOD_LSHIFT
    elif ch == '~':
        kc = KEY_GRAVE
        mod = MOD_LSHIFT
    elif ch == '<':
        kc = KEY_COMMA
        mod = MOD_LSHIFT
    elif ch == '>':
        kc = KEY_PERIOD
        mod = MOD_LSHIFT
    elif ch == '?':
        kc = KEY_SLASH
        mod = MOD_LSHIFT

    # Shifted numbers for symbols
    elif ch == '!':
        kc = KEY_1
        mod = MOD_LSHIFT
    elif ch == '@':
        kc = KEY_2
        mod = MOD_LSHIFT
    elif ch == '#':
        kc = KEY_3
        mod = MOD_LSHIFT
    elif ch == '$':
        kc = KEY_4
        mod = MOD_LSHIFT
    elif ch == '%':
        kc = KEY_5
        mod = MOD_LSHIFT
    elif ch == '^':
        kc = KEY_6
        mod = MOD_LSHIFT
    elif ch == '&':
        kc = KEY_7
        mod = MOD_LSHIFT
    elif ch == '*':
        kc = KEY_8
        mod = MOD_LSHIFT
    elif ch == '(':
        kc = KEY_9
        mod = MOD_LSHIFT
    elif ch == ')':
        kc = KEY_0
        mod = MOD_LSHIFT

    else:
        raise ValueError(f"Unsupported character: {ch} (ord: {ord(ch)})")

    # Send the key
    fd.write(report(mod, kc))
    fd.flush()
    time.sleep(0.01)
    fd.write(report(0, KEY_NONE))  # key up
    fd.flush()
    time.sleep(0.01)

def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} SOCKET_PATH", file=sys.stderr)
        sys.exit(1)

    sock_path = sys.argv[1]
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(5)
    print(f"[+] Listening on {sock_path}", file=sys.stderr)

    # Open the HID device once – reuse it for every client
    with open(DEV, "wb", buffering=0) as hid_fd:
        while True:                  # ← keep accepting forever
            conn, _ = srv.accept()
            print("[+] Client connected", file=sys.stderr)

            try:
                # Read raw bytes directly; process immediately without decoding
                while True:
                    data = conn.recv(4096)
                    if not data:
                        break
                    for b in data:
                        try:
                            send_char(hid_fd, chr(b))
                        except Exception as exc:
                            print(f"[!] Skipping byte {b}: {exc}", file=sys.stderr)
            except Exception as exc:
                print(f"[!] Error while processing client: {exc}", file=sys.stderr)

            conn.close()
            print("[+] Client disconnected – waiting for next one", file=sys.stderr)

if __name__ == "__main__":
    main()
