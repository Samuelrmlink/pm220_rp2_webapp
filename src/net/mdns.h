#ifndef PM220_MDNS_H
#define PM220_MDNS_H

#include <stdbool.h>

struct netif;

#define PM220_MDNS_HOSTNAME "pm220"

void mdns_start(void);
void mdns_add_netif(struct netif *netif);
void mdns_remove_netif(struct netif *netif);
bool mdns_set_hostname(const char *name);
const char *mdns_hostname(void);

#endif
