// Interface language lookup -- see i18n.h.

#include "i18n.h"

#include <string.h>

// Generated from catalog.json by tools/gen_i18n.py.  The keys are sorted by
// strcmp, which is what the binary search below relies on.
extern const int         i18n_n;
extern const char *const i18n_keys[];
extern const char *const *const i18n_text[LANG__COUNT];

static volatile i18n_lang s_lang = LANG_EN;

void      i18n_set(i18n_lang lang) { if ((int)lang >= 0 && lang < LANG__COUNT) s_lang = lang; }
i18n_lang i18n_current(void)       { return s_lang; }

const char *tr(const char *en)
{
    const i18n_lang lang = s_lang;
    if (lang == LANG_EN || !en || !*en) return en;

    int lo = 0, hi = i18n_n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = strcmp(en, i18n_keys[mid]);
        if (c == 0) {
            const char *t = i18n_text[lang][mid];
            return (t && *t) ? t : en;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return en;
}

int         i18n_catalog_size(void)   { return i18n_n; }
const char *i18n_catalog_key(int i)   { return (i >= 0 && i < i18n_n) ? i18n_keys[i] : NULL; }
const char *i18n_catalog_text(i18n_lang lang, int i)
{
    if (lang == LANG_EN || (int)lang < 0 || lang >= LANG__COUNT || i < 0 || i >= i18n_n) return NULL;
    return i18n_text[lang][i];
}

// The names are the languages' own, in their own script, so a person who has
// set the wrong language can still find their way back.
static const char *const k_native[LANG__COUNT] = {
    "English", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "Portugu\xC3\xAAs (Brasil)",
    "Deutsch", "Fran\xC3\xA7" "ais", "Espa\xC3\xB1" "ol",
};
static const char *const k_code[LANG__COUNT] = { "en", "ja", "pt-BR", "de", "fr", "es" };

const char *i18n_native_name(i18n_lang lang)
{
    return ((int)lang >= 0 && lang < LANG__COUNT) ? k_native[lang] : k_native[LANG_EN];
}
const char *i18n_code(i18n_lang lang)
{
    return ((int)lang >= 0 && lang < LANG__COUNT) ? k_code[lang] : k_code[LANG_EN];
}
int i18n_from_code(const char *code)
{
    if (!code) return -1;
    for (int i = 0; i < LANG__COUNT; i++)
        if (strcmp(code, k_code[i]) == 0) return i;
    return -1;
}

// SYSUTIL_LANG_*: 0 Japanese, 1 English (US), 2 French, 3 Spanish, 4 German,
// 5 Italian, 6 Dutch, 7 Portuguese (Portugal), 8 Russian, 9 Korean,
// 10 Chinese (Traditional), 11 Chinese (Simplified), 12 Finnish, 13 Swedish,
// 14 Danish, 15 Norwegian, 16 Polish, 17 Portuguese (Brazil), 18 English (UK).
i18n_lang i18n_from_system(int sysutil_lang)
{
    switch (sysutil_lang) {
    case 0:  return LANG_JA;
    case 2:  return LANG_FR;
    case 3:  return LANG_ES;
    case 4:  return LANG_DE;
    case 7:
    case 17: return LANG_PT_BR;
    default: return LANG_EN;
    }
}
