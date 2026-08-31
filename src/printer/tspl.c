#include "tspl.h"

#include <stdio.h>
#include <string.h>

int tspl_mm_to_dots(float mm) {
    return (int)(mm * TSPL_DPI / 25.4f + (mm >= 0 ? 0.5f : -0.5f));
}

size_t tspl_build_job(uint8_t *out, size_t out_cap,
                      const uint8_t *bitmap, int width_bytes, int height_dots,
                      float width_mm, float height_mm, float gap_mm,
                      int density, int origin_x, int origin_y) {
    if (!out || !bitmap || width_bytes <= 0 || height_dots <= 0) {
        return 0;
    }
    int n = snprintf((char *)out, out_cap,
                     "\x1b!o\r\n"
                     "SIZE %g mm,%g mm\r\n"
                     "GAP %g mm,0 mm\r\n"
                     "DIRECTION 0,0\r\n"
                     "DENSITY %d\r\n"
                     "CLS\r\n"
                     "BITMAP %d,%d,%d,%d,1,",
                     width_mm, height_mm, gap_mm, density,
                     origin_x, origin_y, width_bytes, height_dots);
    if (n < 0 || (size_t)n >= out_cap) {
        return 0;
    }
    size_t bitmap_len = (size_t)width_bytes * (size_t)height_dots;
    if ((size_t)n + bitmap_len + 16 > out_cap) {
        return 0;
    }
    memcpy(out + n, bitmap, bitmap_len);
    int tail = snprintf((char *)out + n + bitmap_len, out_cap - n - bitmap_len, "\r\nPRINT 1\r\n");
    if (tail < 0) {
        return 0;
    }
    return (size_t)n + bitmap_len + (size_t)tail;
}
