/* nDrive - calculator file system backend.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "calcfs.h"

#include <fcntl.h>
#include <libndls.h>
#include <string.h>

#include "resident.h"

#define S_DIR 0040000u

typedef struct {
    unsigned short st_dev;
    unsigned int st_ino, st_mode;
    unsigned short st_nlink, st_uid, st_gid, st_rdev;
    unsigned int st_size, st_atime, st_mtime, st_ctime;
} NucStat;

static struct {
    int fd;
    char path[FS_PATH_MAX];
    void *(*opendir)(const char *);
    struct { char d_name[1]; } *(*readdir)(void *);
    int (*closedir)(void *);
    int (*stat)(const char *, NucStat *);
    int (*mkdir)(const char *, int);
    int (*rmdir)(const char *);
    void *(*malloc)(size_t);
    void (*free)(void *);
} fs = { .fd = -1 };

/* The folder shown as "/" to the computer: "" (everything) or e.g. "/documents". */
static char base[32];

static const char *real(const char *path, char *out) {
    if (!base[0]) return path;
    size_t b = strlen(base), n = strlen(path);
    if (n == 1 && path[0] == '/') n = 0;
    if (b + n + 1 > FS_PATH_MAX + sizeof(base)) n = 0;
    memcpy(out, base, b);
    memcpy(out + b, path, n);
    out[b + n] = 0;
    return out;
}
#define REAL(p) real((p), (char[FS_PATH_MAX + sizeof(base)]){0})

void calcfs_set_root(const char *dir) {
    size_t n = strlen(dir);
    if (n >= sizeof(base)) return;
    memcpy(base, dir, n + 1);
}

bool calcfs_save(const char *path, const char *text) {
    int fd = resident_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t n = strlen(text);
    bool ok = resident_write(fd, text, n) == (int)n;
    return resident_close(fd) >= 0 && ok;
}

/* ------------------------------------------------------- NspireLogs.zip
 * /documents/NspireLogs.zip is an empty placeholder: the OS builds the archive
 * of its log files only when the TI link asks for that name. For a computer
 * that reads files directly, the OS's own routine is called when the folder is
 * listed, and the placeholder then reads as the archive it wrote. */
#define LOGS_ZIP "/documents/NspireLogs.zip"

static struct {
    bool on;
    void (*build)(char *path);
    char path[64];              /* the archive the OS wrote, "" if none */
    uint32_t size, built_at;
} logs;

void calcfs_virtual_logs(bool on, void (*build)(char *path)) {
    logs.on = on && build;
    logs.build = build;
}

/* Build the archive unless that was done in the last half minute (a listing
   is soon followed by the read, which must find the size it was told). */
static uint32_t logs_size(void) {
    uint32_t t = resident_seconds();
    if (logs.path[0] && t - logs.built_at < 30u) return logs.size;
    static char buf[64];
    strcpy(buf, LOGS_ZIP);
    logs.build(buf);
    buf[49] = 0;
    NucStat st;
    memset(&st, 0, sizeof(st));
    logs.path[0] = 0;
    logs.size = 0;
    if (!strcmp(buf, LOGS_ZIP) || fs.stat(buf, &st) < 0 || !st.st_size) return 0;
    strcpy(logs.path, buf);
    logs.size = st.st_size;
    logs.built_at = t;
    return logs.size;
}

/* ---------------------------------------------------------- scratch file */

static const char *const scratch_paths[2] = { "/tmp/ndrive.swp", "/documents/ndless/ndrive.swp.tns" };
static const char *scratch_path;

