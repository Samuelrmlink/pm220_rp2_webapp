#ifndef PM220_FS_H
#define PM220_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FS_NAME_MAX 64
#define FS_FILE_MAX (64u * 1024u)
#define FS_READS 2

bool fs_init(void);
bool fs_valid_name(const char *name);

int fs_list_json(char *buf, size_t cap);
int fs_delete(const char *name);
bool fs_stat(const char *name, size_t *size);

int fs_begin_write(const char *name);
int fs_write(const uint8_t *data, size_t len);
int fs_end_write(void);
void fs_abort_write(void);

int fs_begin_read(const char *name, size_t *size);
int fs_read(int h, uint8_t *buf, size_t cap);
void fs_end_read(int h);

#endif
