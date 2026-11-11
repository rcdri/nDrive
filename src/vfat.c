/* nDrive - virtual FAT16 view of a calculator folder. See vfat.h.
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include "vfat.h"

#include <string.h>

#include "ff.h"
#include "diskio.h"

#define NONE 0xffffffffu
#define ROOT_NODE 0u
#define EOC16 0xffffu
#define BAD16 0xfff7u
#define ARENA_CHUNK 16384u
#define COPY_CHUNK 16384u

typedef struct {
    uint32_t parent, child, next, last_child;
    uint32_t size, mtime;
    uint32_t clus, nclus;
    char *name;         /* calculator name */
    char *disp;         /* name shown to the host */
    char *dpath;        /* display path, "/a/b" ("" for root) */
    uint8_t *dir;       /* generated directory entries (directories) */
    uint32_t dirlen;
    uint32_t slots;     /* directory entries used in the parent */
    char *cpath;        /* calculator path after a move (NULL: by name) */
    uint8_t is_dir;
    uint8_t gone;       /* deleted on the calculator: base data reads as 0 */
} Node;

typedef struct { uint32_t start, count, node; } Run;

typedef struct {
    uint32_t key;       /* block index + 1; 0 = empty */
    uint8_t mask;       /* sectors held in the block */
    uint8_t dirty;      /* sectors written by the host since the last sync */
    uint8_t swapped;    /* in the scratch file (slot) instead of RAM (data) */
    union { uint8_t *data; uint32_t slot; } u;
} OvEntry;

/* What the calculator holds as of the last sync (initially the snapshot). */
typedef struct {
    char *dpath;        /* display path "/a/b" */
    char *cpath;        /* calculator path */
    uint32_t sclust, size, chash;
    uint32_t node;      /* snapshot node backing its base data, or NONE */
    uint8_t is_dir, matched;
} Orig;

typedef struct Chunk { struct Chunk *next; } Chunk;

typedef struct {
    char *path;         /* display path "/a/b" */
    char *cpath;        /* calculator path once applied */
    uint32_t size, sclust, chash;
    uint32_t orig;      /* matched Orig index or NONE */
    uint8_t is_dir;
    uint8_t touched;    /* data written since the last sync */
    uint8_t failed;
    uint32_t temp;      /* temporary name id, 0 = none */
} Final;

struct Vfat {
    VfatBackend be;
    VfatAllocator mem;
    char root[VFAT_PATH_MAX];
    Node *nodes;
    uint32_t nnodes, cap;
    Chunk *chunks;
    char *apos;
    size_t aleft;
    Run *runs;
    uint32_t nruns;
    uint32_t *by_dpath;     /* node indices sorted by display path */
    /* geometry (sectors relative to the partition unless noted) */
    uint32_t spc, cs, rsvd, fat_secs, root_ents, root_secs, data_start;
    uint32_t clusters, used_end, free_clusters, part_secs, disk_secs;
    uint8_t *root_data;
    uint32_t root_len;
    uint32_t volid;
    uint32_t files;
    /* overlay */
    OvEntry *ov;
    uint32_t ov_cap, ov_used, ov_max;
    /* Scratch file for blocks beyond ov_max: one block is held in RAM
       (swap_buf, slot swap_cur); slots below swap_stored are in the file. */
    uint32_t ram_used, swap_max, swap_used, swap_stored, swap_cur;
    int swap_fd;
    bool swap_dirty, swap_failed;
    uint8_t *swap_buf;
    Chunk *blocks;          /* block storage chunks */
    uint8_t *bpos;
    uint32_t bleft;
    uint32_t dirty_blocks;
    uint32_t temp_seq;
    /* sync */
    Orig *origs;
    uint32_t norigs, ocap;
    uint32_t *orig_by_clus, *orig_by_path;
    Final *finals;
    uint32_t nfinals, fcap;
    uint8_t fat_cache[VFAT_SECTOR];
    uint32_t fat_cache_lba;
};

/* ------------------------------------------------------------------ memory */

static void *zalloc(Vfat *v, size_t n) {
    void *p = v->mem.alloc(n ? n : 1);
    if (p) memset(p, 0, n);
    return p;
}

static char *arena(Vfat *v, size_t n) {
    n = (n + 3u) & ~3u;
    if (n > v->aleft) {
        size_t size = n > ARENA_CHUNK - sizeof(Chunk) ? n + sizeof(Chunk) : ARENA_CHUNK;
        Chunk *c = v->mem.alloc(size);
        if (!c) return NULL;
        c->next = v->chunks;
        v->chunks = c;
        v->apos = (char *)(c + 1);
        v->aleft = size - sizeof(Chunk);
    }
    char *p = v->apos;
    v->apos += n;
    v->aleft -= n;
    return p;
}

static char *strdup_a(Vfat *v, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = arena(v, n);
    if (p) memcpy(p, s, n);
    return p;
}

static bool grow(Vfat *v, void **array, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return true;
    uint32_t ncap = *cap ? *cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    void *p = v->mem.alloc((size_t)ncap * elem);
    if (!p) return false;
    if (*array) {
        memcpy(p, *array, (size_t)*cap * elem);
        v->mem.free(*array);
    }
    memset((char *)p + (size_t)*cap * elem, 0, (size_t)(ncap - *cap) * elem);
    *array = p;
    *cap = ncap;
    return true;
}

/* ------------------------------------------------------------- utilities */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

static size_t cat(char *dst, size_t size, size_t len, const char *s) {
    while (*s && len + 1 < size) dst[len++] = *s++;
    dst[len < size ? len : size - 1] = 0;
    return len;
}

static size_t cat_u(char *dst, size_t size, size_t len, uint32_t n) {
    char tmp[12];
    int i = 11;
    tmp[i] = 0;
    do { tmp[--i] = (char)('0' + n % 10); n /= 10; } while (n);
    return cat(dst, size, len, tmp + i);
}

/* Decode one UTF-8 code point; invalid bytes yield 0xFFFD and advance 1. */
static uint32_t utf8_next(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xe0) == 0xc0) { c &= 0x1f; n = 1; }
    else if ((c & 0xf0) == 0xe0) { c &= 0x0f; n = 2; }
    else if ((c & 0xf8) == 0xf0) { c &= 0x07; n = 3; }
    else { *ps += 1; return 0xfffd; }
    for (int i = 1; i <= n; ++i) {
        if ((s[i] & 0xc0) != 0x80) { *ps += 1; return 0xfffd; }
        c = c << 6 | (s[i] & 0x3f);
    }
    *ps += n + 1;
    if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000) ||
        (n == 1 && c < 0x80) || (n == 2 && c < 0x800) || (n == 3 && c < 0x10000))
        return 0xfffd;
    return c;
}

static size_t utf8_put(char *out, uint32_t c) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f)); return 3;
    }
    out[0] = (char)(0xf0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f)); out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

/* UTF-16 length of a UTF-8 string. */
static uint32_t utf16_len(const char *s) {
    uint32_t n = 0;
    while (*s) n += utf8_next(&s) >= 0x10000 ? 2 : 1;
    return n;
}

static int ascii_casecmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return x - y;
    }
}

/* Make a name acceptable as a FAT long name: no control or reserved
   characters, no trailing dots/spaces, valid UTF-8, at most 255 UTF-16 units. */
