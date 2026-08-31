/*
 * DHCP server for the CYW43 AP.
 * Byte-offset parsing (no packed-struct layout risk). Broadcast + ARP-unicast
 * replies: iOS often ignores a broadcast OFFER on Pico AP firmware.
 */
#include <stdio.h>
#include <string.h>
#include "dhcpserver.h"
#include "lwip/udp.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5

#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE        51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_END          255

#define PORT_DHCP_SERVER 67
#define PORT_DHCP_CLIENT 68
#define DEFAULT_LEASE_S  (24u * 60u * 60u)
#define DHCPS_BASE_IP    16
#define DHCP_COOKIE_OFS  236
#define DHCP_OPTIONS_OFS 240
#define DHCP_BUF_MAX     548
#define DHCP_MAGIC_N     0x63825363u

static uint8_t *opt_find(uint8_t *opt, size_t opt_len, uint8_t cmd) {
    size_t i = 0;
    while (i + 2 < opt_len && opt[i] != DHCP_OPT_END) {
        if (opt[i] == cmd) {
            return &opt[i];
        }
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        i += 2u + (size_t)opt[i + 1];
    }
    return NULL;
}

static void opt_u8(uint8_t **o, uint8_t cmd, uint8_t val) {
    uint8_t *p = *o;
    *p++ = cmd;
    *p++ = 1;
    *p++ = val;
    *o = p;
}

static void opt_n(uint8_t **o, uint8_t cmd, const void *data, uint8_t n) {
    uint8_t *p = *o;
    *p++ = cmd;
    *p++ = n;
    memcpy(p, data, n);
    *o = p + n;
}

static void opt_u32(uint8_t **o, uint8_t cmd, uint32_t val) {
    uint8_t b[4] = {
        (uint8_t)(val >> 24), (uint8_t)(val >> 16),
        (uint8_t)(val >> 8), (uint8_t)val
    };
    opt_n(o, cmd, b, 4);
}

static void dhcp_send(dhcp_server_t *d, const uint8_t *buf, size_t len,
                      const ip_addr_t *dest, u16_t port) {
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!out) {
        printf("dhcp: pbuf_alloc fail\n");
        return;
    }
    memcpy(out->payload, buf, len);
    err_t err = d->nif ? udp_sendto_if(d->udp, out, dest, port, d->nif)
                       : udp_sendto(d->udp, out, dest, port);
    printf("dhcp: send %s:%u len %u err %d\n",
           ip4addr_ntoa(ip_2_ip4(dest)), port, (unsigned)len, (int)err);
    pbuf_free(out);
}

void dhcp_handle_bootp(dhcp_server_t *d, const uint8_t *bootp, uint16_t n) {
    if (!d || !bootp || n < DHCP_OPTIONS_OFS + 3) {
        return;
    }

    uint8_t buf[DHCP_BUF_MAX];
    memset(buf, 0, sizeof(buf));
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    memcpy(buf, bootp, n);

    uint32_t cookie;
    memcpy(&cookie, buf + DHCP_COOKIE_OFS, 4);
    if (lwip_ntohl(cookie) != DHCP_MAGIC_N) {
        printf("dhcp: bad cookie 0x%08lx\n", (unsigned long)lwip_ntohl(cookie));
        return;
    }

    size_t opt_len = n > DHCP_OPTIONS_OFS ? n - DHCP_OPTIONS_OFS : 0;
    uint8_t *opt = buf + DHCP_OPTIONS_OFS;
    uint8_t *msgtype = opt_find(opt, opt_len, DHCP_OPT_MSG_TYPE);
    if (!msgtype) {
        printf("dhcp: no MSG_TYPE\n");
        return;
    }

    uint8_t type = msgtype[2];
    uint8_t *chaddr = buf + 28;
    printf("dhcp: %s mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           type == DHCPDISCOVER ? "DISCOVER" : type == DHCPREQUEST ? "REQUEST" : "other",
           chaddr[0], chaddr[1], chaddr[2], chaddr[3], chaddr[4], chaddr[5]);

    int yi = 0;
    if (type == DHCPDISCOVER || type == DHCPREQUEST) {
        yi = DHCP_SERVER_MAX_LEASES;
        for (int i = 0; i < DHCP_SERVER_MAX_LEASES; i++) {
            if (memcmp(d->lease[i].mac, chaddr, 6) == 0) {
                yi = i;
                break;
            }
            if (yi == DHCP_SERVER_MAX_LEASES) {
                int empty = 1;
                for (int b = 0; b < 6; b++) {
                    if (d->lease[i].mac[b]) {
                        empty = 0;
                        break;
                    }
                }
                if (empty) {
                    yi = i;
                }
            }
        }
        if (yi == DHCP_SERVER_MAX_LEASES) {
            yi = 0;
        }
        if (type == DHCPREQUEST) {
            uint8_t *req = opt_find(opt, opt_len, DHCP_OPT_REQUESTED_IP);
            if (req && req[1] == 4) {
                int got = req[5] - DHCPS_BASE_IP;
                if (got >= 0 && got < DHCP_SERVER_MAX_LEASES) {
                    yi = got;
                }
            }
        }
        memcpy(d->lease[yi].mac, chaddr, 6);
    } else {
        return;
    }

    /* yiaddr = gw with last octet = base+slot */
    memcpy(buf + 16, ip_2_ip4(&d->ip), 4);
    buf[19] = (uint8_t)(DHCPS_BASE_IP + yi);
    memcpy(buf + 20, ip_2_ip4(&d->ip), 4); /* siaddr */
    buf[0] = 2;                            /* BOOTREPLY */
    buf[1] = 1;
    buf[2] = 6;

    uint8_t *w = buf + DHCP_OPTIONS_OFS;
    opt_u8(&w, DHCP_OPT_MSG_TYPE, type == DHCPDISCOVER ? DHCPOFFER : DHCPACK);
    opt_n(&w, DHCP_OPT_SERVER_ID, ip_2_ip4(&d->ip), 4);
    opt_n(&w, DHCP_OPT_SUBNET_MASK, ip_2_ip4(&d->nm), 4);
    opt_n(&w, DHCP_OPT_ROUTER, ip_2_ip4(&d->ip), 4);
    opt_n(&w, DHCP_OPT_DNS, ip_2_ip4(&d->ip), 4);
    opt_u32(&w, DHCP_OPT_LEASE, DEFAULT_LEASE_S);
    *w++ = DHCP_OPT_END;

    size_t reply_len = (size_t)(w - buf);
    if (reply_len < 300) {
        reply_len = 300;
    }

    ip4_addr_t yiaddr;
    memcpy(&yiaddr, buf + 16, 4);
    struct eth_addr eth;
    memcpy(eth.addr, chaddr, 6);
    etharp_add_static_entry(&yiaddr, &eth);

    ip_addr_t bcast, uni;
    IP4_ADDR(ip_2_ip4(&bcast), 255, 255, 255, 255);
    ip_addr_copy_from_ip4(uni, yiaddr);

    dhcp_send(d, buf, reply_len, &bcast, PORT_DHCP_CLIENT);
    dhcp_send(d, buf, reply_len, &uni, PORT_DHCP_CLIENT);

    if (type == DHCPREQUEST) {
        printf("dhcp: ACK %s\n", ip4addr_ntoa(&yiaddr));
    }
}

