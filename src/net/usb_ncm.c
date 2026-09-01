#include "net/usb_ncm.h"

#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"
#include "pico/cyw43_arch.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "dhcpserver.h"
#include "net/mdns.h"

#define USB_IP0 192
#define USB_IP1 168
#define USB_IP2 7
#define USB_IP3 1

#define NCM_RX_N    4
#define NCM_RX_MAX  1524

uint8_t tud_network_mac_address[6];

static struct netif ncm_netif;
static dhcp_server_t dhcp_ncm;
static uint8_t ncm_rx_data[NCM_RX_N][NCM_RX_MAX];
static volatile uint16_t ncm_rx_len[NCM_RX_N];
static volatile uint8_t ncm_rx_w;
static uint8_t ncm_rx_r;
static unsigned ncm_rx_ok, ncm_rx_drop, ncm_tx_ok, ncm_tx_drop;
static unsigned ncm_rx_ok_last, ncm_tx_ok_last, ncm_rx_drop_last, ncm_tx_drop_last;

static err_t ncm_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!tud_ready() || !tud_network_can_xmit(p->tot_len)) {
        ncm_tx_drop++;
        return ERR_USE;
    }
    tud_network_xmit(p, 0);
    ncm_tx_ok++;
    return ERR_OK;
}

static err_t ncm_input(struct pbuf *p, struct netif *inp) {
    dhcp_try_eth_input(&dhcp_ncm, p);
    return ethernet_input(p, inp);
}

static err_t ncm_netif_init(struct netif *netif) {
    usb_ncm_ensure_mac();
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->output = etharp_output;
    netif->linkoutput = ncm_linkoutput;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, tud_network_mac_address, 6);
    netif->hwaddr[5] ^= 0x01;
    return ERR_OK;
}

void usb_ncm_ensure_mac(void) {
    if (tud_network_mac_address[0]) {
        return;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    tud_network_mac_address[0] = 0x02;
    memcpy(&tud_network_mac_address[1], id.id + 3, 5);
}

struct netif *usb_ncm_netif(void) {
    return &ncm_netif;
}

void usb_ncm_init(void) {
    usb_ncm_ensure_mac();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, USB_IP0, USB_IP1, USB_IP2, USB_IP3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, USB_IP0, USB_IP1, USB_IP2, USB_IP3);
    netif_add(&ncm_netif, &ip, &mask, &gw, NULL, ncm_netif_init, ncm_input);
    netif_set_up(&ncm_netif);
    netif_set_link_up(&ncm_netif);
    dhcp_server_init(&dhcp_ncm, &ncm_netif, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    mdns_add_netif(&ncm_netif);
    printf("usb ncm %s mac %02x:%02x:%02x:%02x:%02x:%02x  (host 192.168.7.16)\n",
           ip4addr_ntoa(&ip),
           ncm_netif.hwaddr[0], ncm_netif.hwaddr[1], ncm_netif.hwaddr[2],
           ncm_netif.hwaddr[3], ncm_netif.hwaddr[4], ncm_netif.hwaddr[5]);
}

void usb_ncm_poll(void) {
    for (;;) {
        uint16_t n = ncm_rx_len[ncm_rx_r];
        if (!n) {
            break;
        }
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_RAW, n, PBUF_POOL);
        if (p) {
            pbuf_take(p, ncm_rx_data[ncm_rx_r], n);
            if (ncm_netif.input(p, &ncm_netif) != ERR_OK) {
                pbuf_free(p);
            }
            ncm_rx_ok++;
        } else {
            ncm_rx_drop++;
        }
        cyw43_arch_lwip_end();
        ncm_rx_len[ncm_rx_r] = 0;
        ncm_rx_r = (uint8_t)((ncm_rx_r + 1) % NCM_RX_N);
    }
    tud_network_recv_renew();
}

void usb_ncm_log(void) {
    if (ncm_rx_ok == ncm_rx_ok_last && ncm_tx_ok == ncm_tx_ok_last &&
        ncm_rx_drop == ncm_rx_drop_last && ncm_tx_drop == ncm_tx_drop_last) {
        return;
    }
    ncm_rx_ok_last = ncm_rx_ok;
    ncm_tx_ok_last = ncm_tx_ok;
    ncm_rx_drop_last = ncm_rx_drop;
    ncm_tx_drop_last = ncm_tx_drop;
    printf("ncm rx %u drop %u  tx %u drop %u  ready %d can_xmit %d\n",
           ncm_rx_ok, ncm_rx_drop, ncm_tx_ok, ncm_tx_drop,
           (int)tud_ready(), (int)tud_network_can_xmit(64));
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (!size) {
        return true;
    }
    if (size > NCM_RX_MAX) {
        ncm_rx_drop++;
        return false;
    }
    uint8_t w = ncm_rx_w;
    if (ncm_rx_len[w] != 0) {
        ncm_rx_drop++;
        return false;
    }
    memcpy(ncm_rx_data[w], src, size);
    ncm_rx_len[w] = size;
    ncm_rx_w = (uint8_t)((w + 1) % NCM_RX_N);
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
    for (int i = 0; i < NCM_RX_N; i++) {
        ncm_rx_len[i] = 0;
    }
    ncm_rx_w = 0;
    ncm_rx_r = 0;
}

void tud_mount_cb(void) {
    printf("usb: mounted (NCM+CDC)\n");
}

void tud_umount_cb(void) {
    printf("usb: unmounted\n");
}
