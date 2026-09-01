#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "bt/bt_core.h"
#include "printer/job.h"
#include "printer/tspl.h"
#include "fs/fs.h"
#include "net/wifi.h"

#define HTTP_HDR 1024
#define HTTP_CONNS 8
#define FILE_CHUNK 1460
#define HTTP_POLL_INT 2
#define HTTP_MAX_RETRIES 20

static char json_buf[4096];
static uint8_t print_bitmap[TSPL_BITMAP_MAX];
static uint8_t print_job[TSPL_JOB_MAX];
static uint8_t file_chunk[FILE_CHUNK];

typedef struct {
    struct tcp_pcb *pcb;
    char hdr[HTTP_HDR];
    uint16_t hdr_len;
    bool hdr_done;
    size_t content_len;
    size_t body_got;
    bool taking_print_body;
    bool taking_fs_body;
    bool taking_post_body;
    bool sending_file;
    char fs_name[FS_NAME_MAX + 1];
    char method[8];
    char path[96];
    char post_body[256];
    int file_h;
    size_t file_left;
    bool closing;
    bool pending_file;
    bool pending_gzip;
    uint8_t retries;
} http_conn_t;

static http_conn_t conns[HTTP_CONNS];

static bool path_is(const char *path, const char *want) {
    size_t n = strlen(want);
    return strncmp(path, want, n) == 0 && (path[n] == 0 || path[n] == '?' || path[n] == '#');
}

static const char *status_text(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

static const char *cors_methods(void) {
    return "GET, POST, PUT, DELETE, OPTIONS";
}

static err_t http_reply(struct tcp_pcb *tpcb, int code, const char *ctype, const char *body) {
    if (!body) {
        body = "";
    }
    char hdr[360];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "Cache-Control: no-store\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: %s\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n\r\n",
                      code, status_text(code), ctype, (unsigned)strlen(body), cors_methods());
    tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY);
    if (body[0]) {
        tcp_write(tpcb, body, (uint16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    tcp_close(tpcb);
    return ERR_OK;
}

static http_conn_t *conn_find(struct tcp_pcb *tpcb) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb == tpcb) {
            return &conns[i];
        }
    }
    return NULL;
}

static void conn_cleanup(http_conn_t *c) {
    if (c->taking_fs_body) {
        fs_abort_write();
        if (c->fs_name[0]) {
            fs_delete(c->fs_name);
        }
    }
    if (c->sending_file) {
        fs_end_read(c->file_h);
    }
}

static void conn_reset(http_conn_t *c) {
    memset(c, 0, sizeof(*c));
    c->file_h = -1;
}

static bool conn_held(const http_conn_t *c) {
    return c->sending_file || c->closing || c->pending_file;
}

static void conn_drop(struct tcp_pcb *tpcb) {
    http_conn_t *c = conn_find(tpcb);
    if (c) {
        conn_cleanup(c);
        conn_reset(c);
    }
}

static http_conn_t *conn_for(struct tcp_pcb *tpcb) {
    http_conn_t *c = conn_find(tpcb);
    if (c) {
        return c;
    }
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (!conns[i].pcb) {
            conn_reset(&conns[i]);
            conns[i].pcb = tpcb;
            return &conns[i];
        }
    }
    return NULL;
}

static bool find_bt_addr(const char *s, char *out, size_t cap) {
    bd_addr_t dummy;
    for (const char *p = s; *p; p++) {
        if (sscanf_bd_addr(p, dummy)) {
            size_t i = 0;
            while (p[i] && i + 1 < cap && i < 17) {
                out[i] = p[i];
                i++;
            }
            out[i] = 0;
            return true;
        }
    }
    return false;
}

static bool start_test_print(void) {
    size_t n = 0;
    if (!job_test_frame(print_job, sizeof(print_job), &n)) {
        return false;
    }
    return bt_print_job(print_job, n);
}

static bool start_bitmap_print(size_t body_len, job_layout_t *layout) {
    if (body_len == 0 || body_len % TSPL_WIDTH_BYTES != 0) {
        return false;
    }
    int height = (int)(body_len / TSPL_WIDTH_BYTES);
    if (height > TSPL_HEIGHT_DOTS) {
        return false;
    }
    size_t n = 0;
    if (!job_from_bitmap(print_bitmap, TSPL_WIDTH_BYTES, height, print_job, sizeof(print_job), &n,
                         layout)) {
        return false;
    }
    return bt_print_job(print_job, n);
}

