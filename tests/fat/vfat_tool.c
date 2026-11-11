/* vfat_tool: drive the virtual volume from the command line.
 *   vfat_tool dump  ROOT IMG [OVERLAY_MB]        whole disk image as the host sees it
 *   vfat_tool apply ROOT OLD.IMG NEW.IMG [OVERLAY_MB] [--no]
 *       snapshot ROOT, write every sector that differs between OLD and NEW
 *       (in random order), then sync the changes back into ROOT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix_backend.h"
#include "vfat.h"

static bool confirm_no(void *u, unsigned d, unsigned t) { (void)u; printf("confirm deletes %u/%u -> no\n", d, t); return false; }
static bool confirm_yes(void *u, unsigned d, unsigned t) { (void)u; printf("confirm deletes %u/%u -> yes\n", d, t); return true; }

#include <dirent.h>
#include <sys/stat.h>

static void manifest_dir(FILE *out, const char *root, const char *rel) {
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", root, rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r2[2048], p2[4096];
        snprintf(r2, sizeof(r2), "%s/%s", rel, e->d_name);
        snprintf(p2, sizeof(p2), "%s%s", root, r2);
        struct stat st;
        stat(p2, &st);
        if (S_ISDIR(st.st_mode)) { fprintf(out, "D %s\n", r2); manifest_dir(out, root, r2); }
        else {
            FILE *f = fopen(p2, "rb");
            uint32_t h = 2166136261u; int c;
            while ((c = fgetc(f)) != EOF) h = (h ^ (uint32_t)c) * 16777619u;
            fclose(f);
            fprintf(out, "F %s %ld %08x\n", r2, (long)st.st_size, h);
        }
    }
    closedir(d);
}

/* VFAT_RAM_KB: RAM for the overlay instead of the MB given (to force the scratch file). */
static uint32_t ram_bytes(unsigned mb) {
    const char *e = getenv("VFAT_RAM_KB");
    return e ? (uint32_t)atoi(e) << 10 : mb << 20;
}

/* VFAT_FREE_MB: free space to show, beyond what fits in RAM (scratch file). */
static uint32_t free_bytes(void) {
    const char *e = getenv("VFAT_FREE_MB");
    return e ? (uint32_t)atoi(e) << 20 : 0;
}

