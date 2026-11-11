/* nDrive - MTP responder. Copyright (C) 2026 Rodrigo. GPL v3+. */
#include "mtp.h"

#include <string.h>

#define STORAGE_ID 0x00010001u
#define ROOT 0u                             /* internal handle of "/" */

#define FMT_UNDEFINED 0x3000u
#define FMT_ASSOCIATION 0x3001u

enum {
    OP_GET_DEVICE_INFO = 0x1001, OP_OPEN_SESSION, OP_CLOSE_SESSION, OP_GET_STORAGE_IDS,
    OP_GET_STORAGE_INFO, OP_GET_NUM_OBJECTS, OP_GET_OBJECT_HANDLES, OP_GET_OBJECT_INFO,
    OP_GET_OBJECT, OP_GET_THUMB, OP_DELETE_OBJECT, OP_SEND_OBJECT_INFO, OP_SEND_OBJECT,
    OP_MOVE_OBJECT = 0x1019, OP_GET_PARTIAL_OBJECT = 0x101b,
    OP_GET_OBJECT_PROPS_SUPPORTED = 0x9801, OP_GET_OBJECT_PROP_DESC, OP_GET_OBJECT_PROP_VALUE,
    OP_SET_OBJECT_PROP_VALUE, OP_GET_OBJECT_REFERENCES = 0x9810, OP_SET_OBJECT_REFERENCES
};

enum {
    RC_OK = 0x2001, RC_GENERAL_ERROR, RC_SESSION_NOT_OPEN, RC_INVALID_TID, RC_OP_NOT_SUPPORTED,
    RC_PARAM_NOT_SUPPORTED, RC_INCOMPLETE, RC_INVALID_STORAGE, RC_INVALID_HANDLE,
    RC_STORE_FULL = 0x200c, RC_ACCESS_DENIED = 0x200f, RC_NO_THUMBNAIL = 0x2010,
    RC_NO_VALID_OBJECT_INFO = 0x2015, RC_INVALID_PARENT = 0x201a, RC_INVALID_PARAMETER = 0x201d,
    RC_SESSION_ALREADY_OPEN = 0x201e, RC_INVALID_PROP_CODE = 0xa801
};

enum {
    PROP_STORAGE = 0xdc01, PROP_FORMAT, PROP_PROTECTION, PROP_SIZE, PROP_FILENAME = 0xdc07,
    PROP_MODIFIED = 0xdc09, PROP_PARENT = 0xdc0b, PROP_PUID = 0xdc41, PROP_NAME = 0xdc44
};

static const uint16_t operations[] = {
    OP_GET_DEVICE_INFO, OP_OPEN_SESSION, OP_CLOSE_SESSION, OP_GET_STORAGE_IDS, OP_GET_STORAGE_INFO,
    OP_GET_NUM_OBJECTS, OP_GET_OBJECT_HANDLES, OP_GET_OBJECT_INFO, OP_GET_OBJECT, OP_DELETE_OBJECT,
    OP_SEND_OBJECT_INFO, OP_SEND_OBJECT, OP_MOVE_OBJECT, OP_GET_PARTIAL_OBJECT,
    OP_GET_OBJECT_PROPS_SUPPORTED, OP_GET_OBJECT_PROP_DESC, OP_GET_OBJECT_PROP_VALUE,
    OP_SET_OBJECT_PROP_VALUE, OP_GET_OBJECT_REFERENCES, OP_SET_OBJECT_REFERENCES
};

static const uint16_t object_props[] = {
    PROP_STORAGE, PROP_FORMAT, PROP_PROTECTION, PROP_SIZE, PROP_FILENAME, PROP_MODIFIED,
    PROP_PARENT, PROP_PUID, PROP_NAME
};

typedef struct {
    uint32_t parent;            /* handle, ROOT for top level */
    char *name;
    uint32_t size, mtime;
    uint8_t dir, dead, seen;
} Obj;

typedef struct {
    uint8_t *p;
    uint32_t n, cap;
    bool fail;
} Buf;

enum { ST_IDLE, ST_DATA_OUT };
enum { TX_NONE, TX_DATA, TX_RESPONSE };

struct Mtp {
    FsBackend fs;
    FsAllocator mem;
    void (*received)(void *user, const char *path);
    void *user;
    MtpStats stats;

    Obj *obj;
    uint32_t nobj, cap;

    /* operation in progress */
    int state;
    uint16_t code;
    uint32_t tid, p[5];
    unsigned np;
    uint8_t head[32];           /* container being collected */
    uint32_t head_n;

    /* data from the computer */
    uint32_t out_total, out_got;
    bool out_head;              /* data container header complete */
    uint8_t small[1024];
    uint32_t small_n;
    int out_fd;
    bool out_fail;
    uint32_t send_handle;       /* object announced by SendObjectInfo */

    /* data and response to the computer */
    int tx;
    Buf data;                   /* payload from memory ... */
    bool tx_file;               /* ... or from a file */
    uint32_t tx_off, tx_len, tx_pos;
    uint16_t rc;
    uint32_t rp[3];
    unsigned nrp;

