/* nDrive - MTP (Media Transfer Protocol) responder, transport independent.
 * Copyright (C) 2026 Rodrigo. GPL v3+.
 *
 * The transport feeds everything received on the bulk OUT endpoint to mtp_rx()
 * and sends what mtp_tx() produces on bulk IN. Each container mtp_tx() returns
 * is one USB transfer: when *end is set the transport finishes the transfer
 * (short packet, or a zero-length packet after a multiple of the packet size).
 * Portable C99: no OS calls except through FsBackend and the allocator. */
#ifndef NDRIVE_MTP_H
#define NDRIVE_MTP_H

#include "fs.h"

typedef struct Mtp Mtp;

typedef struct {
    uint32_t commands;          /* operations handled */
    uint32_t files_in;          /* files received from the computer */
    uint32_t files_out;         /* files sent to the computer */
    uint32_t deleted, renamed, folders;
    uint32_t errors;            /* operations answered with an error code */
    uint16_t last_op, last_error;
} MtpStats;

/* received(user, path): a file from the computer was saved at path. */
Mtp *mtp_create(const FsBackend *fs, const FsAllocator *mem,
                void (*received)(void *user, const char *path), void *user);
void mtp_destroy(Mtp *m);

/* Bus reset / new connection: forget the session and any transfer in progress. */
void mtp_reset(Mtp *m);

/* The file system behind the backend changed (another root folder): drop every
   object. protect_top: top-level folders cannot be deleted, renamed or moved
   (they are the OS's own when the root is the whole calculator). */
void mtp_remount(Mtp *m, bool protect_top, const char *storage_name);

void mtp_rx(Mtp *m, const uint8_t *data, uint32_t len);

/* Something to send? */
bool mtp_tx_pending(const Mtp *m);
/* Next bytes (up to max) of the container being sent. */
uint32_t mtp_tx(Mtp *m, uint8_t *buf, uint32_t max, bool *end);

/* True while a file from the computer is being written. */
bool mtp_receiving(const Mtp *m);

const MtpStats *mtp_stats(const Mtp *m);
uint32_t mtp_objects(const Mtp *m);

#endif
