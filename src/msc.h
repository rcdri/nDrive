/* nDrive - USB Mass Storage Bulk-Only Transport + SCSI command set over a
 * virtual FAT volume. Hardware independent: the USB driver passes the CBW,
 * moves the data phase in chunks and sends the CSW this module builds.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#ifndef NDRIVE_MSC_H
#define NDRIVE_MSC_H

#include <stdbool.h>
#include <stdint.h>

#include "vfat.h"

#define MSC_CBW_LEN 31u
#define MSC_CSW_LEN 13u

typedef enum { MSC_NONE, MSC_IN, MSC_OUT } MscDir;

typedef struct {
    Vfat *vfat;
    uint32_t sectors;
    /* sense data for REQUEST SENSE */
    uint8_t key, asc, ascq;
    bool ejected;           /* host sent START STOP UNIT (eject) */
    bool prevent;
    uint32_t reads, writes; /* sectors, for the status screen */
    bool write_error;       /* a write was refused (overlay full) */
    /* current command */
    uint32_t tag, host_len;
    bool host_in;
    uint8_t cdb[16];
    MscDir dir;             /* data phase the driver must run */
    uint32_t xfer;          /* bytes in that phase (OUT: all the host sends) */
    uint32_t want;          /* bytes the command actually uses */
    uint32_t done;
    bool stall_in;          /* stall bulk-IN after the data (Hi > Di) */
    unsigned mps;           /* bulk packet size on the bus (64 or 512) */
    bool no_stall;          /* pad short IN data instead of stalling */
    bool media_changed;     /* report UNIT ATTENTION once (new volume) */
    bool busy;              /* volume in use by a save: NOT READY 04/01 */
    uint8_t status;         /* CSW status */
    uint32_t lba;           /* streaming position for READ/WRITE */
    bool rw;                /* data phase is sector I/O */
    bool discard;           /* OUT data is accepted and dropped */
    uint8_t reply[64];      /* small IN responses */
    uint8_t sector[512];    /* partial OUT sector */
    uint32_t fill;
} Msc;

void msc_init(Msc *m, Vfat *vfat);
/* Swap in a new volume; the host is told the medium changed. */
void msc_set_volume(Msc *m, Vfat *vfat);
/* Parse a CBW. Returns false if it is not a valid, meaningful CBW (the driver
   must then stall both bulk endpoints until a Bulk-Only Mass Storage Reset). */
bool msc_command(Msc *m, const uint8_t *cbw, uint32_t len);
/* IN phase: fill up to max bytes (max is a multiple of 512). Returns count. */
uint32_t msc_in(Msc *m, uint8_t *buf, uint32_t max);
/* OUT phase: consume len bytes (whole sectors except possibly the tail). */
void msc_out(Msc *m, const uint8_t *buf, uint32_t len);
/* Build the CSW once the data phase is complete. */
void msc_status(Msc *m, uint8_t csw[MSC_CSW_LEN]);

#endif
