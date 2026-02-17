#!/bin/sh
set -euo pipefail

G=/sys/kernel/config/usb_gadget/kbd

PREFIX="/usr"
SBIN_DIR="$PREFIX/sbin"
SYSTEMD_DIR="/etc/systemd/system"

rm -f "$SBIN_DIR/gadget-bind"
rm -f "$SBIN_DIR/gadget-unbind"
rm -f "$SBIN_DIR/gadget-uninstall"
rm -f "$SBIN_DIR/type-ascii"
rm -f "$SBIN_DIR/ptt"
rm -f "$SYSTEMD_DIR/kb-serve.socket"
rm -f "$SYSTEMD_DIR/type-ascii.service"
rm -f "$SYSTEMD_DIR/ptt.service"

systemctl daemon-reload

if [ ! -d "$G" ]; then
  echo "Gadget not found at $G"
  exit 0
fi

# Unbind from UDC if bound
if [ -e "$G/UDC" ]; then
  echo "" | tee $G/UDC >/dev/null
fi

# Remove function symlinks from configs
find $G/configs -type l -delete 2>/dev/null || true

# Remove HID function
rm -rf $G/functions/hid.usb0 2>/dev/null || true

# Remove config strings and config
rm -rf $G/configs/c.1/strings/0x409 2>/dev/null || true
rm -rf $G/configs/c.1 2>/dev/null || true

# Remove device strings
rm -rf $G/strings/0x409 2>/dev/null || true

# Finally remove the gadget directory
rmdir $G 2>/dev/null || sudo rm -rf $G 2>/dev/null || true

# Try to unload libcomposite if unused
modprobe -r libcomposite 2>/dev/null || true
