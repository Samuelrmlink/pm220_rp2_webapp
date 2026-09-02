#include "bt_core.h"

#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "classic/sdp_client.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "printer/tspl.h"
#include "fs/fs.h"

#define MAX_SCAN 16
#define INQUIRY_INTERVAL 8
#define SPP_FALLBACK_CHANNEL 1
#define BT_PEER_PATH "settings/bt.json"

typedef enum {
    ST_IDLE = 0,
    ST_SCANNING,
    ST_CONNECTING,
    ST_CONNECTED,
} bt_state_t;

typedef struct {
    bd_addr_t addr;
    char name[32];
    int8_t rssi;
    uint32_t cod;
    bool printer;
} scan_dev_t;

static scan_dev_t devices[MAX_SCAN];
static int device_count;
static btstack_packet_callback_registration_t hci_event_cb;
static btstack_context_callback_registration_t sdp_query_reg;

static bt_state_t state;
static bool sdp_after_inquiry;
static bool inhibit_auto_connect;
static bool auth_retry;
static bool retry_after_disc;
static bool pin_alt;
static uint8_t spp_channel;
static uint16_t rfcomm_cid;
static uint16_t rfcomm_mtu;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t peer_addr;
static bool peer_set;
static char last_error[80];
static char last_rx[96];
static bool pending_battery_query;
static uint8_t tx_job[TSPL_JOB_MAX];
static size_t tx_len;
static uint32_t next_page_ms;
static uint32_t page_backoff_ms = 8000;
static uint32_t last_keepalive_ms;
static size_t tx_off;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void print_pump(void);
static void open_rfcomm(uint8_t channel);
static void set_error(const char *msg);

static void peer_save(void) {
    if (!peer_set) {
        return;
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"address\":\"%s\"}", bd_addr_to_str(peer_addr));
    if (fs_begin_write(BT_PEER_PATH) < 0) {
        return;
    }
    int n = fs_write((const uint8_t *)buf, strlen(buf));
    if (fs_end_write() < 0 || n < 0) {
        fs_abort_write();
    }
}

static void peer_load(void) {
    size_t sz = 0;
    int h = fs_begin_read(BT_PEER_PATH, &sz);
    if (h < 0) {
        return;
    }
    char buf[96];
    if (sz >= sizeof(buf)) {
        sz = sizeof(buf) - 1;
    }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    const char *p = strstr(buf, "\"address\"");
    if (!p) {
        return;
    }
    p += 9;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;
    char addr[24];
    size_t i = 0;
    while (p[i] && p[i] != '"' && i + 1 < sizeof(addr)) {
        addr[i] = p[i];
        i++;
    }
    addr[i] = 0;
    if (sscanf_bd_addr(addr, peer_addr)) {
        peer_set = true;
        printf("bt: remembered printer %s\n", bd_addr_to_str(peer_addr));
    }
}

static bool is_auth_status(uint8_t status) {
    return status == ERROR_CODE_AUTHENTICATION_FAILURE ||
           status == ERROR_CODE_PIN_OR_KEY_MISSING ||
           status == ERROR_CODE_CONNECTION_REJECTED_DUE_TO_SECURITY_REASONS ||
           status == L2CAP_CONNECTION_PIN_OR_LINK_KEY_MISSING;
}

static const char *hci_status_name(uint8_t status) {
    switch (status) {
        case ERROR_CODE_PAGE_TIMEOUT:
            return "page timeout";
        case ERROR_CODE_AUTHENTICATION_FAILURE:
            return "auth failure";
        case ERROR_CODE_PIN_OR_KEY_MISSING:
            return "pin/key missing";
        case ERROR_CODE_CONNECTION_TIMEOUT:
            return "connection timeout";
        case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
            return "remote hangup";
        case ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST:
            return "local hangup";
        case ERROR_CODE_LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT:
            return "lmp timeout";
        case ERROR_CODE_CONNECTION_FAILED_TO_BE_ESTABLISHED:
            return "connect not established";
        case ERROR_CODE_CONNECTION_REJECTED_DUE_TO_SECURITY_REASONS:
            return "rejected (security)";
        case L2CAP_CONNECTION_PIN_OR_LINK_KEY_MISSING:
            return "link key missing";
        default:
            return "";
    }
}