static char *display_name(Vfat *v, const char *name) {
    size_t n = strlen(name);
    char *out = arena(v, n * 3 + 8);
    if (!out) return NULL;
    size_t len = 0;
    uint32_t units = 0;
    const char *s = name;
    while (*s) {
        uint32_t c = utf8_next(&s);
        if (c < 0x20 || c == 0xfffd || c == 0x7f || (c < 0x80 && strchr("\"*/:<>?\\|", (int)c))) c = '_';
        uint32_t u = c >= 0x10000 ? 2 : 1;
        if (units + u > 240) break;          /* leave room for a "~N" suffix */
        units += u;
        len += utf8_put(out + len, c);
    }
    while (len && (out[len - 1] == '.' || out[len - 1] == ' ')) --len;
    if (!len) out[len++] = '_';
    out[len] = 0;
    return out;
}

/* ------------------------------------------------------------ tree build */

static uint32_t new_node(Vfat *v, uint32_t parent) {
    if (!grow(v, (void **)&v->nodes, &v->cap, v->nnodes + 1, sizeof(Node))) return NONE;
    uint32_t i = v->nnodes++;
    Node *n = &v->nodes[i];
    memset(n, 0, sizeof(*n));
    n->parent = parent;
    n->child = n->next = n->last_child = NONE;
    if (parent != NONE) {
        Node *p = &v->nodes[parent];
        if (p->last_child == NONE) p->child = i;
        else v->nodes[p->last_child].next = i;
        p->last_child = i;
    }
    return i;
}

/* Calculator path of a node into buf. */
static bool node_path(const Vfat *v, uint32_t i, char *buf, size_t size) {
    if (v->nodes[i].cpath) return cat(buf, size, 0, v->nodes[i].cpath) + 1 < size;
    uint32_t chain[64];
    unsigned depth = 0;
    for (uint32_t k = i; k != ROOT_NODE && k != NONE; k = v->nodes[k].parent) {
        if (depth == 64) return false;
        chain[depth++] = k;
    }
    size_t len = cat(buf, size, 0, v->root);
    while (depth) {
        len = cat(buf, size, len, "/");
        len = cat(buf, size, len, v->nodes[chain[--depth]].name);
    }
    return len + 1 < size;
}

typedef struct { Vfat *v; uint32_t parent; int error; } ListCtx;

static int list_cb(void *user, const char *name, bool is_dir, uint32_t size, uint32_t mtime) {
    ListCtx *l = user;
    Vfat *v = l->v;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !*name) return 0;
    uint32_t i = new_node(v, l->parent);
    if (i == NONE) { l->error = VFAT_NO_MEMORY; return -1; }
    Node *n = &v->nodes[i];
    n->is_dir = is_dir;
    n->size = is_dir ? 0 : size;
    n->mtime = mtime;
    n->name = strdup_a(v, name);
    if (!n->name) { l->error = VFAT_NO_MEMORY; return -1; }
    return 0;
}

static VfatResult scan(Vfat *v) {
    uint32_t root = new_node(v, NONE);
    if (root == NONE) return VFAT_NO_MEMORY;
    v->nodes[root].is_dir = 1;
    v->nodes[root].name = v->nodes[root].disp = v->nodes[root].dpath = "";
    /* Breadth first: nodes are appended, so a cursor walks the queue. */
    for (uint32_t i = 0; i < v->nnodes; ++i) {
        if (!v->nodes[i].is_dir) continue;
        char path[VFAT_PATH_MAX];
        if (!node_path(v, i, path, sizeof(path))) continue;
        ListCtx l = { v, i, 0 };
        if (v->be.list(v->be.ctx, path, list_cb, &l) < 0 && l.error) return (VfatResult)l.error;
        if (l.error) return (VfatResult)l.error;
    }
    return VFAT_OK;
}

/* Unique display names and paths, entry slot counts. */
static VfatResult names(Vfat *v) {
    for (uint32_t i = 1; i < v->nnodes; ++i) {
        Node *n = &v->nodes[i];
        char *d = display_name(v, n->name);
        if (!d) return VFAT_NO_MEMORY;
        /* Case-insensitive duplicates among earlier siblings get a suffix. */
        for (unsigned attempt = 2;; ++attempt) {
            bool clash = false;
            for (uint32_t k = v->nodes[n->parent].child; k != i && k != NONE; k = v->nodes[k].next)
                if (!ascii_casecmp(v->nodes[k].disp, d)) { clash = true; break; }
            if (!clash) break;
            char buf[600];
            const char *base = display_name(v, n->name);
            size_t len = cat(buf, sizeof(buf), 0, base ? base : "_");
            const char *dot = strrchr(buf, '.');
            char ext[300] = "";
            if (dot && dot != buf) { cat(ext, sizeof(ext), 0, dot); len = (size_t)(dot - buf); buf[len] = 0; }
            len = cat(buf, sizeof(buf), len, "~");
            len = cat_u(buf, sizeof(buf), len, attempt);
            cat(buf, sizeof(buf), len, ext);
            d = strdup_a(v, buf);
            if (!d) return VFAT_NO_MEMORY;
        }
        n->disp = d;
        n->slots = (utf16_len(d) + 12) / 13 + 1;
        const char *pp = v->nodes[n->parent].dpath;
        n->dpath = arena(v, strlen(pp) + strlen(d) + 2);
        if (!n->dpath) return VFAT_NO_MEMORY;
        size_t len = cat(n->dpath, (size_t)-1, 0, pp);
        len = cat(n->dpath, (size_t)-1, len, "/");
        cat(n->dpath, (size_t)-1, len, d);
        if (!n->is_dir) ++v->files;
    }
    return VFAT_OK;
}

/* ----------------------------------------------------------------- layout */

static uint32_t dir_bytes(const Vfat *v, uint32_t i) {
    uint32_t slots = i == ROOT_NODE ? 1 : 2;   /* volume label or . and .. */
    for (uint32_t k = v->nodes[i].child; k != NONE; k = v->nodes[k].next) slots += v->nodes[k].slots;
    return slots * 32u;
}

static VfatResult layout(Vfat *v, uint32_t overlay_bytes, uint32_t free_bytes) {
    uint32_t root_bytes = dir_bytes(v, ROOT_NODE);
    v->root_ents = 1024;
    while (v->root_ents * 32u < root_bytes + 128u * 32u) v->root_ents += 512;
    if (v->root_ents > 0xfff0u) return VFAT_TOO_BIG;
    v->root_secs = v->root_ents * 32u / VFAT_SECTOR;
    /* Space the host may fill: overlay minus room for FAT and directory
       updates (metadata writes also land in the overlay). */
    uint32_t usable = overlay_bytes > (512u << 10) ? overlay_bytes - (512u << 10) : 0;
    usable -= usable / 16u;
    if (free_bytes > usable) usable = free_bytes;       /* the rest goes to the scratch file */
    for (v->spc = 8; v->spc <= 64; v->spc *= 2) {
        v->cs = v->spc * VFAT_SECTOR;
        uint32_t used = 0;
        for (uint32_t i = 1; i < v->nnodes; ++i) {
            const Node *n = &v->nodes[i];
            uint32_t bytes = n->is_dir ? dir_bytes(v, i) : n->size;
            used += (bytes + v->cs - 1) / v->cs;
        }
        v->used_end = 2 + used;
        v->free_clusters = usable / v->cs;
        v->clusters = used + v->free_clusters;
        if (v->clusters < 4200) v->clusters = 4200;      /* FAT16 needs >= 4085 */
        if (v->clusters <= 65000) break;
    }
    if (v->clusters > 65000) return VFAT_TOO_BIG;
    v->fat_secs = ((v->clusters + 2) * 2u + VFAT_SECTOR - 1) / VFAT_SECTOR;
    uint32_t meta = 1 + 2 * v->fat_secs + v->root_secs;
    v->rsvd = 1 + (v->spc - meta % v->spc) % v->spc;   /* align data to clusters */
    v->data_start = v->rsvd + 2 * v->fat_secs + v->root_secs;
    v->part_secs = v->data_start + v->clusters * v->spc;
    v->disk_secs = VFAT_PART_START + v->part_secs;
    /* Allocate cluster runs breadth first (directories and files). */
    if (!grow(v, (void **)&v->runs, &(uint32_t){0}, v->nnodes, sizeof(Run))) return VFAT_NO_MEMORY;
    uint32_t next = 2;
    for (uint32_t i = 1; i < v->nnodes; ++i) {
        Node *n = &v->nodes[i];
        uint32_t bytes = n->is_dir ? dir_bytes(v, i) : n->size;
        n->nclus = (bytes + v->cs - 1) / v->cs;
        n->clus = n->nclus ? next : 0;
        if (n->nclus) {
            v->runs[v->nruns].start = next;
            v->runs[v->nruns].count = n->nclus;
            v->runs[v->nruns].node = i;
            ++v->nruns;
        }
        next += n->nclus;
    }
    return VFAT_OK;
}

