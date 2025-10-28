#!/bin/sh
set -e

G=/sys/kernel/config/usb_gadget/kbd

if [ ! -d "$G" ]; then
  echo "Gadget not found at $G"
  exit 0
fi

# Unbind from UDC if bound
if [ -e "$G/UDC" ]; then
  echo "" | sudo tee $G/UDC >/dev/null
fi

# Remove function symlinks from configs
sudo find $G/configs -type l -delete 2>/dev/null || true

# Remove HID function
sudo rm -rf $G/functions/hid.usb0 2>/dev/null || true

# Remove config strings and config
sudo rm -rf $G/configs/c.1/strings/0x409 2>/dev/null || true
sudo rm -rf $G/configs/c.1 2>/dev/null || true

# Remove device strings
sudo rm -rf $G/strings/0x409 2>/dev/null || true

# Finally remove the gadget directory
sudo rmdir $G 2>/dev/null || sudo rm -rf $G 2>/dev/null || true

# Try to unload libcomposite if unused
sudo modprobe -r libcomposite 2>/dev/null || true 