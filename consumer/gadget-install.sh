#!/usr/bin/env bash

set -euo pipefail

PREFIX="/usr"
SBIN_DIR="$PREFIX/sbin"
SYSTEMD_DIR="/etc/systemd/system"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

install -v -m 0755 "$SCRIPT_DIR/kb-serve.sh" "$SBIN_DIR/kb-serve"
install -v -m 0755 "$SCRIPT_DIR/gadget-bind.sh" "$SBIN_DIR/gadget-bind"
install -v -m 0755 "$SCRIPT_DIR/gadget-unbind.sh" "$SBIN_DIR/gadget-unbind"
install -v -m 0755 "$SCRIPT_DIR/gadget-uninstall.sh" "$SBIN_DIR/gadget-uninstall"
install -v -m 0755 "$SCRIPT_DIR/socat.sh" "$SBIN_DIR/kb-socat"
install -v -m 0755 "$SCRIPT_DIR/type-ascii.py" "$SBIN_DIR/type-ascii"

cat > "$SYSTEMD_DIR/kb-serve.service" << 'UNIT'
[Unit]
Description=Keyboard socket server and TCP bridge
After=network-online.target sys-kernel-config.mount
Wants=network-online.target sys-kernel-config.mount

[Service]
Type=simple
Environment=PATH=/usr/sbin:/usr/bin:/bin
ExecStartPre=/usr/sbin/gadget-bind
ExecStart=/usr/sbin/kb-serve
ExecStopPost=/usr/sbin/gadget-unbind || true
Restart=on-failure
RestartSec=2s

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
echo "Installed service unit at $SYSTEMD_DIR/kb-serve.service"
echo "Enable and start with: sudo systemctl enable --now kb-serve.service"


