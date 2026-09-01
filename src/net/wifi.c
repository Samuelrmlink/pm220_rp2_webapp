#include "net/wifi.h"

#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "dhcpserver.h"
#include "net/mdns.h"
#include "net/usb_ncm.h"
#include "fs/fs.h"

#define WIFI_MAX_NET 8
#define WIFI_SSID_MAX 32
#define WIFI_PSK_MAX 63
#define WIFI_SCAN_MAX 16
#define WIFI_BOOT_MS 60000
#define WIFI_JOIN_MS 6000
#define WIFI_DHCP_MS 8000
#define WIFI_STA_LOST_MS 2000
#define WIFI_PERIOD_MS 30000
#define WIFI_JOIN_BACKOFF_MS 10000
#define AP_IP0 192
#define AP_IP1 168
#define AP_IP2 4
#define AP_IP3 1

#define CFG_PATH "settings/config.json"
#define NET_PATH "settings/known_networks.json"

typedef enum {
    SCAN_IDLE = 0,
    SCAN_ALWAYS,
    SCAN_NEVER,
} scan_policy_t;

typedef enum {
    ST_AP = 0,
    ST_STA,
    ST_BOOT,
    ST_JOIN,
} wifi_state_t;

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char password[WIFI_PSK_MAX + 1];
} known_t;

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    int16_t rssi;
    uint16_t chan;
    uint8_t auth;
    uint8_t bssid[6];
} scan_ap_t;

static known_t known[WIFI_MAX_NET];
static int known_n;
static scan_policy_t policy = SCAN_IDLE;
static char ap_ssid[WIFI_SSID_MAX + 1] = "PM220-Pico";
static char ap_pass[WIFI_PSK_MAX + 1] = "pm220pico";

static wifi_state_t state = ST_AP;
static bool ap_up;
static bool dhcp_ap_on;
static bool connecting;
static char sta_ssid[WIFI_SSID_MAX + 1];
static char pending_ssid[WIFI_SSID_MAX + 1];
static char pending_pass[WIFI_PSK_MAX + 1];
static bool pending_join;
static bool pending_scan;
static bool pending_ap;
static bool dirty_known;
static bool dirty_config;
static bool scanning;
static uint32_t boot_start;
static uint32_t join_start;
static uint32_t last_period_scan;
static uint32_t last_join_fail;
static uint32_t sta_not_up_since;
static bool scan_soon;
static uint32_t last_log;
static char last_error[48];

static scan_ap_t scan_aps[WIFI_SCAN_MAX];
static int scan_n;
static bool scan_done;

static dhcp_server_t dhcp_ap;
static err_t (*ap_input_prev)(struct pbuf *p, struct netif *inp);

static bool timed_out(uint32_t now, uint32_t start, uint32_t ms) {
    /* begin_join() samples a later tick than wifi_poll()'s `now`; unsigned
     * subtract would wrap and look like an immediate timeout. */
    return (int32_t)(now - start) >= (int32_t)ms;
}

static bool json_str_from(const char *body, const char *key, char *out, size_t cap, const char **end) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body ? body : "", pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) {
        if (p[n] == '\\' && p[n + 1]) {
            n += 2;
            continue;
        }
        n++;
    }
    if (p[n] != '"') {
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; i++) {
        if (p[i] == '\\' && i + 1 < n) {
            i++;
            out[w++] = p[i];
        } else {
            out[w++] = p[i];
        }
    }
    out[w] = 0;
    if (end) {
        *end = p + n + 1;
    }
    return true;
}

static void json_esc(char *out, size_t cap, const char *in) {
    size_t n = 0;
    if (!in) {
        in = "";
    }
    while (*in && n + 2 < cap) {
        if (*in == '"' || *in == '\\') {
            out[n++] = '\\';
        }
        out[n++] = *in++;
    }
    out[n] = 0;
}

static int load_file(const char *path, char *buf, size_t cap) {
    size_t sz = 0;
    int h = fs_begin_read(path, &sz);
    if (h < 0) {
        return -1;
    }
    if (sz >= cap) {
        sz = cap - 1;
    }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n < 0) {
        buf[0] = 0;
        return -1;
    }
    buf[n] = 0;
    return n;
}

static int save_file(const char *path, const char *text) {
    if (fs_begin_write(path) < 0) {
        return -1;
    }
    int n = fs_write((const uint8_t *)text, strlen(text));
    if (fs_end_write() < 0 || n < 0) {
        fs_abort_write();
        return -1;
    }
    return 0;
}

