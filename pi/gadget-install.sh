#!/usr/bin/env bash

set -euo pipefail

PREFIX="/usr"
SBIN_DIR="$PREFIX/sbin"
SYSTEMD_DIR="/etc/systemd/system"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Install scripts
install -v -m 0755 "$SCRIPT_DIR/gadget-bind.sh" "$SBIN_DIR/gadget-bind"
install -v -m 0755 "$SCRIPT_DIR/gadget-unbind.sh" "$SBIN_DIR/gadget-unbind"
install -v -m 0755 "$SCRIPT_DIR/gadget-uninstall.sh" "$SBIN_DIR/gadget-uninstall"
install -v -m 0755 "$SCRIPT_DIR/type-ascii.py" "$SBIN_DIR/type-ascii"
install -v -m 0755 "$SCRIPT_DIR/ptt.py" "$SBIN_DIR/ptt"

# Install systemd units
install -v -m 0644 "$SCRIPT_DIR/type-ascii.service" "$SYSTEMD_DIR/"
install -v -m 0644 "$SCRIPT_DIR/ptt.service" "$SYSTEMD_DIR/"

systemctl daemon-reload

echo "Installation complete."
echo ""
echo "REQUIRED: Configure ptt service before enabling:"
echo "  sudo cp $SCRIPT_DIR/ptt.env.example /etc/default/ptt"
echo "  sudo nano /etc/default/ptt  # Edit with your GPU host and API key"
echo ""
echo "Then enable and start services:"
echo "  systemctl enable --now type-ascii.service ptt.service"
