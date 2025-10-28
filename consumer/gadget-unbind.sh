#!/bin/sh
set -e
G=/sys/kernel/config/usb_gadget/kbd
if [ -e "$G/UDC" ]; then
  echo "" | tee $G/UDC
fi
find $G/configs -type l -delete 2>/dev/null
rm -rf $G/functions/hid.usb0 2>/dev/null
