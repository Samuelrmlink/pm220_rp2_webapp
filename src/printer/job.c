#include "job.h"
#include "tspl.h"

#include <stdio.h>
#include <string.h>

static bool fb_get(const uint8_t *fb, int wbytes, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return false;
    }
    int bit = 7 - (x % 8);
    return (fb[y * wbytes + (x / 8)] & (1u << bit)) == 0;
}

static void fb_set(uint8_t *fb, int wbytes, int w, int h, int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    int bit = 7 - (x % 8);
    uint8_t *b = &fb[y * wbytes + (x / 8)];
    if (black) {
        *b &= (uint8_t)~(1u << bit);
    } else {
        *b |= (uint8_t)(1u << bit);
    }
}

static void fb_fill_white(uint8_t *fb, size_t n) {
    memset(fb, 0xFF, n);
}

static void draw_frame(uint8_t *fb, int wbytes, int w, int h) {
    const int thickness = 3;
    int inset_x = tspl_mm_to_dots(2.0f);
    int inset_y0 = tspl_mm_to_dots(2.0f);
    int inset_y1 = tspl_mm_to_dots(5.0f);
    if (inset_x < thickness + 1) {
        inset_x = thickness + 1;
    }
    if (inset_y0 < thickness + 1) {
        inset_y0 = thickness + 1;
    }
    if (inset_y1 < thickness + 1) {
        inset_y1 = thickness + 1;
    }
    int x0 = inset_x;
    int y0 = inset_y0;
    int x1 = w - 1 - inset_x;
    int y1 = h - 1 - inset_y1;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (int t = 0; t < thickness; t++) {
        for (int x = x0; x <= x1; x++) {
            fb_set(fb, wbytes, w, h, x, y0 + t, true);
            fb_set(fb, wbytes, w, h, x, y1 - t, true);
        }
        for (int y = y0; y <= y1; y++) {
            fb_set(fb, wbytes, w, h, x0 + t, y, true);
            fb_set(fb, wbytes, w, h, x1 - t, y, true);
        }
    }
    int cx = w / 2;
    int cy = h / 2;
    int arm = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 4;
    for (int i = -arm; i <= arm; i++) {
        fb_set(fb, wbytes, w, h, cx + i, cy, true);
        fb_set(fb, wbytes, w, h, cx + i, cy + 1, true);
        fb_set(fb, wbytes, w, h, cx, cy + i, true);
        fb_set(fb, wbytes, w, h, cx + 1, cy + i, true);
    }
}

void job_compute_layout(int src_w, int src_h, job_layout_t *layout) {
    int page_w = tspl_mm_to_dots(TSPL_DEFAULT_WIDTH_MM);
    int page_h = tspl_mm_to_dots(TSPL_DEFAULT_HEIGHT_MM);
    int origin_x = (page_w - src_w) / 2;
    int origin_y = (page_h - src_h) / 2;
    if (origin_x < 0) {
        origin_x = 0;
    }
    if (origin_y < 0) {
        origin_y = 0;
    }
    int dx = tspl_mm_to_dots(TSPL_OFFSET_X_MM);
    int dy = tspl_mm_to_dots(TSPL_OFFSET_Y_MM);
    int shift_x = 0;
    int shift_y = 0;
    if (origin_x + dx < 0) {
        shift_x = origin_x + dx;
        origin_x = 0;
    } else {
        origin_x += dx;
    }
    if (origin_y + dy < 0) {
        shift_y = origin_y + dy;
        origin_y = 0;
    } else {
        origin_y += dy;
    }
    int avail_w = page_w - origin_x;
    int avail_h = page_h - origin_y;
    if (avail_w < 1) {
        avail_w = 1;
    }
    if (avail_h < 1) {
        avail_h = 1;
    }
    layout->origin_x = origin_x;
    layout->origin_y = origin_y;
    layout->print_width_dots = src_w < avail_w ? src_w : avail_w;
    layout->print_height_dots = src_h < avail_h ? src_h : avail_h;
    layout->shift_x = shift_x;
    layout->shift_y = shift_y;
}