static const char *policy_str(scan_policy_t p) {
    switch (p) {
        case SCAN_ALWAYS:
            return "always";
        case SCAN_NEVER:
            return "never";
        default:
            return "idle";
    }
}

static scan_policy_t policy_parse(const char *s) {
    if (s && strcmp(s, "always") == 0) {
        return SCAN_ALWAYS;
    }
    if (s && strcmp(s, "never") == 0) {
        return SCAN_NEVER;
    }
    return SCAN_IDLE;
}

static void persist_config(void) {
    char ssid_e[WIFI_SSID_MAX * 2 + 4];
    char pass_e[WIFI_PSK_MAX * 2 + 4];
    char mdns_e[128];
    json_esc(ssid_e, sizeof(ssid_e), ap_ssid);
    json_esc(pass_e, sizeof(pass_e), ap_pass);
    json_esc(mdns_e, sizeof(mdns_e), mdns_hostname());
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"scan\":\"%s\",\"mdns\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"%s\"}",
             policy_str(policy), mdns_e, ssid_e, pass_e);
    save_file(CFG_PATH, buf);
}

static void persist_known(void) {
    char buf[1024];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "[");
    for (int i = 0; i < known_n && n + 80 < sizeof(buf); i++) {
        char se[WIFI_SSID_MAX * 2 + 4];
        char pe[WIFI_PSK_MAX * 2 + 4];
        json_esc(se, sizeof(se), known[i].ssid);
        json_esc(pe, sizeof(pe), known[i].password);
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s{\"ssid\":\"%s\",\"password\":\"%s\"}",
                              i ? "," : "", se, pe);
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
    save_file(NET_PATH, buf);
}

static void load_config(void) {
    char buf[640];
    if (load_file(CFG_PATH, buf, sizeof(buf)) < 0) {
        return;
    }
    char tmp[64];
    if (json_str_from(buf, "scan", tmp, sizeof(tmp), NULL)) {
        policy = policy_parse(tmp);
    }
    if (json_str_from(buf, "mdns", tmp, sizeof(tmp), NULL)) {
        mdns_set_hostname(tmp);
    }
    if (json_str_from(buf, "ap_ssid", tmp, sizeof(tmp), NULL) && tmp[0]) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", tmp);
    }
    if (json_str_from(buf, "ap_password", tmp, sizeof(tmp), NULL)) {
        snprintf(ap_pass, sizeof(ap_pass), "%s", tmp);
    }
}

static void load_known(void) {
    char buf[1024];
    known_n = 0;
    if (load_file(NET_PATH, buf, sizeof(buf)) < 0) {
        return;
    }
    const char *p = buf;
    while (known_n < WIFI_MAX_NET) {
        char ssid[WIFI_SSID_MAX + 1];
        const char *end = NULL;
        if (!json_str_from(p, "ssid", ssid, sizeof(ssid), &end)) {
            break;
        }
        char pass[WIFI_PSK_MAX + 1] = {0};
        json_str_from(end, "password", pass, sizeof(pass), NULL);
        snprintf(known[known_n].ssid, sizeof(known[known_n].ssid), "%s", ssid);
        snprintf(known[known_n].password, sizeof(known[known_n].password), "%s", pass);
        known_n++;
        p = end;
    }
}

