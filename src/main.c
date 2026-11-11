/* nDrive - the TI-Nspire CX II as an MTP device or a USB drive, next to the
 * TI link.
 *
 * Put ndrive.tns in /ndless/startup. It installs a resident service that
 * turns the calculator into a composite USB device: the OS keeps the TI link
 * (TI software, n-link) and nDrive adds a second interface, either
 *   - MTP, the protocol phones use (files are read and written directly), or
 *   - a USB drive (mass storage; a FAT volume generated from the files, with
 *     what the computer writes saved once it stops writing).
 * Either shows the whole calculator or just My Documents. Everything runs in
 * the background; a file copied into My Documents is announced with the OS's
 * own "Document Received". ctrl then U shows the status and the options.
 *
 * Copyright (C) 2026 Rodrigo. GNU GPL v3 or later.
 */
#include <libndls.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "build_id.h"
#include "calcfs.h"
#include "msc.h"
#include "mtp.h"
#include "notify.h"
#include "resident.h"
#include "usbbg.h"
#include "usbhook.h"
#include "vfat.h"

#define NDRIVE_ID 0x4e445256u          /* 'NDRV' */
#define CONFIG "/documents/ndless/ndrive.cfg.tns"
#define DEVADDR (*(volatile uint32_t *)0xb0000104u)

#define QUIET_MS 1200u                  /* MTP: computer idle this long: announce changes */
#define SYNC_QUIET_MS 1500u             /* drive: computer idle this long: save its writes */
#define SCAN_EVERY_MS 4000u             /* drive: look for changes on the calculator */
#define REFRESH_IDLE_MS 2500u           /* drive: host quiet before a new medium */

typedef struct {
    uint32_t magic, ticks, events;      /* diagnostics: 'NDTK' */
    int ready, busy;
    /* settings */
    int notify;                         /* show "Document Received" */
    int docs_only;                      /* show My Documents instead of the whole calculator */
    int drive;                          /* USB drive (mass storage) instead of MTP */
    int big_dma;
    /* both modes */
    UsbBgStatus st;
    FsBackend be;
    FsAllocator mem;
    int connected;                      /* a computer has configured the device */
    uint32_t seen_activity;             /* usbhook.activity when last seen unplugged */
    int ui_pending;                     /* files changed: refresh the OS's views */
    int problem;                        /* error[] has not been shown yet */
    char note[FS_PATH_MAX];             /* document to announce (path as the mode sees it) */
    char error[160];
    /* MTP */
    Mtp *mtp;
    uint32_t seen_changes;
    /* USB drive */
    Vfat *vfat;
    Msc msc;
    int syncing;
    uint32_t budget;                    /* RAM for what the computer writes */
    uint32_t calc_sig;                  /* the files as the drive shows them */
    uint32_t last_scan, last_sync_try, syncs, refreshes;
    int calc_changed;
    int sync_fails;                     /* consecutive saves that could not run */
    uint32_t fail_write_ms;             /* last host write at the last failure */
    int deferred;                       /* a save needs a question: not inside a dialog */
} Service;

static Service storage;
static Service *svc;

/* ------------------------------------------------------------------- UI */

static void refresh_os(void) {
    void (*home)(void) = resident_native(e_refresh_homescr);
    void (*browser)(int) = resident_native(e_refresh_docbrowser);
    if (home) home();
    if (browser) browser(0x1b5a);
}

