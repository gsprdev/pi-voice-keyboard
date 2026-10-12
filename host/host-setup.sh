#!/usr/bin/env bash
#
# Configure this computer's end of the keyboard's Ethernet-over-USB link, so a
# transcription service running here is reachable from the keyboard.
# Works for the Pico firmware (CDC-NCM) and the Pi gadget (CDC-ECM), which
# both present the host-side MAC aa:bb:cc:dd:ee:01.

set -euo pipefail

NAME="Voice Keyboard USB Ethernet"

# Delete existing connections if present (including the earlier name), then create fresh.
# Uses nmcli (D-Bus), which leverages polkit for authorization — no sudo needed.
nmcli connection delete "$NAME" 2>/dev/null || true
nmcli connection delete "Pi Voice KB USB Ethernet" 2>/dev/null || true

# Matched by MAC rather than interface name, which depends on the host's
# device naming policy (enxaabbccddee01, or a path-based name)
nmcli connection add \
  type ethernet \
  con-name "$NAME" \
  ethernet.mac-address aa:bb:cc:dd:ee:01 \
  ipv4.method manual \
  ipv4.addresses 192.168.71.2/24 \
  ipv4.route-metric 2000 \
  ipv4.never-default yes \
  ipv6.method disabled \
  connection.autoconnect yes

echo ""
echo "Host setup complete."
echo "Plug in the keyboard via USB — the interface will configure itself automatically."
echo ""
echo "Keyboard will be reachable at: 192.168.71.1"
echo "Keyboard reaches this computer at: 192.168.71.2 (e.g. SERVICE_URLS \"http://192.168.71.2:8080,...\")"
