// The saved interface language -- see i18n_store.h.

#include "i18n_store.h"
#include "jf_paths.h"     // jf_data_path()

#include <stdio.h>
#include <string.h>

#include <sysutil/sysutil.h>

#define LANG_FILE "jellyfin_language.txt"

static int s_pref = LANG_PREF_AUTO;

static i18n_lang system_lang(void)
{
    int v = 1;   // English (US)
    if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_LANG, &v) != 0) v = 1;
    return i18n_from_system(v);
}

static void apply(void)
{
    i18n_set(s_pref == LANG_PREF_AUTO ? system_lang() : (i18n_lang)s_pref);
}

int i18n_pref(void) { return s_pref; }

const char *i18n_pref_label(int pref)
{
    if (pref == LANG_PREF_AUTO) return TR("Auto");
    return i18n_native_name((i18n_lang)pref);
}

void i18n_set_pref(int pref)
{
    if (pref < 0 || pref >= LANG_PREF_COUNT) return;
    s_pref = pref;
    apply();
    FILE *f = fopen(jf_data_path(LANG_FILE), "w");
    if (!f) return;
    fprintf(f, "%s\n", pref == LANG_PREF_AUTO ? "auto" : i18n_code((i18n_lang)pref));
    fclose(f);
}

void i18n_step_pref(int dir)
{
    int p = s_pref + (dir < 0 ? -1 : 1);
    if (p < 0) p = LANG_PREF_COUNT - 1;
    if (p >= LANG_PREF_COUNT) p = 0;
    i18n_set_pref(p);
}

void i18n_load(void)
{
    FILE *f = fopen(jf_data_path(LANG_FILE), "r");
    if (f) {
        char word[16] = "";
        if (fscanf(f, "%15s", word) == 1) {
            if (strcmp(word, "auto") == 0) s_pref = LANG_PREF_AUTO;
            else {
                const int l = i18n_from_code(word);
                if (l >= 0) s_pref = l;
            }
        }
        fclose(f);
    }
    apply();
}
