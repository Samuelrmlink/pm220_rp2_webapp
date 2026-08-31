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

#define HTTP_HDR 1024
#define HTTP_CONNS 2

static char json_buf[2048];
static uint8_t print_bitmap[TSPL_BITMAP_MAX];
static uint8_t print_job[TSPL_JOB_MAX];

typedef struct {
    struct tcp_pcb *pcb;
    char hdr[HTTP_HDR];
    uint16_t hdr_len;
    bool hdr_done;
    size_t content_len;
    size_t body_got;
    bool taking_print_body;
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
        default:
            return "OK";
    }
}

static err_t http_reply(struct tcp_pcb *tpcb, int code, const char *ctype, const char *body) {
    if (!body) {
        body = "";
    }
    char hdr[280];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n\r\n",
                      code, status_text(code), ctype, (unsigned)strlen(body));
    tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY);
    if (body[0]) {
        tcp_write(tpcb, body, (uint16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    tcp_close(tpcb);
    return ERR_OK;
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

static bool start_bitmap_print(size_t body_len) {
    if (body_len == 0 || body_len % TSPL_WIDTH_BYTES != 0) {
        return false;
    }
    int height = (int)(body_len / TSPL_WIDTH_BYTES);
    size_t n = 0;
    if (!job_from_bitmap(print_bitmap, TSPL_WIDTH_BYTES, height, print_job, sizeof(print_job), &n)) {
        return false;
    }
    return bt_print_job(print_job, n);
}

static int dispatch(struct tcp_pcb *tpcb, const char *method, const char *path, const char *body,
                    size_t body_len) {
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
        if (!start_bitmap_print(body_len)) {
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":false,\"error\":\"%s\"}",
                     bt_last_error()[0] ? bt_last_error() : "bad bitmap");
            return http_reply(tpcb, 400, "application/json", json_buf);
        }
        snprintf(json_buf, sizeof(json_buf), "{\"ok\":true,\"bytes\":%u}", (unsigned)body_len);
        return http_reply(tpcb, 202, "application/json", json_buf);
    }
    if ((path_is(path, "/") || path_is(path, "/index.html")) && strcmp(method, "GET") == 0) {
        return http_reply(tpcb, 200, "text/plain",
                          "pm220-pico2w\nhttp://pm220.local/\n"
                          "GET /api/status /api/scan /api/printer /api/media /api/print/test\n"
                          "POST /api/scan /api/printer/connect /api/printer/disconnect\n"
                          "POST /api/print  (octet-stream packed 1-bit, 48 bytes/row)\n"
                          "POST /api/print/test\n");
    }
    return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}\n");
}

static void conn_reset(http_conn_t *c) {
    memset(c, 0, sizeof(*c));
}

static http_conn_t *conn_for(struct tcp_pcb *tpcb) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb == tpcb) {
            return &conns[i];
        }
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

static void conn_drop(struct tcp_pcb *tpcb) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb == tpcb) {
            conn_reset(&conns[i]);
        }
    }
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
        if (want_bitmap) {
            if (c->content_len == 0 || c->content_len > sizeof(print_bitmap)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"Content-Length must be 48*height\"}");
            }
            c->taking_print_body = true;
            c->body_got = 0;
        } else {
            const char *body = split + 4;
            dispatch(tpcb, method, path, body, strlen(body));
            pbuf_free(p);
            conn_drop(tpcb);
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
            return dispatch(pcb, "POST", "/api/print", "", len);
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
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

void http_server_start(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    tcp_bind(pcb, IP_ANY_TYPE, 80);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
}

bool http_print_test(void) {
    return start_test_print();
}