/* ------------------------------------------------------- directory data */

static void fat_datetime(uint32_t t, uint16_t *date, uint16_t *time) {
    if (t < 315532800u) t = 1767225600u;      /* before 1980: 2026-01-01 */
    uint32_t days = t / 86400u, secs = t % 86400u;
    /* civil_from_days (Howard Hinnant) */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    if (y > 2107) y = 2107;
    *date = (uint16_t)((uint32_t)(y - 1980) << 9 | m << 5 | d);
    *time = (uint16_t)((secs / 3600) << 11 | ((secs / 60) % 60) << 5 | (secs % 60) / 2);
}

static uint8_t sfn_checksum(const uint8_t *sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

static bool sfn_char(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && c < 0x80 && strchr("!#$%&'()-@^_`{}~", (int)c));
}

/* Windows-style "BASIS~N.EXT" alias, unique among the entries in dir[0..len). */
static void make_sfn(const char *name, const uint8_t *dir, uint32_t len, uint8_t out[11]) {
    char base[9] = "", ext[4] = "";
    unsigned nb = 0, ne = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    const char *s = name;
    while (*s && (!dot || s < dot)) {
        uint32_t c = utf8_next(&s);
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == '.') continue;
        if (nb < 8) base[nb++] = sfn_char(c) ? (char)c : '_';
    }
    if (dot) {
        s = dot + 1;
        while (*s) {
            uint32_t c = utf8_next(&s);
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '.') continue;
            if (ne < 3) ext[ne++] = sfn_char(c) ? (char)c : '_';
        }
    }
    if (!nb) base[nb++] = '_';
    for (uint32_t n = 1; n < 1000000; ++n) {
        char tail[9];
        size_t tl = cat_u(tail, sizeof(tail), cat(tail, sizeof(tail), 0, "~"), n);
        unsigned keep = nb + tl > 8 ? 8 - (unsigned)tl : nb;
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, tl);
        memcpy(out + 8, ext, ne);
        bool used = false;
        for (uint32_t off = 0; off < len; off += 32)
            if (dir[off + 11] != 0x0f && !memcmp(dir + off, out, 11)) { used = true; break; }
        if (!used) return;
    }
}

static uint32_t put_entry(uint8_t *dir, uint32_t len, const Node *n, uint32_t clus) {
    uint8_t sfn[11];
    make_sfn(n->disp, dir, len, sfn);
    /* UTF-16 name */
    uint16_t u[260];
    uint32_t nu = 0;
    for (const char *s = n->disp; *s && nu < 255;) {
        uint32_t c = utf8_next(&s);
        if (c >= 0x10000) {
            if (nu + 2 > 255) break;
            c -= 0x10000;
            u[nu++] = (uint16_t)(0xd800 | c >> 10);
            u[nu++] = (uint16_t)(0xdc00 | (c & 0x3ff));
        } else u[nu++] = (uint16_t)c;
    }
    uint32_t lfn = (nu + 12) / 13;
    uint8_t sum = sfn_checksum(sfn);
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (uint32_t k = 0; k < lfn; ++k) {
        uint32_t seq = lfn - k;         /* entries are stored last part first */
        uint8_t *e = dir + len;
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = 0x0f;
        e[13] = sum;
        for (int j = 0; j < 13; ++j) {
            uint32_t idx = (seq - 1) * 13 + (uint32_t)j;
            uint16_t ch = idx < nu ? u[idx] : idx == nu ? 0 : 0xffff;
            put16(e + pos[j], ch);
        }
        len += 32;
    }
    uint8_t *e = dir + len;
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = n->is_dir ? 0x10 : 0x20;
    uint16_t date, time;
    fat_datetime(n->mtime, &date, &time);
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date);
    put16(e + 22, time); put16(e + 24, date);
    put16(e + 26, clus);
    put32(e + 28, n->is_dir ? 0 : n->size);
    return len + 32;
}

static void put_dot(uint8_t *e, const char *name, uint32_t clus, uint32_t mtime) {
    memset(e, 0, 32);
    memset(e, ' ', 11);
    memcpy(e, name, strlen(name));
    e[11] = 0x10;
    uint16_t date, time;
    fat_datetime(mtime, &date, &time);
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date);
    put16(e + 22, time); put16(e + 24, date);
    put16(e + 26, clus);
}

static VfatResult directories(Vfat *v) {
    for (uint32_t i = 0; i < v->nnodes; ++i) {
        Node *n = &v->nodes[i];
        if (!n->is_dir) continue;
        uint32_t size = i == ROOT_NODE ? v->root_secs * VFAT_SECTOR : n->nclus * v->cs;
        uint8_t *d = zalloc(v, size);
        if (!d) return VFAT_NO_MEMORY;
        uint32_t len = 0;
        if (i == ROOT_NODE) {
            memset(d, ' ', 11);
            memcpy(d, "NSPIRE", 6);
            d[11] = 0x08;
            uint16_t date, time;
            fat_datetime(v->be.now ? v->be.now(v->be.ctx) : 0, &date, &time);
            put16(d + 22, time); put16(d + 24, date);
            len = 32;
        } else {
            put_dot(d, ".", n->clus, n->mtime);
            put_dot(d + 32, "..", n->parent == ROOT_NODE ? 0 : v->nodes[n->parent].clus, n->mtime);
            len = 64;
        }
        for (uint32_t k = n->child; k != NONE; k = v->nodes[k].next)
            len = put_entry(d, len, &v->nodes[k], v->nodes[k].clus);
        if (i == ROOT_NODE) { v->root_data = d; v->root_len = len; }
        else { n->dir = d; n->dirlen = len; }
    }
    return VFAT_OK;
}

static int dpath_cmp(const Vfat *v, uint32_t a, uint32_t b) {
    return strcmp(v->nodes[a].dpath, v->nodes[b].dpath);
}

static VfatResult index_paths(Vfat *v) {
    v->by_dpath = zalloc(v, sizeof(uint32_t) * v->nnodes);
    if (!v->by_dpath) return VFAT_NO_MEMORY;
    for (uint32_t i = 0; i < v->nnodes; ++i) v->by_dpath[i] = i;
    /* Shell sort: no libc qsort dependency. */
    for (uint32_t gap = v->nnodes / 2; gap; gap /= 2)
        for (uint32_t i = gap; i < v->nnodes; ++i) {
            uint32_t t = v->by_dpath[i], j = i;
            while (j >= gap && dpath_cmp(v, v->by_dpath[j - gap], t) > 0) {
                v->by_dpath[j] = v->by_dpath[j - gap];
                j -= gap;
            }
            v->by_dpath[j] = t;
        }
    return VFAT_OK;
}


static VfatResult init_origs(Vfat *v);

/* ------------------------------------------------------------ open/close */

