/* nDrive - Bulk-Only Transport and SCSI commands. See msc.h.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "msc.h"

#include <string.h>

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

enum { GOOD = 0, FAILED = 1, PHASE = 2 };
enum { NO_SENSE = 0, NOT_READY = 2, MEDIUM_ERROR = 3, ILLEGAL_REQUEST = 5, UNIT_ATTENTION = 6 };

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void msc_init(Msc *m, Vfat *vfat) {
    memset(m, 0, sizeof(*m));
    m->no_stall = true;
    m->mps = 64;
    m->vfat = vfat;
    m->sectors = vfat_sectors(vfat);
}

void msc_set_volume(Msc *m, Vfat *vfat) {
    m->vfat = vfat;
    m->sectors = vfat_sectors(vfat);
    m->media_changed = true;
    m->ejected = false;
}

static void sense(Msc *m, uint8_t key, uint8_t asc, uint8_t ascq) {
    m->key = key; m->asc = asc; m->ascq = ascq;
    m->status = key == NO_SENSE ? GOOD : FAILED;
}

/* Plan a data phase of dev bytes in direction dir against what the host
   announced (BOT 1.0 section 6.7, the thirteen cases). */
static void plan(Msc *m, MscDir dir, uint32_t dev) {
    m->dir = MSC_NONE;
    m->xfer = 0;
    m->want = 0;
    m->stall_in = false;
    m->discard = false;
    if (!m->host_len) {                         /* Hn */
        if (dir != MSC_NONE && dev) { m->status = PHASE; m->rw = false; }
        return;
    }
    if (m->host_in) {                           /* Hi */
        if (dir == MSC_OUT && dev) { m->status = PHASE; m->rw = false; dev = 0; }
        if (dir == MSC_NONE) dev = 0;
        if (dev > m->host_len) { dev = m->host_len; m->status = PHASE; }
        m->dir = dev ? MSC_IN : MSC_NONE;
        m->xfer = m->want = dev;
        /* A short last packet ends the transfer; otherwise stall, or pad
           with zeros up to what the host asked for (residue reports it). */
        if (dev < m->host_len && dev % m->mps == 0) {
            if (m->no_stall) { m->dir = MSC_IN; m->xfer = m->host_len; }
            else m->stall_in = true;
        }
        return;
    }
    /* Ho: the host sends host_len bytes in any case; accept them all. */
    if (dir == MSC_IN && dev) { m->status = PHASE; m->rw = false; dev = 0; }
    if (dir == MSC_NONE) dev = 0;
    if (dev > m->host_len) { m->status = PHASE; dev = m->host_len; }
    m->dir = MSC_OUT;
    m->xfer = m->host_len;
    m->want = dev;
    m->discard = !dev;
}

static void reply(Msc *m, const uint8_t *data, uint32_t len, uint32_t alloc) {
    if (len > alloc) len = alloc;
    memcpy(m->reply, data, len);
    m->rw = false;
    plan(m, MSC_IN, len);
}

static bool medium(Msc *m) {
    if (m->ejected) { sense(m, NOT_READY, 0x3a, 0); return false; }
    return true;
}

static bool range(Msc *m, uint32_t lba, uint32_t count) {
    if (lba > m->sectors || count > m->sectors - lba) { sense(m, ILLEGAL_REQUEST, 0x21, 0); return false; }
    return true;
}

