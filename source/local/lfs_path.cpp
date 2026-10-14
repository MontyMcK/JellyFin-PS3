#include "lfs_path.h"

#include <string.h>
#include <strings.h>

static bool drive_ok(const char *d, size_t n) {
    if (n == 3 && memcmp(d, "hdd", 3) == 0) return true;
    return n == 4 && memcmp(d, "usb", 3) == 0 && d[3] >= '0' && d[3] <= '7';
}

bool lfs_path_split(const char *path, char *drive, size_t drive_cap, char *rest, size_t rest_cap) {
    if (!path) return false;
    const char *colon = strchr(path, ':');
    if (!colon || !drive_ok(path, (size_t)(colon - path)) || (size_t)(colon - path) >= drive_cap) return false;
    memcpy(drive, path, (size_t)(colon - path));
    drive[colon - path] = '\0';

    // "/a//b/" -> "/a/b"; "." and ".." are refused
    char tmp[1024];
    size_t n = 0;
    const char *p = colon + 1;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        const size_t len = (size_t)(p - s);
        if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.')) return false;
        if (n + 1 + len + 1 > sizeof tmp) return false;
        tmp[n++] = '/';
        memcpy(tmp + n, s, len);
        n += len;
    }
    if (n == 0) tmp[n++] = '/';
    tmp[n] = '\0';
    if (rest) {
        if (n + 1 > rest_cap) return false;
        memcpy(rest, tmp, n + 1);
    }
    return true;
}

bool lfs_path_join(const char *dir, const char *name, char *out, size_t cap) {
    const size_t dl = strlen(dir), nl = strlen(name);
    const bool slash = dl > 0 && dir[dl - 1] != '/';
    if (dl + (slash ? 1 : 0) + nl + 1 > cap) return false;
    memcpy(out, dir, dl);
    size_t n = dl;
    if (slash) out[n++] = '/';
    memcpy(out + n, name, nl + 1);
    return true;
}

bool lfs_path_parent(const char *path, char *out, size_t cap) {
    const char *colon = strchr(path, ':');
    if (!colon) return false;
    const char *root_end = colon + 2;                  // "usb0:/"
    const size_t len = strlen(path);
    if (len <= (size_t)(root_end - path)) return false;   // a drive root
    size_t end = len;
    while (end > (size_t)(root_end - path) && path[end - 1] == '/') end--;   // trailing slashes
    while (end > (size_t)(root_end - path) && path[end - 1] != '/') end--;   // the leaf
    if (end > (size_t)(root_end - path)) end--;        // the slash before it
    if (end + 1 > cap) return false;
    memcpy(out, path, end);
    out[end] = '\0';
    return true;
}

const char *lfs_path_leaf(const char *path) {
    const char *colon = strchr(path, ':');
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    const char *base = path;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') base = path + i + 1;
    if (colon && base <= colon + 1) return path;       // the drive root: its id
    return base;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool ext_is(const char *name, const char *ext) {
    const size_t nl = strlen(name), el = strlen(ext);
    if (nl < el + 2 || name[nl - el - 1] != '.') return false;
    for (size_t i = 0; i < el; i++) if (lower((unsigned char)name[nl - el + i]) != ext[i]) return false;
    return true;
}

lfs_media_kind lfs_media_kind_of(const char *name) {
    static const char *const video[] = { "mkv", "ts", "m2ts", "mts" };
    static const char *const music[] = { "flac", "mp3", "wav" };
    for (size_t i = 0; i < sizeof video / sizeof video[0]; i++) if (ext_is(name, video[i])) return LFS_KIND_VIDEO;
    for (size_t i = 0; i < sizeof music / sizeof music[0]; i++) if (ext_is(name, music[i])) return LFS_KIND_MUSIC;
    return LFS_KIND_OTHER;
}

bool lfs_name_hidden(const char *name) {
    if (!name[0] || name[0] == '.' || name[0] == '$') return true;
    static const char *const names[] = { "System Volume Information", "lost+found", "found.000", "RECYCLER", "Recycled" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (strcasecmp(name, names[i]) == 0) return true;
    return false;
}

int lfs_natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        const unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            const char *za = a, *zb = b;                       // skip leading zeros
            while (za < ea - 1 && *za == '0') za++;
            while (zb < eb - 1 && *zb == '0') zb++;
            const ptrdiff_t la = ea - za, lb = eb - zb;
            if (la != lb) return la < lb ? -1 : 1;
            const int c = memcmp(za, zb, (size_t)la);
            if (c) return c < 0 ? -1 : 1;
            a = ea; b = eb;
            continue;
        }
        const int la = lower(ca), lb = lower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

int lfs_entry_cmp(bool a_dir, const char *a_name, bool b_dir, const char *b_name) {
    if (a_dir != b_dir) return a_dir ? -1 : 1;
    const int c = lfs_natural_cmp(a_name, b_name);
    return c ? c : strcmp(a_name, b_name);
}