    bool protect_top;
    char storage_name[32];
    char path[FS_PATH_MAX], path2[FS_PATH_MAX];
};

/* ----------------------------------------------------------------- buffer */

static void buf_free(Mtp *m, Buf *b) {
    if (b->p) m->mem.free(b->p);
    memset(b, 0, sizeof(*b));
}

static void put(Mtp *m, Buf *b, const void *d, uint32_t n) {
    if (b->fail) return;
    if (b->n + n > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = m->mem.alloc(cap);
        if (!p) { b->fail = true; return; }
        if (b->p) { memcpy(p, b->p, b->n); m->mem.free(b->p); }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put8(Mtp *m, Buf *b, uint8_t v) { put(m, b, &v, 1); }
static void put16(Mtp *m, Buf *b, uint16_t v) { uint8_t d[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(m, b, d, 2); }
static void put32(Mtp *m, Buf *b, uint32_t v) {
    uint8_t d[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(m, b, d, 4);
}
static void put64(Mtp *m, Buf *b, uint64_t v) { put32(m, b, (uint32_t)v); put32(m, b, (uint32_t)(v >> 32)); }

/* PTP string: count of UTF-16 units including the terminator, then the units. */
static void put_str(Mtp *m, Buf *b, const char *s) {
    uint16_t u[255];
    unsigned n = 0;
    const uint8_t *c = (const uint8_t *)s;
    while (*c && n < 254) {
        uint32_t cp = *c++;
        if (cp >= 0xf0 && (c[0] & 0xc0) == 0x80 && (c[1] & 0xc0) == 0x80 && (c[2] & 0xc0) == 0x80) {
            cp = (cp & 7) << 18 | (c[0] & 0x3fu) << 12 | (c[1] & 0x3fu) << 6 | (c[2] & 0x3fu);
            c += 3;
        } else if (cp >= 0xe0 && (c[0] & 0xc0) == 0x80 && (c[1] & 0xc0) == 0x80) {
            cp = (cp & 15) << 12 | (c[0] & 0x3fu) << 6 | (c[1] & 0x3fu);
            c += 2;
        } else if (cp >= 0xc0 && (c[0] & 0xc0) == 0x80) {
            cp = (cp & 31) << 6 | (c[0] & 0x3fu);
            c += 1;
        }
        if (cp >= 0x10000) {
            if (n >= 253) break;
            cp -= 0x10000;
            u[n++] = (uint16_t)(0xd800 | cp >> 10);
            u[n++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
        } else u[n++] = (uint16_t)cp;
    }
    if (!n) { put8(m, b, 0); return; }
    put8(m, b, (uint8_t)(n + 1));
    for (unsigned i = 0; i < n; ++i) put16(m, b, u[i]);
    put16(m, b, 0);
}

static void put_array16(Mtp *m, Buf *b, const uint16_t *v, unsigned n) {
    put32(m, b, n);
    for (unsigned i = 0; i < n; ++i) put16(m, b, v[i]);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* PTP string at d (within n bytes) to UTF-8; returns bytes consumed, 0 on error. */
static uint32_t get_str(const uint8_t *d, uint32_t n, char *out, unsigned max) {
    if (!n) return 0;
    unsigned count = d[0];
    if (1 + 2 * count > n) return 0;
    unsigned o = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t cp = rd16(d + 1 + 2 * i);
        if (!cp) break;
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < count) {
            uint32_t lo = rd16(d + 3 + 2 * i);
            if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); ++i; }
        }
        if (o + 5 > max) return 0;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xc0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) {
            out[o++] = (char)(0xe0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | cp >> 18); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = 0;
    return 1 + 2 * count;
}

/* "YYYYMMDDThhmmss" from unix time. */
static void date_string(uint32_t t, char out[16]) {
    if (!t) { out[0] = 0; return; }
    uint32_t days = t / 86400u, rem = t % 86400u;
    /* civil from days (Howard Hinnant) */
    uint32_t z = days + 719468u, era = z / 146097u, doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t y = yoe + era * 400u, doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u, d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t mo = mp < 10u ? mp + 3u : mp - 9u;
    if (mo <= 2u) ++y;
    uint32_t v[6] = { y, mo, d, rem / 3600u, rem % 3600u / 60u, rem % 60u };
    static const uint8_t width[6] = { 4, 2, 2, 2, 2, 2 };
    unsigned o = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (i == 3) out[o++] = 'T';
        for (unsigned k = width[i]; k--;) { out[o + k] = (char)('0' + v[i] % 10u); v[i] /= 10u; }
        o += width[i];
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- objects */

static Obj *obj(Mtp *m, uint32_t h) {
    if (!h || h > m->nobj || m->obj[h - 1].dead) return NULL;
    return &m->obj[h - 1];
}

/* "/a/b" for a handle, "" for the root. */
static bool obj_path(Mtp *m, uint32_t h, char *out) {
    uint32_t chain[32];
    unsigned depth = 0;
    while (h != ROOT) {
        Obj *o = obj(m, h);
        if (!o || depth == 32) return false;
        chain[depth++] = h;
        h = o->parent;
    }
    unsigned n = 0;
    while (depth--) {
        const char *name = m->obj[chain[depth] - 1].name;
        size_t l = strlen(name);
        if (n + l + 2 > FS_PATH_MAX) return false;
        out[n++] = '/';
        memcpy(out + n, name, l);
        n += (unsigned)l;
    }
    out[n] = 0;
    return true;
}

static uint32_t find_child(Mtp *m, uint32_t parent, const char *name) {
    for (uint32_t i = 0; i < m->nobj; ++i) {
        Obj *o = &m->obj[i];
        if (!o->dead && o->parent == parent && !strcmp(o->name, name)) return i + 1;
    }
    return 0;
}

static uint32_t add_obj(Mtp *m, uint32_t parent, const char *name, bool dir, uint32_t size, uint32_t mtime) {
    if (m->nobj == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 256;
        Obj *n = m->mem.alloc(cap * sizeof(Obj));
        if (!n) return 0;
        if (m->obj) { memcpy(n, m->obj, m->nobj * sizeof(Obj)); m->mem.free(m->obj); }
        m->obj = n;
        m->cap = cap;
    }
    size_t l = strlen(name);
    char *copy = m->mem.alloc(l + 1);
    if (!copy) return 0;
    memcpy(copy, name, l + 1);
    Obj *o = &m->obj[m->nobj++];
    memset(o, 0, sizeof(*o));
    o->parent = parent;
    o->name = copy;
    o->dir = dir;
    o->size = size;
    o->mtime = mtime;
    o->seen = 1;
    return m->nobj;
}

static void kill_obj(Mtp *m, uint32_t h) {
    Obj *o = obj(m, h);
    if (!o) return;
    o->dead = 1;
    m->mem.free(o->name);
    o->name = NULL;
    if (m->send_handle == h) m->send_handle = 0;
    if (!o->dir) return;
    for (uint32_t i = 0; i < m->nobj; ++i)
        if (!m->obj[i].dead && m->obj[i].parent == h) kill_obj(m, i + 1);
}

typedef struct { Mtp *m; uint32_t parent; } ListCtx;

static int list_cb(void *user, const char *name, bool is_dir, uint32_t size, uint32_t mtime) {
    ListCtx *c = user;
    uint32_t h = find_child(c->m, c->parent, name);
    if (h) {
        Obj *o = &c->m->obj[h - 1];
        if (o->dir != is_dir) { kill_obj(c->m, h); h = 0; }
        else { o->size = size; o->mtime = mtime; o->seen = 1; }
    }
    if (!h) add_obj(c->m, c->parent, name, is_dir, size, mtime);
    return 0;
}

/* Bring the children of a folder up to date with the file system. */
static bool list_dir(Mtp *m, uint32_t parent) {
    char path[FS_PATH_MAX];
    if (!obj_path(m, parent, path)) return false;
    for (uint32_t i = 0; i < m->nobj; ++i)
        if (m->obj[i].parent == parent) m->obj[i].seen = 0;
    ListCtx c = { m, parent };
    if (m->fs.list(m->fs.ctx, path[0] ? path : "/", list_cb, &c) < 0) return false;
    for (uint32_t i = 0; i < m->nobj; ++i)
        if (!m->obj[i].dead && m->obj[i].parent == parent && !m->obj[i].seen &&
            i + 1 != m->send_handle)            /* announced, its data is still to come */
            kill_obj(m, i + 1);
    return true;
}

static void list_all(Mtp *m) {
    list_dir(m, ROOT);
    for (uint32_t i = 0; i < m->nobj; ++i)
        if (!m->obj[i].dead && m->obj[i].dir) list_dir(m, i + 1);
}

static bool valid_name(const char *n) {
    if (!n[0] || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (; *n; ++n) if (*n == '/' || *n == '\\' || (unsigned char)*n < 32) return false;
    return true;
}

static bool child_path(Mtp *m, uint32_t parent, const char *name, char *out) {
    if (!obj_path(m, parent, out)) return false;
    size_t n = strlen(out), l = strlen(name);
    if (n + l + 2 > FS_PATH_MAX) return false;
    out[n] = '/';
    memcpy(out + n + 1, name, l + 1);
    return true;
}

static bool delete_tree(Mtp *m, uint32_t h) {
    Obj *o = obj(m, h);
    if (!o) return false;
    char path[FS_PATH_MAX];
    if (!obj_path(m, h, path)) return false;
    if (!o->dir) {
        if (m->fs.unlink(m->fs.ctx, path) < 0) return false;
    } else {
        list_dir(m, h);
        for (uint32_t i = 0; i < m->nobj; ++i)
            if (!m->obj[i].dead && m->obj[i].parent == h && !delete_tree(m, i + 1)) return false;
        if (m->fs.rmdir(m->fs.ctx, path) < 0) return false;
    }
    kill_obj(m, h);
    ++m->stats.deleted;
    return true;
}

/* The folders at the top of the calculator's file system (documents, phoenix,
   ...) are the OS's own: what is inside can be changed, but the folders
   themselves cannot be deleted, renamed or moved from the computer. */
static bool system_folder(Mtp *m, uint32_t h) {
    const Obj *o = obj(m, h);
    return m->protect_top && o && o->dir && o->parent == ROOT;
}

/* -------------------------------------------------------------- responses */

static void respond(Mtp *m, uint16_t rc) {
    m->rc = rc;
    if (m->tx == TX_NONE) m->tx = TX_RESPONSE;
    m->tx_pos = 0;
    m->state = ST_IDLE;
    if (rc != RC_OK) { ++m->stats.errors; m->stats.last_error = rc; m->stats.last_op = m->code; }
}


/* Data from memory (m->data), then the response. */
static void send_data(Mtp *m, uint16_t rc) {
    if (m->data.fail) { buf_free(m, &m->data); respond(m, RC_GENERAL_ERROR); return; }
    m->tx = TX_DATA;
    m->tx_file = false;
    m->tx_len = m->data.n;
    respond(m, rc);
}

static uint16_t format_of(const Obj *o) { return o->dir ? FMT_ASSOCIATION : FMT_UNDEFINED; }

static void device_info(Mtp *m) {
    Buf *b = &m->data;
    static const uint16_t formats[] = { FMT_UNDEFINED, FMT_ASSOCIATION };
    put16(m, b, 100);                       /* standard version */
    put32(m, b, 6);                         /* MTP vendor extension */
    put16(m, b, 100);
    put_str(m, b, "microsoft.com: 1.0;");
    put16(m, b, 0);
    put_array16(m, b, operations, sizeof(operations) / sizeof(operations[0]));
    put32(m, b, 0);                         /* events */
    put32(m, b, 0);                         /* device properties */
    put32(m, b, 0);                         /* capture formats */
    put_array16(m, b, formats, 2);
    put_str(m, b, "Texas Instruments");
    put_str(m, b, "TI-Nspire CX II");
    put_str(m, b, "nDrive");
    put_str(m, b, "0000000000000001");
}

static void storage_info(Mtp *m) {
    Buf *b = &m->data;
    put16(m, b, 0x0003);                    /* fixed RAM */
    put16(m, b, 0x0002);                    /* hierarchical */
    put16(m, b, 0x0000);                    /* read-write */
    /* If the file system cannot say, report room and let a full one fail the write. */
    uint64_t total = 128u * 1024 * 1024, available = 100u * 1024 * 1024;
    if (m->fs.space) m->fs.space(m->fs.ctx, &total, &available);
    put64(m, b, total);
    put64(m, b, available);
    put32(m, b, 0xffffffffu);
    put_str(m, b, m->storage_name);
    put_str(m, b, "");
}

static void object_info(Mtp *m, uint32_t h) {
    Buf *b = &m->data;
    const Obj *o = obj(m, h);
    char date[16];
    date_string(o->mtime, date);
    put32(m, b, STORAGE_ID);
    put16(m, b, format_of(o));
    put16(m, b, 0);                         /* protection */
    put32(m, b, o->dir ? 0 : o->size);
    put16(m, b, 0); put32(m, b, 0); put32(m, b, 0); put32(m, b, 0);     /* thumbnail */
    put32(m, b, 0); put32(m, b, 0); put32(m, b, 0);                     /* image */
    put32(m, b, o->parent);
    put16(m, b, o->dir ? 1 : 0);            /* generic folder */
    put32(m, b, 0);
    put32(m, b, 0);
    put_str(m, b, o->name);
    put_str(m, b, "");
    put_str(m, b, date);
    put_str(m, b, "");
}

static int prop_type(uint16_t prop) {
    switch (prop) {
    case PROP_STORAGE: case PROP_PARENT: return 0x0006;
    case PROP_FORMAT: case PROP_PROTECTION: return 0x0004;
    case PROP_SIZE: return 0x0008;
    case PROP_PUID: return 0x000a;
    case PROP_FILENAME: case PROP_MODIFIED: case PROP_NAME: return 0xffff;
    default: return 0;
    }
}

static void prop_desc(Mtp *m, uint16_t prop) {
    Buf *b = &m->data;
    int type = prop_type(prop);
    put16(m, b, prop);
    put16(m, b, (uint16_t)type);
    put8(m, b, prop == PROP_FILENAME ? 1 : 0);          /* only the name can be set */
    switch (type) {
    case 0x0004: put16(m, b, 0); break;
    case 0x0006: put32(m, b, 0); break;
    case 0x0008: put64(m, b, 0); break;
    case 0x000a: put64(m, b, 0); put64(m, b, 0); break;
    default: put8(m, b, 0); break;
    }
    put32(m, b, 0);                         /* group */
    put8(m, b, prop == PROP_MODIFIED ? 3 : 0);          /* form: date-time / none */
}

static void prop_value(Mtp *m, uint32_t h, uint16_t prop) {
    Buf *b = &m->data;
    const Obj *o = obj(m, h);
    char date[16];
    switch (prop) {
    case PROP_STORAGE: put32(m, b, STORAGE_ID); break;
    case PROP_FORMAT: put16(m, b, format_of(o)); break;
    case PROP_PROTECTION: put16(m, b, 0); break;
    case PROP_SIZE: put64(m, b, o->dir ? 0 : o->size); break;
    case PROP_FILENAME: case PROP_NAME: put_str(m, b, o->name); break;
    case PROP_MODIFIED: date_string(o->mtime, date); put_str(m, b, date); break;
    case PROP_PARENT: put32(m, b, o->parent); break;
    case PROP_PUID: put32(m, b, h); put32(m, b, 0); put64(m, b, 0); break;
    }
}

static bool format_matches(const Obj *o, uint32_t format) {
    return !format || format == format_of(o);
}

/* Handles (or just the count) for GetObjectHandles / GetNumObjects. */
static uint16_t handles(Mtp *m, bool count_only) {
    uint32_t storage = m->p[0], format = m->p[1], parent = m->p[2];
    if (storage != 0xffffffffu && storage != STORAGE_ID) return RC_INVALID_STORAGE;
    bool all = parent == 0;
    if (parent == 0xffffffffu) parent = ROOT;
    if (all) list_all(m);
    else {
        if (parent != ROOT && (!obj(m, parent) || !obj(m, parent)->dir)) return RC_INVALID_PARENT;
        if (!list_dir(m, parent)) return RC_GENERAL_ERROR;
    }
    uint32_t n = 0;
    for (int pass = 0; pass < (count_only ? 1 : 2); ++pass) {
        if (pass) put32(m, &m->data, n);
        for (uint32_t i = 0; i < m->nobj; ++i) {
            const Obj *o = &m->obj[i];
            if (o->dead || (!all && o->parent != parent) || !format_matches(o, format)) continue;
            if (pass) put32(m, &m->data, i + 1); else ++n;
        }
    }
    if (count_only) { m->rp[0] = n; m->nrp = 1; }
    return RC_OK;
}

/* ------------------------------------------------------------- operations */

static void send_object_info(Mtp *m) {
    char name[FS_PATH_MAX];
    const uint8_t *d = m->small;
    uint32_t parent = m->p[1];
    if (parent == 0xffffffffu) parent = ROOT;
    if (m->small_n < 53 || !get_str(d + 52, m->small_n - 52, name, sizeof(name)) || !valid_name(name)) {
        respond(m, RC_INVALID_PARAMETER);
        return;
    }
    if (m->p[0] && m->p[0] != STORAGE_ID) { respond(m, RC_INVALID_STORAGE); return; }
    if (parent != ROOT && (!obj(m, parent) || !obj(m, parent)->dir)) { respond(m, RC_INVALID_PARENT); return; }
    if (!child_path(m, parent, name, m->path)) { respond(m, RC_INVALID_PARAMETER); return; }
    bool dir = rd16(d + 4) == FMT_ASSOCIATION;
    list_dir(m, parent);
    uint32_t h = find_child(m, parent, name);
    if (h && m->obj[h - 1].dir != dir) { respond(m, RC_ACCESS_DENIED); return; }
    if (dir) {
        if (!h && m->fs.mkdir(m->fs.ctx, m->path) < 0) { respond(m, RC_ACCESS_DENIED); return; }
        if (!h) { h = add_obj(m, parent, name, true, 0, m->fs.now ? m->fs.now(m->fs.ctx) : 0); ++m->stats.folders; }
    } else {
        if (!h) h = add_obj(m, parent, name, false, rd32(d + 8), 0);
        m->send_handle = h;
    }
    if (!h) { respond(m, RC_GENERAL_ERROR); return; }
    m->rp[0] = STORAGE_ID;
    m->rp[1] = parent == ROOT ? 0xffffffffu : parent;
    m->rp[2] = h;
    m->nrp = 3;
    respond(m, RC_OK);
}

static void send_object_done(Mtp *m) {
    uint32_t h = m->send_handle;
    m->send_handle = 0;
    if (m->out_fd >= 0 && m->fs.close(m->fs.ctx, m->out_fd) < 0) m->out_fail = true;
    bool opened = m->out_fd >= 0;
    m->out_fd = -1;
    Obj *o = obj(m, h);
    if (!o) { respond(m, RC_NO_VALID_OBJECT_INFO); return; }
    if (!obj_path(m, h, m->path)) { respond(m, RC_GENERAL_ERROR); return; }
    if (m->out_fail) {
        if (opened) m->fs.unlink(m->fs.ctx, m->path);
        kill_obj(m, h);
        respond(m, opened ? RC_STORE_FULL : RC_ACCESS_DENIED);
        return;
    }
    o->size = m->out_got;
    o->mtime = m->fs.now ? m->fs.now(m->fs.ctx) : 0;
    ++m->stats.files_in;
    respond(m, RC_OK);
    if (m->received) m->received(m->user, m->path);
}

static void set_prop_value(Mtp *m) {
    char name[FS_PATH_MAX];
    uint32_t h = m->p[0];
    Obj *o = obj(m, h);
    if (!o) { respond(m, RC_INVALID_HANDLE); return; }
    if (m->p[1] != PROP_FILENAME || system_folder(m, h)) { respond(m, RC_ACCESS_DENIED); return; }
    if (!get_str(m->small, m->small_n, name, sizeof(name)) || !valid_name(name)) {
        respond(m, RC_INVALID_PARAMETER);
        return;
    }
    if (!strcmp(name, o->name)) { respond(m, RC_OK); return; }
    if (find_child(m, o->parent, name) || !obj_path(m, h, m->path) ||
        !child_path(m, o->parent, name, m->path2)) { respond(m, RC_ACCESS_DENIED); return; }
    size_t l = strlen(name);
    char *copy = m->mem.alloc(l + 1);
    if (!copy) { respond(m, RC_GENERAL_ERROR); return; }
    if (m->fs.rename(m->fs.ctx, m->path, m->path2) < 0) { m->mem.free(copy); respond(m, RC_ACCESS_DENIED); return; }
    memcpy(copy, name, l + 1);
    m->mem.free(o->name);
    o->name = copy;
    ++m->stats.renamed;
    respond(m, RC_OK);
}

static void move_object(Mtp *m) {
    uint32_t h = m->p[0], parent = m->p[2];
    Obj *o = obj(m, h);
    if (parent == 0xffffffffu) parent = ROOT;
    if (!o) { respond(m, RC_INVALID_HANDLE); return; }
    if (system_folder(m, h)) { respond(m, RC_ACCESS_DENIED); return; }
    if (parent != ROOT && (!obj(m, parent) || !obj(m, parent)->dir)) { respond(m, RC_INVALID_PARENT); return; }
    for (uint32_t a = parent; a != ROOT; a = m->obj[a - 1].parent)      /* not into itself */
        if (a == h) { respond(m, RC_INVALID_PARENT); return; }
    list_dir(m, parent);
    if (find_child(m, parent, o->name) || !obj_path(m, h, m->path) ||
        !child_path(m, parent, o->name, m->path2) ||
        m->fs.rename(m->fs.ctx, m->path, m->path2) < 0) { respond(m, RC_ACCESS_DENIED); return; }
    o->parent = parent;
    ++m->stats.renamed;
    respond(m, RC_OK);
}

/* Data phase from the computer finished. */
static void data_out_done(Mtp *m) {
    switch (m->code) {
    case OP_SEND_OBJECT_INFO: send_object_info(m); break;
    case OP_SEND_OBJECT: send_object_done(m); break;
    case OP_SET_OBJECT_PROP_VALUE: set_prop_value(m); break;
    case OP_SET_OBJECT_REFERENCES: respond(m, RC_OK); break;
    default: respond(m, RC_GENERAL_ERROR); break;
    }
}

static void expect_data(Mtp *m) {
    m->state = ST_DATA_OUT;
    m->out_head = false;
    m->head_n = 0;
    m->out_total = m->out_got = 0;
    m->small_n = 0;
    m->out_fd = -1;
    m->out_fail = false;
}

static void command(Mtp *m) {
    Obj *o;
    uint16_t rc;
    ++m->stats.commands;
    m->nrp = 0;
    buf_free(m, &m->data);
    m->tx = TX_NONE;
    switch (m->code) {
    case OP_GET_DEVICE_INFO: device_info(m); send_data(m, RC_OK); break;
    case OP_OPEN_SESSION: m->send_handle = 0; respond(m, RC_OK); break;
    case OP_CLOSE_SESSION: respond(m, RC_OK); break;
    case OP_GET_STORAGE_IDS: put32(m, &m->data, 1); put32(m, &m->data, STORAGE_ID); send_data(m, RC_OK); break;
    case OP_GET_STORAGE_INFO:
        if (m->p[0] != STORAGE_ID) { respond(m, RC_INVALID_STORAGE); break; }
        storage_info(m);
        send_data(m, RC_OK);
        break;
    case OP_GET_NUM_OBJECTS: respond(m, handles(m, true)); break;
    case OP_GET_OBJECT_HANDLES:
        rc = handles(m, false);
        if (rc == RC_OK) send_data(m, RC_OK); else { buf_free(m, &m->data); respond(m, rc); }
        break;
    case OP_GET_OBJECT_INFO:
        if (!obj(m, m->p[0])) { respond(m, RC_INVALID_HANDLE); break; }
        object_info(m, m->p[0]);
        send_data(m, RC_OK);
        break;
    case OP_GET_OBJECT:
    case OP_GET_PARTIAL_OBJECT:
        o = obj(m, m->p[0]);
        if (!o || o->dir || !obj_path(m, m->p[0], m->path)) { respond(m, RC_INVALID_HANDLE); break; }
        list_dir(m, o->parent);                         /* current size */
        o = obj(m, m->p[0]);
        if (!o) { respond(m, RC_INVALID_HANDLE); break; }
        m->tx_off = 0;
        m->tx_len = o->size;
        if (m->code == OP_GET_PARTIAL_OBJECT) {
            m->tx_off = m->p[1] < o->size ? m->p[1] : o->size;
            m->tx_len = o->size - m->tx_off;
            if (m->tx_len > m->p[2]) m->tx_len = m->p[2];
            m->rp[0] = m->tx_len;
            m->nrp = 1;
        } else ++m->stats.files_out;
        m->tx = TX_DATA;
        m->tx_file = true;
        respond(m, RC_OK);
        break;
    case OP_GET_THUMB: respond(m, RC_NO_THUMBNAIL); break;
    case OP_DELETE_OBJECT:
        if (!obj(m, m->p[0])) { respond(m, RC_INVALID_HANDLE); break; }
        if (system_folder(m, m->p[0])) { respond(m, RC_ACCESS_DENIED); break; }
        respond(m, delete_tree(m, m->p[0]) ? RC_OK : RC_ACCESS_DENIED);
        break;
    case OP_SEND_OBJECT_INFO:
    case OP_SET_OBJECT_PROP_VALUE:
    case OP_SET_OBJECT_REFERENCES:
        expect_data(m);
        break;
    case OP_SEND_OBJECT:
        expect_data(m);
        if (!obj(m, m->send_handle) || !obj_path(m, m->send_handle, m->path)) { m->out_fail = true; break; }
        m->out_fd = m->fs.create(m->fs.ctx, m->path);
        if (m->out_fd < 0) m->out_fail = true;
        break;
    case OP_MOVE_OBJECT: move_object(m); break;
    case OP_GET_OBJECT_PROPS_SUPPORTED:
        put_array16(m, &m->data, object_props, sizeof(object_props) / sizeof(object_props[0]));
        send_data(m, RC_OK);
        break;
    case OP_GET_OBJECT_PROP_DESC:
        if (!prop_type((uint16_t)m->p[0])) { respond(m, RC_INVALID_PROP_CODE); break; }
        prop_desc(m, (uint16_t)m->p[0]);
        send_data(m, RC_OK);
        break;
    case OP_GET_OBJECT_PROP_VALUE:
        if (!obj(m, m->p[0])) { respond(m, RC_INVALID_HANDLE); break; }
        if (!prop_type((uint16_t)m->p[1])) { respond(m, RC_INVALID_PROP_CODE); break; }
        prop_value(m, m->p[0], (uint16_t)m->p[1]);
        send_data(m, RC_OK);
        break;
    case OP_GET_OBJECT_REFERENCES:
        if (!obj(m, m->p[0])) { respond(m, RC_INVALID_HANDLE); break; }
        put32(m, &m->data, 0);
        send_data(m, RC_OK);
        break;
    default: respond(m, RC_OP_NOT_SUPPORTED); break;
    }
}

/* -------------------------------------------------------------- transport */

static void abort_transfer(Mtp *m) {
    if (m->state == ST_DATA_OUT && m->out_fd >= 0) {
        m->fs.close(m->fs.ctx, m->out_fd);
        if (obj_path(m, m->send_handle, m->path)) m->fs.unlink(m->fs.ctx, m->path);
        kill_obj(m, m->send_handle);
    }
    m->out_fd = -1;
    if (m->tx_file && m->fs.read_done) m->fs.read_done(m->fs.ctx);
    m->tx_file = false;
    buf_free(m, &m->data);
    m->tx = TX_NONE;
    m->state = ST_IDLE;
    m->head_n = 0;
}

void mtp_rx(Mtp *m, const uint8_t *d, uint32_t n) {
    while (n) {
        if (m->state == ST_DATA_OUT && m->out_head) {
            uint32_t take = m->out_total - m->out_got;
            if (take > n) take = n;
            if (m->code == OP_SEND_OBJECT) {
                if (!m->out_fail && m->fs.write(m->fs.ctx, m->out_fd, d, take) < 0) m->out_fail = true;
            } else {
                uint32_t room = sizeof(m->small) - m->small_n, c = take < room ? take : room;
                memcpy(m->small + m->small_n, d, c);
                m->small_n += c;
            }
            m->out_got += take;
            d += take;
            n -= take;
            if (m->out_got == m->out_total) data_out_done(m);
            continue;
        }
        /* Collect a container header (12 bytes) or a whole command (12..32). */
        if (m->state == ST_IDLE && m->tx != TX_NONE && !m->head_n) abort_transfer(m);  /* host moved on */
        uint32_t want = m->head_n < 12 ? 12 : rd32(m->head);
        if (m->state == ST_DATA_OUT) want = 12;
        if (want < 12 || want > sizeof(m->head)) want = 12;
        uint32_t take = want - m->head_n;
        if (take > n) take = n;
        memcpy(m->head + m->head_n, d, take);
        m->head_n += take;
        d += take;
        n -= take;
        if (m->head_n < 12) continue;
        uint32_t length = rd32(m->head);
        uint16_t type = rd16(m->head + 4);
        if (m->state == ST_DATA_OUT) {
            if (type == 2 && length >= 12) {
                m->out_head = true;
                m->out_total = length - 12;
                m->out_got = 0;
                m->head_n = 0;
                if (!m->out_total) data_out_done(m);
                continue;
            }
            abort_transfer(m);                          /* a new command instead of the data */
            m->head_n = 12;
        }
        if (type != 1 || length < 12 || length > sizeof(m->head)) { m->head_n = 0; continue; }
        if (m->head_n < length) continue;
        m->code = rd16(m->head + 6);
        m->tid = rd32(m->head + 8);
        m->np = (length - 12) / 4;
        memset(m->p, 0, sizeof(m->p));
        for (unsigned i = 0; i < m->np && i < 5; ++i) m->p[i] = rd32(m->head + 12 + 4 * i);
        m->head_n = 0;
        command(m);
    }
}

static void header(uint8_t h[12], uint32_t length, uint16_t type, uint16_t code, uint32_t tid) {
    h[0] = (uint8_t)length; h[1] = (uint8_t)(length >> 8); h[2] = (uint8_t)(length >> 16); h[3] = (uint8_t)(length >> 24);
    h[4] = (uint8_t)type; h[5] = (uint8_t)(type >> 8);
    h[6] = (uint8_t)code; h[7] = (uint8_t)(code >> 8);
    h[8] = (uint8_t)tid; h[9] = (uint8_t)(tid >> 8); h[10] = (uint8_t)(tid >> 16); h[11] = (uint8_t)(tid >> 24);
}

bool mtp_tx_pending(const Mtp *m) { return m->state == ST_IDLE && m->tx != TX_NONE; }

uint32_t mtp_tx(Mtp *m, uint8_t *buf, uint32_t max, bool *end) {
    uint8_t h[12];
    uint32_t n = 0;
    *end = false;
    if (!mtp_tx_pending(m) || max < 32) return 0;
    if (m->tx == TX_DATA) {
        uint32_t total = 12 + m->tx_len;
        if (m->tx_pos < 12) {
            header(h, total, 2, m->code, m->tid);
            n = 12 - m->tx_pos;
            memcpy(buf, h + m->tx_pos, n);
            m->tx_pos = 12;
        }
        uint32_t take = total - m->tx_pos;
        if (take > max - n) take = max - n;
        if (take) {
            uint32_t off = m->tx_pos - 12;
            if (m->tx_file) {
                int r = m->fs.read(m->fs.ctx, m->path, m->tx_off + off, buf + n, take);
                if (r < 0) r = 0;
                if ((uint32_t)r < take) memset(buf + n + r, 0, take - (uint32_t)r);   /* file shrank */
            } else memcpy(buf + n, m->data.p + off, take);
            n += take;
            m->tx_pos += take;
        }
        if (m->tx_pos == total) {
            if (m->tx_file && m->fs.read_done) m->fs.read_done(m->fs.ctx);
            m->tx_file = false;
            buf_free(m, &m->data);
            m->tx = TX_RESPONSE;
            m->tx_pos = 0;
            *end = true;
        }
        return n;
    }
    header(buf, 12 + 4 * m->nrp, 3, m->rc, m->tid);
    for (unsigned i = 0; i < m->nrp; ++i) {
        buf[12 + 4 * i] = (uint8_t)m->rp[i]; buf[13 + 4 * i] = (uint8_t)(m->rp[i] >> 8);
        buf[14 + 4 * i] = (uint8_t)(m->rp[i] >> 16); buf[15 + 4 * i] = (uint8_t)(m->rp[i] >> 24);
    }
    m->tx = TX_NONE;
    *end = true;
    return 12 + 4 * m->nrp;
}

bool mtp_receiving(const Mtp *m) { return m->state == ST_DATA_OUT && m->code == OP_SEND_OBJECT; }

void mtp_reset(Mtp *m) {
    abort_transfer(m);
    m->send_handle = 0;
}

void mtp_remount(Mtp *m, bool protect_top, const char *storage_name) {
    mtp_reset(m);
    for (uint32_t i = 0; i < m->nobj; ++i) if (m->obj[i].name) m->mem.free(m->obj[i].name);
    m->nobj = 0;
    m->protect_top = protect_top;
    size_t n = strlen(storage_name);
    if (n >= sizeof(m->storage_name)) n = sizeof(m->storage_name) - 1;
    memcpy(m->storage_name, storage_name, n);
    m->storage_name[n] = 0;
}

const MtpStats *mtp_stats(const Mtp *m) { return &m->stats; }

uint32_t mtp_objects(const Mtp *m) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->nobj; ++i) n += !m->obj[i].dead;
    return n;
}

Mtp *mtp_create(const FsBackend *fs, const FsAllocator *mem,
                void (*received)(void *user, const char *path), void *user) {
    Mtp *m = mem->alloc(sizeof(Mtp));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->fs = *fs;
    m->mem = *mem;
    m->received = received;
    m->user = user;
    m->out_fd = -1;
    m->protect_top = false;
    memcpy(m->storage_name, "Calculator", 11);
    return m;
}

void mtp_destroy(Mtp *m) {
    if (!m) return;
    abort_transfer(m);
    for (uint32_t i = 0; i < m->nobj; ++i) if (m->obj[i].name) m->mem.free(m->obj[i].name);
    if (m->obj) m->mem.free(m->obj);
    m->mem.free(m);
}
