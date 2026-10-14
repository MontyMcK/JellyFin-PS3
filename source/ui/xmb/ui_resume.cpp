// Where items last stopped -- see ui_resume.h.

#include <string.h>

#include "ui_resume.h"

uint32_t resume_secs_after(uint32_t pos, uint32_t total) {
    if (pos < 10) return 0;
    if (total && (uint64_t)pos * 100 < (uint64_t)total * 5)  return 0;
    if (total && (uint64_t)pos * 100 >= (uint64_t)total * 90) return 0;
    return pos;
}

// A few dozen is plenty: it only has to outlive the screens that hold the
// stale item.  Oldest entry goes first.
#define RESUME_SLOTS 64
static struct { char id[64]; uint32_t secs; } s_tab[RESUME_SLOTS];
static int s_n = 0;

void xmb_resume_note(const char *id, uint32_t pos, uint32_t total) {
    if (!id || !id[0]) return;
    const uint32_t secs = resume_secs_after(pos, total);
    int i = 0;
    while (i < s_n && strcmp(s_tab[i].id, id) != 0) i++;
    if (i == s_n) {
        if (s_n == RESUME_SLOTS) {
            memmove(&s_tab[0], &s_tab[1], sizeof s_tab[0] * (RESUME_SLOTS - 1));
            i = RESUME_SLOTS - 1;
        } else {
            s_n++;
        }
        strncpy(s_tab[i].id, id, sizeof s_tab[i].id - 1);
        s_tab[i].id[sizeof s_tab[i].id - 1] = '\0';
    }
    s_tab[i].secs = secs;
}

uint32_t xmb_resume_for(const char *id, uint32_t listed) {
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_tab[i].id, id) == 0) return s_tab[i].secs;
    return listed;
}
