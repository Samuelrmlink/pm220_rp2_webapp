#include "fs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "littlefs/lfs.h"

/* 1 MiB window at 3 MiB. Last 16 KiB of a 4 MB part is BTstack NVM
 * (RP2350-E10 spare sector + two flash banks), so stay below that. */
#define FS_FLASH_OFF  (3u * 1024u * 1024u)
#define FS_BT_TAIL    (16u * 1024u)
#define FS_SIZE       ((1u * 1024u * 1024u) - FS_BT_TAIL)
#define FS_BLOCK      FLASH_SECTOR_SIZE

static_assert(FS_FLASH_OFF % FLASH_SECTOR_SIZE == 0, "fs offset");
static_assert(FS_SIZE % FLASH_SECTOR_SIZE == 0, "fs size");
static_assert(FS_FLASH_OFF + FS_SIZE + FS_BT_TAIL <= 4u * 1024u * 1024u, "fs vs 4MB");

static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t read_buf[FLASH_PAGE_SIZE];
static uint8_t prog_buf[FLASH_PAGE_SIZE];
static uint8_t lookahead_buf[32];
static bool mounted;

static lfs_file_t wfile;
static bool wopen;
static lfs_file_t rfiles[FS_READS];
static bool ropen[FS_READS];

typedef struct {
    uint32_t off;
    const uint8_t *data;
    size_t len;
    int prog;
} flash_job_t;

static void __not_in_flash_func(flash_job_run)(void *p) {
    flash_job_t *j = p;
    if (j->prog) {
        flash_range_program(j->off, j->data, j->len);
    } else {
        flash_range_erase(j->off, j->len);
    }
}

