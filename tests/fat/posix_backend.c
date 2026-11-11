/* POSIX VfatBackend for host tests: a directory plays the calculator. */
#define _GNU_SOURCE
#include "posix_backend.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int fd; char path[1024]; unsigned opens; } Ctx;
static Ctx ctx = { -1, "", 0 };
unsigned posix_backend_fail_writes_after = 0;   /* testing: fail the Nth write */
static unsigned writes;

static int list(void *c, const char *dir, VfatListCb cb, void *user) {
    (void)c;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || stat(p, &st)) continue;
        if (cb(user, e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size, (uint32_t)st.st_mtime) < 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int rd(void *c, const char *path, uint32_t off, void *buf, uint32_t len) {
    Ctx *x = c;
    if (x->fd < 0 || strcmp(x->path, path)) {
        if (x->fd >= 0) close(x->fd);
        x->fd = open(path, O_RDONLY);
        if (x->fd < 0) return -1;
        snprintf(x->path, sizeof(x->path), "%s", path);
        x->opens++;
    }
    ssize_t n = pread(x->fd, buf, len, off);
    return n < 0 ? -1 : (int)n;
}

static void read_done(void *c) {
    Ctx *x = c;
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
}

static int create(void *c, const char *path) {
    (void)c;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static int wr(void *c, int h, const void *buf, uint32_t len) {
    (void)c;
    if (posix_backend_fail_writes_after && ++writes >= posix_backend_fail_writes_after) return -1;
    return (int)write(h, buf, len);
}

static int cl(void *c, int h) { (void)c; return close(h); }
static int mk(void *c, const char *p) { (void)c; return mkdir(p, 0755); }
static int rm(void *c, const char *p) { (void)c; return rmdir(p); }
static int ul(void *c, const char *p) { (void)c; return unlink(p); }
static int rn(void *c, const char *a, const char *b) {
    (void)c;
    /* The calculator's rename refuses existing targets: model that. */
    struct stat st;
    if (!lstat(b, &st)) { errno = EEXIST; return -1; }
    return rename(a, b);
}
static uint32_t now(void *c) { (void)c; return 1767225600u + 12345u; }

/* Scratch file: an unlinked temporary file. */
unsigned posix_scratch_opens;
static int scratch_open(void *c) {
    (void)c;
    char name[] = "/tmp/ndrive-scratch-XXXXXX";
    int fd = mkstemp(name);
    if (fd >= 0) unlink(name);
    ++posix_scratch_opens;
    return fd;
}
static int scratch_io(void *c, int h, uint32_t off, void *buf, uint32_t len, bool write) {
    (void)c;
    ssize_t n = write ? pwrite(h, buf, len, off) : pread(h, buf, len, off);
    return n == (ssize_t)len ? 0 : -1;
}
static void scratch_close(void *c, int h) { (void)c; close(h); }

const VfatBackend posix_backend = { &ctx, list, rd, read_done, create, wr, cl, mk, rm, ul, rn, now,
                                    NULL, scratch_open, scratch_io, scratch_close };

static void *a(size_t n) { return malloc(n); }
static void f(void *p) { free(p); }
const VfatAllocator posix_alloc = { a, f };