bool msc_command(Msc *m, const uint8_t *cbw, uint32_t len) {
    if (len != MSC_CBW_LEN || le32(cbw) != CBW_SIG) return false;
    uint8_t cblen = cbw[14] & 0x1f;
    if (cbw[13] & 0xf0 || !cblen || cblen > 16) return false;
    m->tag = le32(cbw + 4);
    m->host_len = le32(cbw + 8);
    m->host_in = (cbw[12] & 0x80) != 0;
    memset(m->cdb, 0, sizeof(m->cdb));
    memcpy(m->cdb, cbw + 15, cblen);
    m->done = 0;
    m->fill = 0;
    m->rw = false;
    m->status = GOOD;
    const uint8_t *c = m->cdb;
    uint8_t r[36];
    memset(r, 0, sizeof(r));
    if (c[0] != 0x03) sense(m, NO_SENSE, 0, 0);
    if (m->busy && c[0] != 0x12 && c[0] != 0x03) {
        sense(m, NOT_READY, 0x04, 0x01);                    /* becoming ready */
        plan(m, m->host_in ? MSC_IN : MSC_OUT, 0);
        return true;
    }
    if (m->media_changed && c[0] != 0x12 && c[0] != 0x03) {
        m->media_changed = false;
        sense(m, UNIT_ATTENTION, 0x28, 0);              /* not ready to ready: medium changed */
        plan(m, m->host_in ? MSC_IN : MSC_OUT, 0);
        return true;
    }
    switch (c[0]) {
    case 0x00:                                              /* TEST UNIT READY */
        medium(m);
        plan(m, MSC_NONE, 0);
        break;
    case 0x03: {                                            /* REQUEST SENSE */
        r[0] = 0x70; r[2] = m->key; r[7] = 10; r[12] = m->asc; r[13] = m->ascq;
        sense(m, NO_SENSE, 0, 0);
        reply(m, r, 18, c[4]);
        break;
    }
    case 0x12:                                              /* INQUIRY */
        if (c[1] & 1) {
            if (c[2] == 0x00) { r[1] = 0; r[3] = 2; r[4] = 0x00; r[5] = 0x80; reply(m, r, 6, be16(c + 3)); }
            else if (c[2] == 0x80) { r[1] = 0x80; r[3] = 8; memcpy(r + 4, "NDRIVE01", 8); reply(m, r, 12, be16(c + 3)); }
            else { sense(m, ILLEGAL_REQUEST, 0x24, 0); plan(m, MSC_IN, 0); }
            break;
        }
        r[0] = 0x00; r[1] = 0x80; r[2] = 0x04; r[3] = 0x02; r[4] = 31;
        memcpy(r + 8, "TI      ", 8);
        memcpy(r + 16, "TI-Nspire CX II ", 16);
        memcpy(r + 32, "1.0 ", 4);
        reply(m, r, 36, be16(c + 3) ? be16(c + 3) : c[4]);
        break;
    case 0x1a:                                              /* MODE SENSE(6) */
        r[0] = 3;
        reply(m, r, 4, c[4]);
        break;
    case 0x5a:                                              /* MODE SENSE(10) */
        r[1] = 6;
        reply(m, r, 8, be16(c + 7));
        break;
    case 0x1e:                                              /* PREVENT ALLOW */
        m->prevent = c[4] & 1;
        plan(m, MSC_NONE, 0);
        break;
    case 0x1b:                                              /* START STOP UNIT */
        if ((c[4] & 2) && !(c[4] & 1)) m->ejected = true;
        plan(m, MSC_NONE, 0);
        break;
    case 0x23:                                              /* READ FORMAT CAPACITIES */
        if (!medium(m)) { plan(m, MSC_IN, 0); break; }
        r[3] = 8;
        put_be32(r + 4, m->sectors);
        r[8] = 0x02;
        r[10] = 0x02;                                       /* 512-byte blocks */
        reply(m, r, 12, be16(c + 7));
        break;
    case 0x25:                                              /* READ CAPACITY(10) */
        if (!medium(m)) { plan(m, MSC_IN, 0); break; }
        put_be32(r, m->sectors - 1);
        put_be32(r + 4, 512);
        reply(m, r, 8, 8);
        break;
    case 0x9e:                                              /* READ CAPACITY(16) */
        if ((c[1] & 0x1f) != 0x10) { sense(m, ILLEGAL_REQUEST, 0x20, 0); plan(m, MSC_IN, 0); break; }
        if (!medium(m)) { plan(m, MSC_IN, 0); break; }
        put_be32(r + 4, m->sectors - 1);
        put_be32(r + 8, 512);
        reply(m, r, 32, be32(c + 10));
        break;
    case 0x08: case 0x28: case 0x0a: case 0x2a: {           /* READ/WRITE (6/10) */
        bool six = c[0] == 0x08 || c[0] == 0x0a;
        bool write = c[0] == 0x0a || c[0] == 0x2a;
        uint32_t lba = six ? ((uint32_t)(c[1] & 0x1f) << 16 | c[2] << 8 | c[3]) : be32(c + 2);
        uint32_t count = six ? (c[4] ? c[4] : 256) : be16(c + 7);
        if (!medium(m) || !range(m, lba, count)) { plan(m, write ? MSC_OUT : MSC_IN, 0); break; }
        m->lba = lba;
        m->rw = true;
        plan(m, write ? MSC_OUT : MSC_IN, count * 512u);
        if (m->status == PHASE) m->rw = false;
        break;
    }
    case 0x2f:                                              /* VERIFY(10) */
    case 0x35:                                              /* SYNCHRONIZE CACHE */
        medium(m);
        plan(m, MSC_NONE, 0);
        break;
    default:
        sense(m, ILLEGAL_REQUEST, 0x20, 0);
        plan(m, m->host_in ? MSC_IN : MSC_OUT, 0);
        break;
    }
    return true;
}