VfatResult vfat_open(Vfat **out, const VfatBackend *backend, const VfatAllocator *mem,
                     const char *root, uint32_t overlay_bytes, uint32_t free_bytes) {
    *out = NULL;
    Vfat *v = mem->alloc(sizeof(Vfat));
    if (!v) return VFAT_NO_MEMORY;
    memset(v, 0, sizeof(*v));
    v->be = *backend;
    v->mem = *mem;
    v->fat_cache_lba = NONE;
    cat(v->root, sizeof(v->root), 0, root);
    VfatResult r = scan(v);
    if (r == VFAT_OK) r = names(v);
    bool scratch = v->be.scratch_open && v->be.scratch_io && v->be.scratch_close;
    if (!scratch) free_bytes = 0;
    v->swap_fd = -1;
    v->swap_cur = NONE;
    if (r == VFAT_OK) r = layout(v, overlay_bytes, free_bytes);
    if (r == VFAT_OK) r = directories(v);
    if (r == VFAT_OK) r = index_paths(v);
    if (r == VFAT_OK) r = init_origs(v);
    if (r == VFAT_OK) {
        v->ov_max = overlay_bytes / VFAT_BLOCK;
        /* Blocks the free space can take, plus room for FAT and directory updates. */
        uint32_t want = v->free_clusters * (v->cs / VFAT_BLOCK) + 2u * v->fat_secs / 8u + 512u;
        if (scratch && want > v->ov_max) v->swap_max = want - v->ov_max;
        v->ov_cap = 64;
        while (v->ov_cap < (v->ov_max + v->swap_max) * 2u) v->ov_cap *= 2;
        if (v->swap_max) {
            v->swap_buf = v->mem.alloc(VFAT_BLOCK);
            if (!v->swap_buf) r = VFAT_NO_MEMORY;
        }
        v->ov = zalloc(v, sizeof(OvEntry) * v->ov_cap);
        if (!v->ov) r = VFAT_NO_MEMORY;
    }
    v->volid = (v->be.now ? v->be.now(v->be.ctx) : 0) ^ 0x4e445256u;
    if (r != VFAT_OK) { vfat_close(v); return r; }
    *out = v;
    return VFAT_OK;
}

void vfat_close(Vfat *v) {
    if (!v) return;
    if (v->be.read_done) v->be.read_done(v->be.ctx);
    for (uint32_t i = 1; i < v->nnodes; ++i) if (v->nodes[i].dir) v->mem.free(v->nodes[i].dir);
    if (v->root_data) v->mem.free(v->root_data);
    if (v->nodes) v->mem.free(v->nodes);
    if (v->runs) v->mem.free(v->runs);
    if (v->by_dpath) v->mem.free(v->by_dpath);
    if (v->ov) v->mem.free(v->ov);
    if (v->swap_fd >= 0) v->be.scratch_close(v->be.ctx, v->swap_fd);
    if (v->swap_buf) v->mem.free(v->swap_buf);
    if (v->finals) v->mem.free(v->finals);
    if (v->origs) v->mem.free(v->origs);
    if (v->orig_by_clus) v->mem.free(v->orig_by_clus);
    if (v->orig_by_path) v->mem.free(v->orig_by_path);
    while (v->chunks) { Chunk *c = v->chunks; v->chunks = c->next; v->mem.free(c); }
    while (v->blocks) { Chunk *c = v->blocks; v->blocks = c->next; v->mem.free(c); }
    v->mem.free(v);
}

uint32_t vfat_sectors(const Vfat *v) { return v->disk_secs; }
uint32_t vfat_file_count(const Vfat *v) { return v->files; }
uint32_t vfat_free_bytes(const Vfat *v) { return v->free_clusters * v->cs; }
bool vfat_dirty(const Vfat *v) { return v->dirty_blocks != 0; }
uint32_t vfat_written_blocks(const Vfat *v) { return v->ov_used; }

/* --------------------------------------------------------------- overlay */

static OvEntry *ov_find(const Vfat *v, uint32_t block, bool *found) {
    uint32_t h = (block * 2654435761u) & (v->ov_cap - 1);
    for (;;) {
        OvEntry *e = &v->ov[h];
        if (!e->key) { *found = false; return e; }
        if (e->key == block + 1) { *found = true; return e; }
        h = (h + 1) & (v->ov_cap - 1);
    }
}

/* Is the sector in the overlay? (No scratch file access.) */
static bool ov_has(const Vfat *v, uint32_t lba) {
    if (!v->ov_used) return false;
    bool found;
    OvEntry *e = ov_find(v, lba / 8u, &found);
    return found && (e->mask & (1u << (lba % 8u)));
}

static bool swap_flush(Vfat *v) {
    if (!v->swap_dirty || v->swap_cur == NONE) return true;
    if (v->be.scratch_io(v->be.ctx, v->swap_fd, v->swap_cur * VFAT_BLOCK, v->swap_buf, VFAT_BLOCK, true) < 0) {
        v->swap_failed = true;
        return false;
    }
    if (v->swap_cur == v->swap_stored) ++v->swap_stored;     /* appended */
    v->swap_dirty = false;
    return true;
}

/* The block in a scratch slot, in RAM until another one is needed. */
static uint8_t *swap_block(Vfat *v, uint32_t slot) {
    if (v->swap_cur == slot) return v->swap_buf;
    if (!swap_flush(v)) return NULL;
    v->swap_cur = slot;
    if (slot >= v->swap_stored) memset(v->swap_buf, 0, VFAT_BLOCK);     /* new */
    else if (v->be.scratch_io(v->be.ctx, v->swap_fd, slot * VFAT_BLOCK, v->swap_buf, VFAT_BLOCK, false) < 0) {
        v->swap_failed = true;
        v->swap_cur = NONE;
        return NULL;
    }
    return v->swap_buf;
}

/* Data of an overlay block (valid until the next overlay access). */
static uint8_t *ov_data(Vfat *v, OvEntry *e) {
    return e->swapped ? swap_block(v, e->u.slot) : e->u.data;
}

static const uint8_t *ov_sector(Vfat *v, uint32_t lba) {
    if (!v->ov_used) return NULL;
    bool found;
    OvEntry *e = ov_find(v, lba / 8u, &found);
    if (!found || !(e->mask & (1u << (lba % 8u)))) return NULL;
    static const uint8_t zeros[VFAT_SECTOR];
    const uint8_t *d = ov_data(v, e);
    return d ? d + (lba % 8u) * VFAT_SECTOR : zeros;    /* scratch file failed: never an I/O error */
}

static uint8_t *block_alloc(Vfat *v) {
    if (v->bleft < VFAT_BLOCK) {
        /* 64 blocks per chunk keeps allocations few and moderate. */
        uint32_t size = 64u * VFAT_BLOCK + 64u;
        Chunk *c = v->mem.alloc(size);
        if (!c) return NULL;
        c->next = v->blocks;
        v->blocks = c;
        uintptr_t p = ((uintptr_t)(c + 1) + 31u) & ~(uintptr_t)31u;
        v->bpos = (uint8_t *)p;
        v->bleft = (uint32_t)((uintptr_t)c + size - p);
    }
    uint8_t *p = v->bpos;
    v->bpos += VFAT_BLOCK;
    v->bleft -= VFAT_BLOCK;
    return p;
}

/* Store one sector in the overlay: in RAM while there is room, then in the
   scratch file. */
