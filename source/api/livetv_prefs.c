#include "livetv_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lp_init(LtPrefs *p) { memset(p, 0, sizeof *p); }

static bool id_ok(const char *s) {
    if (!s[0]) return false;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') || *s == '-'))
            return false;
    return true;
}

bool lp_is_hidden(const LtPrefs *p, int key) {
    for (int i = 0; i < p->n_hidden; i++) if (p->hidden[i] == key) return true;
    return false;
}

void lp_set_hidden(LtPrefs *p, int key, bool hidden) {
    if (key == 0) return;
    for (int i = 0; i < p->n_hidden; i++) {
        if (p->hidden[i] != key) continue;
        if (!hidden) {
            memmove(&p->hidden[i], &p->hidden[i + 1], sizeof(int) * (size_t)(p->n_hidden - i - 1));
            p->n_hidden--;
        }
        return;
    }
    if (hidden && p->n_hidden < LP_MAX_HIDDEN) p->hidden[p->n_hidden++] = key;
}

int lp_recent_rank(const LtPrefs *p, const char *id) {
    if (!id) return LP_MAX_RECENT;
    for (int i = 0; i < p->n_recent; i++)
        if (strcmp(p->recent[i], id) == 0) return i;
    return LP_MAX_RECENT;
}

void lp_note_recent(LtPrefs *p, const char *id) {
    if (!id || strlen(id) >= LP_ID_MAX || !id_ok(id)) return;
    int at = lp_recent_rank(p, id);
    if (at == LP_MAX_RECENT) {
        at = p->n_recent < LP_MAX_RECENT ? p->n_recent++ : LP_MAX_RECENT - 1;
    }
    memmove(&p->recent[1], &p->recent[0], sizeof p->recent[0] * (size_t)at);
    snprintf(p->recent[0], LP_ID_MAX, "%s", id);
}

int lp_parse(const char *text, LtPrefs *p) {
    lp_init(p);
    if (!text) return 0;
    int used = 0;
    const char *s = text;
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char line[96];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = '\0';
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
        s = e ? e + 1 : s + strlen(s);

        char word[16], arg[LP_ID_MAX + 8];
        if (sscanf(line, "%15s %47s", word, arg) != 2) continue;
        char *end = NULL;
        if (!strcmp(word, "hide")) {
            const long k = strtol(arg, &end, 10);
            if (*end || k == 0 || k > 100000 || k < -2) continue;
            lp_set_hidden(p, (int)k, true);
            used++;
        } else if (!strcmp(word, "last")) {
            const long k = strtol(arg, &end, 10);
            if (*end || k == 0 || k > 100000 || k < -2) continue;
            p->last = (int)k;
            used++;
        } else if (!strcmp(word, "recent")) {
            if (strlen(arg) >= LP_ID_MAX || !id_ok(arg)) continue;
            // The file lists newest first: append, keeping that order.
            if (lp_recent_rank(p, arg) != LP_MAX_RECENT || p->n_recent >= LP_MAX_RECENT) continue;
            snprintf(p->recent[p->n_recent++], LP_ID_MAX, "%s", arg);
            used++;
        }
    }
    return used;
}

int lp_format(const LtPrefs *p, char *out, int cap) {
    int n = 0;
#define PUT(...) do { \
        const int w = snprintf(out + n, (size_t)(cap - n), __VA_ARGS__); \
        if (w < 0 || w >= cap - n) { return -1; } \
        n += w; \
    } while (0)
    if (cap <= 0) return -1;
    out[0] = '\0';
    for (int i = 0; i < p->n_hidden; i++) PUT("hide %d\n", p->hidden[i]);
    for (int i = 0; i < p->n_recent; i++) PUT("recent %s\n", p->recent[i]);
    if (p->last) PUT("last %d\n", p->last);
#undef PUT
    return n;
}

int lp_visible_categories(const LtPrefs *p, const int *bases, int n_bases) {
    int n = 0;
    for (int i = 0; i < n_bases; i++) if (!lp_is_hidden(p, bases[i])) n++;
    return n;
}

int lp_build_pages(const LtPrefs *p, const int *bases, int n_bases, int *out, int max) {
    int n = 0;
    if (!lp_is_hidden(p, LP_FAVS) && n < max) out[n++] = LP_FAVS;
    if (!lp_is_hidden(p, LP_RECENT) && p->n_recent > 0 && n < max) out[n++] = LP_RECENT;
    const bool all = lp_visible_categories(p, bases, n_bases) == 0;
    for (int i = 0; i < n_bases && n < max; i++)
        if (all || !lp_is_hidden(p, bases[i])) out[n++] = bases[i];
    return n;
}

int lp_pick_page(const int *keys, int n, int want) {
    if (want != 0)
        for (int i = 0; i < n; i++) if (keys[i] == want) return i;
    for (int i = 0; i < n; i++) if (keys[i] > 0) return i;
    return 0;
}