uint32_t msc_in(Msc *m, uint8_t *buf, uint32_t max) {
    uint32_t n = m->xfer - m->done;
    if (n > max) n = max;
    if (!n) return 0;
    if (m->done >= m->want) {                   /* padding */
        memset(buf, 0, n);
        m->done += n;
        return n;
    }
    if (n > m->want - m->done) n = m->want - m->done;
    if (m->rw) {
        /* n is a whole number of sectors unless the host asked for less. */
        uint32_t secs = (n + 511u) / 512u;
        if (vfat_read(m->vfat, m->lba, secs, buf) != VFAT_OK) {
            memset(buf, 0, secs * 512u);
            if (m->status == GOOD) sense(m, MEDIUM_ERROR, 0x11, 0);
        }
        m->lba += secs;
        m->reads += secs;
    } else {
        memcpy(buf, m->reply + m->done, n);
    }
    m->done += n;
    return n;
}

static void write_sectors(Msc *m, const uint8_t *buf, uint32_t secs) {
    if (m->status == GOOD) {
        VfatResult r = vfat_write(m->vfat, m->lba, secs, buf);
        if (r != VFAT_OK) {
            m->write_error = true;
            sense(m, MEDIUM_ERROR, 0x0c, r == VFAT_NO_SPACE ? 0x02 : 0x00);
        }
    }
    m->lba += secs;
    m->writes += secs;
}

void msc_out(Msc *m, const uint8_t *buf, uint32_t len) {
    if (len > m->xfer - m->done) len = m->xfer - m->done;
    uint32_t start = m->done;
    m->done += len;
    if (!m->rw || m->discard || start >= m->want) return;
    if (len > m->want - start) len = m->want - start;   /* drop the excess */
    if (m->fill) {
        uint32_t take = 512u - m->fill < len ? 512u - m->fill : len;
        memcpy(m->sector + m->fill, buf, take);
        m->fill += take;
        buf += take;
        len -= take;
        if (m->fill == 512u) { write_sectors(m, m->sector, 1); m->fill = 0; }
    }
    if (len >= 512u) {
        uint32_t secs = len / 512u;
        write_sectors(m, buf, secs);
        buf += secs * 512u;
        len -= secs * 512u;
    }
    if (len) { memcpy(m->sector, buf, len); m->fill = len; }
}

void msc_status(Msc *m, uint8_t csw[MSC_CSW_LEN]) {
    put_le32(csw, CSW_SIG);
    put_le32(csw + 4, m->tag);
    /* Residue: what the host announced minus what the command used. */
    uint32_t used = m->dir == MSC_IN ? (m->done < m->want ? m->done : m->want) :
                    m->dir == MSC_OUT ? m->want : 0;
    put_le32(csw + 8, m->host_len - used);
    csw[12] = m->status;
}