static VfatResult ov_put(Vfat *v, uint32_t lba, const uint8_t *sector, OvEntry **out) {
    bool found;
    OvEntry *e = ov_find(v, lba / 8u, &found);
    if (!found) {
        uint8_t *data = v->ram_used < v->ov_max ? block_alloc(v) : NULL;
        uint32_t slot = 0;
        if (!data) {
            if (v->swap_failed || v->swap_used >= v->swap_max) return VFAT_NO_SPACE;
            if (v->swap_fd < 0) {
                v->swap_fd = v->be.scratch_open(v->be.ctx);
                if (v->swap_fd < 0) { v->swap_failed = true; return VFAT_NO_SPACE; }
            }
            slot = v->swap_used;
            if (!swap_block(v, slot)) return VFAT_NO_SPACE;
            v->swap_dirty = true;                   /* reaches the file even if left empty */
            ++v->swap_used;
        } else ++v->ram_used;
        e->key = lba / 8u + 1;
        e->mask = e->dirty = 0;
        e->swapped = data == NULL;
        if (data) e->u.data = data; else e->u.slot = slot;
        ++v->ov_used;
    }
    uint8_t *d = ov_data(v, e);
    if (!d) return VFAT_NO_SPACE;
    memcpy(d + (lba % 8u) * VFAT_SECTOR, sector, VFAT_SECTOR);
    if (e->swapped) v->swap_dirty = true;
    e->mask |= (uint8_t)(1u << (lba % 8u));
    *out = e;
    return VFAT_OK;
}

VfatResult vfat_write(Vfat *v, uint32_t lba, uint32_t count, const uint8_t *buf) {
    if (lba >= v->disk_secs || count > v->disk_secs - lba) return VFAT_IO;
    for (uint32_t i = 0; i < count; ++i, ++lba, buf += VFAT_SECTOR) {
        OvEntry *e;
        VfatResult r = ov_put(v, lba, buf, &e);
        if (r != VFAT_OK) return r;
        if (!e->dirty) ++v->dirty_blocks;
        e->dirty |= (uint8_t)(1u << (lba % 8u));
    }
    return VFAT_OK;
}

/* ------------------------------------------------------- base sectors */

static const Run *find_run(const Vfat *v, uint32_t clus) {
    uint32_t lo = 0, hi = v->nruns;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const Run *r = &v->runs[mid];
        if (clus < r->start) hi = mid;
        else if (clus >= r->start + r->count) lo = mid + 1;
        else return r;
    }
    return NULL;
}

static uint32_t base_fat(const Vfat *v, uint32_t c) {
    if (c == 0) return 0xfff8;
    if (c == 1) return 0xffff;
    if (c >= v->clusters + 2) return 0;
    if (c < v->used_end) {
        const Run *r = find_run(v, c);
        if (!r) return 0;
        return c + 1 == r->start + r->count ? EOC16 : c + 1;
    }
    if (c < v->used_end + v->free_clusters) return 0;
    return BAD16;               /* padding to keep the volume FAT16 */
}

static void mbr(const Vfat *v, uint8_t *s) {
    uint8_t *p = s + 446;
    p[0] = 0x00;
    p[1] = 0xfe; p[2] = 0xff; p[3] = 0xff;
    p[4] = 0x0e;                /* FAT16, LBA */
    p[5] = 0xfe; p[6] = 0xff; p[7] = 0xff;
    put32(p + 8, VFAT_PART_START);
    put32(p + 12, v->part_secs);
    put32(s + 440, v->volid);
    s[510] = 0x55; s[511] = 0xaa;
}

static void boot(const Vfat *v, uint8_t *s) {
    s[0] = 0xeb; s[1] = 0x3c; s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    put16(s + 11, VFAT_SECTOR);
    s[13] = (uint8_t)v->spc;
    put16(s + 14, v->rsvd);
    s[16] = 2;
    put16(s + 17, v->root_ents);
    if (v->part_secs < 0x10000) put16(s + 19, v->part_secs);
    s[21] = 0xf8;
    put16(s + 22, v->fat_secs);
    put16(s + 24, 63);
    put16(s + 26, 255);
    put32(s + 28, VFAT_PART_START);
    if (v->part_secs >= 0x10000) put32(s + 32, v->part_secs);
    s[36] = 0x80;
    s[38] = 0x29;
    put32(s + 39, v->volid);
    memcpy(s + 43, "NSPIRE     ", 11);
    memcpy(s + 54, "FAT16   ", 8);
    s[510] = 0x55; s[511] = 0xaa;
}

/* Fill one base (unmodified) sector, except file data. Returns the node
   whose file data covers the sector (offset in *off) or NONE. */
static uint32_t base_sector(const Vfat *v, uint32_t lba, uint8_t *s, uint32_t *off) {
    memset(s, 0, VFAT_SECTOR);
    if (lba == 0) { mbr(v, s); return NONE; }
    if (lba < VFAT_PART_START || lba >= v->disk_secs) return NONE;
    uint32_t rel = lba - VFAT_PART_START;
    if (rel == 0) { boot(v, s); return NONE; }
    if (rel < v->rsvd) return NONE;
    if (rel < v->rsvd + 2 * v->fat_secs) {
        uint32_t fs = (rel - v->rsvd) % v->fat_secs;
        for (uint32_t k = 0; k < VFAT_SECTOR / 2; ++k) put16(s + 2 * k, base_fat(v, fs * 256 + k));
        return NONE;
    }
    if (rel < v->data_start) {
        uint32_t o = (rel - v->rsvd - 2 * v->fat_secs) * VFAT_SECTOR;
        if (o < v->root_len) memcpy(s, v->root_data + o, v->root_len - o < VFAT_SECTOR ? v->root_len - o : VFAT_SECTOR);
        return NONE;
    }
    uint32_t clus = 2 + (rel - v->data_start) / v->spc;
    const Run *r = clus < v->used_end ? find_run(v, clus) : NULL;
    if (!r) return NONE;
    const Node *n = &v->nodes[r->node];
    uint32_t o = (clus - r->start) * v->cs + ((rel - v->data_start) % v->spc) * VFAT_SECTOR;
    if (n->is_dir) {
        if (o < n->dirlen) memcpy(s, n->dir + o, n->dirlen - o < VFAT_SECTOR ? n->dirlen - o : VFAT_SECTOR);
        return NONE;
    }
    if (o >= n->size || n->gone) return NONE;
    *off = o;
    return r->node;
}

VfatResult vfat_read(Vfat *v, uint32_t lba, uint32_t count, uint8_t *buf) {
    if (lba >= v->disk_secs || count > v->disk_secs - lba) return VFAT_IO;
    while (count) {
        const uint8_t *o = ov_sector(v, lba);
        if (o) {
            memcpy(buf, o, VFAT_SECTOR);
            ++lba; --count; buf += VFAT_SECTOR;
            continue;
        }
        uint32_t off = 0;
        uint32_t node = base_sector(v, lba, buf, &off);
        if (node == NONE) { ++lba; --count; buf += VFAT_SECTOR; continue; }
        /* Batch consecutive sectors of the same file without overlay data. */
        const Node *n = &v->nodes[node];
        uint32_t run = 1;
        while (run < count && off + (run + 1) * VFAT_SECTOR <= n->nclus * v->cs &&
               off + run * VFAT_SECTOR < n->size && !ov_has(v, lba + run)) ++run;
        uint32_t want = run * VFAT_SECTOR;
        if (want > n->size - off) want = n->size - off;
        char path[VFAT_PATH_MAX];
        if (!node_path(v, node, path, sizeof(path))) return VFAT_IO;
        int got = v->be.read(v->be.ctx, path, off, buf, want);
        if (got < 0) got = 0;   /* changed behind our back: zeros, never an I/O error */
        if ((uint32_t)got < run * VFAT_SECTOR) memset(buf + got, 0, run * VFAT_SECTOR - (uint32_t)got);
        lba += run; count -= run; buf += run * VFAT_SECTOR;
    }
    return VFAT_OK;
}

/* =================================================================== sync */