static void drop_peer_key(void) {
    if (!peer_set) {
        return;
    }
    gap_drop_link_key_for_bd_addr(peer_addr);
    printf("bt: dropped link key for %s\n", bd_addr_to_str(peer_addr));
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void arm_page_backoff(uint32_t ms) {
    next_page_ms = now_ms() + ms;
}

static void connect_failed(uint8_t status) {
    char msg[80];
    const char *why = hci_status_name(status);
    if (why[0]) {
        snprintf(msg, sizeof(msg), "connect failed 0x%02x (%s)", status, why);
    } else {
        snprintf(msg, sizeof(msg), "connect failed 0x%02x", status);
    }
    set_error(msg);
    rfcomm_cid = 0;
    if (status == ERROR_CODE_PAGE_TIMEOUT || status == ERROR_CODE_CONNECTION_TIMEOUT) {
        drop_peer_key();
        arm_page_backoff(page_backoff_ms);
        if (page_backoff_ms < 60000) {
            page_backoff_ms *= 2;
            if (page_backoff_ms > 60000) {
                page_backoff_ms = 60000;
            }
        }
        retry_after_disc = false;
        auth_retry = false;
        state = ST_IDLE;
        return;
    }
    if (is_auth_status(status) && !auth_retry) {
        auth_retry = true;
        pin_alt = true;
        drop_peer_key();
        if (acl_handle != HCI_CON_HANDLE_INVALID) {
            retry_after_disc = true;
            gap_disconnect(acl_handle);
            return;
        }
        open_rfcomm(spp_channel ? spp_channel : SPP_FALLBACK_CHANNEL);
        return;
    }
    retry_after_disc = false;
    auth_retry = false;
    state = ST_IDLE;
}

static void set_error(const char *msg) {
    snprintf(last_error, sizeof(last_error), "%s", msg);
    printf("bt: %s\n", last_error);
}

static bool looks_like_printer(const char *name, uint32_t cod) {
    uint32_t major = (cod >> 8) & 0x1Fu;
    if (major == 6u) {
        return true;
    }
    if (!name[0]) {
        return false;
    }
    return strstr(name, "PM220") || strstr(name, "Nelko") || strstr(name, "NELKO") ||
           strstr(name, "Polono") || strstr(name, "POLONO") || strstr(name, "T45R");
}

static int json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= cap) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = *p;
        } else if ((unsigned char)*p < 0x20) {
            dst[o++] = '?';
        } else {
            dst[o++] = *p;
        }
    }
    if (o < cap) {
        dst[o] = 0;
    }
    return (int)o;
}

static bool pick_scanned_printer(bd_addr_t out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].printer) {
            memcpy(out, devices[i].addr, 6);
            return true;
        }
    }
    if (device_count == 1) {
        memcpy(out, devices[0].addr, 6);
        return true;
    }
    return false;
}

static bool pick_target(bd_addr_t out) {
    if (pick_scanned_printer(out)) {
        return true;
    }
    if (peer_set) {
        memcpy(out, peer_addr, 6);
        return true;
    }
    return false;
}

static void start_sdp_query(void *context) {
    (void)context;
    spp_channel = 0;
    uint8_t err = sdp_client_query_rfcomm_channel_and_name_for_uuid(
        packet_handler, peer_addr, BLUETOOTH_SERVICE_CLASS_SERIAL_PORT);
    if (err) {
        char msg[64];
        snprintf(msg, sizeof(msg), "SDP query start failed 0x%02x", err);
        set_error(msg);
        state = ST_IDLE;
        arm_page_backoff(page_backoff_ms);
    } else {
        printf("bt: SDP query for SPP on %s\n", bd_addr_to_str(peer_addr));
    }
}

static void begin_sdp(void) {
    state = ST_CONNECTING;
    sdp_query_reg.callback = &start_sdp_query;
    sdp_client_register_query_callback(&sdp_query_reg);
}

static void open_rfcomm(uint8_t channel) {
    printf("bt: RFCOMM connect %s channel %u\n", bd_addr_to_str(peer_addr), channel);
    uint8_t err = rfcomm_create_channel(packet_handler, peer_addr, channel, NULL);
    if (err) {
        char msg[64];
        snprintf(msg, sizeof(msg), "RFCOMM create failed 0x%02x", err);
        set_error(msg);
        state = ST_IDLE;
    }
}