static int known_index(const char *ssid) {
    for (int i = 0; i < known_n; i++) {
        if (strcmp(known[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static int save_known(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > WIFI_SSID_MAX) {
        return -1;
    }
    if (password && strlen(password) > WIFI_PSK_MAX) {
        return -1;
    }
    int i = known_index(ssid);
    if (i < 0) {
        if (known_n >= WIFI_MAX_NET) {
            return -2;
        }
        i = known_n++;
        snprintf(known[i].ssid, sizeof(known[i].ssid), "%s", ssid);
    }
    snprintf(known[i].password, sizeof(known[i].password), "%s", password ? password : "");
    dirty_known = true;
    return 0;
}

static int ap_client_count(void) {
    if (!ap_up) {
        return 0;
    }
    int n = 8;
    uint8_t macs[8 * 6];
    cyw43_wifi_ap_get_stas(&cyw43_state, &n, macs);
    return n;
}

static void stop_ap(void);

static void use_default_netif(struct netif *n) {
    if (n) {
        netif_set_default(n);
    }
}

static err_t ap_input_log(struct pbuf *p, struct netif *inp) {
    if (p && p->len >= 38) {
        const uint8_t *b = (const uint8_t *)p->payload;
        uint16_t etype = (uint16_t)((b[12] << 8) | b[13]);
        if (etype == 0x0800) {
            int ihl = (b[14] & 0x0f) * 4;
            int off = 14 + ihl;
            if (b[23] == 17 && p->len >= off + 8) {
                uint16_t dport = (uint16_t)((b[off + 2] << 8) | b[off + 3]);
                uint16_t ulen = (uint16_t)((b[off + 4] << 8) | b[off + 5]);
                if (dport == 67 && ulen > 8) {
                    uint16_t bootp_len = (uint16_t)(ulen - 8);
                    if (off + 8 + bootp_len > p->len) {
                        bootp_len = (uint16_t)(p->len - off - 8);
                    }
                    dhcp_handle_bootp(&dhcp_ap, b + off + 8, bootp_len);
                }
            }
        }
    }
    return ap_input_prev ? ap_input_prev(p, inp) : ERR_OK;
}

static void sta_disconnect(void) {
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    mdns_remove_netif(&cyw43_state.netif[CYW43_ITF_STA]);
    netif_set_link_down(&cyw43_state.netif[CYW43_ITF_STA]);
}

static void start_ap(void) {
    if (ap_up) {
        stop_ap();
    }
    cyw43_arch_lwip_begin();
    /* Always DISASSOC. Join fail is ST_JOIN with connecting already false. */
    sta_disconnect();
    cyw43_arch_enable_ap_mode(ap_ssid, ap_pass[0] ? ap_pass : NULL,
                              ap_pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, AP_IP0, AP_IP1, AP_IP2, AP_IP3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(ap, &gw, &mask, &gw);
    netif_set_up(ap);
    netif_set_link_up(ap);
    use_default_netif(ap);
    if (ap->input != ap_input_log) {
        ap_input_prev = ap->input;
        ap->input = ap_input_log;
    }
    if (dhcp_ap_on) {
        dhcp_server_deinit(&dhcp_ap);
    }
    dhcp_server_init(&dhcp_ap, ap, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    dhcp_ap_on = true;
    mdns_add_netif(ap);
    cyw43_arch_lwip_end();
    ap_up = true;
    connecting = false;
    sta_ssid[0] = 0;
    sta_not_up_since = 0;
    scan_soon = true;
    state = ST_AP;
    printf("wifi AP '%s' 192.168.4.1\n", ap_ssid);
}

static void stop_ap(void) {
    if (!ap_up) {
        return;
    }
    cyw43_arch_lwip_begin();
    mdns_remove_netif(&cyw43_state.netif[CYW43_ITF_AP]);
    if (dhcp_ap_on) {
        dhcp_server_deinit(&dhcp_ap);
        dhcp_ap_on = false;
    }
    cyw43_arch_disable_ap_mode();
    use_default_netif(usb_ncm_netif());
    cyw43_arch_lwip_end();
    ap_up = false;
}

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r || !r->ssid_len) {
        return 0;
    }
    char ssid[WIFI_SSID_MAX + 1];
    size_t n = r->ssid_len > WIFI_SSID_MAX ? WIFI_SSID_MAX : r->ssid_len;
    memcpy(ssid, r->ssid, n);
    ssid[n] = 0;
    for (int i = 0; i < scan_n; i++) {
        if (strcmp(scan_aps[i].ssid, ssid) == 0) {
            if (r->rssi > scan_aps[i].rssi) {
                scan_aps[i].rssi = r->rssi;
                scan_aps[i].chan = r->channel;
                scan_aps[i].auth = r->auth_mode;
                memcpy(scan_aps[i].bssid, r->bssid, 6);
            }
            return 0;
        }
    }
    if (scan_n >= WIFI_SCAN_MAX) {
        return 0;
    }
    snprintf(scan_aps[scan_n].ssid, sizeof(scan_aps[scan_n].ssid), "%s", ssid);
    scan_aps[scan_n].rssi = r->rssi;
    scan_aps[scan_n].chan = r->channel;
    scan_aps[scan_n].auth = r->auth_mode;
    memcpy(scan_aps[scan_n].bssid, r->bssid, 6);
    scan_n++;
    return 0;
}

static void begin_scan(void) {
    if (state == ST_JOIN || connecting) {
        return;
    }
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        return;
    }
    scan_n = 0;
    scan_done = false;
    scanning = true;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb);
    if (err) {
        printf("wifi scan start %d\n", err);
        scanning = false;
        scan_done = true;
    }
}

static const known_t *known_in_scan(void) {
    for (int i = 0; i < known_n; i++) {
        for (int j = 0; j < scan_n; j++) {
            if (strcmp(known[i].ssid, scan_aps[j].ssid) == 0) {
                return &known[i];
            }
        }
    }
    return NULL;
}

static const scan_ap_t *scan_find(const char *ssid) {
    for (int i = 0; i < scan_n; i++) {
        if (strcmp(scan_aps[i].ssid, ssid) == 0) {
            return &scan_aps[i];
        }
    }
    return NULL;
}

static void begin_join(const char *ssid, const char *password) {
    if (cyw43_wifi_scan_active(&cyw43_state) || scanning) {
        pending_join = true;
        snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
        snprintf(pending_pass, sizeof(pending_pass), "%s", password ? password : "");
        return;
    }
    stop_ap();
    snprintf(sta_ssid, sizeof(sta_ssid), "%s", ssid);
    connecting = true;
    state = ST_JOIN;
    join_start = to_ms_since_boot(get_absolute_time());
    last_error[0] = 0;
    const char *pw = (password && password[0]) ? password : NULL;
    uint32_t auth = pw ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    const scan_ap_t *seen = scan_find(ssid);
    const uint8_t *bssid = seen ? seen->bssid : NULL;
    int err = cyw43_arch_wifi_connect_bssid_async(ssid, bssid, pw, auth);
    printf("wifi join '%s' auth %lu bssid %s err %d\n", ssid, (unsigned long)auth,
           bssid ? "yes" : "no", err);
    if (err) {
        snprintf(last_error, sizeof(last_error), "join ioctl %d", err);
        connecting = false;
        last_join_fail = to_ms_since_boot(get_absolute_time());
        start_ap();
    }
}

static void on_sta_up(void) {
    connecting = false;
    state = ST_STA;
    sta_not_up_since = 0;
    cyw43_arch_lwip_begin();
    struct netif *sta = &cyw43_state.netif[CYW43_ITF_STA];
    netif_set_default(sta);
    mdns_add_netif(sta);
    cyw43_arch_lwip_end();
    printf("wifi STA '%s' ip %s\n", sta_ssid, ip4addr_ntoa(netif_ip4_addr(sta)));
}

static bool scan_allowed_periodic(void) {
    if (policy == SCAN_NEVER || state != ST_AP) {
        return false;
    }
    if (policy == SCAN_ALWAYS) {
        return true;
    }
    return ap_client_count() == 0;
}

void wifi_init(void) {
    load_config();
    load_known();
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    cyw43_arch_lwip_begin();
    use_default_netif(usb_ncm_netif());
    cyw43_arch_lwip_end();
    boot_start = to_ms_since_boot(get_absolute_time());
    last_period_scan = boot_start;
    if (known_n > 0) {
        state = ST_BOOT;
        begin_scan();
        printf("wifi: %d known, searching 60s\n", known_n);
    } else {
        start_ap();
    }
}

void wifi_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (scanning && !cyw43_wifi_scan_active(&cyw43_state)) {
        scanning = false;
        scan_done = true;
        printf("wifi scan done %d aps\n", scan_n);
    }
    if (dirty_known) {
        dirty_known = false;
        persist_known();
    }
    if (dirty_config) {
        dirty_config = false;
        persist_config();
    }
    if (pending_ap) {
        pending_ap = false;
        pending_join = false;
        start_ap();
    }
    if (pending_join && !cyw43_wifi_scan_active(&cyw43_state) && !scanning) {
        pending_join = false;
        begin_join(pending_ssid, pending_pass);
    }
    if (pending_scan && state != ST_JOIN && !connecting) {
        pending_scan = false;
        begin_scan();
    }

    if (state == ST_BOOT && timed_out(now, boot_start, WIFI_BOOT_MS)) {
        start_ap();
    } else if (state == ST_BOOT && scan_done && !pending_join) {
        const known_t *k = known_in_scan();
        if (k) {
            begin_join(k->ssid, k->password);
        } else if (!scanning) {
            begin_scan();
        }
    }

    if (state == ST_JOIN) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) {
            on_sta_up();
        } else if (st == CYW43_LINK_BADAUTH) {
            snprintf(last_error, sizeof(last_error), "bad password");
            printf("wifi join bad auth\n");
            connecting = false;
            last_join_fail = now;
            start_ap();
        } else if (st == CYW43_LINK_NOIP) {
            /* Associated; wait for DHCP rather than aborting a live join. */
            if (timed_out(now, join_start, WIFI_JOIN_MS + WIFI_DHCP_MS)) {
                snprintf(last_error, sizeof(last_error), "dhcp timeout");
                printf("wifi join dhcp timeout\n");
                connecting = false;
                last_join_fail = now;
                start_ap();
            }
        } else if (timed_out(now, join_start, WIFI_JOIN_MS)) {
            snprintf(last_error, sizeof(last_error), "join timeout status %d", st);
            printf("wifi join timeout status %d\n", st);
            connecting = false;
            last_join_fail = now;
            start_ap();
        }
    }

    if (state == ST_STA) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        int ws = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        bool up = (st == CYW43_LINK_UP) &&
                  (ws != CYW43_LINK_DOWN) &&
                  (ws != CYW43_LINK_FAIL) &&
                  (ws != CYW43_LINK_NONET) &&
                  (ws != CYW43_LINK_BADAUTH);
        if (up) {
            sta_not_up_since = 0;
        } else {
            if (!sta_not_up_since) {
                sta_not_up_since = now;
            }
            if (timed_out(now, sta_not_up_since, WIFI_STA_LOST_MS)) {
                snprintf(last_error, sizeof(last_error), "STA lost status %d", st);
                printf("wifi STA lost tcp %d wifi %d, starting AP\n", st, ws);
                last_join_fail = now;
                connecting = false;
                start_ap();
            }
        }
    }

    if (state == ST_AP && scan_allowed_periodic() && !scanning && !pending_join &&
        (scan_soon || timed_out(now, last_period_scan, WIFI_PERIOD_MS))) {
        scan_soon = false;
        last_period_scan = now;
        begin_scan();
    }
    if (state == ST_AP && scan_done && !scanning && !pending_join &&
        timed_out(now, last_join_fail, WIFI_JOIN_BACKOFF_MS)) {
        scan_done = false;
        const known_t *k = known_in_scan();
        if (k && scan_allowed_periodic()) {
            begin_join(k->ssid, k->password);
        }
    }

    if (ap_up && now - last_log > 3000) {
        last_log = now;
        struct netif *ap_if = &cyw43_state.netif[CYW43_ITF_AP];
        cyw43_arch_lwip_begin();
        if (!(ap_if->flags & NETIF_FLAG_LINK_UP)) {
            netif_set_link_up(ap_if);
        }
        cyw43_arch_lwip_end();
    }
}

