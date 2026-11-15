#!/usr/bin/env bash
# Types a text file (default: stdin) on the Pico's keyboard, to test typing speed.
# Text is reduced to what the Pico can type; the console blocks when the typing
# queue is full, so no pacing is needed here.
set -euo pipefail
stty -F /dev/ttyACM0 raw -echo
sleep 3 # time to focus the window that should receive the text
tr -d '\r' < "${1:-/dev/stdin}" \
    | sed "s/[“”]/\"/g; s/[‘’]/'/g; s/[—–]/-/g; s/™/(TM)/g; s/•/*/g" \
    | iconv -f utf-8 -t ascii//TRANSLIT \
    | LC_ALL=C tr -cd '\t\n\b\040-\176' > /dev/ttyACM0
