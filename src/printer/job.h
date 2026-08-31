#ifndef PM220_JOB_H
#define PM220_JOB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int job_media_json(char *buf, size_t cap);

/* Packed 1-bit MSB-first, 0=burn. Default 48 bytes x height. */
bool job_from_bitmap(const uint8_t *bitmap, int width_bytes, int height_dots,
                     uint8_t *out, size_t out_cap, size_t *out_len);

/* Inset frame that matched the last on-label CLI print. */
bool job_test_frame(uint8_t *out, size_t out_cap, size_t *out_len);

#endif