static Vfat *sync_vfat;

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !sync_vfat) return RES_PARERR;
    return vfat_read(sync_vfat, (uint32_t)sector, count, buff) == VFAT_OK ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)buff;
    if (pdrv) return RES_PARERR;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}

static uint32_t fat_next(Vfat *v, uint32_t c) {
    uint32_t lba = VFAT_PART_START + v->rsvd + c / 256u;
    if (lba != v->fat_cache_lba) {
        if (vfat_read(v, lba, 1, v->fat_cache) != VFAT_OK) return 0;
        v->fat_cache_lba = lba;
    }
    return get16(v->fat_cache + (c % 256u) * 2);
}

static uint32_t clus_lba(const Vfat *v, uint32_t c) {
    return VFAT_PART_START + v->data_start + (c - 2) * v->spc;
}

static uint32_t mix(uint32_t h, uint32_t c) { return (h ^ c) * 16777619u; }

/* Hash of a cluster chain; also reports whether any data sector up to size
   was written by the host since the last sync. */
static uint32_t chain_hash(Vfat *v, uint32_t c, uint32_t size, uint8_t *touched) {
    uint32_t h = 2166136261u, secs = (size + VFAT_SECTOR - 1) / VFAT_SECTOR;
    if (touched) *touched = 0;
    for (uint32_t k = 0; c >= 2 && c < v->clusters + 2 && k <= v->clusters; ++k) {
        h = mix(h, c);
        if (touched && secs) {
            for (uint32_t s = 0; s < v->spc && secs; ++s, --secs) {
                uint32_t lba = clus_lba(v, c) + s;
                bool found;
                OvEntry *e = v->ov_used ? ov_find(v, lba / 8u, &found) : NULL;
                if (e && found && (e->dirty & (1u << (lba % 8u)))) *touched = 1;
            }
        }
        uint32_t next = fat_next(v, c);
        if (next >= 0xfff8) break;
        c = next;
    }
    return h;
}

static uint32_t run_hash(uint32_t start, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < n; ++k) h = mix(h, start + k);
    return h;
}

static int orig_path_cmp(const Vfat *v, uint32_t a, uint32_t b) {
    return strcmp(v->origs[a].dpath, v->origs[b].dpath);
}

static void sort_idx(Vfat *v, uint32_t *idx, uint32_t n, bool by_path) {
    for (uint32_t gap = n / 2; gap; gap /= 2)
        for (uint32_t i = gap; i < n; ++i) {
            uint32_t t = idx[i], j = i;
            while (j >= gap && (by_path ? orig_path_cmp(v, idx[j - gap], t) > 0 :
                                v->origs[idx[j - gap]].sclust > v->origs[t].sclust)) {
                idx[j] = idx[j - gap];
                j -= gap;
            }
            idx[j] = t;
        }
}

static VfatResult index_origs(Vfat *v) {
    if (v->orig_by_clus) v->mem.free(v->orig_by_clus);
    if (v->orig_by_path) v->mem.free(v->orig_by_path);
    v->orig_by_clus = zalloc(v, sizeof(uint32_t) * (v->norigs + 1));
    v->orig_by_path = zalloc(v, sizeof(uint32_t) * (v->norigs + 1));
    if (!v->orig_by_clus || !v->orig_by_path) return VFAT_NO_MEMORY;
    for (uint32_t i = 0; i < v->norigs; ++i) v->orig_by_clus[i] = v->orig_by_path[i] = i;
    sort_idx(v, v->orig_by_clus, v->norigs, false);
    sort_idx(v, v->orig_by_path, v->norigs, true);
    v->files = 0;
    for (uint32_t i = 0; i < v->norigs; ++i) v->files += !v->origs[i].is_dir;
    return VFAT_OK;
}

static uint32_t orig_by_path(const Vfat *v, const char *path) {
    uint32_t lo = 0, hi = v->norigs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(v->origs[v->orig_by_path[mid]].dpath, path);
        if (!c) return v->orig_by_path[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NONE;
}

static uint32_t orig_by_clus(const Vfat *v, uint32_t c) {
    uint32_t lo = 0, hi = v->norigs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t x = v->origs[v->orig_by_clus[mid]].sclust;
        if (x == c) {
            /* first file with this cluster (directories never share one) */
            while (mid && v->origs[v->orig_by_clus[mid - 1]].sclust == c) --mid;
            for (; mid < v->norigs && v->origs[v->orig_by_clus[mid]].sclust == c; ++mid)
                if (!v->origs[v->orig_by_clus[mid]].is_dir) return v->orig_by_clus[mid];
            return NONE;
        }
        if (x < c) lo = mid + 1; else hi = mid;
    }
    return NONE;
}

/* Initial state: the snapshot as built. */
static VfatResult init_origs(Vfat *v) {
    if (!grow(v, (void **)&v->origs, &v->ocap, v->nnodes, sizeof(Orig))) return VFAT_NO_MEMORY;
    v->norigs = 0;
    for (uint32_t i = 1; i < v->nnodes; ++i) {
        Node *n = &v->nodes[i];
        Orig *o = &v->origs[v->norigs++];
        char path[VFAT_PATH_MAX];
        memset(o, 0, sizeof(*o));
        if (!node_path(v, i, path, sizeof(path))) return VFAT_TOO_BIG;
        o->dpath = n->dpath;
        o->cpath = strdup_a(v, path);
        if (!o->cpath) return VFAT_NO_MEMORY;
        o->is_dir = n->is_dir;
        o->size = n->size;
        o->sclust = n->is_dir || n->size ? n->clus : 0;
        o->chash = o->sclust ? run_hash(n->clus, n->nclus) : 0;
        o->node = i;
    }
    return index_origs(v);
}

static bool junk(const char *name) {
    static const char *const names[] = {
        ".Trashes", ".fseventsd", ".Spotlight-V100", ".TemporaryItems", ".DS_Store",
        "System Volume Information", "$RECYCLE.BIN", ".Trash-1000", "Thumbs.db",
        "desktop.ini", ".metadata_never_index", ".apdisk", NULL
    };
    if (name[0] == '.' && name[1] == '_') return true;   /* AppleDouble */
    for (int i = 0; names[i]; ++i) if (!ascii_casecmp(name, names[i])) return true;
    return false;
}

static VfatResult add_final(Vfat *v, const char *path, const FILINFO *fi, uint32_t sclust) {
    if (!grow(v, (void **)&v->finals, &v->fcap, v->nfinals + 1, sizeof(Final))) return VFAT_NO_MEMORY;
    Final *f = &v->finals[v->nfinals];
    memset(f, 0, sizeof(*f));
    f->path = strdup_a(v, path);
    if (!f->path) return VFAT_NO_MEMORY;
    f->is_dir = (fi->fattrib & AM_DIR) != 0;
    f->size = f->is_dir ? 0 : (uint32_t)fi->fsize;
    f->sclust = sclust;
    f->orig = NONE;
    if (!f->is_dir && sclust) f->chash = chain_hash(v, sclust, f->size, &f->touched);
    ++v->nfinals;
    return VFAT_OK;
}

