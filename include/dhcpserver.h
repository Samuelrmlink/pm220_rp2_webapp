#ifndef PM220_DHCPSERVER_H
#define PM220_DHCPSERVER_H

#include "lwip/ip_addr.h"
#include "lwip/netif.h"

struct pbuf;

#define DHCP_SERVER_MAX_LEASES 8

typedef struct {
    uint8_t mac[6];
    uint32_t expiry;
} dhcp_lease_t;

typedef struct {
    ip_addr_t ip;
    ip_addr_t nm;
    dhcp_lease_t lease[DHCP_SERVER_MAX_LEASES];
    struct udp_pcb *udp;
    struct netif *nif;
} dhcp_server_t;

void dhcp_server_init(dhcp_server_t *d, struct netif *nif, const ip_addr_t *ip, const ip_addr_t *nm);
void dhcp_server_deinit(dhcp_server_t *d);
void dhcp_handle_bootp(dhcp_server_t *d, const uint8_t *bootp, uint16_t len);
void dhcp_try_eth_input(dhcp_server_t *d, struct pbuf *p);

#endif