void wifi_status_json(char *buf, size_t cap) {
    const char *mode = "ap";
    if (state == ST_STA) {
        mode = "sta";
    } else if (state == ST_JOIN || state == ST_BOOT) {
        mode = connecting ? "connecting" : "down";
    }
    char ip[16] = "";
    if (state == ST_STA) {
        snprintf(ip, sizeof(ip), "%s",
                 ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
    } else if (ap_up) {
        snprintf(ip, sizeof(ip), "192.168.4.1");
    }
    char mdns_e[128];
    char ap_e[WIFI_SSID_MAX * 2 + 4];
    char pass_e[WIFI_PSK_MAX * 2 + 4];
    char sta_e[WIFI_SSID_MAX * 2 + 4];
    char err_e[96];
    json_esc(mdns_e, sizeof(mdns_e), mdns_hostname());
    json_esc(ap_e, sizeof(ap_e), ap_ssid);
    json_esc(pass_e, sizeof(pass_e), ap_pass);
    json_esc(sta_e, sizeof(sta_e), sta_ssid);
    json_esc(err_e, sizeof(err_e), last_error);
    snprintf(buf, cap,
             "{\"ok\":true,\"mode\":\"%s\",\"mdns\":\"%s\",\"ap_ssid\":\"%s\","
             "\"ap_password\":\"%s\",\"sta_ssid\":\"%s\",\"ip\":\"%s\",\"scan\":\"%s\","
             "\"scan_disturbs_ap\":true,\"ap_clients\":%d,\"connecting\":%s,"
             "\"last_error\":\"%s\"}",
             mode, mdns_e, ap_e, pass_e, sta_e, ip, policy_str(policy),
             ap_client_count(), connecting ? "true" : "false", err_e);
}

static const char *auth_name(uint8_t a) {
    if (a & 4) {
        return "wpa2";
    }
    if (a & 2) {
        return "wpa";
    }
    if (a & 1) {
        return "wep";
    }
    return "open";
}

void wifi_scan_json(char *buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "{\"scanning\":%s,\"aps\":[",
                          scanning ? "true" : "false");
    for (int i = 0; i < scan_n && n + 80 < cap; i++) {
        char se[WIFI_SSID_MAX * 2 + 4];
        json_esc(se, sizeof(se), scan_aps[i].ssid);
        n += (size_t)snprintf(buf + n, cap - n,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"chan\":%u,\"auth\":\"%s\",\"known\":%s}",
                              i ? "," : "", se, (int)scan_aps[i].rssi, (unsigned)scan_aps[i].chan,
                              auth_name(scan_aps[i].auth),
                              known_index(scan_aps[i].ssid) >= 0 ? "true" : "false");
    }
    snprintf(buf + n, cap - n, "]}");
}