/* Walk the volume as the host left it (breadth first). */
static VfatResult walk(Vfat *v) {
    uint32_t cursor = 0;
    DIR dir;
    FILINFO fi;
    char path[VFAT_PATH_MAX], child[VFAT_PATH_MAX];
    for (int32_t current = -1;;) {
        const char *base = current < 0 ? "" : v->finals[current].path;
        cat(path, sizeof(path), 0, current < 0 ? "/" : base);
        if (f_opendir(&dir, path) != FR_OK) return VFAT_BAD_FS;
        for (;;) {
            if (f_readdir(&dir, &fi) != FR_OK) { f_closedir(&dir); return VFAT_BAD_FS; }
            if (!fi.fname[0]) break;
            size_t len = cat(child, sizeof(child), 0, base);
            len = cat(child, sizeof(child), len, "/");
            len = cat(child, sizeof(child), len, fi.fname);
            if (len + 1 >= sizeof(child)) continue;
            if (junk(fi.fname) && orig_by_path(v, child) == NONE) continue;
            uint32_t sclust = 0;
            if ((fi.fattrib & AM_DIR) || fi.fsize) {
                if (fi.fattrib & AM_DIR) {
                    DIR sub;
                    if (f_opendir(&sub, child) != FR_OK) { f_closedir(&dir); return VFAT_BAD_FS; }
                    sclust = sub.obj.sclust;
                    f_closedir(&sub);
                } else {
                    FIL fil;
                    if (f_open(&fil, child, FA_READ) != FR_OK) { f_closedir(&dir); return VFAT_BAD_FS; }
                    sclust = fil.obj.sclust;
                    f_close(&fil);
                }
            }
            VfatResult r = add_final(v, child, &fi, sclust);
            if (r != VFAT_OK) { f_closedir(&dir); return r; }
        }
        f_closedir(&dir);
        while (cursor < v->nfinals && !v->finals[cursor].is_dir) ++cursor;
        if (cursor >= v->nfinals) break;
        current = (int32_t)cursor++;
    }
    return VFAT_OK;
}

static void report_error(VfatReport *rep, const char *what, const char *path) {
    ++rep->errors;
    if (!rep->first_error[0]) {
        size_t len = cat(rep->first_error, sizeof(rep->first_error), 0, what);
        len = cat(rep->first_error, sizeof(rep->first_error), len, ": ");
        cat(rep->first_error, sizeof(rep->first_error), len, path);
    }
}

/* Calculator path for a display path: existing folders keep their calculator
   names, new components use the host's names. */
static bool calc_path(Vfat *v, const char *dpath, char *out, size_t size) {
    char buf[VFAT_PATH_MAX];
    cat(buf, sizeof(buf), 0, dpath);
    size_t len = strlen(buf);
    for (;;) {
        if (!len) {
            size_t o = cat(out, size, 0, v->root);
            return cat(out, size, o, dpath) + 1 < size;
        }
        uint32_t o = orig_by_path(v, buf);
        if (o != NONE && (v->origs[o].is_dir || len == strlen(dpath))) {
            size_t n = cat(out, size, 0, v->origs[o].cpath);
            return cat(out, size, n, dpath + len) + 1 < size;
        }
        char *slash = strrchr(buf, '/');
        if (!slash || slash == buf) { len = 0; buf[0] = 0; continue; }
        *slash = 0;
        len = (size_t)(slash - buf);
    }
}

static void temp_path(const Vfat *v, uint32_t id, char *out, size_t size) {
    size_t len = cat(out, size, 0, v->root);
    len = cat(out, size, len, "/~ndrive");
    len = cat_u(out, size, len, v->volid % 10000u);
    len = cat(out, size, len, "_");
    len = cat_u(out, size, len, id);
    cat(out, size, len, ".tmp");
}

static int depth(const char *p) {
    int d = 0;
    for (; *p; ++p) d += *p == '/';
    return d;
}

/* Keep the host's view of a rewritten file independent of the calculator
   files it was built from (they may be moved or deleted next). */
static VfatResult materialize(Vfat *v, const Final *f) {
    uint32_t secs = (f->size + VFAT_SECTOR - 1) / VFAT_SECTOR;
    uint8_t s[VFAT_SECTOR];
    for (uint32_t c = f->sclust, k = 0; secs && c >= 2 && c < v->clusters + 2 && k <= v->clusters; ++k) {
        for (uint32_t i = 0; i < v->spc && secs; ++i, --secs) {
            uint32_t lba = clus_lba(v, c) + i;
            if (ov_has(v, lba)) continue;
            uint32_t off = 0;
            uint32_t node = base_sector(v, lba, s, &off);
            if (node == NONE) continue;             /* reads as zeros anyway */
            if (vfat_read(v, lba, 1, s) != VFAT_OK) return VFAT_IO;
            OvEntry *e;
            VfatResult r = ov_put(v, lba, s, &e);
            if (r != VFAT_OK) return r;
        }
        uint32_t next = fat_next(v, c);
        if (next >= 0xfff8) break;
        c = next;
    }
    return VFAT_OK;
}

static VfatResult copy_new(Vfat *v, Final *f, const char *tmp, uint8_t *buf) {
    FIL fil;
    if (f->size && f_open(&fil, f->path, FA_READ) != FR_OK) return VFAT_BAD_FS;
    int h = v->be.create(v->be.ctx, tmp);
    if (h < 0) { if (f->size) f_close(&fil); return VFAT_IO; }
    VfatResult r = VFAT_OK;
    for (uint32_t done = 0; done < f->size;) {
        UINT got = 0;
        UINT want = f->size - done > COPY_CHUNK ? COPY_CHUNK : f->size - done;
        if (f_read(&fil, buf, want, &got) != FR_OK || got != want) { r = VFAT_BAD_FS; break; }
        if (v->be.write(v->be.ctx, h, buf, got) != (int)got) { r = VFAT_IO; break; }
        done += got;
    }
    if (f->size) f_close(&fil);
    if (v->be.close(v->be.ctx, h) < 0 && r == VFAT_OK) r = VFAT_IO;
    if (r != VFAT_OK) v->be.unlink(v->be.ctx, tmp);
    return r;
}

static void clear_dirty(Vfat *v) {
    for (uint32_t i = 0; i < v->ov_cap; ++i) v->ov[i].dirty = 0;
    v->dirty_blocks = 0;
}

/* After applying: the calculator now holds the host's view. */
static VfatResult rebase(Vfat *v) {
    Orig *origs = zalloc(v, sizeof(Orig) * (v->nfinals + 1));
    if (!origs) return VFAT_NO_MEMORY;
    uint32_t n = 0;
    for (uint32_t i = 0; i < v->nfinals; ++i) {
        Final *f = &v->finals[i];
        if (f->failed && !f->is_dir) continue;
        if (!f->cpath) continue;
        Orig *o = &origs[n++];
        o->dpath = f->path;
        o->cpath = f->cpath;
        o->sclust = f->sclust;
        o->size = f->size;
        o->chash = f->chash;
        o->is_dir = f->is_dir;
        o->node = f->orig != NONE ? v->origs[f->orig].node : NONE;
    }
    v->mem.free(v->origs);
    v->origs = origs;
    v->norigs = n;
    v->ocap = v->nfinals + 1;
    return index_origs(v);
}