/* session ROOT OVERLAY_MB IMG0 IMG1 ... : one snapshot, one sync per image. */
static int session(int argc, char **argv) {
    unsigned mb = (unsigned)atoi(argv[3]);
    Vfat *v;
    if (vfat_open(&v, &posix_backend, &posix_alloc, argv[2], ram_bytes(mb), free_bytes()) != VFAT_OK) return 1;
    uint32_t n = vfat_sectors(v);
    uint8_t *prev = malloc((size_t)n * 512), *cur = malloc((size_t)n * 512), *view = malloc((size_t)n * 512);
    FILE *f = fopen(argv[4], "rb");
    if (!f || fread(prev, 512, n, f) != n) { printf("bad img0\n"); return 1; }
    fclose(f);
    if (vfat_read(v, 0, n, view) != VFAT_OK || memcmp(view, prev, (size_t)n * 512)) { printf("img0 differs from volume\n"); return 1; }
    int rc = 0;
    for (int k = 5; k < argc; ++k) {
        f = fopen(argv[k], "rb");
        if (!f || fread(cur, 512, n, f) != n) { printf("bad img %s\n", argv[k]); return 1; }
        fclose(f);
        uint32_t nc = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (memcmp(prev + (size_t)i * 512, cur + (size_t)i * 512, 512)) {
                if (vfat_write(v, i, 1, cur + (size_t)i * 512) != VFAT_OK) { printf("write error\n"); return 1; }
                ++nc;
            }
        VfatReport rep;
        VfatResult r = vfat_sync(v, NULL, &rep);
        printf("round %d: %u sectors; sync %d: written %u moved %u deleted %u mkdir %u rmdir %u errors %u %s\n",
               k - 4, nc, r, rep.files_written, rep.files_moved, rep.files_deleted, rep.dirs_created,
               rep.dirs_removed, rep.errors, rep.first_error);
        if (r != VFAT_OK || rep.errors) rc = 1;
        /* The host's view must not change because of a sync (free clusters
           excepted: their content is meaningless to a FAT driver). */
        if (vfat_read(v, 0, n, view) != VFAT_OK) { printf("read error\n"); return 1; }
        const uint8_t *bs = cur + 2048 * 512;
        uint32_t spc = bs[13], rsvd = bs[14] | bs[15] << 8, nfat = bs[16];
        uint32_t rootents = bs[17] | bs[18] << 8, fatsz = bs[22] | bs[23] << 8;
        uint32_t data0 = 2048 + rsvd + nfat * fatsz + rootents * 32 / 512;
        const uint8_t *fat = cur + (size_t)(2048 + rsvd) * 512;
        uint32_t bad = 0, first = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!memcmp(view + (size_t)i * 512, cur + (size_t)i * 512, 512)) continue;
            if (i >= data0) {
                uint32_t c = 2 + (i - data0) / spc;
                uint32_t e = fat[2 * c] | fat[2 * c + 1] << 8;
                if (e == 0 || e == 0xfff7) continue;
            }
            if (!bad++) first = i;
        }
        if (bad) { printf("round %d: host view changed by sync (%u sectors, first %u)\n", k - 4, bad, first); rc = 1; }
        char name[512];
        snprintf(name, sizeof(name), "%s.manifest", argv[k]);
        FILE *m = fopen(name, "w");
        manifest_dir(m, argv[2], "");
        fclose(m);
        memcpy(prev, cur, (size_t)n * 512);
    }
    vfat_close(v);
    free(prev); free(cur); free(view);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    if (!strcmp(argv[1], "session")) return session(argc, argv);
    bool apply = !strcmp(argv[1], "apply");
    unsigned mb = (unsigned)atoi(argc > (apply ? 5 : 4) ? argv[apply ? 5 : 4] : "8");
    bool no = argc > 6 && !strcmp(argv[6], "--no");
    Vfat *v;
    VfatResult r = vfat_open(&v, &posix_backend, &posix_alloc, argv[2], ram_bytes(mb), free_bytes());
    if (r != VFAT_OK) { printf("open failed %d\n", r); return 1; }
    uint32_t n = vfat_sectors(v);
    printf("sectors %u files %u free %u\n", n, vfat_file_count(v), vfat_free_bytes(v));
    if (!strcmp(argv[1], "dump")) {
        FILE *f = fopen(argv[3], "wb");
        uint8_t *buf = malloc(64 * 512);
        for (uint32_t lba = 0; lba < n; lba += 64) {
            uint32_t c = n - lba < 64 ? n - lba : 64;
            if (vfat_read(v, lba, c, buf) != VFAT_OK) { printf("read error at %u\n", lba); return 1; }
            fwrite(buf, 512, c, f);
        }
        fclose(f);
        free(buf);
        vfat_close(v);
        return 0;
    }
    if (!apply) return 2;
    FILE *a = fopen(argv[3], "rb"), *b = fopen(argv[4], "rb");
    if (!a || !b) { perror("img"); return 1; }
    uint8_t *x = malloc((size_t)n * 512), *y = calloc(n, 512);
    if (fread(x, 512, n, a) != n) { printf("old image short\n"); return 1; }
    size_t got = fread(y, 512, n, b);
    (void)got;
    uint32_t *changed = malloc(sizeof(uint32_t) * n), nc = 0;
    for (uint32_t i = 0; i < n; ++i) if (memcmp(x + (size_t)i * 512, y + (size_t)i * 512, 512)) changed[nc++] = i;
    /* Shuffle to model hosts writing in any order. */
    srand(1234);
    for (uint32_t i = nc; i > 1; --i) { uint32_t j = (uint32_t)rand() % i, t = changed[i - 1]; changed[i - 1] = changed[j]; changed[j] = t; }
    for (uint32_t i = 0; i < nc; ++i) {
        r = vfat_write(v, changed[i], 1, y + (size_t)changed[i] * 512);
        if (r != VFAT_OK) { printf("write error %d at %u\n", r, changed[i]); break; }
    }
    printf("wrote %u sectors, %u overlay blocks\n", nc, vfat_written_blocks(v));
    /* The host reads its own writes back. */
    uint8_t s[512];
    for (uint32_t i = 0; i < n && r == VFAT_OK; i += 97) {
        vfat_read(v, i, 1, s);
        if (memcmp(s, y + (size_t)i * 512, 512)) { printf("readback mismatch at %u\n", i); return 1; }
    }
    VfatSyncHooks hooks = { no ? confirm_no : confirm_yes, NULL, NULL };
    VfatReport rep;
    r = vfat_sync(v, &hooks, &rep);
    printf("sync %d: written %u moved %u deleted %u mkdir %u rmdir %u skipped %u errors %u %s\n", r,
           rep.files_written, rep.files_moved, rep.files_deleted, rep.dirs_created, rep.dirs_removed,
           rep.skipped, rep.errors, rep.first_error);
    vfat_close(v);
    free(x); free(y); free(changed); fclose(a); fclose(b);
    return r == VFAT_OK && !rep.errors ? 0 : 1;
}
