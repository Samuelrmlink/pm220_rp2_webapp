#ifndef PM220_TSPL_H
#define PM220_TSPL_H

#include <stddef.h>
#include <stdint.h>

#define TSPL_DPI 203
#define TSPL_MAX_WIDTH_MM 48
#define TSPL_DEFAULT_WIDTH_MM 50
#define TSPL_DEFAULT_HEIGHT_MM 30
#define TSPL_DEFAULT_GAP_MM 2.0f
#define TSPL_DEFAULT_DENSITY 8
#define TSPL_OFFSET_X_MM (-1.25f)
#define TSPL_OFFSET_Y_MM 3.0f
#define TSPL_SAFE_MARGIN_MM 4.0f
#define TSPL_WIDTH_BYTES 48
#define TSPL_WIDTH_DOTS 384
#define TSPL_HEIGHT_DOTS 240
#define TSPL_BITMAP_MAX (TSPL_WIDTH_BYTES * TSPL_HEIGHT_DOTS)
#define TSPL_JOB_MAX 16384

int tspl_mm_to_dots(float mm);
size_t tspl_build_job(uint8_t *out, size_t out_cap,
                      const uint8_t *bitmap, int width_bytes, int height_dots,
                      float width_mm, float height_mm, float gap_mm,
                      int density, int origin_x, int origin_y);

#endif
