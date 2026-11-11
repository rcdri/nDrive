#ifndef POSIX_BACKEND_H
#define POSIX_BACKEND_H
#include "vfat.h"
extern const VfatBackend posix_backend;
extern const VfatAllocator posix_alloc;
extern unsigned posix_backend_fail_writes_after;
#endif
