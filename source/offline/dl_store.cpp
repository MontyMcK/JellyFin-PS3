// Offline downloads -- on-disk layout.  See dl_store.h.

#include "dl_store.h"
#include "dl_platform.h"

#include <stdio.h>
#include <string.h>

#define LAYOUT_FILE  "layout.txt"
#define LAYOUT_MAGIC "jfdl-layout 1\n"
#define TEXT_MAX     4096   // a meta.txt with a full overview is ~2 KB

static char s_root[DL_PATH_MAX] = "";

const char *dl_store_root(void) { return s_root; }

static bool path_join(char *out, int cap, const char *a, const char *b) {
    int n = snprintf(out, (size_t)cap, "%s/%s", a, b);
    if (n < 0 || n >= cap) { if (cap > 0) out[0] = '\0'; return false; }
    return true;
}

// Whole small file into buf (NUL-terminated).  False if missing, empty or
// too big to be one of ours.
static bool read_text(const char *path, char *buf, int cap) {
    const int n = dl_plat_read_file(path, buf, cap);   // cap bytes back = more than cap-1 there
    if (n <= 0 || n >= cap) return false;
    buf[n] = '\0';
    // An embedded NUL means binary junk, not a text record.
    return (int)strlen(buf) == n;
}

static bool write_text_atomic(const char *path, const char *text, int len) {
    char tmp[DL_PATH_MAX + 8];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || tn >= (int)sizeof(tmp)) return false;
    if (!dl_plat_write_file(tmp, text, len)) { dl_plat_remove(tmp); return false; }
    // lv2's rename does not promise to replace an existing file, so remove
    // first.  The window between the two leaves only the .tmp, which load
    // falls back to.
    dl_plat_remove(path);
    return dl_plat_rename(tmp, path);
}

bool dl_store_init(const char *root) {
    if (!root || !root[0] || strlen(root) >= sizeof(s_root) - 64) return false;
    snprintf(s_root, sizeof(s_root), "%s", root);
    char p[DL_PATH_MAX];
    if (!dl_plat_mkdir(s_root)) { s_root[0] = '\0'; return false; }
    if (!path_join(p, sizeof(p), s_root, "items") || !dl_plat_mkdir(p)) {
        s_root[0] = '\0';
        return false;
    }
    // Writing the marker doubles as the "is this root actually writable?"
    // probe -- mkdir can succeed on a directory that already exists on a
    // read-only mount.
    if (!path_join(p, sizeof(p), s_root, LAYOUT_FILE) ||
        !write_text_atomic(p, LAYOUT_MAGIC, (int)strlen(LAYOUT_MAGIC))) {
        s_root[0] = '\0';
        return false;
    }
    return true;
}

bool dl_store_item_dir(char *out, int cap, const char *id) {
    if (cap > 0) out[0] = '\0';
    if (!s_root[0] || !dl_id_valid(id)) return false;
    int n = snprintf(out, (size_t)cap, "%s/items/%s", s_root, id);
    if (n < 0 || n >= cap) { if (cap > 0) out[0] = '\0'; return false; }
    return true;
}

bool dl_store_item_file(char *out, int cap, const char *id, const char *leaf) {
    char dir[DL_PATH_MAX];
    if (!dl_store_item_dir(dir, sizeof(dir), id)) { if (cap > 0) out[0] = '\0'; return false; }
    return path_join(out, cap, dir, leaf);
}

bool dl_store_make_item_dir(const char *id) {
    char dir[DL_PATH_MAX];
    return dl_store_item_dir(dir, sizeof(dir), id) && dl_plat_mkdir(dir);
}

// Load path, falling back to path.tmp; `parse` decides what "damaged" means.
template <typename T>
static bool load_with_fallback(const char *id, const char *leaf, T *out,
                               bool (*parse)(const char *, T *)) {
    char path[DL_PATH_MAX], tmp[DL_PATH_MAX + 8];
    if (!dl_store_item_file(path, sizeof(path), id, leaf)) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    static char text[TEXT_MAX];   // callers hold the manager lock or are init
    if (read_text(path, text, sizeof(text)) && parse(text, out)) return true;
    return read_text(tmp, text, sizeof(text)) && parse(text, out);
}