static void dhcp_cb(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                    const ip_addr_t *src, u16_t src_port) {
    dhcp_server_t *d = arg;
    (void)upcb;
    printf("dhcp: udp pcb rx %uB from %s:%u\n", p->tot_len,
           ip4addr_ntoa(ip_2_ip4(src)), src_port);
    uint8_t tmp[DHCP_BUF_MAX];
    u16_t n = pbuf_copy_partial(p, tmp, sizeof(tmp), 0);
    pbuf_free(p);
    dhcp_handle_bootp(d, tmp, n);
}

void dhcp_server_init(dhcp_server_t *d, struct netif *nif, const ip_addr_t *ip, const ip_addr_t *nm) {
    memset(d, 0, sizeof(*d));
    ip_addr_copy(d->ip, *ip);
    ip_addr_copy(d->nm, *nm);
    d->nif = nif;
    d->udp = udp_new();
    if (!d->udp) {
        printf("dhcp: udp_new failed\n");
        return;
    }
    ip_set_option(d->udp, SOF_BROADCAST);
    ip_set_option(d->udp, SOF_REUSEADDR);
    err_t err = udp_bind(d->udp, IP4_ADDR_ANY, PORT_DHCP_SERVER);
    if (nif) {
        udp_bind_netif(d->udp, nif);
    }
    printf("dhcp: bind :67 err %d gw %s nif %p\n", (int)err,
           ip4addr_ntoa(ip_2_ip4(ip)), (void *)nif);
    udp_recv(d->udp, dhcp_cb, d);
}

void dhcp_try_eth_input(dhcp_server_t *d, struct pbuf *p) {
    if (!d || !p || p->len < 38) {
        return;
    }
    const uint8_t *b = (const uint8_t *)p->payload;
    uint16_t etype = (uint16_t)((b[12] << 8) | b[13]);
    if (etype != 0x0800) {
        return;
    }
    int ihl = (b[14] & 0x0f) * 4;
    int off = 14 + ihl;
    if (b[23] != 17 || p->len < off + 8) {
        return;
    }
    uint16_t dport = (uint16_t)((b[off + 2] << 8) | b[off + 3]);
    uint16_t ulen = (uint16_t)((b[off + 4] << 8) | b[off + 5]);
    if (dport != 67 || ulen <= 8) {
        return;
    }
    uint16_t bootp_len = (uint16_t)(ulen - 8);
    if (off + 8 + bootp_len > p->len) {
        bootp_len = (uint16_t)(p->len - off - 8);
    }
    dhcp_handle_bootp(d, b + off + 8, bootp_len);
}

void dhcp_server_deinit(dhcp_server_t *d) {
    if (d->udp) {
        udp_remove(d->udp);
        d->udp = NULL;
    }
}