static bool copy_path_token(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '#') {
        n++;
    }
    if (n == 0 || n >= cap) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool fs_api_name(const char *path, char *out, size_t cap) {
    if (strncmp(path, "/api/fs/", 8) != 0) {
        return false;
    }
    if (!copy_path_token(path + 8, out, cap)) {
        return false;
    }
    size_t n = strlen(out);
    if (n && out[n - 1] == '/') {
        out[n - 1] = 0;
    }
    return out[0] != 0;
}

static bool json_str_field(const char *body, const char *key, char *out, size_t cap) {
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
        n++;
    }
    if (p[n] != '"') {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool static_req_name(const char *path, char *out, size_t cap) {
    if (strncmp(path, "/api/", 5) == 0) {
        return false;
    }
    const char *p = path;
    if (*p == '/') {
        p++;
    }
    if (*p == 0 || *p == '?' || *p == '#') {
        if (cap < 11) {
            return false;
        }
        memcpy(out, "index.html", 11);
        return true;
    }
    if (!copy_path_token(p, out, cap)) {
        return false;
    }
    if (strchr(out, '/')) {
        return false;
    }
    return fs_valid_name(out);
}

static bool ends_with(const char *s, const char *suf) {
    size_t n = strlen(s);
    size_t m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static bool resolve_static(const char *req, char *name, size_t cap, bool *gzipped) {
    *gzipped = false;
    size_t n = strlen(req);
    if (!ends_with(req, ".gz") && n + 4 < cap) {
        memcpy(name, req, n);
        memcpy(name + n, ".gz", 4);
        if (fs_stat(name, NULL)) {
            *gzipped = true;
            return true;
        }
    }
    if (n >= cap) {
        return false;
    }
    memcpy(name, req, n + 1);
    return fs_stat(name, NULL);
}

static const char *mime_of(const char *name) {
    char tmp[FS_NAME_MAX + 1];
    size_t n = strlen(name);
    if (n >= 3 && strcmp(name + n - 3, ".gz") == 0) {
        n -= 3;
    }
    if (n == 0 || n >= sizeof(tmp)) {
        return "application/octet-stream";
    }
    memcpy(tmp, name, n);
    tmp[n] = 0;
    const char *ext = strrchr(tmp, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0 || strcmp(ext, ".mjs") == 0) {
        return "text/javascript; charset=utf-8";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(ext, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

static err_t http_start_file(http_conn_t *c, struct tcp_pcb *tpcb, const char *name, bool gzipped);
static void http_kick_pending(void);

static void http_try_close(http_conn_t *c, struct tcp_pcb *tpcb) {
    if (c->sending_file) {
        fs_end_read(c->file_h);
        c->sending_file = false;
        c->file_h = -1;
    }
    c->closing = true;
    if (tcp_close(tpcb) == ERR_OK) {
        conn_reset(c);
    }
}

/* 1 = all bytes queued, 0 = wait for sent/poll, -1 = abort. */
static int http_pump_file(http_conn_t *c, struct tcp_pcb *tpcb) {
    while (c->file_left) {
        u16_t room = tcp_sndbuf(tpcb);
        if (room < 32) {
            break;
        }
        size_t want = c->file_left;
        if (want > sizeof(file_chunk)) {
            want = sizeof(file_chunk);
        }
        if (want > room) {
            want = room;
        }
        int n = fs_read(c->file_h, file_chunk, want);
        if (n <= 0) {
            return -1;
        }
        if (tcp_write(tpcb, file_chunk, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            if (fs_rewind(c->file_h, (size_t)n) < 0) {
                return -1;
            }
            break;
        }
        c->file_left -= (size_t)n;
        c->retries = 0;
    }
    tcp_output(tpcb);
    return c->file_left == 0 ? 1 : 0;
}

static err_t http_finish_file(http_conn_t *c, struct tcp_pcb *tpcb, int rc) {
    if (rc < 0) {
        conn_cleanup(c);
        conn_reset(c);
        tcp_abort(tpcb);
        http_kick_pending();
        return ERR_ABRT;
    }
    if (rc > 0) {
        http_try_close(c, tpcb);
        http_kick_pending();
    }
    return ERR_OK;
}

static void http_park_file(http_conn_t *c, const char *name, bool gzipped) {
    snprintf(c->fs_name, sizeof(c->fs_name), "%s", name);
    c->pending_file = true;
    c->pending_gzip = gzipped;
    c->sending_file = false;
    c->file_h = -1;
}

static err_t http_start_file(http_conn_t *c, struct tcp_pcb *tpcb, const char *name, bool gzipped) {
    size_t sz = 0;
    int h = fs_begin_read(name, &sz);
    if (h < 0) {
        http_park_file(c, name, gzipped);
        return ERR_OK;
    }
    char hdr[400];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "%s"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: %s\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n\r\n",
                      mime_of(name), (unsigned)sz,
                      gzipped ? "Content-Encoding: gzip\r\n" : "",
                      cors_methods());
    if (tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        fs_end_read(h);
        http_park_file(c, name, gzipped);
        return ERR_OK;
    }
    c->pending_file = false;
    c->sending_file = true;
    c->file_h = h;
    c->file_left = sz;
    c->retries = 0;
    return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
}

static void http_kick_pending(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        http_conn_t *c = &conns[i];
        if (c->pcb && c->pending_file) {
            http_start_file(c, c->pcb, c->fs_name, c->pending_gzip);
            if (!c->pending_file) {
                break;
            }
        }
    }
}

static int dispatch(struct tcp_pcb *tpcb, http_conn_t *c, const char *method, const char *path,
                    const char *body, size_t body_len) {
    if (strcmp(method, "OPTIONS") == 0) {
        return http_reply(tpcb, 204, "text/plain", "");
    }
    if (path_is(path, "/api/status") && strcmp(method, "GET") == 0) {
        bt_status_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/wifi") && strcmp(method, "GET") == 0) {
        wifi_status_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/wifi") && strcmp(method, "PUT") == 0) {
        char tmp[64];
        bool any = false;
        if (json_str_field(body, "scan", tmp, sizeof(tmp))) {
            if (wifi_set_scan_policy(tmp) < 0) {
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"bad scan policy\"}");
            }
            any = true;
        }
        if (json_str_field(body, "mdns", tmp, sizeof(tmp))) {
            if (wifi_set_mdns(tmp) < 0) {
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"bad mdns name\"}");
            }
            any = true;
        }
        char ssid[33] = {0};
        char pass[64];
        int have_ssid = json_str_field(body, "ap_ssid", ssid, sizeof(ssid));
        int have_pass = json_str_field(body, "ap_password", pass, sizeof(pass));
        if (have_ssid || have_pass) {
            if (wifi_set_ap_creds(have_ssid ? ssid : NULL, have_pass ? pass : NULL) < 0) {
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"bad AP credentials\"}");
            }
            any = true;
        }
        if (!any) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"no fields\"}");
        }
        wifi_status_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/wifi/scan") && strcmp(method, "GET") == 0) {
        wifi_scan_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/wifi/scan") && strcmp(method, "POST") == 0) {
        wifi_request_scan();
        return http_reply(tpcb, 202, "application/json", "{\"ok\":true,\"scanning\":true}");
    }
    if (path_is(path, "/api/wifi/networks") && strcmp(method, "GET") == 0) {
        wifi_networks_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/wifi/connect") && strcmp(method, "POST") == 0) {
        char ssid[33];
        char pass[64] = {0};
        if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"ssid required\"}");
        }
        int err;
        if (json_str_field(body, "password", pass, sizeof(pass))) {
            err = wifi_connect_save(ssid, pass);
        } else {
            err = wifi_connect_known(ssid);
            if (err) {
                return http_reply(tpcb, 404, "application/json",
                                  "{\"ok\":false,\"error\":\"not found\"}");
            }
        }
        if (err == -2) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"too many networks\"}");
        }
        if (err) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"bad ssid\"}");
        }
        return http_reply(tpcb, 202, "application/json",
                          "{\"ok\":true,\"saved\":true,\"connecting\":true}");
    }
    if (path_is(path, "/api/wifi/networks") && strcmp(method, "PUT") == 0) {
        char ssid[33];
        char pass[64];
        char new_ssid[33];
        if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"ssid required\"}");
        }
        const char *pw = json_str_field(body, "password", pass, sizeof(pass)) ? pass : NULL;
        const char *ren = json_str_field(body, "new_ssid", new_ssid, sizeof(new_ssid))
                              ? new_ssid
                              : NULL;
        int err = wifi_save_network(ssid, pw, ren);
        if (err == -2) {
            return http_reply(tpcb, 409, "application/json",
                              "{\"ok\":false,\"error\":\"ssid exists\"}");
        }
        if (err) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"bad ssid\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true,\"saved\":true}");
    }
    if (path_is(path, "/api/wifi/networks") && strcmp(method, "DELETE") == 0) {
        char ssid[33];
        if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"ssid required\"}");
        }
        if (wifi_delete_network(ssid) < 0) {
            return http_reply(tpcb, 404, "application/json",
                              "{\"ok\":false,\"error\":\"not found\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }
    if (path_is(path, "/api/wifi/ap") && strcmp(method, "POST") == 0) {
        wifi_force_ap();
        return http_reply(tpcb, 202, "application/json", "{\"ok\":true,\"mode\":\"ap\"}");
    }
    if (path_is(path, "/api/scan") && strcmp(method, "GET") == 0) {
        bt_scan_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/scan") && strcmp(method, "POST") == 0) {
        bt_scan_start(8);
        bt_scan_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 202, "application/json", json_buf);
    }
    if (path_is(path, "/api/printer") && strcmp(method, "GET") == 0) {
        bt_printer_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/media") && strcmp(method, "GET") == 0) {
        job_media_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/printer/connect") && strcmp(method, "POST") == 0) {
        char addr[24] = {0};
        const char *arg_addr = NULL;
        if (find_bt_addr(path, addr, sizeof(addr)) || find_bt_addr(body, addr, sizeof(addr))) {
            arg_addr = addr;
        }
        if (!bt_connect(arg_addr)) {
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":false,\"error\":\"%s\"}", bt_last_error());
            return http_reply(tpcb, 400, "application/json", json_buf);
        }
        bt_printer_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 202, "application/json", json_buf);
    }
    if (path_is(path, "/api/printer/disconnect") && strcmp(method, "POST") == 0) {
        bt_disconnect();
        bt_printer_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/print") && strcmp(method, "GET") == 0) {
        job_media_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/print/test") &&
        (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0)) {
        if (!bt_is_connected()) {
            return http_reply(tpcb, 409, "application/json",
                              "{\"ok\":false,\"error\":\"not connected\"}");
        }
        if (!start_test_print()) {
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":false,\"error\":\"%s\"}",
                     bt_last_error()[0] ? bt_last_error() : "job build failed");
            return http_reply(tpcb, 409, "application/json", json_buf);
        }
        return http_reply(tpcb, 202, "application/json", "{\"ok\":true,\"job\":\"test-frame\"}");
    }
    if (path_is(path, "/api/print") && strcmp(method, "POST") == 0) {
        if (!bt_is_connected()) {
            return http_reply(tpcb, 409, "application/json",
                              "{\"ok\":false,\"error\":\"not connected\"}");
        }
        if (bt_is_printing()) {
            return http_reply(tpcb, 409, "application/json",
                              "{\"ok\":false,\"error\":\"print in progress\"}");
        }
        if (body_len == 0 || body_len % TSPL_WIDTH_BYTES != 0 ||
            body_len / TSPL_WIDTH_BYTES > (size_t)TSPL_HEIGHT_DOTS) {
            snprintf(json_buf, sizeof(json_buf),
                     "{\"ok\":false,\"error\":\"bitmap must be %d bytes/row, 1..%d rows "
                     "(%d bytes for a full 50x30 mm label)\"}",
                     TSPL_WIDTH_BYTES, TSPL_HEIGHT_DOTS, TSPL_BITMAP_MAX);
            return http_reply(tpcb, 400, "application/json", json_buf);
        }
        job_layout_t layout;
        if (!start_bitmap_print(body_len, &layout)) {
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":false,\"error\":\"%s\"}",
                     bt_last_error()[0] ? bt_last_error() : "bad bitmap");
            return http_reply(tpcb, 400, "application/json", json_buf);
        }
        snprintf(json_buf, sizeof(json_buf),
                 "{\"ok\":true,\"bytes\":%u,\"width_bytes\":%d,\"height_dots\":%u,"
                 "\"origin_x\":%d,\"origin_y\":%d,\"print_width_dots\":%d,"
                 "\"print_height_dots\":%d}",
                 (unsigned)body_len, TSPL_WIDTH_BYTES,
                 (unsigned)(body_len / TSPL_WIDTH_BYTES),
                 layout.origin_x, layout.origin_y,
                 layout.print_width_dots, layout.print_height_dots);
        return http_reply(tpcb, 202, "application/json", json_buf);
    }
    if (path_is(path, "/api/fs/rename") && strcmp(method, "POST") == 0) {
        char from[FS_NAME_MAX + 1];
        char to[FS_NAME_MAX + 1];
        if (!json_str_field(body, "from", from, sizeof(from)) ||
            !json_str_field(body, "to", to, sizeof(to)) ||
            !fs_valid_path(from) || !fs_valid_path(to)) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"bad name\"}");
        }
        int err = fs_rename(from, to);
        if (err == 1) {
            return http_reply(tpcb, 409, "application/json",
                              "{\"ok\":false,\"error\":\"exists\"}");
        }
        if (err < 0) {
            if (!fs_stat(from, NULL)) {
                return http_reply(tpcb, 404, "application/json",
                                  "{\"ok\":false,\"error\":\"not found\"}");
            }
            return http_reply(tpcb, 500, "application/json",
                              "{\"ok\":false,\"error\":\"rename failed\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }
    if (strncmp(path, "/api/fs", 7) == 0 && strcmp(method, "GET") == 0) {
        if (path_is(path, "/api/fs") || path_is(path, "/api/fs/")) {
            if (fs_list_json("", json_buf, sizeof(json_buf)) < 0) {
                return http_reply(tpcb, 500, "application/json",
                                  "{\"ok\":false,\"error\":\"fs unavailable\"}");
            }
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        char name[FS_NAME_MAX + 1];
        if (!fs_api_name(path, name, sizeof(name))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"bad name\"}");
        }
        if (fs_is_dir(name)) {
            if (fs_list_json(name, json_buf, sizeof(json_buf)) < 0) {
                return http_reply(tpcb, 500, "application/json",
                                  "{\"ok\":false,\"error\":\"fs unavailable\"}");
            }
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        if (!fs_valid_path(name) || !fs_stat(name, NULL)) {
            return http_reply(tpcb, 404, "application/json",
                              "{\"ok\":false,\"error\":\"not found\"}");
        }
        return http_start_file(c, tpcb, name, ends_with(name, ".gz"));
    }
    if (strcmp(method, "DELETE") == 0) {
        char name[FS_NAME_MAX + 1];
        if (!fs_api_name(path, name, sizeof(name)) || !fs_valid_path(name)) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"bad name\"}");
        }
        if (!fs_stat(name, NULL)) {
            return http_reply(tpcb, 404, "application/json",
                              "{\"ok\":false,\"error\":\"not found\"}");
        }
        if (fs_delete(name) < 0) {
            return http_reply(tpcb, 500, "application/json",
                              "{\"ok\":false,\"error\":\"delete failed\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }
    if (strcmp(method, "GET") == 0) {
        char req[FS_NAME_MAX + 1];
        char name[FS_NAME_MAX + 1];
        bool gzipped = false;
        if (static_req_name(path, req, sizeof(req)) &&
            resolve_static(req, name, sizeof(name), &gzipped)) {
            return http_start_file(c, tpcb, name, gzipped);
        }
        if (path_is(path, "/") || path_is(path, "/index.html")) {
            return http_reply(tpcb, 200, "text/plain",
                              "pm220-pico2w\nhttp://pm220.local/\n"
                              "GET /api/status /api/scan /api/wifi /api/printer /api/media /api/print /api/fs\n"
                              "PUT /api/fs/<name>  DELETE /api/fs/<name>\n"
                              "POST /api/scan /api/printer/connect /api/printer/disconnect\n"
                              "POST /api/print  application/octet-stream, packed 1-bit 48 bytes/row\n"
                              "GET|POST /api/print/test\n");
        }
    }
    return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}\n");
}

static size_t header_content_length(const char *hdr) {
    const char *p = hdr;
    while (*p) {
        if ((p == hdr || p[-1] == '\n') && strncasecmp(p, "Content-Length:", 15) == 0) {
            return (size_t)atoi(p + 15);
        }
        p++;
    }
    return 0;
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    (void)len;
    http_conn_t *c = conn_find(tpcb);
    if (!c) {
        return ERR_OK;
    }
    if (c->closing) {
        http_try_close(c, tpcb);
        return ERR_OK;
    }
    if (c->sending_file) {
        return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
    }
    return ERR_OK;
}

static err_t http_poll(void *arg, struct tcp_pcb *tpcb) {
    (void)arg;
    http_conn_t *c = conn_find(tpcb);
    if (!c) {
        return ERR_OK;
    }
    if (!conn_held(c)) {
        return ERR_OK;
    }
    if (c->retries < 255) {
        c->retries++;
    }
    if (c->retries > HTTP_MAX_RETRIES) {
        conn_cleanup(c);
        conn_reset(c);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }
    if (c->pending_file) {
        return http_start_file(c, tpcb, c->fs_name, c->pending_gzip);
    }
    if (c->sending_file) {
        return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
    }
    http_try_close(c, tpcb);
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    (void)err;
    if (!p) {
        conn_drop(tpcb);
        tcp_close(tpcb);
        return ERR_OK;
    }
    tcp_recved(tpcb, p->tot_len);
    http_conn_t *c = conn_for(tpcb);
    if (!c) {
        pbuf_free(p);
        return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }

    size_t offset = 0;
    if (!c->hdr_done) {
        uint16_t space = (uint16_t)(sizeof(c->hdr) - 1 - c->hdr_len);
        uint16_t n = pbuf_copy_partial(p, c->hdr + c->hdr_len, space, 0);
        c->hdr_len += n;
        c->hdr[c->hdr_len] = 0;
        char *split = strstr(c->hdr, "\r\n\r\n");
        if (!split) {
            pbuf_free(p);
            if (c->hdr_len >= sizeof(c->hdr) - 1) {
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"headers too large\"}");
            }
            return ERR_OK;
        }
        c->hdr_done = true;
        size_t header_bytes = (size_t)(split + 4 - c->hdr);
        size_t hdr_before_this = c->hdr_len - n;
        offset = header_bytes > hdr_before_this ? header_bytes - hdr_before_this : 0;
        c->content_len = header_content_length(c->hdr);

        char method[8] = {0};
        char path[96] = {0};
        sscanf(c->hdr, "%7s %95s", method, path);
        bool want_bitmap = path_is(path, "/api/print") && strcmp(method, "POST") == 0 &&
                           !path_is(path, "/api/print/test");
        bool want_fs_put = strcmp(method, "PUT") == 0 && strncmp(path, "/api/fs/", 8) == 0;
        bool want_post_body = c->content_len > 0 && !want_bitmap && !want_fs_put &&
                              (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                               strcmp(method, "DELETE") == 0);
        if (want_bitmap) {
            if (c->content_len == 0 || c->content_len > sizeof(print_bitmap)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"Content-Length must be 48*height\"}");
            }
            c->taking_print_body = true;
            c->body_got = 0;
        } else if (want_fs_put) {
            if (!fs_api_name(path, c->fs_name, sizeof(c->fs_name)) ||
                !fs_valid_path(c->fs_name)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"bad name\"}");
            }
            if (c->content_len > FS_FILE_MAX) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 413, "application/json",
                                  "{\"ok\":false,\"error\":\"file too large\"}");
            }
            if (fs_begin_write(c->fs_name) < 0) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 409, "application/json",
                                  "{\"ok\":false,\"error\":\"fs write busy or failed\"}");
            }
            c->taking_fs_body = true;
            c->body_got = 0;
        } else if (want_post_body) {
            if (c->content_len >= sizeof(c->post_body)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 413, "application/json",
                                  "{\"ok\":false,\"error\":\"body too large\"}");
            }
            snprintf(c->method, sizeof(c->method), "%s", method);
            snprintf(c->path, sizeof(c->path), "%s", path);
            c->taking_post_body = true;
            c->body_got = 0;
        } else {
            const char *body = split + 4;
            err_t derr = dispatch(tpcb, c, method, path, body, strlen(body));
            pbuf_free(p);
            if (derr == ERR_ABRT) {
                return ERR_ABRT;
            }
            if (!conn_held(c)) {
                conn_drop(tpcb);
            }
            return derr;
        }
    }

    if (c->taking_print_body) {
        while (offset < p->tot_len && c->body_got < c->content_len) {
            size_t room = c->content_len - c->body_got;
            uint16_t chunk = pbuf_copy_partial(p, print_bitmap + c->body_got, (uint16_t)room,
                                               (uint16_t)offset);
            if (!chunk) {
                break;
            }
            c->body_got += chunk;
            offset += chunk;
        }
        pbuf_free(p);
        if (c->body_got >= c->content_len) {
            struct tcp_pcb *pcb = tpcb;
            size_t len = c->content_len;
            conn_drop(tpcb);
            return dispatch(pcb, NULL, "POST", "/api/print", "", len);
        }
        return ERR_OK;
    }

    if (c->taking_fs_body) {
        while (offset < p->tot_len && c->body_got < c->content_len) {
            size_t room = c->content_len - c->body_got;
            if (room > sizeof(file_chunk)) {
                room = sizeof(file_chunk);
            }
            uint16_t chunk = pbuf_copy_partial(p, file_chunk, (uint16_t)room, (uint16_t)offset);
            if (!chunk) {
                break;
            }
            if (fs_write(file_chunk, chunk) < 0) {
                pbuf_free(p);
                fs_abort_write();
                if (c->fs_name[0]) {
                    fs_delete(c->fs_name);
                }
                c->taking_fs_body = false;
                conn_drop(tpcb);
                return http_reply(tpcb, 500, "application/json",
                                  "{\"ok\":false,\"error\":\"write failed\"}");
            }
            c->body_got += chunk;
            offset += chunk;
        }
        pbuf_free(p);
        if (c->body_got >= c->content_len) {
            char name[FS_NAME_MAX + 1];
            memcpy(name, c->fs_name, sizeof(name));
            size_t len = c->content_len;
            c->taking_fs_body = false;
            int err = fs_end_write();
            conn_drop(tpcb);
            if (err < 0) {
                fs_delete(name);
                return http_reply(tpcb, 500, "application/json",
                                  "{\"ok\":false,\"error\":\"close failed\"}");
            }
            snprintf(json_buf, sizeof(json_buf),
                     "{\"ok\":true,\"name\":\"%s\",\"bytes\":%u}", name, (unsigned)len);
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        return ERR_OK;
    }

    if (c->taking_post_body) {
        while (offset < p->tot_len && c->body_got < c->content_len) {
            size_t room = c->content_len - c->body_got;
            uint16_t chunk = pbuf_copy_partial(p, c->post_body + c->body_got, (uint16_t)room,
                                               (uint16_t)offset);
            if (!chunk) {
                break;
            }
            c->body_got += chunk;
            offset += chunk;
        }
        pbuf_free(p);
        if (c->body_got >= c->content_len) {
            c->post_body[c->content_len] = 0;
            c->taking_post_body = false;
            err_t derr = dispatch(tpcb, c, c->method, c->path, c->post_body, c->content_len);
            if (derr == ERR_ABRT) {
                return ERR_ABRT;
            }
            if (!conn_held(c)) {
                conn_drop(tpcb);
            }
        }
        return ERR_OK;
    }

    pbuf_free(p);
    return ERR_OK;
}

static void http_err(void *arg, err_t err) {
    (void)err;
    struct tcp_pcb *tpcb = (struct tcp_pcb *)arg;
    conn_drop(tpcb);
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    (void)err;
    tcp_arg(newpcb, newpcb);
    tcp_nagle_disable(newpcb);
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    tcp_poll(newpcb, http_poll, HTTP_POLL_INT);
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

void http_server_start(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        conn_reset(&conns[i]);
    }
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    tcp_bind(pcb, IP_ANY_TYPE, 80);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
}

bool http_print_test(void) {
    return start_test_print();
}