static void flash_do(flash_job_t *j) {
    int rc = flash_safe_execute(flash_job_run, j, 2000);
    if (rc == PICO_OK) {
        return;
    }
    uint32_t ints = save_and_disable_interrupts();
    flash_job_run(j);
    restore_interrupts(ints);
}

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size) {
    (void)c;
    memcpy(buffer, (const uint8_t *)(XIP_BASE + FS_FLASH_OFF + block * FS_BLOCK + off), size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size) {
    (void)c;
    flash_job_t j = {
        .off = FS_FLASH_OFF + block * FS_BLOCK + off,
        .data = buffer,
        .len = size,
        .prog = 1,
    };
    flash_do(&j);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    flash_job_t j = {
        .off = FS_FLASH_OFF + block * FS_BLOCK,
        .data = NULL,
        .len = FS_BLOCK,
        .prog = 0,
    };
    flash_do(&j);
    return 0;
}

static int bd_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

bool fs_valid_name(const char *name) {
    if (!name || !name[0] || strlen(name) > FS_NAME_MAX) {
        return false;
    }
    if (name[0] == '.') {
        return false;
    }
    for (const char *p = name; *p; p++) {
        char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

bool fs_valid_path(const char *path) {
    if (!path || !path[0] || strlen(path) > FS_NAME_MAX) {
        return false;
    }
    const char *slash = strchr(path, '/');
    if (!slash) {
        return fs_valid_name(path);
    }
    if (slash == path || strchr(slash + 1, '/')) {
        return false;
    }
    size_t dlen = (size_t)(slash - path);
    if (!((dlen == strlen(FS_LABELS_DIR) && strncmp(path, FS_LABELS_DIR, dlen) == 0) ||
          (dlen == strlen(FS_SETTINGS_DIR) && strncmp(path, FS_SETTINGS_DIR, dlen) == 0))) {
        return false;
    }
    return fs_valid_name(slash + 1);
}

static bool is_managed_dir(const char *path) {
    return path && (strcmp(path, FS_LABELS_DIR) == 0 || strcmp(path, FS_SETTINGS_DIR) == 0);
}

bool fs_is_dir(const char *path) {
    if (!mounted) {
        return false;
    }
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        return true;
    }
    if (!is_managed_dir(path)) {
        return false;
    }
    struct lfs_info info;
    return lfs_stat(&lfs, path, &info) >= 0 && info.type == LFS_TYPE_DIR;
}

static void cfg_init(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = FLASH_PAGE_SIZE;
    cfg.prog_size = FLASH_PAGE_SIZE;
    cfg.block_size = FS_BLOCK;
    cfg.block_count = FS_SIZE / FS_BLOCK;
    cfg.cache_size = FLASH_PAGE_SIZE;
    cfg.lookahead_size = sizeof(lookahead_buf);
    cfg.block_cycles = 500;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = lookahead_buf;
}

bool fs_init(void) {
    cfg_init();
    int err = lfs_mount(&lfs, &cfg);
    if (err) {
        printf("fs: mount %d, formatting\n", err);
        err = lfs_format(&lfs, &cfg);
        if (err) {
            printf("fs: format failed %d\n", err);
            return false;
        }
        err = lfs_mount(&lfs, &cfg);
        if (err) {
            printf("fs: remount failed %d\n", err);
            return false;
        }
    }
    mounted = true;
    err = lfs_mkdir(&lfs, FS_LABELS_DIR);
    if (err && err != LFS_ERR_EXIST) {
        printf("fs: mkdir %s %d\n", FS_LABELS_DIR, err);
    }
    err = lfs_mkdir(&lfs, FS_SETTINGS_DIR);
    if (err && err != LFS_ERR_EXIST) {
        printf("fs: mkdir %s %d\n", FS_SETTINGS_DIR, err);
    }
    printf("fs: mounted 1MB at flash 0x%lx\n", (unsigned long)FS_FLASH_OFF);
    return true;
}

int fs_list_json(const char *dir, char *buf, size_t cap) {
    if (!mounted || !buf || cap < 16) {
        return -1;
    }
    const char *open_path = "/";
    const char *shown = "";
    if (dir && dir[0] && strcmp(dir, "/") != 0) {
        if (!fs_is_dir(dir)) {
            return -1;
        }
        open_path = dir;
        shown = dir;
    }
    size_t n = 0;
    if (shown[0]) {
        n += (size_t)snprintf(buf + n, cap - n, "{\"dir\":\"%s\",\"files\":[", shown);
    } else {
        n += (size_t)snprintf(buf + n, cap - n, "{\"files\":[");
    }
    lfs_dir_t ldir;
    if (lfs_dir_open(&lfs, &ldir, open_path) < 0) {
        if (shown[0]) {
            snprintf(buf, cap, "{\"dir\":\"%s\",\"files\":[]}", shown);
        } else {
            snprintf(buf, cap, "{\"files\":[]}");
        }
        return 0;
    }
    struct lfs_info info;
    int first = 1;
    while (lfs_dir_read(&lfs, &ldir, &info) > 0) {
        if (info.type != LFS_TYPE_REG) {
            continue;
        }
        if (!first) {
            n += (size_t)snprintf(buf + n, cap - n, ",");
        }
        first = 0;
        n += (size_t)snprintf(buf + n, cap - n, "{\"name\":\"%s\",\"size\":%u}",
                              info.name, (unsigned)info.size);
        if (n + 32 >= cap) {
            break;
        }
    }
    lfs_dir_close(&lfs, &ldir);
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return 0;
}

bool fs_stat(const char *name, size_t *size) {
    if (!mounted || !fs_valid_path(name)) {
        return false;
    }
    struct lfs_info info;
    if (lfs_stat(&lfs, name, &info) < 0 || info.type != LFS_TYPE_REG) {
        return false;
    }
    if (size) {
        *size = (size_t)info.size;
    }
    return true;
}

int fs_delete(const char *name) {
    if (!mounted || !fs_valid_path(name) || fs_is_dir(name)) {
        return -1;
    }
    int err = lfs_remove(&lfs, name);
    return err < 0 ? err : 0;
}

int fs_rename(const char *from, const char *to) {
    if (!mounted || !fs_valid_path(from) || !fs_valid_path(to)) {
        return -1;
    }
    if (strcmp(from, to) == 0) {
        return 0;
    }
    if (fs_stat(to, NULL)) {
        return 1;
    }
    int err = lfs_rename(&lfs, from, to);
    if (err == LFS_ERR_EXIST) {
        return 1;
    }
    return err < 0 ? err : 0;
}

int fs_begin_write(const char *name) {
    if (!mounted || !fs_valid_path(name) || fs_is_dir(name) || wopen) {
        return -1;
    }
    int err = lfs_file_open(&lfs, &wfile, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        printf("fs: open write %s %d\n", name, err);
        return err;
    }
    wopen = true;
    return 0;
}

int fs_write(const uint8_t *data, size_t len) {
    if (!wopen) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        lfs_ssize_t n = lfs_file_write(&lfs, &wfile, data + off, len - off);
        if (n <= 0) {
            return n < 0 ? (int)n : -1;
        }
        off += (size_t)n;
    }
    return (int)off;
}

int fs_end_write(void) {
    if (!wopen) {
        return -1;
    }
    int err = lfs_file_close(&lfs, &wfile);
    wopen = false;
    return err;
}

void fs_abort_write(void) {
    if (wopen) {
        lfs_file_close(&lfs, &wfile);
        wopen = false;
    }
}

int fs_begin_read(const char *name, size_t *size) {
    if (!mounted || !fs_valid_path(name)) {
        return -1;
    }
    int h = -1;
    for (int i = 0; i < FS_READS; i++) {
        if (!ropen[i]) {
            h = i;
            break;
        }
    }
    if (h < 0) {
        return -1;
    }
    int err = lfs_file_open(&lfs, &rfiles[h], name, LFS_O_RDONLY);
    if (err < 0) {
        return err;
    }
    ropen[h] = true;
    if (size) {
        *size = (size_t)lfs_file_size(&lfs, &rfiles[h]);
    }
    return h;
}

int fs_read(int h, uint8_t *buf, size_t cap) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return -1;
    }
    return (int)lfs_file_read(&lfs, &rfiles[h], buf, cap);
}

void fs_end_read(int h) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return;
    }
    lfs_file_close(&lfs, &rfiles[h]);
    ropen[h] = false;
}
