/* nDrive - FsBackend on the calculator file system (OS entry points called
 * directly, safe inside OS callbacks). Copyright (C) 2026 Rodrigo. GPL v3+. */
#ifndef NDRIVE_CALCFS_H
#define NDRIVE_CALCFS_H

#include <stdbool.h>
#include "fs.h"

/* Fails if the needed OS entries are not available. */
bool calcfs_init(FsBackend *backend, FsAllocator *alloc);

/* Folder the backend shows as "/": "" for the whole calculator, or a folder
   such as "/documents". */
void calcfs_set_root(const char *dir);

/* Serve /documents/NspireLogs.zip (an empty placeholder on the calculator) as
   the archive of the OS's log files, written by the OS routine build (see
   notify_logs_fn) when the folder is listed. */
void calcfs_virtual_logs(bool on, void (*build)(char *path));

/* Write a small text file at a real path (safe in OS callbacks). */
bool calcfs_save(const char *path, const char *text);

#endif
