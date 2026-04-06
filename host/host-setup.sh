#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

install -v -m 0600 -o root -g root \
  "$SCRIPT_DIR/pi-usb-ethernet.nmconnection" \
  /etc/NetworkManager/system-connections/

nmcli connection reload

echo ""
echo "Host setup complete."
echo "Plug in the Pi via USB — the interface will configure itself automatically."
echo ""
echo "Pi will be reachable at: 192.168.71.1"
