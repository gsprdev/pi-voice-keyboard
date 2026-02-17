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
install -v -m 0755 "$SCRIPT_DIR/type-ascii.py" "$SBIN_DIR/type-ascii"

cp "$SCRIPT_DIR/kb-serve.service" "$SYSTEMD_DIR/"
cp "$SCRIPT_DIR/ptt.service" "$SYSTEMD_DIR/"

systemctl daemon-reload

echo "Enable and start with:"
echo "systemctl enable --now kb-serve.service"
echo "systemctl enable --now ptt.service"
