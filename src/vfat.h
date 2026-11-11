/* nDrive - virtual FAT16 view of a calculator folder.
 *
 * The volume is generated on the fly from a snapshot of the folder tree:
 * an MBR, a FAT16 partition, FAT/root/directory sectors built in RAM and file
 * data read from the calculator when the host asks for it. Host writes go to a
 * RAM overlay; vfat_sync() parses the resulting FAT volume and applies the
 * difference (new/changed/moved/deleted files and folders) to the calculator.
 *
 * Portable C99: no OS calls except through VfatBackend and the allocator.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#ifndef NDRIVE_VFAT_H
#define NDRIVE_VFAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs.h"

#define VFAT_SECTOR 512u
#define VFAT_BLOCK 4096u              /* overlay granularity (8 sectors) */
#define VFAT_PART_START 2048u         /* partition offset in sectors */
#define VFAT_PATH_MAX FS_PATH_MAX

/* Calculator side of the drive: see fs.h. */
typedef FsListCb VfatListCb;
typedef FsBackend VfatBackend;
typedef FsAllocator VfatAllocator;

typedef struct Vfat Vfat;

typedef enum {
    VFAT_OK = 0,
    VFAT_NO_MEMORY = -1,
    VFAT_IO = -2,
    VFAT_TOO_BIG = -3,
    VFAT_BAD_FS = -4,       /* host left something we cannot parse */
    VFAT_CANCELLED = -5,
    VFAT_NO_SPACE = -6      /* overlay full */
} VfatResult;

typedef struct {
    unsigned files_written, files_moved, files_deleted;
    unsigned dirs_created, dirs_removed;
    unsigned errors, skipped;
    char first_error[96];
    char last_written[VFAT_PATH_MAX];   /* calculator path of a new file */
} VfatReport;

typedef struct {
    /* Called before applying when the host deleted many files. Return true
       to apply. NULL means apply. */
    bool (*confirm_deletes)(void *user, unsigned deleted, unsigned total);
    /* Progress: done/total operations. May be NULL. */
    void (*progress)(void *user, unsigned done, unsigned total, const char *name);
    void *user;
} VfatSyncHooks;

/* Snapshot root (e.g. "/documents"). ram_bytes bounds the RAM used for what
   the host writes. free_bytes is the free space shown to the host: what does
   not fit in RAM goes to the backend's scratch file. 0 (or no scratch file):
   only RAM, and the free space follows from ram_bytes. */
VfatResult vfat_open(Vfat **out, const VfatBackend *backend, const VfatAllocator *mem,
                     const char *root, uint32_t ram_bytes, uint32_t free_bytes);
void vfat_close(Vfat *v);

uint32_t vfat_sectors(const Vfat *v);          /* total disk sectors */
uint32_t vfat_file_count(const Vfat *v);
uint32_t vfat_free_bytes(const Vfat *v);
VfatResult vfat_read(Vfat *v, uint32_t lba, uint32_t count, uint8_t *buf);
VfatResult vfat_write(Vfat *v, uint32_t lba, uint32_t count, const uint8_t *buf);
bool vfat_dirty(const Vfat *v);
uint32_t vfat_written_blocks(const Vfat *v);

VfatResult vfat_sync(Vfat *v, const VfatSyncHooks *hooks, VfatReport *report);

#endif
