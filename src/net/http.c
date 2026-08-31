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

#define HTTP_HDR 1024
#define HTTP_CONNS 2
#define FILE_CHUNK 1024

static char json_buf[2048];
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
    bool sending_file;
    char fs_name[FS_NAME_MAX + 1];
    int file_h;
    size_t file_left;
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
    return copy_path_token(path + 8, out, cap) && fs_valid_name(out);
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

static bool http_pump_file(http_conn_t *c, struct tcp_pcb *tpcb) {
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
            c->file_left = 0;
            break;
        }
        if (tcp_write(tpcb, file_chunk, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        c->file_left -= (size_t)n;
    }
    tcp_output(tpcb);
    if (c->file_left != 0) {
        return false;
    }
    fs_end_read(c->file_h);
    c->sending_file = false;
    tcp_close(tpcb);
    return true;
}

static err_t http_start_file(http_conn_t *c, struct tcp_pcb *tpcb, const char *name, bool gzipped) {
    size_t sz = 0;
    int h = fs_begin_read(name, &sz);
    if (h < 0) {
        return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"busy\"}\n");
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
    tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY);
    c->sending_file = true;
    c->file_h = h;
    c->file_left = sz;
    http_pump_file(c, tpcb);
    return ERR_OK;
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
    if ((path_is(path, "/api/fs") || path_is(path, "/api/fs/")) && strcmp(method, "GET") == 0) {
        if (fs_list_json(json_buf, sizeof(json_buf)) < 0) {
            return http_reply(tpcb, 500, "application/json",
                              "{\"ok\":false,\"error\":\"fs unavailable\"}");
        }
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (strcmp(method, "DELETE") == 0) {
        char name[FS_NAME_MAX + 1];
        if (!fs_api_name(path, name, sizeof(name))) {
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
                              "GET /api/status /api/scan /api/printer /api/media /api/print /api/fs\n"
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
    if (c && c->sending_file && http_pump_file(c, tpcb)) {
        conn_drop(tpcb);
    }
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
            if (!fs_api_name(path, c->fs_name, sizeof(c->fs_name))) {
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
        } else {
            const char *body = split + 4;
            dispatch(tpcb, c, method, path, body, strlen(body));
            pbuf_free(p);
            if (!c->sending_file) {
                conn_drop(tpcb);
            }
            return ERR_OK;
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
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
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