static bool layout_and_build(const uint8_t *src, int src_wbytes, int src_w, int src_h,
                             uint8_t *out, size_t out_cap, size_t *out_len,
                             job_layout_t *layout_out) {
    job_layout_t L;
    job_compute_layout(src_w, src_h, &L);
    int out_w = L.print_width_dots;
    int out_h = L.print_height_dots;
    int out_wbytes = (out_w + 7) / 8;
    static uint8_t cropped[TSPL_BITMAP_MAX];
    if ((size_t)out_wbytes * (size_t)out_h > sizeof(cropped)) {
        return false;
    }
    fb_fill_white(cropped, (size_t)out_wbytes * (size_t)out_h);
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            if (fb_get(src, src_wbytes, src_w, src_h, x - L.shift_x, y - L.shift_y)) {
                fb_set(cropped, out_wbytes, out_w, out_h, x, y, true);
            }
        }
    }
    bool all_black = true;
    size_t nbytes = (size_t)out_wbytes * (size_t)out_h;
    for (size_t i = 0; i < nbytes; i++) {
        if (cropped[i] != 0x00) {
            all_black = false;
            break;
        }
    }
    if (all_black) {
        for (int y = 0; y < out_h; y += 16) {
            fb_set(cropped, out_wbytes, out_w, out_h, 0, y, false);
        }
    }
    size_t n = tspl_build_job(out, out_cap, cropped, out_wbytes, out_h,
                              TSPL_DEFAULT_WIDTH_MM, TSPL_DEFAULT_HEIGHT_MM,
                              TSPL_DEFAULT_GAP_MM, TSPL_DEFAULT_DENSITY,
                              L.origin_x, L.origin_y);
    if (!n) {
        return false;
    }
    *out_len = n;
    if (layout_out) {
        *layout_out = L;
    }
    printf("job: %u bytes BITMAP %dx%d at %d,%d (shift %d,%d)\n", (unsigned)n, out_w, out_h,
           L.origin_x, L.origin_y, L.shift_x, L.shift_y);
    return true;
}

int job_media_json(char *buf, size_t cap) {
    job_layout_t L;
    job_compute_layout(TSPL_WIDTH_DOTS, TSPL_HEIGHT_DOTS, &L);
    int inset = tspl_mm_to_dots(TSPL_SAFE_MARGIN_MM);
    int x0 = inset;
    int y0 = inset;
    int x1 = TSPL_WIDTH_DOTS - 1 - inset;
    int y1 = TSPL_HEIGHT_DOTS - 1 - inset;
    return snprintf(buf, cap,
                    "{\"width_mm\":%g,\"height_mm\":%g,\"gap_mm\":%g,\"dpi\":%d,"
                    "\"max_print_width_mm\":%g,\"offset_x_mm\":%g,\"offset_y_mm\":%g,"
                    "\"safe_margin_mm\":%g,\"density\":%d,"
                    "\"width_dots\":%d,\"height_dots\":%d,\"width_bytes\":%d,"
                    "\"bitmap_bytes\":%d,\"bit_order\":\"msb-first\",\"polarity\":\"0=burn\","
                    "\"safe_rect\":{\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d},"
                    "\"after_offset\":{\"origin_x\":%d,\"origin_y\":%d,"
                    "\"width_dots\":%d,\"height_dots\":%d,\"shift_x\":%d,\"shift_y\":%d}}",
                    (double)TSPL_DEFAULT_WIDTH_MM, (double)TSPL_DEFAULT_HEIGHT_MM,
                    (double)TSPL_DEFAULT_GAP_MM, TSPL_DPI,
                    (double)TSPL_MAX_WIDTH_MM, (double)TSPL_OFFSET_X_MM,
                    (double)TSPL_OFFSET_Y_MM, (double)TSPL_SAFE_MARGIN_MM,
                    TSPL_DEFAULT_DENSITY,
                    TSPL_WIDTH_DOTS, TSPL_HEIGHT_DOTS, TSPL_WIDTH_BYTES,
                    TSPL_BITMAP_MAX, x0, y0, x1, y1,
                    L.origin_x, L.origin_y, L.print_width_dots, L.print_height_dots,
                    L.shift_x, L.shift_y);
}

bool job_from_bitmap(const uint8_t *bitmap, int width_bytes, int height_dots,
                     uint8_t *out, size_t out_cap, size_t *out_len,
                     job_layout_t *layout) {
    if (!bitmap || width_bytes <= 0 || height_dots <= 0) {
        return false;
    }
    if (width_bytes > TSPL_WIDTH_BYTES || height_dots > TSPL_HEIGHT_DOTS) {
        return false;
    }
    return layout_and_build(bitmap, width_bytes, width_bytes * 8, height_dots,
                            out, out_cap, out_len, layout);
}

bool job_test_frame(uint8_t *out, size_t out_cap, size_t *out_len) {
    static uint8_t fb[TSPL_BITMAP_MAX];
    fb_fill_white(fb, sizeof(fb));
    draw_frame(fb, TSPL_WIDTH_BYTES, TSPL_WIDTH_DOTS, TSPL_HEIGHT_DOTS);
    return layout_and_build(fb, TSPL_WIDTH_BYTES, TSPL_WIDTH_DOTS, TSPL_HEIGHT_DOTS,
                            out, out_cap, out_len, NULL);
}
