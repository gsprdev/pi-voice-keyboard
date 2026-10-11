#!/usr/bin/env bash

set -euo pipefail

# Delete existing connection if present, then create fresh.
# Uses nmcli (D-Bus), which leverages polkit for authorization — no sudo needed.
nmcli connection delete "Pi Voice KB USB Ethernet" 2>/dev/null || true

nmcli connection add \
  type ethernet \
  con-name "Pi Voice KB USB Ethernet" \
  ifname enxaabbccddee01 \
  ipv4.method manual \
  ipv4.addresses 192.168.71.2/24 \
  ipv4.route-metric 2000 \
  ipv4.never-default yes \
  connection.autoconnect yes

echo ""
echo "Host setup complete."
echo "Plug in the Pi via USB — the interface will configure itself automatically."
echo ""
echo "Pi will be reachable at: 192.168.71.1"