static void handle_sdp_event(uint8_t *packet) {
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
            uint8_t ch = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
            const char *name = sdp_event_query_rfcomm_service_get_name(packet);
            printf("bt: SDP service '%s' channel %u\n", name ? name : "", ch);
            if (ch && (spp_channel == 0 || ch == SPP_FALLBACK_CHANNEL)) {
                spp_channel = ch;
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE: {
            uint8_t status = sdp_event_query_complete_get_status(packet);
            if (status) {
                char msg[64];
                snprintf(msg, sizeof(msg), "SDP query failed 0x%02x, trying channel %d",
                         status, SPP_FALLBACK_CHANNEL);
                set_error(msg);
                spp_channel = SPP_FALLBACK_CHANNEL;
            }
            if (spp_channel == 0) {
                printf("bt: no SPP in SDP, falling back to channel %d\n", SPP_FALLBACK_CHANNEL);
                spp_channel = SPP_FALLBACK_CHANNEL;
            }
            if (retry_after_disc || state != ST_CONNECTING) {
                break;
            }
            open_rfcomm(spp_channel);
            break;
        }
        default:
            break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == RFCOMM_DATA_PACKET) {
        if (size == 0) {
            return;
        }
        size_t n = size < sizeof(last_rx) - 1 ? size : sizeof(last_rx) - 1;
        memcpy(last_rx, packet, n);
        last_rx[n] = 0;
        printf("bt: RFCOMM rx %u bytes: ", (unsigned)size);
        for (uint16_t i = 0; i < size && i < 48; i++) {
            char c = (char)packet[i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        putchar('\n');
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    uint8_t event = hci_event_packet_get_type(packet);
    if (event == SDP_EVENT_QUERY_RFCOMM_SERVICE || event == SDP_EVENT_QUERY_COMPLETE) {
        handle_sdp_event(packet);
        return;
    }
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bd_addr_t local;
                gap_local_bd_addr(local);
                printf("BT Classic up, local addr %s\n", bd_addr_to_str(local));
                if (peer_set && !inhibit_auto_connect) {
                    printf("bt: page remembered printer %s\n", bd_addr_to_str(peer_addr));
                    begin_sdp();
                } else {
                    bt_scan_start(8);
                }
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            if (device_count >= MAX_SCAN) {
                break;
            }
            bd_addr_t addr;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            for (int i = 0; i < device_count; i++) {
                if (memcmp(devices[i].addr, addr, 6) == 0) {
                    if (gap_event_inquiry_result_get_name_available(packet) && !devices[i].name[0]) {
                        int n = gap_event_inquiry_result_get_name_len(packet);
                        if (n > (int)sizeof(devices[i].name) - 1) {
                            n = (int)sizeof(devices[i].name) - 1;
                        }
                        memcpy(devices[i].name, gap_event_inquiry_result_get_name(packet), n);
                        devices[i].name[n] = 0;
                        devices[i].printer = looks_like_printer(devices[i].name, devices[i].cod);
                    }
                    return;
                }
            }
            scan_dev_t *d = &devices[device_count++];
            memcpy(d->addr, addr, 6);
            d->cod = gap_event_inquiry_result_get_class_of_device(packet);
            d->rssi = gap_event_inquiry_result_get_rssi_available(packet)
                          ? gap_event_inquiry_result_get_rssi(packet)
                          : 0;
            d->name[0] = 0;
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int n = gap_event_inquiry_result_get_name_len(packet);
                if (n > (int)sizeof(d->name) - 1) {
                    n = (int)sizeof(d->name) - 1;
                }
                memcpy(d->name, gap_event_inquiry_result_get_name(packet), n);
                d->name[n] = 0;
            }
            d->printer = looks_like_printer(d->name, d->cod);
            printf("scan: %s  COD %06lx  rssi %d  %s%s\n",
                   bd_addr_to_str(addr), (unsigned long)d->cod, d->rssi, d->name,
                   d->printer ? " [printer]" : "");
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (state == ST_SCANNING) {
                state = ST_IDLE;
            }
            printf("scan complete, %d device(s)\n", device_count);
            if (sdp_after_inquiry) {
                sdp_after_inquiry = false;
                begin_sdp();
            } else if (!inhibit_auto_connect && state == ST_IDLE) {
                bd_addr_t found;
                if (pick_scanned_printer(found)) {
                    memcpy(peer_addr, found, 6);
                    peer_set = true;
                    printf("bt: auto-connect scanned %s\n", bd_addr_to_str(peer_addr));
                    begin_sdp();
                }
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (status == ERROR_CODE_SUCCESS) {
                acl_handle = hci_event_connection_complete_get_connection_handle(packet);
                printf("bt: ACL up handle 0x%04x %s\n", acl_handle, bd_addr_to_str(addr));
            } else {
                acl_handle = HCI_CON_HANDLE_INVALID;
                printf("bt: ACL failed 0x%02x %s\n", status, bd_addr_to_str(addr));
                if (state == ST_CONNECTING) {
                    connect_failed(status);
                }
            }
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            bool was_up = (state == ST_CONNECTED);
            const char *why = hci_status_name(reason);
            printf("bt: ACL down reason 0x%02x %s\n", reason, why);
            acl_handle = HCI_CON_HANDLE_INVALID;
            rfcomm_cid = 0;
            rfcomm_mtu = 0;
            pending_battery_query = false;
            tx_len = 0;
            tx_off = 0;
            if (retry_after_disc && state == ST_CONNECTING) {
                retry_after_disc = false;
                open_rfcomm(spp_channel ? spp_channel : SPP_FALLBACK_CHANNEL);
                break;
            }
            if (was_up) {
                char msg[80];
                if (why[0]) {
                    snprintf(msg, sizeof(msg), "link dropped 0x%02x (%s)", reason, why);
                } else {
                    snprintf(msg, sizeof(msg), "link dropped 0x%02x", reason);
                }
                set_error(msg);
                if (reason != ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST) {
                    drop_peer_key();
                }
            }
            if (state == ST_CONNECTED || state == ST_CONNECTING) {
                state = ST_IDLE;
            }
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            uint8_t status = hci_event_authentication_complete_get_status(packet);
            printf("bt: authentication complete 0x%02x\n", status);
            if (status && state == ST_CONNECTING) {
                connect_failed(status);
            }
            break;
        }
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
            uint8_t status = hci_event_simple_pairing_complete_get_status(packet);
            printf("bt: SSP complete 0x%02x\n", status);
            if (status && state == ST_CONNECTING) {
                connect_failed(status);
            }
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            const char *pin = pin_alt ? "1234" : "0000";
            printf("bt: PIN request %s -> %s\n", bd_addr_to_str(addr), pin);
            gap_pin_code_response(addr, pin);
            break;
        }
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("bt: SSP confirm %s\n", bd_addr_to_str(addr));
            gap_ssp_confirmation_response(addr);
            break;
        }
        case HCI_EVENT_USER_PASSKEY_REQUEST: {
            bd_addr_t addr;
            hci_event_user_passkey_request_get_bd_addr(packet, addr);
            printf("bt: SSP passkey request %s -> 000000\n", bd_addr_to_str(addr));
            gap_ssp_passkey_response(addr, 0);
            break;
        }
        case RFCOMM_EVENT_CHANNEL_OPENED: {
            uint8_t status = rfcomm_event_channel_opened_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
                rfcomm_mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
                acl_handle = rfcomm_event_channel_opened_get_con_handle(packet);
                state = ST_CONNECTED;
                last_error[0] = 0;
                auth_retry = false;
                retry_after_disc = false;
                pin_alt = false;
                printf("bt: RFCOMM open cid %u mtu %u %s\n", rfcomm_cid, rfcomm_mtu,
                       bd_addr_to_str(peer_addr));
                page_backoff_ms = 8000;
                next_page_ms = 0;
                peer_save();
                last_keepalive_ms = now_ms();
                pending_battery_query = true;
                rfcomm_request_can_send_now_event(rfcomm_cid);
            } else {
                hci_con_handle_t h = rfcomm_event_channel_opened_get_con_handle(packet);
                if (h != HCI_CON_HANDLE_INVALID) {
                    acl_handle = h;
                }
                connect_failed(status);
            }
            break;
        }
        case RFCOMM_EVENT_CAN_SEND_NOW:
            if (pending_battery_query && rfcomm_cid && !tx_len) {
                pending_battery_query = false;
                const char *q = "BATTERY?\r\n";
                printf("bt: send BATTERY?\n");
                rfcomm_send(rfcomm_cid, (uint8_t *)q, (uint16_t)strlen(q));
            } else {
                print_pump();
            }
            break;
        case RFCOMM_EVENT_CHANNEL_CLOSED:
            printf("bt: RFCOMM closed\n");
            rfcomm_cid = 0;
            rfcomm_mtu = 0;
            pending_battery_query = false;
            tx_len = 0;
            tx_off = 0;
            if (retry_after_disc) {
                break;
            }
            if (state == ST_CONNECTED || state == ST_CONNECTING) {
                state = ST_IDLE;
            }
            break;
        default:
            break;
    }
}

