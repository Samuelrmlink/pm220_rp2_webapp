#ifndef PM220_JOB_H
#define PM220_JOB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int origin_x;
    int origin_y;
    int print_width_dots;
    int print_height_dots;
    int shift_x;
    int shift_y;
} job_layout_t;

int job_media_json(char *buf, size_t cap);
void job_compute_layout(int src_w, int src_h, job_layout_t *layout);

/* Packed 1-bit MSB-first, 0=burn, 48 bytes/row. layout may be NULL. */
bool job_from_bitmap(const uint8_t *bitmap, int width_bytes, int height_dots,
                     uint8_t *out, size_t out_cap, size_t *out_len,
                     job_layout_t *layout);

/* Inset frame that matched the last on-label CLI print. */
bool job_test_frame(uint8_t *out, size_t out_cap, size_t *out_len);

#endif
