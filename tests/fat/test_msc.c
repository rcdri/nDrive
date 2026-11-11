/* Bulk-Only Transport / SCSI tests: a simulated host drives msc.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msc.h"
#include "posix_backend.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static Msc m;
static uint32_t tag = 1;

typedef struct { uint8_t status; uint32_t residue; uint32_t got; bool stall; bool ok; } Result;

static void cbw(uint8_t *b, uint32_t len, bool in, const uint8_t *cdb, int cdblen) {
    memset(b, 0, 31);
    b[0] = 'U'; b[1] = 'S'; b[2] = 'B'; b[3] = 'C';
    b[4] = (uint8_t)tag; b[5] = (uint8_t)(tag >> 8);
    b[8] = (uint8_t)len; b[9] = (uint8_t)(len >> 8); b[10] = (uint8_t)(len >> 16); b[11] = (uint8_t)(len >> 24);
    b[12] = in ? 0x80 : 0;
    b[14] = (uint8_t)cdblen;
    memcpy(b + 15, cdb, (size_t)cdblen);
}

/* One command; data in/out through buf; chunk = transfer piece size. */
static Result cmd(const uint8_t *cdb, int cdblen, uint32_t len, bool in, uint8_t *buf, uint32_t chunk) {
    uint8_t b[31], csw[13];
    Result r = {0};
    cbw(b, len, in, cdb, cdblen);
    r.ok = msc_command(&m, b, 31);
    if (!r.ok) return r;
    if (m.dir == MSC_IN) {
        CHECK(in);
        while (m.done < m.xfer) {
            uint32_t n = msc_in(&m, buf + r.got, chunk);
            CHECK(n > 0);
            r.got += n;
        }
    } else if (m.dir == MSC_OUT) {
        CHECK(!in);
        uint32_t off = 0;
        while (m.done < m.xfer) {
            uint32_t n = m.xfer - m.done < chunk ? m.xfer - m.done : chunk;
            msc_out(&m, buf + off, n);
            off += n;
        }
        r.got = off;
    }
    r.stall = m.stall_in;
    msc_status(&m, csw);
    CHECK(!memcmp(csw, "USBS", 4));
    CHECK(csw[4] == (uint8_t)tag);
    r.residue = csw[8] | csw[9] << 8 | csw[10] << 16 | (uint32_t)csw[11] << 24;
    r.status = csw[12];
    ++tag;
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Vfat *v;
    CHECK(vfat_open(&v, &posix_backend, &posix_alloc, argv[1], 4u << 20, 0) == VFAT_OK);
    msc_init(&m, v);
    m.no_stall = false;
    static uint8_t buf[1 << 20], ref[1 << 20];

    uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    Result r = cmd(inquiry, 6, 36, true, buf, 512);
    CHECK(r.status == 0 && r.got == 36 && r.residue == 0 && buf[1] == 0x80 && !memcmp(buf + 8, "TI", 2));

    uint8_t inquiry_big[6] = {0x12, 0, 0, 0, 255, 0};
    r = cmd(inquiry_big, 6, 255, true, buf, 512);
    CHECK(r.status == 0 && r.got == 36 && r.residue == 219 && !r.stall);   /* short packet ends it */

    uint8_t unknown[10] = {0xd5};
    r = cmd(unknown, 10, 64, true, buf, 512);
    CHECK(r.status == 1 && r.got == 0 && r.residue == 64 && r.stall);
    uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    r = cmd(sense, 6, 18, true, buf, 512);
    CHECK(r.status == 0 && r.got == 18 && buf[2] == 5 && buf[12] == 0x20);
    r = cmd(sense, 6, 18, true, buf, 512);
    CHECK(buf[2] == 0);                                     /* sense cleared */

    uint8_t tur[6] = {0};
    r = cmd(tur, 6, 0, false, buf, 512);
    CHECK(r.status == 0 && r.residue == 0);

    uint8_t cap[10] = {0x25};
    r = cmd(cap, 10, 8, true, buf, 512);
    uint32_t last = (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
    CHECK(r.status == 0 && last + 1 == vfat_sectors(v) && buf[6] == 2);

    uint8_t fmt[10] = {0x23, 0, 0, 0, 0, 0, 0, 0, 0xfc, 0};
    r = cmd(fmt, 10, 0xfc, true, buf, 512);
    CHECK(r.status == 0 && r.got == 12 && r.residue == 0xfc - 12);

    uint8_t ms6[6] = {0x1a, 0, 0x3f, 0, 192, 0};
    r = cmd(ms6, 6, 192, true, buf, 512);
    CHECK(r.status == 0 && r.got == 4 && !(buf[2] & 0x80));

    /* READ(10) of the first 64 KiB, in 4 KiB chunks, equals vfat_read. */
    uint8_t rd[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 128, 0};
    r = cmd(rd, 10, 65536, true, buf, 4096);
    CHECK(vfat_read(v, 0, 128, ref) == VFAT_OK);
    CHECK(r.status == 0 && r.got == 65536 && r.residue == 0 && !memcmp(buf, ref, 65536));

    /* Out of range */
    uint8_t bad[10] = {0x28, 0, 0xff, 0xff, 0xff, 0x00, 0, 0, 1, 0};
    r = cmd(bad, 10, 512, true, buf, 512);
    CHECK(r.status == 1 && r.residue == 512 && r.stall);

    /* WRITE(10) 3 sectors at LBA 5000 delivered in 64-byte packets. */
    for (int i = 0; i < 1536; ++i) buf[i] = (uint8_t)(i * 7 + 3);
    uint8_t wr[10] = {0x2a, 0, 0, 0, 0x13, 0x88, 0, 0, 3, 0};
    r = cmd(wr, 10, 1536, false, buf, 64);
    CHECK(r.status == 0 && r.residue == 0);
    CHECK(vfat_read(v, 5000, 3, ref) == VFAT_OK && !memcmp(ref, buf, 1536));
    CHECK(vfat_dirty(v));

    /* Ho > Do: host sends 1024 bytes for a 1-sector write. */
    uint8_t before[512];
    vfat_read(v, 6001, 1, before);
    memset(buf, 0xab, 1024);
    uint8_t wr1[10] = {0x2a, 0, 0, 0, 0x17, 0x70, 0, 0, 1, 0};
    r = cmd(wr1, 10, 1024, false, buf, 512);
    CHECK(r.status == 0 && r.residue == 512);
    vfat_read(v, 6000, 1, ref);
    CHECK(ref[0] == 0xab && ref[511] == 0xab);
    vfat_read(v, 6001, 1, ref);
    CHECK(!memcmp(ref, before, 512));                       /* excess not written */

    /* Hi < Di: phase error. */
    uint8_t rd2[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    r = cmd(rd2, 10, 512, true, buf, 512);
    CHECK(r.status == 2);

    /* Hn with a data command: phase error, no data. */
    r = cmd(rd2, 10, 0, true, buf, 512);
    CHECK(r.status == 2 && r.got == 0);

    /* Bad CBW */
    uint8_t junk[31] = {0};
    CHECK(!msc_command(&m, junk, 31));
    CHECK(!msc_command(&m, junk, 30));

    /* Eject */
    uint8_t prevent[6] = {0x1e, 0, 0, 0, 1, 0};
    r = cmd(prevent, 6, 0, false, buf, 512);
    CHECK(r.status == 0 && m.prevent);
    uint8_t eject[6] = {0x1b, 0, 0, 0, 0x02, 0};
    r = cmd(eject, 6, 0, false, buf, 512);
    CHECK(r.status == 0 && m.ejected);
    r = cmd(tur, 6, 0, false, buf, 512);
    CHECK(r.status == 1);
    r = cmd(sense, 6, 18, true, buf, 512);
    CHECK(buf[2] == 2 && buf[12] == 0x3a);

    /* No-stall mode (EP2 shares the controller with the OS): pad instead. */
    msc_init(&m, v);
    r = cmd(unknown, 10, 64, true, buf, 512);
    CHECK(r.status == 1 && r.got == 64 && r.residue == 64 && !r.stall);
    r = cmd(bad, 10, 512, true, buf, 512);
    CHECK(r.status == 1 && r.got == 512 && r.residue == 512 && !r.stall);
    uint8_t inq64[6] = {0x12, 0, 0, 0, 64, 0};
    r = cmd(inq64, 6, 128, true, buf, 512);                 /* 36 bytes: short packet ends it */
    CHECK(r.status == 0 && r.got == 36 && r.residue == 92 && !r.stall);
    uint8_t fmt64[10] = {0x23, 0, 0, 0, 0, 0, 0, 0, 0xfc, 0};
    r = cmd(fmt64, 10, 0xfc, true, buf, 512);               /* 12 bytes, short */
    CHECK(r.status == 0 && r.got == 12);
    r = cmd(cap, 10, 64, true, buf, 512);                   /* 8 of 64: short */
    CHECK(r.status == 0 && r.got == 8 && r.residue == 56);
    r = cmd(inquiry, 6, 36, true, buf, 512);
    CHECK(r.status == 0 && r.got == 36 && r.residue == 0 && buf[1] == 0x80);
    r = cmd(rd, 10, 65536, true, buf, 4096);
    CHECK(r.status == 0 && r.got == 65536 && r.residue == 0);

    vfat_close(v);
    printf(failures ? "%d FAILED\n" : "msc: ALL PASSED\n", failures);
    return failures != 0;
}
