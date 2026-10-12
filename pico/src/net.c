// Network interfaces: Ethernet over USB always, Wi-Fi station on the Pico 2 W.

#include <stdio.h>

#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "net.h"
#include "pico/time.h"
#include "usbnet.h"

#if VOICEKB_WIFI

#include "pico/cyw43_arch.h"
#include "secrets.h"

#define CHECK_INTERVAL_MS 1000
#define RETRY_DELAY_MS    5000

static bool wifi_initialized;
static bool wifi_up;
static int last_status = CYW43_LINK_DOWN;
static absolute_time_t next_check;
static absolute_time_t next_attempt;

static const char *status_name(int status) {
    switch (status) {
    case CYW43_LINK_DOWN:    return "down";
    case CYW43_LINK_JOIN:    return "joined, waiting for DHCP";
    case CYW43_LINK_NOIP:    return "joined, no IP";
    case CYW43_LINK_UP:      return "up";
    case CYW43_LINK_FAIL:    return "connection failed";
    case CYW43_LINK_NONET:   return "network not found";
    case CYW43_LINK_BADAUTH: return "authentication failed";
    default:                 return "unknown";
    }
}

static void wifi_connect(void) {
    printf("Wi-Fi: joining %s\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        printf("Wi-Fi: join request failed\n");
    }
    next_attempt = make_timeout_time_ms(RETRY_DELAY_MS);
}

static void wifi_report(void) {
    printf("Wi-Fi: %s", status_name(last_status));
    if (last_status == CYW43_LINK_UP) {
        printf(" (%s)", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
    }
    printf("\n");
}

// Also initializes lwIP
static bool wifi_init(void) {
    if (cyw43_arch_init() != 0) {
        printf("Wi-Fi: chip init failed\n");
        return false;
    }
    wifi_initialized = true;
    cyw43_arch_enable_sta_mode();
    // USB powered, so trade power for latency
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    wifi_connect();
    return true;
}

static void wifi_task(void) {
    if (!wifi_initialized) {
        return;
    }
    cyw43_arch_poll();

    if (!time_reached(next_check)) {
        return;
    }
    next_check = make_timeout_time_ms(CHECK_INTERVAL_MS);

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (status != last_status) {
        last_status = status;
        wifi_report();
    }
    wifi_up = status == CYW43_LINK_UP;

    // Joining or waiting for DHCP is still in progress; anything else is a retry
    bool in_progress = status == CYW43_LINK_JOIN || status == CYW43_LINK_NOIP;
    if (!wifi_up && !in_progress && time_reached(next_attempt)) {
        wifi_connect();
    }
}

#else // No Wi-Fi: lwIP runs for USB alone

#include "lwip/init.h"

static bool wifi_up;

static bool wifi_init(void) {
    lwip_init();
    return true;
}

static void wifi_task(void) {}
static void wifi_report(void) {}

#endif

static bool initialized;

void net_init(void) {
    if (!wifi_init()) {
        return; // lwIP isn't running, so neither is USB networking
    }
    usbnet_init();
    initialized = true;
}

void net_task(void) {
    if (!initialized) {
        return;
    }
    wifi_task();
    usbnet_task();
    sys_check_timeouts(); // already run by cyw43_arch_poll, harmless twice
}

bool net_up(void) {
    return initialized && (wifi_up || usbnet_up());
}

bool net_routable(const ip_addr_t *addr) {
    if (!initialized || !IP_IS_V4(addr)) {
        return false;
    }
    const ip4_addr_t *a = ip_2_ip4(addr);
    struct netif *n;
    NETIF_FOREACH(n) {
        const ip4_addr_t *own = netif_ip4_addr(n);
        bool usable = netif_is_up(n) && netif_is_link_up(n);
        if (!usable && !ip4_addr_isany(own) && ip4_addr_net_eq(a, own, netif_ip4_netmask(n))) {
            return false;
        }
    }
    return ip4_route(a) != NULL;
}

void net_report(void) {
    wifi_report();
    printf("USB network: %s\n", usbnet_up() ? "up (" USBNET_DEVICE_IP ")" : "down");
}