void bt_scan_start(int seconds) {
    (void)seconds;
    if (state == ST_CONNECTING || state == ST_CONNECTED) {
        return;
    }
    device_count = 0;
    state = ST_SCANNING;
    int err = gap_inquiry_start(INQUIRY_INTERVAL);
    if (err) {
        printf("bt: inquiry start failed 0x%02x\n", (unsigned)err);
        state = ST_IDLE;
    }
}

bool bt_connect(const char *addr) {
    bd_addr_t target;
    memset(target, 0, sizeof(target));
    if (addr && addr[0]) {
        if (!sscanf_bd_addr(addr, target)) {
            set_error("invalid Bluetooth address");
            return false;
        }
    } else if (!pick_target(target)) {
        set_error("no address; scan first or pass address");
        return false;
    }
    if (state == ST_CONNECTED && peer_set && memcmp(peer_addr, target, 6) == 0) {
        return true;
    }
    if (state == ST_CONNECTED) {
        bt_disconnect();
    }
    memcpy(peer_addr, target, 6);
    peer_set = true;
    peer_save();
    inhibit_auto_connect = false;
    page_backoff_ms = 8000;
    next_page_ms = 0;
    auth_retry = false;
    retry_after_disc = false;
    pin_alt = false;
    last_rx[0] = 0;
    printf("bt: connect %s\n", bd_addr_to_str(peer_addr));
    if (state == ST_SCANNING) {
        sdp_after_inquiry = true;
        state = ST_CONNECTING;
        gap_inquiry_stop();
        return true;
    }
    begin_sdp();
    return true;
}