void wifi_networks_json(char *buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "{\"scan\":\"%s\",\"networks\":[", policy_str(policy));
    for (int i = 0; i < known_n && n + 200 < cap; i++) {
        char se[WIFI_SSID_MAX * 2 + 4];
        char pe[WIFI_PSK_MAX * 2 + 4];
        json_esc(se, sizeof(se), known[i].ssid);
        json_esc(pe, sizeof(pe), known[i].password);
        n += (size_t)snprintf(buf + n, cap - n, "%s{\"ssid\":\"%s\",\"password\":\"%s\"}",
                              i ? "," : "", se, pe);
    }
    snprintf(buf + n, cap - n, "]}");
}

int wifi_request_scan(void) {
    pending_scan = true;
    return 0;
}

int wifi_connect_save(const char *ssid, const char *password) {
    int err = save_known(ssid, password);
    if (err) {
        return err;
    }
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
    snprintf(pending_pass, sizeof(pending_pass), "%s", password ? password : "");
    pending_join = true;
    connecting = true;
    return 0;
}

int wifi_connect_known(const char *ssid) {
    int i = known_index(ssid);
    if (i < 0) {
        return -1;
    }
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", known[i].ssid);
    snprintf(pending_pass, sizeof(pending_pass), "%s", known[i].password);
    pending_join = true;
    connecting = true;
    return 0;
}