bool dl_store_save_record(const DlRecord *r) {
    char path[DL_PATH_MAX];
    char text[TEXT_MAX];          // url + two artwork urls + the rest: < 3 KB
    if (!dl_store_item_file(path, sizeof(path), r->id, DL_FILE_STATE)) return false;
    int n = dl_record_format(r, text, sizeof(text));
    return n > 0 && write_text_atomic(path, text, n);
}

bool dl_store_load_record(const char *id, DlRecord *out) {
    DlRecord r;
    if (!load_with_fallback(id, DL_FILE_STATE, &r, dl_record_parse)) return false;
    // A record filed under the wrong directory is not this item's record.
    if (strcmp(r.id, id) != 0) return false;
    *out = r;
    return true;
}

bool dl_store_save_meta(const DlMeta *m) {
    char path[DL_PATH_MAX];
    static char text[TEXT_MAX];   // callers hold the manager lock or are init
    if (!dl_store_item_file(path, sizeof(path), m->id, DL_FILE_META)) return false;
    int n = dl_meta_format(m, text, sizeof(text));
    return n > 0 && write_text_atomic(path, text, n);
}

bool dl_store_load_meta(const char *id, DlMeta *out) {
    static DlMeta m;              // ~2 KB; same locking rule as above
    if (!load_with_fallback(id, DL_FILE_META, &m, dl_meta_parse)) return false;
    if (strcmp(m.id, id) != 0) return false;
    *out = m;
    return true;
}

static const char *const k_item_files[] = {
    DL_FILE_STATE, DL_FILE_STATE ".tmp",
    DL_FILE_META,  DL_FILE_META ".tmp",
    DL_FILE_PART,  DL_FILE_MEDIA,
    DL_FILE_POSTER, DL_FILE_POSTER ".tmp",
    DL_FILE_BACKDROP, DL_FILE_BACKDROP ".tmp",
};

bool dl_store_remove_item(const char *id) {
    char dir[DL_PATH_MAX], path[DL_PATH_MAX];
    if (!dl_store_item_dir(dir, sizeof(dir), id)) return false;
    // Media first: it is the space the user is asking for back, and if the
    // record goes first a crash here would strand a multi-GB file with no
    // record pointing at it.  With media gone first, the worst case is a
    // record for an item whose media is missing -- which restore reports.
    static const char *const order[] = { DL_FILE_MEDIA, DL_FILE_PART };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (path_join(path, sizeof(path), dir, order[i])) dl_plat_remove(path);
    for (unsigned i = 0; i < sizeof(k_item_files) / sizeof(k_item_files[0]); i++)
        if (path_join(path, sizeof(path), dir, k_item_files[i])) dl_plat_remove(path);
    dl_plat_rmdir(dir);
    return !dl_plat_exists(dir);
}

bool dl_store_save_blob(const char *id, const char *leaf,
                        const uint8_t *data, int len) {
    char path[DL_PATH_MAX], tmp[DL_PATH_MAX + 8];
    if (!dl_store_item_file(path, sizeof(path), id, leaf)) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!dl_plat_truncate(tmp)) return false;
    int fh = dl_plat_file_open_append(tmp);
    if (fh < 0) return false;
    bool ok = dl_plat_file_write(fh, data, len) == len;
    ok = dl_plat_file_sync(fh) && ok;
    dl_plat_file_close(fh);
    if (!ok) { dl_plat_remove(tmp); return false; }
    dl_plat_remove(path);
    return dl_plat_rename(tmp, path);
}

bool dl_store_remove_partial(const char *id) {
    char path[DL_PATH_MAX];
    return dl_store_item_file(path, sizeof(path), id, DL_FILE_PART) &&
           dl_plat_remove(path);
}

struct ListCtx { char (*ids)[DL_ID_MAX]; int max; int n; };

static void list_cb(const char *name, void *vctx) {
    ListCtx *c = (ListCtx *)vctx;
    if (c->n >= c->max || !dl_id_valid(name)) return;
    snprintf(c->ids[c->n], DL_ID_MAX, "%s", name);
    c->n++;
}

int dl_store_list_ids(char (*ids)[DL_ID_MAX], int max) {
    char items[DL_PATH_MAX];
    if (!s_root[0] || !path_join(items, sizeof(items), s_root, "items")) return 0;
    ListCtx c = { ids, max, 0 };
    if (dl_plat_list_dirs(items, list_cb, &c) < 0) return 0;
    return c.n;
}
