#ifndef PM220_WIFI_H
#define PM220_WIFI_H

#include <stddef.h>
#include <stdbool.h>

void wifi_init(void);
void wifi_poll(void);

void wifi_status_json(char *buf, size_t cap);
void wifi_scan_json(char *buf, size_t cap);
void wifi_networks_json(char *buf, size_t cap);

int wifi_request_scan(void);
int wifi_connect_save(const char *ssid, const char *password);
int wifi_connect_known(const char *ssid);
int wifi_save_network(const char *ssid, const char *password, const char *new_ssid);
int wifi_delete_network(const char *ssid);
int wifi_set_scan_policy(const char *policy);
int wifi_set_mdns(const char *name);
int wifi_set_ap_creds(const char *ssid, const char *password);
int wifi_force_ap(void);

/* Pause SoftAP and block Wi-Fi scans/joins while Classic BT pages or inquires.
 * After SPP is up the AP comes back; the CYW43439 PTA can share the radio then.
 * Paging still needs the AP down or HCI 0x04 is common. */
void wifi_bt_radio_hold(bool hold);

#endif