int wifi_save_network(const char *ssid, const char *password, const char *new_ssid) {
    if (!ssid || !ssid[0] || strlen(ssid) > WIFI_SSID_MAX) {
        return -1;
    }
    if (password && strlen(password) > WIFI_PSK_MAX) {
        return -1;
    }
    if (new_ssid && new_ssid[0] && strlen(new_ssid) > WIFI_SSID_MAX) {
        return -1;
    }
    int i = known_index(ssid);
    if (i < 0) {
        return save_known(new_ssid && new_ssid[0] ? new_ssid : ssid,
                          password ? password : "");
    }
    if (new_ssid && new_ssid[0] && strcmp(new_ssid, ssid) != 0) {
        int j = known_index(new_ssid);
        if (j >= 0 && j != i) {
            return -2;
        }
        snprintf(known[i].ssid, sizeof(known[i].ssid), "%s", new_ssid);
    }
    if (password) {
        snprintf(known[i].password, sizeof(known[i].password), "%s", password);
    }
    dirty_known = true;
    return 0;
}

int wifi_delete_network(const char *ssid) {
    int i = known_index(ssid);
    if (i < 0) {
        return -1;
    }
    for (int j = i; j < known_n - 1; j++) {
        known[j] = known[j + 1];
    }
    known_n--;
    dirty_known = true;
    return 0;
}

int wifi_set_scan_policy(const char *p) {
    if (!p || (strcmp(p, "idle") && strcmp(p, "always") && strcmp(p, "never"))) {
        return -1;
    }
    policy = policy_parse(p);
    dirty_config = true;
    return 0;
}

int wifi_set_mdns(const char *name) {
    if (!mdns_set_hostname(name)) {
        return -1;
    }
    dirty_config = true;
    return 0;
}

int wifi_set_ap_creds(const char *ssid, const char *password) {
    if (ssid) {
        if (!ssid[0] || strlen(ssid) > WIFI_SSID_MAX) {
            return -1;
        }
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
    }
    if (password) {
        if (strlen(password) > WIFI_PSK_MAX) {
            return -1;
        }
        snprintf(ap_pass, sizeof(ap_pass), "%s", password);
    }
    dirty_config = true;
    if (state == ST_AP && ap_up) {
        pending_ap = true;
    }
    return 0;
}

int wifi_force_ap(void) {
    pending_ap = true;
    return 0;
}
