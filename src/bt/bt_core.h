#ifndef PM220_BT_CORE_H
#define PM220_BT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void bt_core_init(void);
void bt_scan_start(int seconds);

bool bt_is_scanning(void);
bool bt_is_connecting(void);
bool bt_is_connected(void);
const char *bt_state_name(void);
const char *bt_connected_addr(void);
const char *bt_last_error(void);

/* addr NULL/empty: last printer-like scan result, else the only scanned device. */
bool bt_connect(const char *addr);
void bt_disconnect(void);

bool bt_is_printing(void);
bool bt_print_job(const uint8_t *data, size_t len);

int bt_status_json(char *buf, size_t cap);
int bt_scan_json(char *buf, size_t cap);
int bt_printer_json(char *buf, size_t cap);

#endif
