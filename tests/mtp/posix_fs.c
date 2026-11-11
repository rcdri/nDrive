/* FsBackend for host tests: a directory on the PC plays the calculator's "/". */
#define _GNU_SOURCE
#include "posix_fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char root[1024];
static int rfd = -1;
static char rpath[2048];
unsigned posix_fs_fail_writes_after;            /* testing: fail the Nth write */
static unsigned writes;

static const char *full(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s%s", root, path);
    return out;
}

static int list(void *c, const char *dir, FsListCb cb, void *user) {
    (void)c;
    char p[2048], q[4096];
    DIR *d = opendir(full(dir, p, sizeof(p)));
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        struct stat st;
        snprintf(q, sizeof(q), "%s/%s", p, e->d_name);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || stat(q, &st)) continue;
        if (cb(user, e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size, (uint32_t)st.st_mtime) < 0) break;
    }
    closedir(d);
    return 0;
}

static void read_done(void *c) {
    (void)c;
    if (rfd >= 0) close(rfd);
    rfd = -1;
    rpath[0] = 0;
}

static int rd(void *c, const char *path, uint32_t off, void *buf, uint32_t len) {
    char p[2048];
    full(path, p, sizeof(p));
    if (rfd < 0 || strcmp(rpath, p)) {
        read_done(c);
        rfd = open(p, O_RDONLY);
        if (rfd < 0) return -1;
        snprintf(rpath, sizeof(rpath), "%s", p);
    }
    ssize_t n = pread(rfd, buf, len, off);
    return n < 0 ? -1 : (int)n;
}

static int create(void *c, const char *path) {
    (void)c;
    char p[2048];
    return open(full(path, p, sizeof(p)), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static int wr(void *c, int h, const void *buf, uint32_t len) {
    (void)c;
    if (posix_fs_fail_writes_after && ++writes >= posix_fs_fail_writes_after) return -1;
    return write(h, buf, len) == (ssize_t)len ? (int)len : -1;
}

static int cl(void *c, int h) { (void)c; return close(h); }
static int mk(void *c, const char *path) { (void)c; char p[2048]; return mkdir(full(path, p, sizeof(p)), 0755); }
static int rmd(void *c, const char *path) { (void)c; char p[2048]; return rmdir(full(path, p, sizeof(p))); }
static int ul(void *c, const char *path) { (void)c; char p[2048]; return unlink(full(path, p, sizeof(p))); }
static int rn(void *c, const char *a, const char *b) {
    (void)c;
    char p[2048], q[2048];
    return rename(full(a, p, sizeof(p)), full(b, q, sizeof(q)));
}
static uint32_t now(void *c) { (void)c; return (uint32_t)time(NULL); }

static int space(void *c, uint64_t *total, uint64_t *available) {
    (void)c;
    *total = 96u * 1024 * 1024;
    *available = 64u * 1024 * 1024;
    return 0;
}

const FsBackend posix_fs = { NULL, list, rd, read_done, create, wr, cl, mk, rmd, ul, rn, now, space };
const FsAllocator posix_mem = { malloc, free };

void posix_fs_root(const char *dir) { snprintf(root, sizeof(root), "%s", dir); }