static void print_pump(void) {
    static bool pumping;
    if (pumping) {
        return;
    }
    pumping = true;
    while (rfcomm_cid && tx_len && tx_off < tx_len && rfcomm_can_send_packet_now(rfcomm_cid)) {
        uint16_t n = (uint16_t)(tx_len - tx_off);
        uint16_t mtu = rfcomm_mtu ? rfcomm_mtu : 64;
        if (n > mtu) {
            n = mtu;
        }
        uint8_t err = rfcomm_send(rfcomm_cid, tx_job + tx_off, n);
        if (err) {
            printf("bt: rfcomm_send 0x%02x at %u\n", err, (unsigned)tx_off);
            break;
        }
        tx_off += n;
    }
    if (tx_len && tx_off >= tx_len) {
        printf("bt: print sent %u bytes\n", (unsigned)tx_len);
        tx_len = 0;
        tx_off = 0;
    }
    bool need_more = tx_len && tx_off < tx_len && rfcomm_cid;
    pumping = false;
    if (need_more) {
        rfcomm_request_can_send_now_event(rfcomm_cid);
    }
}

bool bt_is_printing(void) {
    return tx_len != 0 && tx_off < tx_len;
}

bool bt_print_job(const uint8_t *data, size_t len) {
    if (!bt_is_connected()) {
        set_error("not connected");
        return false;
    }
    if (bt_is_printing()) {
        set_error("print in progress");
        return false;
    }
    if (!data || len == 0 || len > sizeof(tx_job)) {
        set_error("invalid print job");
        return false;
    }
    memcpy(tx_job, data, len);
    tx_len = len;
    tx_off = 0;
    pending_battery_query = false;
    printf("bt: print %u bytes (mtu %u)\n", (unsigned)len, rfcomm_mtu);
    rfcomm_request_can_send_now_event(rfcomm_cid);
    return true;
}

void bt_disconnect(void) {
    inhibit_auto_connect = true;
    sdp_after_inquiry = false;
    retry_after_disc = false;
    auth_retry = false;
    pending_battery_query = false;
    tx_len = 0;
    tx_off = 0;
    if (rfcomm_cid) {
        printf("bt: disconnect cid %u\n", rfcomm_cid);
        rfcomm_disconnect(rfcomm_cid);
        return;
    }
    if (state == ST_SCANNING) {
        gap_inquiry_stop();
    }
    state = ST_IDLE;
}

