#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "net/http.h"
#include "net/mdns.h"
#include "net/usb_ncm.h"
#include "net/wifi.h"
#include "bt/bt_core.h"
#include "fs/fs.h"

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

    cyw43_arch_lwip_begin();
    http_server_start();
    mdns_start();
    usb_ncm_init();
    cyw43_arch_lwip_end();
    wifi_init();

    bt_core_init();
#ifdef PIMORONI_PICO_PLUS2_W_USER_SW_PIN
    gpio_init(PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
    gpio_set_dir(PIMORONI_PICO_PLUS2_W_USER_SW_PIN, GPIO_IN);
    gpio_pull_up(PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
    printf("USER button (GPIO %d): press to print test frame\n",
           PIMORONI_PICO_PLUS2_W_USER_SW_PIN);
#endif
    printf("HTTP http://%s.local/  AP http://192.168.4.1/  USB http://192.168.7.1/\n",
           mdns_hostname());

    uint32_t last_scan = to_ms_since_boot(get_absolute_time());
    uint32_t last_sta_log = 0;
    bool sw_was_up = true;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_sta_log > 3000) {
            last_sta_log = now;
            usb_ncm_log();
        }
        wifi_poll();
        bt_poll();
        if (!bt_has_peer() && !bt_is_connected() && !bt_is_connecting() &&
            !bt_is_scanning() && now - last_scan > 15000) {
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