/* No stdio in OS callbacks: it can end up in Ndless's malloc. */
static char *cat(char *d, const char *s) { while (*d) ++d; while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *d, uint32_t n) {
    char t[12];
    int i = 0;
    do { t[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (*d) ++d;
    while (i) *d++ = t[--i];
    *d = 0;
    return d;
}
static char *hex(char *d, uint32_t n) {
    while (*d) ++d;
    for (int s = 12; s >= 0; s -= 4) *d++ = "0123456789abcdef"[(n >> s) & 15];
    *d = 0;
    return d;
}

static void set_error(Service *s, const char *a, const char *b) {
    unsigned n = 0;
    for (; *a && n + 1 < sizeof(s->error); ++a) s->error[n++] = *a;
    for (; b && *b && n + 1 < sizeof(s->error); ++b) s->error[n++] = *b;
    s->error[n] = 0;
}

static bool link_up(void) {
    return usbhook.configured && usbhook_vbus();
}

static void utf16(uint16_t *out, const char *in, unsigned max) {
    unsigned i = 0;
    for (; in[i] && i + 1 < max; ++i) out[i] = (uint8_t)in[i];
    out[i] = 0;
}

static unsigned ask(const char *title, const char *text, const char *first, const char *second) {
    static uint16_t t[20], m[240], a[24], b[24];
    utf16(t, title, 20);
    utf16(m, text, 240);
    utf16(a, first, 24);
    utf16(b, second, 24);
    unsigned pending;
    return resident_question(t, m, a, b, &pending);
}

/* Refresh the OS's file views and announce a document the way the TI link
   does. path is as the current mode sees it (inside My Documents if only that
   is shown). */
static void announce(Service *s, const char *path) {
    static char full[FS_PATH_MAX + 16];
    refresh_os();
    if (!path[0] || !s->notify) return;
    full[0] = 0;
    if (s->docs_only) cat(full, "/documents");
    cat(full, path);
    if (!strncmp(full, "/documents/", 11)) notify_received(full);
}

static void status_dialog(Service *s);

static void problem_dialog(Service *s) {
    if (ask("nDrive", s->error, "OK", "Status") == 2) status_dialog(s);
}

/* ============================================================== MTP mode */

/* A file from the computer was saved (called from inside the transfer). */
static void received(void *user, const char *path) {
    Service *s = user;
    if (strlen(path) < sizeof(s->note)) strcpy(s->note, path);
}

static uint32_t changes(const Service *s) {
    const MtpStats *m = mtp_stats(s->mtp);
    return m->files_in + m->deleted + m->renamed + m->folders;
}

static void mtp_serve(Service *s, unsigned budget_ms) {
    if (usbbg_pending()) {
        usbbg_service(s->mtp, &s->st, 80, budget_ms);
        resident_keep_awake();          /* no automatic power-down in the middle of a transfer */
    }
    uint32_t c = changes(s);
    if (c != s->seen_changes) { s->seen_changes = c; s->ui_pending = 1; }
}

static bool announce_due(const Service *s, uint32_t now) {
    return s->ui_pending && !mtp_receiving(s->mtp) && now - s->st.last_activity_ms > QUIET_MS;
}

static void mtp_work(Service *s) {
    mtp_serve(s, 1500);
    /* The computer has finished for now: show what changed, and announce the
       last document that arrived. While that box is open the transfers go on
       (see reentry). */
    while (announce_due(s, usb_millis())) {
        static char path[FS_PATH_MAX];
        s->ui_pending = 0;
        strcpy(path, s->note);
        s->note[0] = 0;
        announce(s, path);
    }
    int link = link_up();
    if (!link && s->connected) mtp_reset(s->mtp);       /* unplugged: close what was in progress */
    if (!link) s->seen_activity = usbhook.activity;     /* back to the idle fast path */
    s->connected = link;
}

static bool mtp_tick(Service *s) {
    uint32_t now = usb_millis();
    if (usbbg_pending()) return true;
    if (s->busy) return false;
    if (announce_due(s, now)) return true;
    bool link = link_up();
    if (link != (s->connected != 0)) return true;
    if (!link && !s->ui_pending) s->seen_activity = usbhook.activity;
    return false;
}

/* ======================================================== USB drive mode */

typedef struct { uint32_t h; unsigned depth; char path[FS_PATH_MAX]; } SigCtx;
static int sig_dir(SigCtx *c);

static int sig_cb(void *user, const char *name, bool is_dir, uint32_t size, uint32_t mtime) {
    SigCtx *c = user;
    uint32_t h = c->h;
    for (const char *p = name; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ size) * 16777619u;
    h = (h ^ mtime) * 16777619u;
    h = (h ^ (uint32_t)is_dir) * 16777619u;
    c->h = h;
    if (is_dir && c->depth < 16) {
        size_t len = strlen(c->path), n = strlen(name);
        if (len + n + 2 >= sizeof(c->path)) return 0;
        c->path[len] = '/';
        memcpy(c->path + len + 1, name, n + 1);
        ++c->depth;
        sig_dir(c);
        --c->depth;
        c->path[len] = 0;
    }
    return 0;
}

static int sig_dir(SigCtx *c) {
    return svc->be.list(svc->be.ctx, c->path, sig_cb, c);
}

/* The OS lists in a stable order, which is enough for a change check. */
static uint32_t calc_signature(void) {
    static SigCtx c;
    c.h = 2166136261u;
    c.depth = 0;
    c.path[0] = 0;
    sig_dir(&c);
    return c.h;
}

/* RAM for the computer's writes: a third of what is free, 2..12 MB. */
static uint32_t overlay_budget(void) {
    void *chunks[64];
    unsigned n = 0;
    while (n < 64 && (chunks[n] = svc->mem.alloc(1u << 20))) ++n;
    uint32_t total = n << 20;
    while (n) svc->mem.free(chunks[--n]);
    uint32_t budget = total / 3u;
    if (budget > (12u << 20)) budget = 12u << 20;
    if (budget < (2u << 20)) budget = 2u << 20;
    return budget;
}

/* Free space to show on the drive. What does not fit in RAM is kept in a
   scratch file until it is saved, so a file needs its size twice for a moment:
   the drive can take about half of what is free on the calculator. */
static uint32_t drive_free(const Service *s) {
    uint64_t total, avail;
    if (!resident_free_space(&total, &avail)) return 0;         /* RAM only */
    uint64_t reserve = 4u << 20;
    avail = avail > reserve ? avail - reserve : 0;
    uint64_t f = (avail + s->budget) / 2u;
    if (f > (1900u << 20)) f = 1900u << 20;
    return (uint32_t)f;
}

static bool open_volume(Vfat **out) {
    VfatResult r = vfat_open(out, &svc->be, &svc->mem, "", svc->budget, drive_free(svc));
    if (r != VFAT_OK) {
        set_error(svc, r == VFAT_NO_MEMORY ? "Not enough memory for the USB drive."
                                            : "Cannot read the calculator's files.", NULL);
        return false;
    }
    return true;
}

/* Calculator-side changes: give the computer a fresh medium. */
static void refresh_volume(Service *s) {
    Vfat *nv;
    uint32_t sig = calc_signature();
    if (!open_volume(&nv)) return;
    Vfat *old = s->vfat;
    s->vfat = nv;
    msc_set_volume(&s->msc, nv);
    vfat_close(old);
    s->calc_sig = sig;
    s->calc_changed = 0;
    ++s->refreshes;
}

static bool confirm_deletes(void *user, unsigned deleted, unsigned total) {
    (void)user;
    char text[160] = "";
    cat(text, "The computer deleted ");
    num(text, deleted);
    cat(text, " of ");
    num(text, total);
    cat(text, " files on the NSPIRE drive.\nDelete them on the calculator too?");
    return ask("nDrive", text, "Delete", "Keep") == 1;
}

static void drive_serve(Service *s, unsigned budget_ms) {
    /* While the volume is being saved the computer is told "becoming ready"
       instead of getting no answer. */
    s->msc.busy = s->syncing != 0;
    if (usbbg_drive_pending()) {
        usbbg_drive_service(&s->msc, &s->st, 80, budget_ms);
        resident_keep_awake();
    }
}

static void sync_progress(void *user, unsigned done, unsigned total, const char *name) {
    (void)user; (void)done; (void)total; (void)name;
    drive_serve(svc, 100);
}

/* Inside one of our dialogs a save must not ask a question of its own. */
static bool defer_deletes(void *user, unsigned deleted, unsigned total) {
    (void)user; (void)deleted; (void)total;
    svc->deferred = 1;
    return false;
}

static bool sync_due(const Service *s, uint32_t now) {
    if (!vfat_dirty(s->vfat) || now - s->st.last_write_ms <= SYNC_QUIET_MS ||
        now - s->st.last_command_ms <= 300) return false;
    /* After a failed save: when the computer has written again, else every 15 s. */
    if (s->sync_fails && s->st.last_write_ms == s->fail_write_ms) return now - s->last_sync_try > 15000;
    return now - s->last_sync_try > 1000;
}

/* nested: called from inside a dialog nDrive opened (no UI of its own). */
static void do_sync(Service *s, bool nested) {
    VfatSyncHooks hooks = { nested ? defer_deletes : confirm_deletes, sync_progress, NULL };
    static VfatReport rep;
    static char path[FS_PATH_MAX];
    s->syncing = 1;
    VfatResult r = vfat_sync(s->vfat, &hooks, &rep);
    s->syncing = 0;
    s->msc.busy = false;
    s->last_sync_try = usb_millis();
    if (r == VFAT_CANCELLED) {
        if (s->deferred) return;                /* asked again outside the dialog */
        /* Deletions refused: show the computer the calculator as it is. */
        refresh_volume(s);
        return;
    }
    if (r != VFAT_OK) {
        /* Usually the computer is half-way through writing; it may also have
           formatted the drive with something other than FAT. */
        s->fail_write_ms = s->st.last_write_ms;
        if (++s->sync_fails < 3) return;
        const char *why = r == VFAT_NO_MEMORY ? "not enough memory." : "the drive's file system cannot be read.";
        if (!link_up()) {
            /* Nothing more can arrive: drop the changes instead of trying forever. */
            set_error(s, "The last changes from the computer were not saved: ", why);
            s->sync_fails = 0;
            s->problem = 1;
            refresh_volume(s);
        } else if (s->sync_fails == 3) {
            set_error(s, "Changes from the computer cannot be saved: ", why);
            s->problem = 1;
        }
        return;
    }
    s->sync_fails = 0;
    ++s->syncs;
    bool changed = rep.files_written || rep.files_moved || rep.files_deleted ||
                   rep.dirs_created || rep.dirs_removed;
    if (rep.errors) {
        /* e.g. calculator storage full. Show the computer what is really there. */
        set_error(s, "Some files from the computer were not saved. ", rep.first_error);
        s->problem = 1;
        refresh_volume(s);
    }
    s->calc_sig = calc_signature();
    if (!changed) return;
    if (nested) {
        s->ui_pending = 1;
        if (rep.last_written[0]) strcpy(s->note, rep.last_written);
        return;
    }
    strcpy(path, rep.last_written);
    announce(s, path);
}

static void drive_work(Service *s) {
    drive_serve(s, 1500);
    uint32_t now = usb_millis();
    s->deferred = 0;
    if (sync_due(s, now)) do_sync(s, false);
    /* Saved while one of our dialogs was open: announce it now. */
    while (s->ui_pending) {
        static char path[FS_PATH_MAX];
        s->ui_pending = 0;
        strcpy(path, s->note);
        s->note[0] = 0;
        announce(s, path);
    }
    now = usb_millis();
    /* Battery: with no computer there is nothing to keep up to date, so no
       scanning at all. On connection, check once for changes made while
       unplugged and offer the computer a fresh medium right away. */
    int link = link_up();
    if (!link) s->seen_activity = usbhook.activity; /* back to the idle fast path */
    if (link != s->connected) {
        s->connected = link;
        s->last_scan = now;
        if (s->connected && !vfat_dirty(s->vfat) && (s->calc_changed || calc_signature() != s->calc_sig))
            refresh_volume(s);
    } else if (s->connected && now - s->last_scan > SCAN_EVERY_MS) {
        s->last_scan = now;
        if (!s->calc_changed && calc_signature() != s->calc_sig) s->calc_changed = 1;
    }
    /* Changed on the calculator (TI link, the user, a program): offer the
       computer a fresh medium once it has nothing pending. */
    if (s->calc_changed && !vfat_dirty(s->vfat) && now - s->st.last_command_ms > REFRESH_IDLE_MS)
        refresh_volume(s);
    /* Give RAM and the scratch file back after big transfers. */
    else if (!vfat_dirty(s->vfat) && vfat_written_blocks(s->vfat) * VFAT_BLOCK > s->budget / 2 &&
             now - s->st.last_command_ms > 10000)
        refresh_volume(s);
}

static bool drive_tick(Service *s) {
    uint32_t now = usb_millis();
    if (usbbg_drive_pending()) return true;
    /* Inside one of our dialogs: only serving and saving go on. */
    if (s->busy) return !s->syncing && !s->deferred && sync_due(s, now);
    if (sync_due(s, now) || s->ui_pending) return true;
    bool link = link_up();
    if (link != (s->connected != 0)) return true;
    if (!link) {
        /* A host that never configures the device: nothing to do for it. */
        if (!vfat_dirty(s->vfat)) s->seen_activity = usbhook.activity;
        return false;
    }
    if (now - s->last_scan > SCAN_EVERY_MS) return true;
    if (s->calc_changed && now - s->st.last_command_ms > REFRESH_IDLE_MS) return true;
    return false;
}

/* ============================================================ both modes */

static void work(Service *s) {
    s->busy = 1;
    if (s->drive) drive_work(s); else mtp_work(s);
    if (s->problem) {
        s->problem = 0;
        problem_dialog(s);
    }
    s->busy = 0;
}

static void reentry(void) {
    /* Inside a dialog we opened (Document Received, a question): keep serving
       the computer, and in drive mode keep saving what it writes. */
    Service *s = svc;
    if (!s || !s->ready) return;
    if (!s->drive) { mtp_serve(s, 300); return; }
    drive_serve(s, 300);
    if (!s->syncing && !s->deferred && sync_due(s, usb_millis())) do_sync(s, true);
}

/* Settings file: one "key=value" per line. Written without stdio so it can be
   saved from the options dialog (an OS callback). */
static void save_config(const Service *s) {
    char text[128];
    text[0] = 0;
    cat(text, s->notify ? "notify=on\n" : "notify=off\n");
    cat(text, s->drive ? "mode=drive\n" : "mode=mtp\n");
    cat(text, s->docs_only ? "root=documents\n" : "root=all\n");
    if (s->big_dma) cat(text, "dma=big\n");
    calcfs_save(CONFIG, text);
}

/* Bring the service in line with s->drive and s->docs_only. Returns an error
   text, or NULL. On an error it falls back to MTP. */
static const char *apply_settings(Service *s) {
    const char *error = NULL;
    if (s->vfat) { vfat_close(s->vfat); s->vfat = NULL; }
    calcfs_set_root(s->docs_only ? "/documents" : "");
    /* NspireLogs.zip is generated on request (as the TI link does); the drive
       needs files that hold still. */
    calcfs_virtual_logs(!s->drive, notify_logs_fn());
    mtp_remount(s->mtp, false, s->docs_only ? "My Documents" : "Calculator");     /* nothing is protected */
    s->ui_pending = 0;
    s->note[0] = 0;
    s->calc_changed = s->sync_fails = s->deferred = 0;
    if (s->drive) {
        s->budget = overlay_budget();
        if (open_volume(&s->vfat)) {
            msc_init(&s->msc, s->vfat);
            s->calc_sig = calc_signature();
            s->last_scan = usb_millis();
        } else {
            s->drive = 0;
            calcfs_virtual_logs(true, notify_logs_fn());
            error = s->error;
        }
    }
    usbhook_set_mtp(!s->drive);
    return error;
}

static void options_dialog(Service *s) {
    char text[200];
    int drive = s->drive, docs = s->docs_only;
    text[0] = 0;
    cat(text, "How should the calculator appear on a computer?\n\nNow: ");
    cat(text, s->drive ? "USB drive." : "MTP device (like a phone).");
    unsigned c = ask("nDrive options", text, "MTP device", "USB drive");
    if (c == 1) drive = 0; else if (c == 2) drive = 1;
    text[0] = 0;
    cat(text, "What should the computer see?\n\nNow: ");
    cat(text, s->docs_only ? "My Documents only." : "the whole calculator.");
    c = ask("nDrive options", text, "Whole calculator", "My Documents");
    if (c == 1) docs = 0; else if (c == 2) docs = 1;
    if (drive == s->drive && docs == s->docs_only) return;
    /* Save first what the computer wrote to the drive. */
    if (s->drive && s->vfat && vfat_dirty(s->vfat)) do_sync(s, false);
    s->drive = drive;
    s->docs_only = docs;
    const char *error = apply_settings(s);
    save_config(s);
    usbhook_reconnect();                /* the computer starts again with the new device */
    if (error) ask("nDrive", error, "OK", "OK");
}

static void status_dialog(Service *s) {
    static char text[700];
    const UsbBgStatus *u = &s->st;
    text[0] = 0;
    cat(text, s->drive ? "USB drive, " : "MTP device, ");
    cat(text, s->docs_only ? "My Documents" : "whole calculator");
    cat(text, link_up() ? ": connected.\n" : ": no computer.\n");
    if (s->drive) {
        num(text, vfat_file_count(s->vfat));
        cat(text, " files, ");
        num(text, vfat_free_bytes(s->vfat) / 1024u);
        cat(text, " KB free on the drive. Saved ");
        num(text, s->syncs);
        cat(text, ", refreshed ");
        num(text, s->refreshes);
        cat(text, " time(s).");
    } else {
        const MtpStats *m = mtp_stats(s->mtp);
        cat(text, "Received "); num(text, m->files_in);
        cat(text, ", sent "); num(text, m->files_out);
        cat(text, ", deleted "); num(text, m->deleted);
        cat(text, ", renamed/moved "); num(text, m->renamed);
        cat(text, ".");
    }
    if (s->error[0]) { cat(text, "\n"); cat(text, s->error); }
    cat(text, "\n\nDiag: ");
    if (s->drive) { cat(text, "cmd "); num(text, u->commands); }
    else {
        const MtpStats *m = mtp_stats(s->mtp);
        cat(text, "op "); num(text, m->commands);
        cat(text, " err "); num(text, m->errors);
        if (m->errors) { cat(text, " ("); hex(text, m->last_op); cat(text, ":"); hex(text, m->last_error); cat(text, ")"); }
        cat(text, " obj "); num(text, mtp_objects(s->mtp));
    }
    cat(text, " cfg "); num(text, usbhook.configs);
    cat(text, " rst "); num(text, usbhook.resets);
    cat(text, "\nin "); num(text, u->bytes_in);
    cat(text, " out "); num(text, u->bytes_out);
    cat(text, " clk "); num(text, usb_clock_rate());
    cat(text, usb_small_dma() ? " small" : " big");
    cat(text, usb_uncached_buffers() ? " U" : " C");
    num(text, usbhook_mps());
    cat(text, "\nusb err "); num(text, u->errors);
    cat(text, " TO in "); num(text, u->in_timeouts);
    cat(text, " out "); num(text, u->out_timeouts);
    cat(text, " wait "); num(text, u->wait_timeouts);
    cat(text, " fault "); num(text, u->dma_faults);
    cat(text, " abort "); num(text, u->aborted);
    cat(text, " zlp "); num(text, u->zlps);
    cat(text, "/"); num(text, u->zlp_timeouts);
    cat(text, "/"); num(text, u->rx_zlps);
    if (u->fail_len) {
        cat(text, "\nfail ch "); num(text, u->fail_ch);
        cat(text, " len "); num(text, u->fail_len);
        cat(text, " op "); hex(text, u->fail_cmd);
        cat(text, " sr "); hex(text, u->fail_sr >> 16); hex(text, u->fail_sr);
        cat(text, " ms "); num(text, u->fail_ms);
        cat(text, " polls "); num(text, u->fail_polls);
    }
    static uint16_t t[16], msg[700], ok[16], more[16];
    utf16(t, "nDrive", 16);
    utf16(msg, text, 700);
    utf16(ok, "OK", 16);
    utf16(more, "Options", 16);
    unsigned pending;
    if (resident_question(t, msg, ok, more, &pending) == 2) options_dialog(s);
}

/* ------------------------------------------------------------ callbacks */

/* No computer, nothing to save or announce: the cheapest possible check
   (memory only, no timer or USB register reads). Anything a host does on the
   bus bumps usbhook.activity, which ends it. */
static bool idle_unplugged(const Service *s) {
    return !s->connected && s->seen_activity == usbhook.activity && !s->ui_pending && !s->problem &&
           !(s->drive && vfat_dirty(s->vfat));
}

static bool service_tick(void *data) {
    Service *s = data;
    ++s->ticks;
    if (!s->ready) return false;
    if (idle_unplugged(s)) return false;
    if (!s->busy && s->problem) return true;
    return s->drive ? drive_tick(s) : mtp_tick(s);
}

static void service_event(void *data, struct s_ns_event *event) {
    Service *s = data;
    ++s->events;
    if (!s->ready || s->busy) return;
    bool hotkey = event->type == 8 && (event->modifiers & 4) &&
                  (event->ascii == 'u' || event->ascii == 'U' || event->ascii == 0x15);
    if (hotkey) {
        event->type = 0;
        s->busy = 1;
        status_dialog(s);
        s->busy = 0;
        return;
    }
    if (idle_unplugged(s)) return;
    work(s);
}

/* -------------------------------------------------------------- install */

static bool cx2_os(void) {
    uint32_t marker = *(volatile uint32_t *)0x10000020u;
    return marker == 0x10429ec0u || marker == 0x1042aee0u || marker == 0x1042a600u;
}

/* Settings file: "notify=on|off" and optionally "dma=big" (long DMA transfers,
   faster; the default sends one USB packet per DMA). */
static bool config_has(const char *what) {
    FILE *f = fopen(CONFIG, "r");
    if (!f) return false;
    char text[96];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return strstr(text, what) != NULL;
}

static int load_notify(void) {
    return !config_has("notify=off");
}


static const char *start_service(Service *s) {
    svc = s;
    if (!calcfs_init(&s->be, &s->mem)) return "This OS is not supported.";
    s->mtp = mtp_create(&s->be, &s->mem, received, s);
    if (!s->mtp) return "Not enough memory.";
    const char *warning = apply_settings(s);
    notify_init();
    usb_clock_calibrate();
    const char *err = usbhook_install();
    if (err) return err;
    resident_set_reentry(reentry);
    s->seen_activity = usbhook.activity - 1u;   /* look at the link once */
    s->magic = 0x4b54444e;
    s->ready = 1;
    /* Already enumerated by a computer: reconnect so it sees the new interface. */
    if (DEVADDR & 0x7f) usbhook_reconnect();
    return warning;
}

int main(void) {
    if (!cx2_os()) {
        if (!nl_isstartup()) show_msgbox("nDrive", "nDrive needs a TI-Nspire CX II (CAS / -T) with OS 6.4.0.74.");
        return 1;
    }
    bool fresh = false;
    const char *error = NULL;
    Service *s = resident_install(NDRIVE_ID, NDRIVE_BUILD_ID, &storage, sizeof(storage),
                                  service_tick, service_event, &fresh, &error);
    if (!s) {
        if (!nl_isstartup()) show_msgbox("nDrive", error ? error : "Install failed.");
        return 1;
    }
    svc = s;
    if (fresh) {
        s->notify = load_notify();
        s->docs_only = config_has("root=documents");
        s->drive = config_has("mode=drive");
        s->big_dma = config_has("dma=big");
        usb_set_small_dma(!s->big_dma);
        s->busy = 1;
        error = start_service(s);
        s->busy = 0;
        if (error && !nl_isstartup()) show_msgbox("nDrive", error);
    }
    if (!nl_isstartup()) {
        for (;;) {
            char text[400];
            sprintf(text, "%s\n\nThe calculator shows up on a computer as %s and still works with "
                    "the TI link at the same time. ctrl then U shows the status and the options "
                    "(MTP or USB drive; whole calculator or My Documents).\n\n"
                    "Put ndrive.tns in /ndless/startup to start it with the calculator.",
                    s->ready ? "nDrive is running." : "nDrive is not running.",
                    s->drive ? "a USB drive" : "an MTP device (like a phone)");
            int choice = show_msgbox_2b("nDrive", text, "OK",
                                        s->notify ? "Notifications: on" : "Notifications: off");
            if (choice == 2) { s->notify = !s->notify; save_config(s); continue; }
            break;
        }
    }
    if (fresh) _exit(0);        /* keep the resident image as it is */
    return 0;
}
