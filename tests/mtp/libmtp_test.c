/* Drives nDrive's MTP responder through real libmtp, the way KDE's and GNOME's
 * file managers do (folder by folder), and checks the "calculator" directory.
 * usage: LD_PRELOAD=fakeusb.so NDRIVE_ROOT=dir libmtp_test dir workdir */
#define _GNU_SOURCE
#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static const char *root, *work;

static void ok(int c, const char *what) {
    printf("%s %s\n", c ? "PASS" : "FAIL", what);
    failures += !c;
}

static char *p(const char *base, const char *rel) {
    static char buf[8][2048];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, 2048, "%s/%s", base, rel);
    return b;
}

static void make(const char *path, size_t n, unsigned seed) {
    FILE *f = fopen(path, "wb");
    srand(seed);
    for (size_t i = 0; i < n; ++i) fputc(rand() & 0xff, f);
    fclose(f);
}

static int same(const char *a, const char *b) {
    char cmd[4600];
    snprintf(cmd, sizeof(cmd), "cmp -s '%s' '%s'", a, b);
    return system(cmd) == 0;
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

/* Child of a folder by name, as a file manager finds it. */
static uint32_t find(LIBMTP_mtpdevice_t *d, uint32_t storage, uint32_t parent, const char *name, uint64_t *size, int *is_dir) {
    uint32_t id = 0;
    LIBMTP_file_t *f = LIBMTP_Get_Files_And_Folders(d, storage, parent);
    while (f) {
        if (!strcmp(f->filename, name)) {
            id = f->item_id;
            if (size) *size = f->filesize;
            if (is_dir) *is_dir = f->filetype == LIBMTP_FILETYPE_FOLDER;
        }
        LIBMTP_file_t *n = f->next;
        LIBMTP_destroy_file_t(f);
        f = n;
    }
    return id;
}

static uint32_t send(LIBMTP_mtpdevice_t *d, uint32_t storage, uint32_t parent, const char *local, const char *name) {
    struct stat st;
    stat(local, &st);
    LIBMTP_file_t *f = LIBMTP_new_file_t();
    f->filename = strdup(name);
    f->filesize = (uint64_t)st.st_size;
    f->filetype = LIBMTP_FILETYPE_UNKNOWN;
    f->parent_id = parent;
    f->storage_id = storage;
    int r = LIBMTP_Send_File_From_File(d, local, f, NULL, NULL);
    uint32_t id = r ? 0 : f->item_id;
    if (r) LIBMTP_Dump_Errorstack(d);
    LIBMTP_Clear_Errorstack(d);
    LIBMTP_destroy_file_t(f);
    return id;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    root = argv[1];
    work = argv[2];
    LIBMTP_Init();
    LIBMTP_raw_device_t *raw;
    int n = 0;
    ok(LIBMTP_Detect_Raw_Devices(&raw, &n) == LIBMTP_ERROR_NONE && n == 1, "libmtp recognises the device as MTP");
    if (n != 1) return 1;
    LIBMTP_mtpdevice_t *d = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
    ok(d != NULL, "session opens");
    if (!d) return 1;
    char *model = LIBMTP_Get_Modelname(d);
    ok(model && !strcmp(model, "TI-Nspire CX II"), "model name");
    ok(LIBMTP_Get_Storage(d, LIBMTP_STORAGE_SORTBY_NOTSORTED) == 0 && d->storage, "storage listed");
    uint32_t st = d->storage->id;
    const uint32_t ROOT = LIBMTP_FILES_AND_FOLDERS_ROOT;

    /* --- browsing: the calculator's root, not just /documents */
    int is_dir = 0;
    uint64_t size = 0;
    uint32_t docs = find(d, st, ROOT, "documents", NULL, &is_dir);
    ok(docs && is_dir, "root shows /documents");
    ok(find(d, st, ROOT, "phoenix", NULL, &is_dir) && is_dir, "root shows system folders too");
    uint32_t a = find(d, st, docs, "a.tns", &size, &is_dir);
    ok(a && !is_dir && size == 6, "file listed with its size");
    uint32_t ex = find(d, st, docs, "Examples", NULL, NULL);
    uint32_t big = find(d, st, ex, "big.tns", &size, NULL);
    ok(big && size == 100000, "nested folder listed");

    /* --- download */
    ok(LIBMTP_Get_File_To_File(d, big, p(work, "big.out"), NULL, NULL) == 0 &&
       same(p(work, "big.out"), p(root, "documents/Examples/big.tns")), "download 100 kB file");
    uint32_t exact = find(d, st, docs, "exact.tns", NULL, NULL);
    ok(LIBMTP_Get_File_To_File(d, exact, p(work, "exact.out"), NULL, NULL) == 0 &&
       same(p(work, "exact.out"), p(root, "documents/exact.tns")),
       "download a file that ends exactly on a packet boundary (zero-length packet)");
    ok(LIBMTP_Get_File_To_File(d, a, p(work, "a.out"), NULL, NULL) == 0 &&
       same(p(work, "a.out"), p(root, "documents/a.tns")), "download after that still in step");

    /* --- upload, sizes around packet boundaries */
    static const size_t sizes[] = { 0, 1, 499, 500, 501, 511, 512, 513, 1012, 1024, 70000, 3000000 };
    int all = 1;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        char name[64], msg[96];
        snprintf(name, sizeof(name), "up%zu.tns", sizes[i]);
        make(p(work, name), sizes[i], 100 + i);
        uint32_t id = send(d, st, docs, p(work, name), name);
        char rel[128];
        snprintf(rel, sizeof(rel), "documents/%s", name);
        int good = id && same(p(work, name), p(root, rel));
        snprintf(msg, sizeof(msg), "upload %zu bytes", sizes[i]);
        if (!good) ok(0, msg);
        all &= good;
    }
    ok(all, "uploads of 0 B to 3 MB arrive intact (including packet-boundary sizes)");
    ok(find(d, st, docs, "up70000.tns", &size, NULL) && size == 70000, "uploaded file shows in the listing");

    /* --- overwrite an existing file */
    make(p(work, "new_a"), 4321, 7);
    ok(send(d, st, docs, p(work, "new_a"), "a.tns") && same(p(work, "new_a"), p(root, "documents/a.tns")),
       "sending a name that exists replaces the file");

    /* --- folders */
    uint32_t folder = LIBMTP_Create_Folder(d, strdup("FromPC"), docs, st);
    ok(folder && exists(p(root, "documents/FromPC")), "create folder");
    make(p(work, "note"), 2000, 3);
    uint32_t note = send(d, st, folder, p(work, "note"), "note.tns");
    ok(note && same(p(work, "note"), p(root, "documents/FromPC/note.tns")), "upload into the new folder");

    /* --- rename and move */
    LIBMTP_file_t *meta = LIBMTP_Get_Filemetadata(d, note);
    ok(meta && LIBMTP_Set_File_Name(d, meta, "renamed.tns") == 0 &&
       exists(p(root, "documents/FromPC/renamed.tns")) && !exists(p(root, "documents/FromPC/note.tns")), "rename file");
    if (meta) LIBMTP_destroy_file_t(meta);
    ok(LIBMTP_Move_Object(d, note, st, docs) == 0 && exists(p(root, "documents/renamed.tns")) &&
       !exists(p(root, "documents/FromPC/renamed.tns")), "move file to another folder");
    ok(find(d, st, docs, "renamed.tns", &size, NULL) == note && size == 2000, "moved file keeps its handle");

    /* --- delete */
    ok(LIBMTP_Delete_Object(d, note) == 0 && !exists(p(root, "documents/renamed.tns")), "delete file");
    ok(LIBMTP_Delete_Object(d, ex) == 0 && !exists(p(root, "documents/Examples")), "delete folder with contents");
    ok(find(d, st, docs, "Examples", NULL, NULL) == 0, "deleted folder gone from listing");

    uint32_t ph = find(d, st, ROOT, "phoenix", NULL, NULL);
    ok(LIBMTP_Delete_Object(d, ph) == 0 && !exists(p(root, "phoenix")),
       "nothing is protected: a top-level folder can be deleted too");
    LIBMTP_Clear_Errorstack(d);

    /* --- a file that appeared on the calculator meanwhile (e.g. over the TI link) */
    make(p(root, "documents/fromlink.tns"), 30000, 9);
    uint32_t fl = find(d, st, docs, "fromlink.tns", &size, NULL);
    ok(fl && size == 30000 && LIBMTP_Get_File_To_File(d, fl, p(work, "fl.out"), NULL, NULL) == 0 &&
       same(p(work, "fl.out"), p(root, "documents/fromlink.tns")), "file added on the calculator shows on refresh");

    /* --- errors: unknown handle, then things still work */
    ok(LIBMTP_Get_File_To_File(d, 999999, p(work, "none"), NULL, NULL) != 0, "unknown object refused");
    LIBMTP_Clear_Errorstack(d);
    ok(find(d, st, docs, "a.tns", &size, NULL) == a && size == 4321, "session fine after an error");

    LIBMTP_Release_Device(d);
    printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures != 0;
}
