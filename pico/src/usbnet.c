// Ethernet over USB: glue between TinyUSB's CDC-NCM driver and an lwIP
// interface. Everything runs from the main loop, so no locking is needed.

#include <stdio.h>
#include <string.h>

#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "pico/time.h"
#include "tusb.h"
#include "usbnet.h"

// Waiting for the previous NTB to go out; only long enough for USB to drain
#define TX_WAIT_MS 5

// The host's end of the link, reported in the NCM descriptor. Same as the Pi
// gadget's host_addr, so the host-side profile matches either keyboard.
uint8_t tud_network_mac_address[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};

// This end
static const uint8_t device_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02};

static struct netif netif;

// A frame from the host, held until usbnet_task hands it to lwIP
static struct pbuf *received;

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    // Still processing the previous frame: the driver keeps this one until
    // tud_network_recv_renew
    if (received) {
        return false;
    }
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            pbuf_take(p, src, size);
            received = p;
        }
        // Out of buffers: drop it, as a busy Ethernet interface would
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

static err_t linkoutput(struct netif *n, struct pbuf *p) {
    (void)n;
    absolute_time_t deadline = make_timeout_time_ms(TX_WAIT_MS);
    while (tud_ready()) {
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        if (time_reached(deadline)) {
            break; // the host isn't reading: drop, TCP retransmits
        }
        tud_task(); // completes the transfer in flight
    }
    return ERR_IF;
}

static err_t netif_init_cb(struct netif *n) {
    n->name[0] = 'u';
    n->name[1] = 's';
    n->mtu = CFG_TUD_NET_MTU - SIZEOF_ETH_HDR;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, device_mac, ETH_HWADDR_LEN);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    return ERR_OK;
}

void usbnet_init(void) {
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(USBNET_DEVICE_IP, &ip);
    ip4addr_aton(USBNET_NETMASK, &mask);
    ip4_addr_set_zero(&gw); // the host is reached directly; nothing routes beyond it
    netif_add(&netif, &ip, &mask, &gw, NULL, netif_init_cb, ethernet_input);
    netif_set_up(&netif);
    // The link comes up once the host configures the device
}

void usbnet_task(void) {
    bool up = tud_ready();
    if (up != netif_is_link_up(&netif)) {
        if (up) {
            netif_set_link_up(&netif);
        } else {
            netif_set_link_down(&netif);
        }
        printf("USB network: %s\n", up ? "up (" USBNET_DEVICE_IP ")" : "down");
    }

    if (received) {
        // lwIP owns the pbuf unless input fails
        if (netif.input(received, &netif) != ERR_OK) {
            pbuf_free(received);
        }
        received = NULL;
        tud_network_recv_renew();
    }
}

bool usbnet_up(void) {
    return netif_is_link_up(&netif);
}
