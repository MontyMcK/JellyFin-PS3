// local_resume: see local_resume.h.

#include "local_resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "jf-local-resume 1\n"

uint64_t lres_key(const char *path, uint64_t size, uint64_t mtime) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= (size >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; }
    h ^= 0xFF; h *= 0x100000001B3ull;                  // a separator no path or number byte run can fake
    for (int i = 0; i < 8; i++) { h ^= (mtime >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; }
    return h;
}

bool lres_worth_resuming(uint32_t secs, uint32_t total) {
    if (secs < 30) return false;
    if (total == 0) return true;
    if (secs + 60 > total) return false;
    return (uint64_t)secs * 100 < (uint64_t)total * 95;
}

static int index_of(const LResTable *t, uint64_t key) {
    for (int i = 0; i < t->n; i++) if (t->e[i].key == key) return i;
    return -1;
}

static void remove_at(LResTable *t, int i) {
    memmove(&t->e[i], &t->e[i + 1], (size_t)(t->n - i - 1) * sizeof t->e[0]);
    t->n--;
}

void lres_forget(LResTable *t, uint64_t key) {
    const int i = index_of(t, key);
    if (i >= 0) remove_at(t, i);
}

void lres_update(LResTable *t, uint64_t key, uint32_t secs, uint32_t total) {
    lres_forget(t, key);
    if (!lres_worth_resuming(secs, total)) return;
    if (t->n >= LRES_MAX) remove_at(t, 0);              // the least recently played
    t->e[t->n].key = key; t->e[t->n].secs = secs; t->e[t->n].total = total;
    t->n++;
}

bool lres_find(const LResTable *t, uint64_t key, uint32_t *secs, uint32_t *total) {
    const int i = index_of(t, key);
    if (i < 0) return false;
    if (secs) *secs = t->e[i].secs;
    if (total) *total = t->e[i].total;
    return true;
}

int lres_format(const LResTable *t, char *out, int cap) {
    int n = 0;
    const int hl = (int)strlen(HEADER);
    if (cap < hl + 1) return -1;
    memcpy(out, HEADER, (size_t)hl);
    n = hl;
    for (int i = 0; i < t->n; i++) {
        char line[64];
        const int ll = snprintf(line, sizeof line, "%016llx %u %u\n", (unsigned long long)t->e[i].key,
                                (unsigned)t->e[i].secs, (unsigned)t->e[i].total);
        if (n + ll + 1 > cap) return -1;
        memcpy(out + n, line, (size_t)ll);
        n += ll;
    }
    out[n] = '\0';
    return n;
}

bool lres_parse(LResTable *t, const char *text) {
    t->n = 0;
    const int hl = (int)strlen(HEADER);
    if (strncmp(text, HEADER, (size_t)hl) != 0) return false;
    const char *p = text + hl;
    while (*p) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[96];
        if (len > 0 && len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            unsigned long long key; unsigned secs, total;
            char extra;
            // exactly three fields, the key 16 hex digits
            if (sscanf(line, "%16llx %u %u%c", &key, &secs, &total, &extra) == 3 && strchr(line, ' ') - line == 16) {
                if (t->n >= LRES_MAX) { memmove(&t->e[0], &t->e[1], (size_t)(LRES_MAX - 1) * sizeof t->e[0]); t->n--; }
                // a repeated key keeps its newest line
                for (int i = 0; i < t->n; i++) if (t->e[i].key == (uint64_t)key) { memmove(&t->e[i], &t->e[i + 1], (size_t)(t->n - i - 1) * sizeof t->e[0]); t->n--; break; }
                t->e[t->n].key = (uint64_t)key; t->e[t->n].secs = secs; t->e[t->n].total = total;
                t->n++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return true;
}

bool lres_load_file(LResTable *t, const char *path) {
    t->n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        // a save interrupted between taking the old file away and swapping the new one in
        char tmp[512];
        if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
        f = fopen(tmp, "rb");
    }
    if (!f) return false;
    const int cap = 64 + LRES_MAX * 40;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) { fclose(f); return false; }
    const size_t got = fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    buf[got] = '\0';
    const bool ok = lres_parse(t, buf);
    free(buf);
    return ok;
}

bool lres_save_file(const LResTable *t, const char *path) {
    const int cap = 64 + LRES_MAX * 40;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return false;
    const int len = lres_format(t, buf, cap);
    if (len < 0) { free(buf); return false; }
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) { free(buf); return false; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return false; }
    bool ok = fwrite(buf, 1, (size_t)len, f) == (size_t)len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    free(buf);
    if (!ok) { remove(tmp); return false; }
    // lv2's rename may not replace an existing file: take the old one away first (a crash between
    // the two leaves the .tmp, which lres_load_file falls back to)
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}
