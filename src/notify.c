/* nDrive - "Document Received" exactly as for a TI link transfer.
 *
 * The OS handles a finished TI link (NavNet) transfer with a notification
 * routine that records the folder and document and shows the familiar dialog
 * (Folder / Document, Open / Go To / OK). nDrive finds that routine by its
 * code signature and calls it with the same kind of message for files that
 * arrived through the drive.
 *
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "notify.h"

#include <string.h>

#include "usbhook.h"

typedef void (*NotifyFn)(uint8_t *msg);
static NotifyFn notify_fn;
static uint8_t msg[0x400] __attribute__((aligned(8)));
static char doc[256];

/* push {r4,lr}; subs r4,r0,#0; popeq; ldrsh r3,[r4,#2]; cmp r3,#0; blt; bl;
   cmp r0,#0; popne; mov r0,r4; bl; mov r0,r4; bl; ldrsh r0,[r4,#2]; cmp r0,#1 */
static const uint32_t sig[] = {
    0xe92d4010u, 0xe2504000u, 0x08bd8010u, 0xe1d430f2u, 0xe3530000u, 0xba00000fu,
    0xeb000000u, 0xe3500000u, 0x18bd8010u, 0xe1a00004u, 0xeb000000u, 0xe1a00004u,
    0xeb000000u, 0xe1d400f2u, 0xe3500001u
};
#define SIG_N (sizeof(sig) / sizeof(sig[0]))

/* The one place in the OS's code that matches a signature (words of ARM code;
   the targets of "bl" instructions are not compared). NULL if none or several. */
static const void *find_code(const uint32_t *want, unsigned n) {
    const volatile uint32_t *found = NULL;
    /* OS code is mapped from 0x10000000; it ends well before 0x10c00000. */
    for (const volatile uint32_t *p = (const volatile uint32_t *)0x10000000u;
         p < (const volatile uint32_t *)0x10c00000u; ++p) {
        if (!((uintptr_t)p & 0xfff) && (!mem_page_mapped((uintptr_t)p) || !mem_page_mapped((uintptr_t)p + 4096))) {
            p += 1023;                      /* skip the page */
            continue;
        }
        if (*p != want[0]) continue;
        unsigned k;
        for (k = 1; k < n; ++k) {
            uint32_t mask = (want[k] >> 24) == 0xeb ? 0xff000000u : 0xffffffffu;
            if ((p[k] & mask) != want[k]) break;
        }
        if (k != n) continue;
        if (found) return NULL;             /* ambiguous: do not guess */
        found = p;
    }
    return (const void *)(uintptr_t)found;
}

bool notify_init(void) {
    if (!notify_fn) notify_fn = (NotifyFn)(uintptr_t)find_code(sig, SIG_N);
    return notify_fn != NULL;
}

/* The OS routine behind "NspireLogs.zip": when the TI link asks for
   /documents/NspireLogs.zip (an empty placeholder), the OS calls it with the
   path buffer; it writes the archive of its log files and puts the archive's
   real path in the buffer (50 bytes).
   push {r4,lr}; mov r4,r0; sub sp,#8; mov r0,#2; bl; bl; mov r1,#0; mov r2,r1;
   ldr r0,[pc,#112]; bl; bl; cmp r0,#0; bne */
NotifyLogsFn notify_logs_fn(void) {
    static const uint32_t logs_sig[] = {
        0xe92d4010u, 0xe1a04000u, 0xe24dd008u, 0xe3a00002u, 0xeb000000u, 0xeb000000u,
        0xe3a01000u, 0xe1a02001u, 0xe59f0070u, 0xeb000000u, 0xeb000000u, 0xe3500000u, 0x1a000013u
    };
    return (NotifyLogsFn)(uintptr_t)find_code(logs_sig, sizeof(logs_sig) / sizeof(logs_sig[0]));
}

bool notify_received(const char *path) {
    if (!notify_fn) return false;
    const char *rel = path;
    if (!strncmp(rel, "/documents/", 11)) rel += 11;
    const char *slash = strrchr(rel, '/');
    size_t flen = slash ? (size_t)(slash - rel) : 0;
    const char *name = slash ? slash + 1 : rel;
    if (flen > 255 || strlen(name) > 255) return false;
    memset(msg, 0, sizeof(msg));
    msg[0] = 2;                                 /* document received */
    *(int16_t *)(msg + 2) = 1;                  /* completed */
    memcpy(msg + 0x208, rel, flen);
    strcpy(doc, name);
    *(char **)(msg + 0x308) = doc;
    notify_fn(msg);
    return true;
}
