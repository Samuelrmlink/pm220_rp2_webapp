#include "net/mdns.h"

#include <stdio.h>
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"

static void srv_txt(struct mdns_service *service, void *txt_userdata) {
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}

void mdns_add_netif(struct netif *netif) {
    err_t err = mdns_resp_add_netif(netif, PM220_MDNS_HOSTNAME);
    if (err != ERR_OK) {
        printf("mdns add netif failed: %d\n", (int)err);
        return;
    }
    netif_set_hostname(netif, PM220_MDNS_HOSTNAME);
    s8_t slot = mdns_resp_add_service(netif, PM220_MDNS_HOSTNAME, "_http",
                                      DNSSD_PROTO_TCP, 80, srv_txt, NULL);
    if (slot < 0) {
        printf("mdns add _http failed: %d\n", (int)slot);
    }
}

void mdns_start(void) {
    mdns_resp_init();
    mdns_add_netif(&cyw43_state.netif[CYW43_ITF_AP]);
}
