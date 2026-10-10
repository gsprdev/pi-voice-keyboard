// Wi-Fi station bring-up and reconnection.

#include <stdio.h>

#include "lwip/netif.h"
#include "net.h"
#include "pico/cyw43_arch.h"
#include "secrets.h"

#define CHECK_INTERVAL_MS 1000
#define RETRY_DELAY_MS    5000

static bool initialized;
static bool up;
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

static void connect(void) {
    printf("Wi-Fi: joining %s\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        printf("Wi-Fi: join request failed\n");
    }
    next_attempt = make_timeout_time_ms(RETRY_DELAY_MS);
}

void net_init(void) {
    if (cyw43_arch_init() != 0) {
        printf("Wi-Fi: chip init failed\n");
        return;
    }
    initialized = true;
    cyw43_arch_enable_sta_mode();
    // USB powered, so trade power for latency
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    connect();
}

void net_report(void) {
    printf("Wi-Fi: %s", status_name(last_status));
    if (last_status == CYW43_LINK_UP) {
        printf(" (%s)", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    }
    printf("\n");
}

void net_task(void) {
    if (!initialized) {
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
        net_report();
    }
    up = status == CYW43_LINK_UP;

    // Joining or waiting for DHCP is still in progress; anything else is a retry
    bool in_progress = status == CYW43_LINK_JOIN || status == CYW43_LINK_NOIP;
    if (!up && !in_progress && time_reached(next_attempt)) {
        connect();
    }
}

bool net_up(void) {
    return up;
}
