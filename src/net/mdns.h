#ifndef PM220_MDNS_H
#define PM220_MDNS_H

struct netif;

#define PM220_MDNS_HOSTNAME "pm220"

void mdns_start(void);
void mdns_add_netif(struct netif *netif);

#endif