void bt_poll(void) {
    uint32_t now = now_ms();
    if (state == ST_CONNECTED && rfcomm_cid && !bt_is_printing()) {
        if (now - last_keepalive_ms > 25000) {
            last_keepalive_ms = now;
            pending_battery_query = true;
            rfcomm_request_can_send_now_event(rfcomm_cid);
        }
        return;
    }
    if (inhibit_auto_connect || !peer_set) {
        return;
    }
    if (state != ST_IDLE) {
        return;
    }
    if ((int32_t)(now - next_page_ms) < 0) {
        return;
    }
    printf("bt: retry page %s (waited %u ms)\n", bd_addr_to_str(peer_addr),
           (unsigned)page_backoff_ms);
    begin_sdp();
}

bool bt_has_peer(void) {
    return peer_set;
}

bool bt_is_scanning(void) {
    return state == ST_SCANNING;
}

bool bt_is_connecting(void) {
    return state == ST_CONNECTING;
}

bool bt_is_connected(void) {
    return state == ST_CONNECTED && rfcomm_cid != 0;
}

const char *bt_state_name(void) {
    switch (state) {
        case ST_SCANNING:
            return "scanning";
        case ST_CONNECTING:
            return "connecting";
        case ST_CONNECTED:
            return "connected";
        default:
            return "idle";
    }
}

const char *bt_connected_addr(void) {
    return (state == ST_CONNECTED && peer_set) ? bd_addr_to_str(peer_addr) : "";
}

const char *bt_last_error(void) {
    return last_error;
}

int bt_status_json(char *buf, size_t cap) {
    return snprintf(buf, cap,
                    "{\"ok\":true,\"device\":\"pm220-pico2w\",\"mdns\":\"pm220.local\","
                    "\"bt\":\"%s\",\"printer_connected\":%s,\"printer\":\"%s\"}",
                    bt_state_name(),
                    bt_is_connected() ? "true" : "false",
                    bt_connected_addr());
}

int bt_scan_json(char *buf, size_t cap) {
    int n = snprintf(buf, cap, "{\"scanning\":%s,\"devices\":[",
                     bt_is_scanning() ? "true" : "false");
    if (n < 0) {
        return n;
    }
    for (int i = 0; i < device_count; i++) {
        char name_esc[64];
        json_escape(name_esc, sizeof(name_esc), devices[i].name);
        int m = snprintf(buf + n, cap > (size_t)n ? cap - (size_t)n : 0,
                         "%s{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d,"
                         "\"cod\":\"%06lx\",\"printer\":%s}",
                         i ? "," : "",
                         bd_addr_to_str(devices[i].addr), name_esc, devices[i].rssi,
                         (unsigned long)devices[i].cod,
                         devices[i].printer ? "true" : "false");
        if (m < 0) {
            return m;
        }
        n += m;
        if ((size_t)n >= cap) {
            buf[cap - 1] = 0;
            return (int)cap - 1;
        }
    }
    int m = snprintf(buf + n, cap > (size_t)n ? cap - (size_t)n : 0, "]}");
    return (m < 0) ? m : n + m;
}

int bt_printer_json(char *buf, size_t cap) {
    char rx_esc[128];
    json_escape(rx_esc, sizeof(rx_esc), last_rx);
    return snprintf(buf, cap,
                    "{\"connected\":%s,\"state\":\"%s\",\"address\":\"%s\","
                    "\"channel\":%u,\"mtu\":%u,\"printing\":%s,\"last_rx\":\"%s\",\"error\":\"%s\"}",
                    bt_is_connected() ? "true" : "false", bt_state_name(),
                    peer_set ? bd_addr_to_str(peer_addr) : "",
                    spp_channel, rfcomm_mtu, bt_is_printing() ? "true" : "false",
                    rx_esc, last_error);
}

void bt_core_init(void) {
    l2cap_init();
    rfcomm_init();
    sdp_init();
    sdp_client_init();
    hci_event_cb.callback = &packet_handler;
    hci_add_event_handler(&hci_event_cb);
    gap_set_local_name("PM220 Pico");
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    gap_set_required_encryption_key_size(7);
    gap_set_page_timeout(0x2000);
    gap_set_bondable_mode(1);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    acl_handle = HCI_CON_HANDLE_INVALID;
    peer_load();
    hci_power_control(HCI_POWER_ON);
}
