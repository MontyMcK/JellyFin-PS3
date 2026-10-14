// Music on a drive, read through lfs: see local_music_fs.h.

#include "local_music_fs.h"

#include <stdio.h>
#include <string.h>

#include "local_audio.h"
#include "lfs_path.h"

static int rd(void *ctx, uint64_t off, uint8_t *buf, int len) {
    return lfs_read(*(const int *)ctx, off, buf, (uint32_t)len);
}

int lm_scan_folder(const char *dir, const lfs_entry *entries, int n, LmTrack *out, int max, int *skipped) {
    int count = 0, skip = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir || lfs_media_kind_of(entries[i].name) != LFS_KIND_MUSIC) continue;
        if (count >= max) { skip++; continue; }
        char path[256];
        if (!lfs_path_join(dir, entries[i].name, path, sizeof path)) { skip++; continue; }
        const int fd = lfs_open(path);
        if (fd < 0) { skip++; continue; }
        LaMeta m;
        const bool ok = la_read_meta(rd, (void *)&fd, lfs_size(fd), la_kind_of(entries[i].name), &m) && la_can_decode(&m);
        lfs_close(fd);
        if (!ok) { skip++; continue; }
        lm_track_fill(&out[count++], path, &m);
    }
    if (lm_is_one_album(out, count)) lm_sort(out, count);              // an album plays in its own order, a pile of songs in the listing's
    if (skipped) *skipped = skip;
    return count;
}

bool lm_register_cover(const char *dir, const LmTrack *t, int n, char *key, int cap) {
    if (cap > 0) key[0] = '\0';
    const int with_pic = lm_pick_art(t, n);
    if (with_pic >= 0) {
        lm_art_register(t[with_pic].path, t[with_pic].pic_off, t[with_pic].pic_len, key, cap);
        return true;
    }
    static const char *const bases[] = { "cover", "folder", "front", "albumart", "album" };
    static const char *const exts[] = { ".jpg", ".jpeg", ".png" };
    for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
            for (int upper = 0; upper < 2; upper++) {
                char name[40], path[256];
                snprintf(name, sizeof name, "%s%s", bases[b], exts[e]);
                if (upper && name[0] >= 'a' && name[0] <= 'z') name[0] = (char)(name[0] - 32);
                if (!lfs_path_join(dir, name, path, sizeof path)) continue;
                lfs_entry ent;
                if (lfs_stat(path, &ent) && !ent.is_dir && ent.size > 0) {
                    lm_art_register(path, 0, 0, key, cap);
                    return true;
                }
            }
        }
    }
    return false;
}

int lm_art_bytes(const char *key, unsigned char *buf, int cap) {
    if (!lm_art_is_key(key)) return 0;
    LmArt art;
    if (!lm_art_find(key, &art)) return -1;
    const int fd = lfs_open(art.path);
    if (fd < 0) return -1;
    uint64_t want = art.len;
    if (want == 0) want = lfs_size(fd);
    int got = -1;
    if (want > 0 && want <= (uint64_t)cap) {
        int have = 0;
        while ((uint64_t)have < want) {
            const int r = lfs_read(fd, art.off + (uint64_t)have, buf + have, (uint32_t)(want - (uint64_t)have));
            if (r <= 0) break;
            have += r;
        }
        if ((uint64_t)have == want) got = have;
    }
    lfs_close(fd);
    return got;
}
