#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "dhcpserver.h"
#include "net/http.h"
#include "net/mdns.h"
#include "net/usb_ncm.h"
#include "bt/bt_core.h"
#include "fs/fs.h"

#define AP_SSID "PM220-Pico"
#define AP_PASS "pm220pico"

static dhcp_server_t dhcp_server;
static err_t (*ap_input_prev)(struct pbuf *p, struct netif *inp);

static err_t ap_input_log(struct pbuf *p, struct netif *inp) {
    if (p && p->len >= 38) {
        const uint8_t *b = (const uint8_t *)p->payload;
        uint16_t etype = (uint16_t)((b[12] << 8) | b[13]);
        if (etype == 0x0800) {
            int ihl = (b[14] & 0x0f) * 4;
            int off = 14 + ihl;
            if (b[23] == 17 && p->len >= off + 8) {
                uint16_t dport = (uint16_t)((b[off + 2] << 8) | b[off + 3]);
                uint16_t ulen = (uint16_t)((b[off + 4] << 8) | b[off + 5]);
                printf("eth ip %u.%u.%u.%u udp dport %u tot %u\n",
                       b[30], b[31], b[32], b[33], dport, p->tot_len);
                if (dport == 67 && ulen > 8) {
                    uint16_t bootp_len = (uint16_t)(ulen - 8);
                    if (off + 8 + bootp_len > p->len) {
                        bootp_len = (uint16_t)(p->len - off - 8);
                    }
                    printf("dhcp: eth path %uB\n", bootp_len);
                    dhcp_handle_bootp(&dhcp_server, b + off + 8, bootp_len);
                }
            }
        }
    }
    return ap_input_prev ? ap_input_prev(p, inp) : ERR_OK;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);
    printf("pm220-pico2w starting\n");
    if (!fs_init()) {
        printf("fs: unavailable (HTTP still serves the API)\n");
    }

    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASS, CYW43_AUTH_WPA2_AES_PSK);
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    cyw43_arch_lwip_begin();
    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(ap, &gw, &mask, &gw);
    netif_set_up(ap);
    netif_set_link_up(ap);
    netif_set_default(ap);
    ap_input_prev = ap->input;
    ap->input = ap_input_log;
    printf("ap %s flags=0x%02x\n", ip4addr_ntoa(netif_ip4_addr(ap)), ap->flags);
    dhcp_server_init(&dhcp_server, ap, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    http_server_start();
    mdns_start();
    usb_ncm_init();
    cyw43_arch_lwip_end();

    bt_core_init();
#ifdef PIMORONI_PICO_PLUS2_W_USER_SW_PIN
    gpio_init(PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
    gpio_set_dir(PIMORONI_PICO_PLUS2_W_USER_SW_PIN, GPIO_IN);
    gpio_pull_up(PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
    printf("USER button (GPIO %d): press to print test frame\n",
           PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
#endif
    printf("AP '%s' at 192.168.4.1  pass '%s'\n", AP_SSID, AP_PASS);
    printf("HTTP http://pm220.local/  AP http://192.168.4.1/  USB http://192.168.7.1/\n");

    uint32_t last_scan = to_ms_since_boot(get_absolute_time());
    uint32_t last_sta_log = 0;
    int last_sta_n = -1;
    bool sw_was_up = true;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_sta_log > 3000) {
            last_sta_log = now;
            struct netif *ap_if = &cyw43_state.netif[CYW43_ITF_AP];
            cyw43_arch_lwip_begin();
            if (!(ap_if->flags & NETIF_FLAG_LINK_UP)) {
                printf("ap link down flags=0x%02x, forcing up\n", ap_if->flags);
                netif_set_link_up(ap_if);
            }
            cyw43_arch_lwip_end();
            int n = 8;
            uint8_t macs[8 * 6];
            cyw43_wifi_ap_get_stas(&cyw43_state, &n, macs);
            if (n != last_sta_n) {
                last_sta_n = n;
                printf("ap stations: %d flags=0x%02x\n", n, ap_if->flags);
                for (int i = 0; i < n; i++) {
                    uint8_t *m = macs + i * 6;
                    printf("  sta %02x:%02x:%02x:%02x:%02x:%02x\n",
                           m[0], m[1], m[2], m[3], m[4], m[5]);
                }
            }
            usb_ncm_log();
        }
        if (!bt_is_connected() && !bt_is_connecting() && !bt_is_scanning() &&
            now - last_scan > 15000) {
            last_scan = now;
            printf("starting BT inquiry\n");
            bt_scan_start(8);
        }
#ifdef PIMORONI_PICO_PLUS2_W_USER_SW_PIN
        bool sw_up = gpio_get(PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
        if (sw_was_up && !sw_up && bt_is_connected() && !bt_is_printing()) {
            printf("USER: test print\n");
            if (!http_print_test()) {
                printf("test print failed: %s\n", bt_last_error());
            }
        }
        sw_was_up = sw_up;
#endif
        usb_ncm_poll();
        bool led = bt_is_connected() ? true : ((now / 250) & 1);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        sleep_ms(10);
    }
}
