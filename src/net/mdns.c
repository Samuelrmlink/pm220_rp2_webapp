#include "net/mdns.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"

#define MDNS_NETIFS 4

static char hostname[64] = PM220_MDNS_HOSTNAME;
static struct netif *bound[MDNS_NETIFS];
static s8_t svc_slot[MDNS_NETIFS];

static void srv_txt(struct mdns_service *service, void *txt_userdata) {
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}

static int find_netif(struct netif *netif) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (bound[i] == netif) {
            return i;
        }
    }
    return -1;
}

bool mdns_set_hostname(const char *name) {
    if (!name || !name[0] || strlen(name) > 63 || name[0] == '-') {
        return false;
    }
    char tmp[64];
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(tmp); p++) {
        char c = *p;
        if (isupper((unsigned char)c)) {
            c = (char)tolower((unsigned char)c);
        }
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) {
            return false;
        }
        tmp[n++] = c;
    }
    tmp[n] = 0;
    if (!tmp[0] || tmp[0] == '-') {
        return false;
    }
    snprintf(hostname, sizeof(hostname), "%s", tmp);
    cyw43_arch_lwip_begin();
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (!bound[i]) {
            continue;
        }
        netif_set_hostname(bound[i], hostname);
        mdns_resp_rename_netif(bound[i], hostname);
        if (svc_slot[i] >= 0) {
            mdns_resp_rename_service(bound[i], (u8_t)svc_slot[i], hostname);
        }
    }
    cyw43_arch_lwip_end();
    printf("mdns hostname %s.local\n", hostname);
    return true;
}

const char *mdns_hostname(void) {
    return hostname;
}

void mdns_add_netif(struct netif *netif) {
    if (!netif || find_netif(netif) >= 0) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (!bound[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("mdns: no slot\n");
        return;
    }
    err_t err = mdns_resp_add_netif(netif, hostname);
    if (err != ERR_OK) {
        printf("mdns add netif failed: %d\n", (int)err);
        return;
    }
    netif_set_hostname(netif, hostname);
    s8_t svc = mdns_resp_add_service(netif, hostname, "_http", DNSSD_PROTO_TCP, 80, srv_txt, NULL);
    bound[slot] = netif;
    svc_slot[slot] = svc;
    if (svc < 0) {
        printf("mdns add _http failed: %d\n", (int)svc);
    }
}

void mdns_remove_netif(struct netif *netif) {
    int i = find_netif(netif);
    if (i < 0) {
        return;
    }
    mdns_resp_remove_netif(netif);
    bound[i] = NULL;
    svc_slot[i] = -1;
}

void mdns_start(void) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        bound[i] = NULL;
        svc_slot[i] = -1;
    }
    mdns_resp_init();
}