static int scratch_open(void *ctx) {
    (void)ctx;
    for (unsigned i = 0; i < 2; ++i) {
        int fd = resident_open(scratch_paths[i], O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { scratch_path = scratch_paths[i]; return fd; }
    }
    return -1;
}

static int scratch_io(void *ctx, int h, uint32_t off, void *buf, uint32_t len, bool write) {
    (void)ctx;
    if (resident_seek(h, (int)off, SEEK_SET) < 0) return -1;
    for (uint32_t done = 0; done < len;) {
        int n = write ? resident_write(h, (const char *)buf + done, len - done)
                      : resident_read(h, (char *)buf + done, len - done);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static void scratch_close(void *ctx, int h) {
    (void)ctx;
    resident_close(h);
    if (scratch_path) resident_remove(scratch_path);
    scratch_path = NULL;
}

static bool is_scratch(const char *name) {
    return !strcmp(name, "ndrive.swp") || !strcmp(name, "ndrive.swp.tns");
}

static int list(void *ctx, const char *dir, FsListCb cb, void *user) {
    (void)ctx;
    if (!dir[0]) dir = "/";
    dir = REAL(dir);
    void *d = fs.opendir(dir);
    if (!d) return -1;
    char path[FS_PATH_MAX];
    size_t dl = strlen(dir);
    if (dl == 1 && dir[0] == '/') dl = 0;           /* children of the root are "/name" */
    int result = 0;
    for (;;) {
        __typeof__(fs.readdir(d)) e = fs.readdir(d);
        if (!e) break;
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || is_scratch(name)) continue;
        size_t nl = strlen(name);
        if (dl + nl + 2 > sizeof(path)) continue;
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1, name, nl + 1);
        NucStat st;
        memset(&st, 0, sizeof(st));
        if (fs.stat(path, &st) < 0) continue;
        if (logs.on && !st.st_size && !strcmp(path, LOGS_ZIP)) st.st_size = logs_size();
        if (cb(user, name, (st.st_mode & S_DIR) != 0, st.st_size, st.st_mtime) < 0) { result = -1; break; }
    }
    fs.closedir(d);
    return result;
}

static void read_done(void *ctx) {
    (void)ctx;
    if (fs.fd >= 0) resident_close(fs.fd);
    fs.fd = -1;
    fs.path[0] = 0;
}

static int read_at(void *ctx, const char *path, uint32_t off, void *buf, uint32_t len) {
    if (fs.fd < 0 || strcmp(fs.path, path)) {
        read_done(ctx);
        const char *file = REAL(path);
        if (logs.on && logs.path[0] && !strcmp(file, LOGS_ZIP)) file = logs.path;
        fs.fd = resident_open(file, O_RDONLY, 0);
        if (fs.fd < 0) return -1;
        size_t n = strlen(path);
        if (n >= sizeof(fs.path)) n = sizeof(fs.path) - 1;
        memcpy(fs.path, path, n);
        fs.path[n] = 0;
    }
    if (resident_seek(fs.fd, (int)off, SEEK_SET) < 0) { read_done(ctx); return -1; }
    uint32_t got = 0;
    while (got < len) {
        int n = resident_read(fs.fd, (char *)buf + got, len - got);
        if (n < 0) { read_done(ctx); return -1; }
        if (n == 0) break;
        got += (uint32_t)n;
    }
    return (int)got;
}

static int create(void *ctx, const char *path) {
    (void)ctx;
    return resident_open(REAL(path), O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

static int write_all(void *ctx, int h, const void *buf, uint32_t len) {
    (void)ctx;
    uint32_t done = 0;
    while (done < len) {
        int n = resident_write(h, (const char *)buf + done, len - done);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return (int)done;
}

static int close_h(void *ctx, int h) { (void)ctx; return resident_close(h); }
static int mk(void *ctx, const char *p) { (void)ctx; return fs.mkdir(REAL(p), 0777) < 0 ? -1 : 0; }
static int rm(void *ctx, const char *p) { (void)ctx; return fs.rmdir(REAL(p)) < 0 ? -1 : 0; }
static int ul(void *ctx, const char *p) { (void)ctx; return resident_remove(REAL(p)); }
static int rn(void *ctx, const char *a, const char *b) { (void)ctx; return resident_rename(REAL(a), REAL(b)); }
static uint32_t now(void *ctx) { (void)ctx; return resident_seconds(); }
static int space(void *ctx, uint64_t *total, uint64_t *available) {
    (void)ctx;
    return resident_free_space(total, available) ? 0 : -1;
}

static void *alloc(size_t n) { return fs.malloc(n); }
static void release(void *p) { fs.free(p); }

bool calcfs_init(FsBackend *b, FsAllocator *a) {
    fs.opendir = resident_native(e_opendir);
    fs.readdir = resident_native(e_readdir);
    fs.closedir = resident_native(e_closedir);
    fs.stat = resident_native(e_stat);
    fs.mkdir = resident_native(e_mkdir);
    fs.rmdir = resident_native(e_rmdir);
    fs.malloc = resident_native(e_malloc);
    fs.free = resident_native(e_free);
    if (!fs.opendir || !fs.readdir || !fs.closedir || !fs.stat || !fs.mkdir ||
        !fs.rmdir || !fs.malloc || !fs.free) return false;
    fs.fd = -1;
    for (unsigned i = 0; i < 2; ++i) resident_remove(scratch_paths[i]);     /* left by a reset */
    FsBackend be = { NULL, list, read_at, read_done, create, write_all, close_h, mk, rm, ul, rn, now, space,
                     scratch_open, scratch_io, scratch_close };
    *b = be;
    a->alloc = alloc;
    a->free = release;
    return true;
}
