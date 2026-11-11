#ifndef POSIX_FS_H
#define POSIX_FS_H
#include "fs.h"
extern const FsBackend posix_fs;
extern const FsAllocator posix_mem;
extern unsigned posix_fs_fail_writes_after;
void posix_fs_root(const char *dir);
#endif
