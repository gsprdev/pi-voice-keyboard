#ifndef USBNET_H
#define USBNET_H

#include <stdbool.h>

// Ethernet over USB (CDC-NCM) to the USB host, on a fixed point-to-point
// subnet shared with the Pi gadget and host/host-setup.sh. The host is
// configured statically, so there's no DHCP on this link.
#define USBNET_DEVICE_IP "192.168.71.1"
#define USBNET_HOST_IP   "192.168.71.2"
#define USBNET_NETMASK   "255.255.255.0"

// Add the lwIP interface. Call after lwIP is initialized.
void usbnet_init(void);

// Track the link and pass received frames to lwIP. Call from the main loop.
void usbnet_task(void);

// The host has the device configured (the link may still be unused there)
bool usbnet_up(void);

#endif
