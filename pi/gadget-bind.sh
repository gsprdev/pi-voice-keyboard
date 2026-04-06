#!/bin/sh
set -e

# Load required module
modprobe libcomposite

G=/sys/kernel/config/usb_gadget/kbd
mkdir -p $G
echo 0x1d6b > $G/idVendor      # Linux Foundation
echo 0x0104 > $G/idProduct     # Multifunction Composite Gadget (kbd-ish)
echo 0x0100 > $G/bcdDevice
echo 0x0200 > $G/bcdUSB
echo 0xEF > $G/bDeviceClass      # Miscellaneous Device
echo 0x02 > $G/bDeviceSubClass   # Common Class
echo 0x01 > $G/bDeviceProtocol   # Interface Association Descriptor

mkdir -p $G/strings/0x409
echo "1234567890" > $G/strings/0x409/serialnumber
echo "gspr.dev" > $G/strings/0x409/manufacturer
echo "Voice Keyboard" > $G/strings/0x409/product

mkdir -p $G/configs/c.1/strings/0x409
echo "Config 1" > $G/configs/c.1/strings/0x409/configuration
echo 120 > $G/configs/c.1/MaxPower

# HID function: Boot keyboard, 8-byte report
mkdir -p $G/functions/hid.usb0
echo 1 > $G/functions/hid.usb0/protocol      # Keyboard
echo 1 > $G/functions/hid.usb0/subclass      # Boot Interface
echo 8 > $G/functions/hid.usb0/report_length

# Standard boot keyboard report descriptor
bash -c 'printf "\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0" > '"$G"'/functions/hid.usb0/report_desc'

ln -s $G/functions/hid.usb0 $G/configs/c.1/

# ECM function: USB Ethernet (CDC-ECM)
mkdir -p $G/functions/ecm.usb0
echo "aa:bb:cc:dd:ee:01" > $G/functions/ecm.usb0/host_addr
echo "aa:bb:cc:dd:ee:02" > $G/functions/ecm.usb0/dev_addr
ln -s $G/functions/ecm.usb0 $G/configs/c.1/

# Bind to UDC (pick first available)
UDC=$(ls /sys/class/udc | head -n1)
echo $UDC > ${G}/UDC

