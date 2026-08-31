#ifndef PM220_FS_H
#define PM220_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FS_NAME_MAX 64
#define FS_FILE_MAX (128u * 1024u)
#define FS_READS 2
#define FS_LABELS_DIR "labels"

bool fs_init(void);
bool fs_valid_name(const char *name);
bool fs_valid_path(const char *path);
bool fs_is_dir(const char *path);

int fs_list_json(const char *dir, char *buf, size_t cap);
int fs_delete(const char *name);
int fs_rename(const char *from, const char *to);
bool fs_stat(const char *name, size_t *size);

int fs_begin_write(const char *name);
int fs_write(const uint8_t *data, size_t len);
int fs_end_write(void);
void fs_abort_write(void);

int fs_begin_read(const char *name, size_t *size);
int fs_read(int h, uint8_t *buf, size_t cap);
void fs_end_read(int h);

#endif
