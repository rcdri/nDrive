/* nDrive - file system interface between the MTP responder and the calculator
 * (or a test directory on a PC). Copyright (C) 2026 Rodrigo. GPL v3+. */
#ifndef NDRIVE_FS_H
#define NDRIVE_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_PATH_MAX 512u

typedef int (*FsListCb)(void *user, const char *name, bool is_dir,
                        uint32_t size, uint32_t mtime);

/* Paths are absolute ("/documents/a/b.tns"); the root is "/".
   All functions return >= 0 on success, < 0 on failure. */
typedef struct {
    void *ctx;
    int (*list)(void *ctx, const char *dir, FsListCb cb, void *user);
    /* Read len bytes at off; returns bytes read (short at EOF). May cache. */
    int (*read)(void *ctx, const char *path, uint32_t off, void *buf, uint32_t len);
    void (*read_done)(void *ctx);           /* drop cached read handles */
    int (*create)(void *ctx, const char *path);    /* truncate/create, handle */
    int (*write)(void *ctx, int handle, const void *buf, uint32_t len);
    int (*close)(void *ctx, int handle);
    int (*mkdir)(void *ctx, const char *path);
    int (*rmdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *from, const char *to);
    uint32_t (*now)(void *ctx);             /* unix time, may be NULL */
    /* Size and free space in bytes; may be NULL or fail (< 0). */
    int (*space)(void *ctx, uint64_t *total, uint64_t *available);
    /* A scratch file on the calculator's storage (the USB drive mode keeps
       what does not fit in RAM there). All three may be NULL. */
    int (*scratch_open)(void *ctx);                 /* new and empty; handle */
    int (*scratch_io)(void *ctx, int handle, uint32_t off, void *buf, uint32_t len, bool write);
    void (*scratch_close)(void *ctx, int handle);   /* close and delete */
} FsBackend;

typedef struct {
    void *(*alloc)(size_t size);
    void (*free)(void *p);
} FsAllocator;

#endif