VfatResult vfat_sync(Vfat *v, const VfatSyncHooks *hooks, VfatReport *rep) {
    static FATFS fs;
    VfatReport dummy;
    if (!rep) rep = &dummy;
    memset(rep, 0, sizeof(*rep));
    if (!vfat_dirty(v)) return VFAT_OK;
    if (!v->origs && init_origs(v) != VFAT_OK) return VFAT_NO_MEMORY;
    sync_vfat = v;
    v->fat_cache_lba = NONE;
    v->nfinals = 0;
    for (uint32_t i = 0; i < v->norigs; ++i) v->origs[i].matched = 0;
    if (f_mount(&fs, "", 1) != FR_OK) { sync_vfat = NULL; return VFAT_BAD_FS; }
    VfatResult r = walk(v);
    if (r != VFAT_OK) { f_unmount(""); sync_vfat = NULL; return r; }

    /* Classify. */
    unsigned new_files = 0, moves = 0, deletes = 0;
    for (uint32_t i = 0; i < v->nfinals; ++i) {
        Final *f = &v->finals[i];
        if (f->is_dir) {
            uint32_t o = orig_by_path(v, f->path);
            if (o != NONE && v->origs[o].is_dir) { f->orig = o; f->cpath = v->origs[o].cpath; v->origs[o].matched = 1; }
            continue;
        }
        uint32_t o = NONE;
        if (f->sclust && !f->touched) {
            o = orig_by_clus(v, f->sclust);
            if (o != NONE && (v->origs[o].matched || v->origs[o].size != f->size ||
                              v->origs[o].chash != f->chash)) o = NONE;
        } else if (!f->sclust) {
            o = orig_by_path(v, f->path);       /* empty file kept in place */
            if (o != NONE && (v->origs[o].is_dir || v->origs[o].size || v->origs[o].matched)) o = NONE;
        }
        f->orig = o;
        if (o != NONE) {
            v->origs[o].matched = 1;
            if (strcmp(v->origs[o].dpath, f->path)) ++moves;
            else f->cpath = v->origs[o].cpath;
        } else ++new_files;
    }
    for (uint32_t i = 0; i < v->norigs; ++i)
        if (!v->origs[i].is_dir && !v->origs[i].matched) ++deletes;
    if (deletes && hooks && hooks->confirm_deletes && (deletes >= 20 || deletes * 2 > v->files) &&
        !hooks->confirm_deletes(hooks->user, deletes, v->files)) {
        f_unmount("");
        sync_vfat = NULL;
        return VFAT_CANCELLED;
    }
    unsigned total = new_files + moves + deletes, done = 0;
    char tmp[VFAT_PATH_MAX], dst[VFAT_PATH_MAX], src[VFAT_PATH_MAX];
    #define PROGRESS(name) do { if (hooks && hooks->progress) hooks->progress(hooks->user, ++done, total, name); } while (0)

    /* 1. Stage new and changed files under temporary names, and keep the
          host's view of them in RAM. */
    uint8_t *buf = v->mem.alloc(COPY_CHUNK);
    if (!buf) { f_unmount(""); sync_vfat = NULL; return VFAT_NO_MEMORY; }
    for (uint32_t i = 0; i < v->nfinals; ++i) {
        Final *f = &v->finals[i];
        if (f->is_dir || f->orig != NONE) continue;
        const char *slash = strrchr(f->path, '/');
        if (junk(slash ? slash + 1 : f->path)) { ++rep->skipped; f->failed = 1; continue; }
        PROGRESS(f->path);
        f->temp = ++v->temp_seq;
        temp_path(v, f->temp, tmp, sizeof(tmp));
        VfatResult cr = copy_new(v, f, tmp, buf);
        if (cr == VFAT_OK && f->sclust) cr = materialize(v, f);
        if (cr != VFAT_OK) {
            report_error(rep, cr == VFAT_IO ? "Cannot write" : cr == VFAT_NO_SPACE ? "Buffer full" : "Unreadable", f->path);
            v->be.unlink(v->be.ctx, tmp);
            f->failed = 1;
            f->temp = 0;
        }
    }
    v->mem.free(buf);
    f_unmount("");
    sync_vfat = NULL;
    if (v->be.read_done) v->be.read_done(v->be.ctx);

    /* 2. Move renamed originals out of the way. */
    for (uint32_t i = 0; i < v->nfinals; ++i) {
        Final *f = &v->finals[i];
        if (f->is_dir || f->orig == NONE || f->cpath) continue;
        PROGRESS(f->path);
        f->temp = ++v->temp_seq;
        temp_path(v, f->temp, tmp, sizeof(tmp));
        if (v->be.rename(v->be.ctx, v->origs[f->orig].cpath, tmp) < 0) {
            report_error(rep, "Cannot move", v->origs[f->orig].dpath);
            f->temp = 0;
            f->failed = 1;
            f->cpath = v->origs[f->orig].cpath;     /* stays where it was */
        }
    }

    /* 3. Delete what the host removed or replaced (keeping any whose
          replacement could not be written). */
    for (uint32_t i = 0; i < v->norigs; ++i) {
        Orig *o = &v->origs[i];
        if (o->is_dir || o->matched) continue;
        bool keep = false;
        for (uint32_t k = 0; k < v->nfinals; ++k)
            if (v->finals[k].failed && !v->finals[k].is_dir && !strcmp(v->finals[k].path, o->dpath)) { keep = true; break; }
        if (keep) { ++rep->skipped; continue; }
        PROGRESS(o->dpath);
        if (v->be.unlink(v->be.ctx, o->cpath) < 0) report_error(rep, "Cannot delete", o->dpath);
        else {
            ++rep->files_deleted;
            if (o->node != NONE) v->nodes[o->node].gone = 1;
        }
    }

    /* 4. Remove folders the host removed, deepest first. */
    for (int d = 64; d >= 1; --d)
        for (uint32_t i = 0; i < v->norigs; ++i) {
            Orig *o = &v->origs[i];
            if (!o->is_dir || o->matched || depth(o->dpath) != d) continue;
            if (v->be.rmdir(v->be.ctx, o->cpath) < 0) report_error(rep, "Cannot remove folder", o->dpath);
            else ++rep->dirs_removed;
        }

    /* 5. Create new folders, parents first. */
    for (int d = 1; d <= 64; ++d)
        for (uint32_t i = 0; i < v->nfinals; ++i) {
            Final *f = &v->finals[i];
            if (!f->is_dir || f->orig != NONE || depth(f->path) != d) continue;
            if (!calc_path(v, f->path, dst, sizeof(dst)) || v->be.mkdir(v->be.ctx, dst) < 0) {
                report_error(rep, "Cannot create folder", f->path);
                continue;
            }
            f->cpath = strdup_a(v, dst);
            /* Children resolve through this folder from now on. */
            if (f->cpath && grow(v, (void **)&v->origs, &v->ocap, v->norigs + 1, sizeof(Orig))) {
                Orig *o = &v->origs[v->norigs++];
                memset(o, 0, sizeof(*o));
                o->dpath = f->path;
                o->cpath = f->cpath;
                o->is_dir = 1;
                o->matched = 1;
                o->node = NONE;
                index_origs(v);
            }
            ++rep->dirs_created;
        }

    /* 6. Put staged files in place. */
    for (uint32_t i = 0; i < v->nfinals; ++i) {
        Final *f = &v->finals[i];
        if (f->is_dir || !f->temp) continue;
        temp_path(v, f->temp, tmp, sizeof(tmp));
        if (!calc_path(v, f->path, dst, sizeof(dst))) { report_error(rep, "Path too long", f->path); f->failed = 1; continue; }
        v->be.unlink(v->be.ctx, dst);           /* rename keeps existing targets */
        if (v->be.rename(v->be.ctx, tmp, dst) < 0) {
            report_error(rep, "Cannot place", f->path);
            f->failed = 1;
            continue;
        }
        f->cpath = strdup_a(v, dst);
        if (f->orig == NONE) {
            size_t n = strlen(dst);
            bool tns = n > 4 && !ascii_casecmp(dst + n - 4, ".tns");
            size_t m = strlen(rep->last_written);
            bool had_tns = m > 4 && !ascii_casecmp(rep->last_written + m - 4, ".tns");
            if (tns || !had_tns) cat(rep->last_written, sizeof(rep->last_written), 0, dst);
        }
        if (f->orig != NONE) {
            ++rep->files_moved;
            uint32_t node = v->origs[f->orig].node;
            if (node != NONE) v->nodes[node].cpath = f->cpath;   /* base data follows */
        } else ++rep->files_written;
    }
    #undef PROGRESS
    (void)src;
    clear_dirty(v);
    return rebase(v);
}
