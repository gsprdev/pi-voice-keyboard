#ifndef NET_H
#define NET_H

#include <stdbool.h>

#include "lwip/ip_addr.h"

// Networking: Ethernet over USB to the host on every board, plus Wi-Fi on
// the Pico 2 W. Both are lwIP interfaces in one stack; each address goes out
// whichever interface its subnet is on, so the USB host is reached over USB.

// Start lwIP and the interfaces. Call before tusb_init: bringing up the
// Wi-Fi chip loads its firmware, which takes a moment.
void net_init(void);

// Join Wi-Fi, rejoin after drops, track the USB link, and run lwIP. Call
// from the main loop.
void net_task(void);

// Some interface is up
bool net_up(void);

// The address has an interface to go out on. False for an address on a
// subnet whose link is down (the USB host while USB isn't configured), which
// would otherwise be sent to the default route and time out.
bool net_routable(const ip_addr_t *addr);

// Log the state of each interface
void net_report(void);

#endif
