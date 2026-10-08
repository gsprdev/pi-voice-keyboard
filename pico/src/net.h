#ifndef NET_H
#define NET_H

#include <stdbool.h>

// Bring up the Wi-Fi chip. Call before tusb_init: it loads the chip's
// firmware, which takes a moment.
void net_init(void);

// Join Wi-Fi, rejoin after drops, and run lwIP. Call from the main loop.
void net_task(void);

// Joined and holding an IP address
bool net_up(void);

// Log the current Wi-Fi state
void net_report(void);

#endif
