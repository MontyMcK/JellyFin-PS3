// Host test for the interface language (source/i18n/i18n.c and the generated
// catalog tables): lookup, fallback, the language codes and the console
// language mapping.  The catalog's own consistency (every marked string has
// all five translations, same conversions) is tools/i18n.py check.
//
//   make -f Makefile.host test_i18n
#include <stdio.h>
#include <string.h>

#include "i18n.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void lookup(void) {
    printf("- lookup\n");
    i18n_set(LANG_EN);
    CHECK(strcmp(tr("Playback"), "Playback") == 0);
    CHECK(strcmp(tr("not in the catalog"), "not in the catalog") == 0);
    CHECK(strcmp(tr(""), "") == 0);

    i18n_set(LANG_DE);
    CHECK(i18n_current() == LANG_DE);
    CHECK(strcmp(tr("Playback"), "Wiedergabe") == 0);
    // Not in the catalog: the English comes back, the very same pointer.
    const char *s = "Some server-supplied title";
    CHECK(tr(s) == s);

    i18n_set(LANG_JA);
    CHECK(strcmp(tr("Playback"), "\xE5\x86\x8D\xE7\x94\x9F") == 0);   // 再生
    i18n_set(LANG_FR);
    CHECK(strcmp(tr("Log Out"), "Se d\xC3\xA9" "connecter") == 0);
    i18n_set(LANG_ES);
    CHECK(strcmp(tr("Log Out"), "Cerrar sesi\xC3\xB3n") == 0);
    i18n_set(LANG_PT_BR);
    CHECK(strcmp(tr("Log Out"), "Sair") == 0);

    i18n_set((i18n_lang)99);                 // ignored
    CHECK(i18n_current() == LANG_PT_BR);
    i18n_set(LANG_EN);
}

static void catalog(void) {
    printf("- catalog\n");
    const int n = i18n_catalog_size();
    CHECK(n > 0);
    for (int i = 0; i < n; i++) {
        const char *k = i18n_catalog_key(i);
        CHECK(k && *k);
        if (i > 0) CHECK(strcmp(i18n_catalog_key(i - 1), k) < 0);       // sorted, no duplicates
        for (int l = LANG_JA; l < LANG__COUNT; l++) {
            const char *t = i18n_catalog_text((i18n_lang)l, i);
            CHECK(t && *t);
            // Every entry is found by the binary search in every language.
            i18n_set((i18n_lang)l);
            CHECK(strcmp(tr(k), t) == 0);
        }
    }
    CHECK(i18n_catalog_text(LANG_EN, 0) == NULL);
    CHECK(i18n_catalog_key(-1) == NULL && i18n_catalog_key(n) == NULL);
    i18n_set(LANG_EN);
}

static void codes(void) {
    printf("- codes and system language\n");
    for (int l = 0; l < LANG__COUNT; l++) {
        CHECK(i18n_from_code(i18n_code((i18n_lang)l)) == l);
        CHECK(strlen(i18n_native_name((i18n_lang)l)) > 0);
    }
    CHECK(i18n_from_code("xx") == -1 && i18n_from_code("") == -1 && i18n_from_code(NULL) == -1);
    CHECK(strcmp(i18n_native_name(LANG_JA), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == 0);
    CHECK(strcmp(i18n_native_name(LANG_PT_BR), "Portugu\xC3\xAAs (Brasil)") == 0);

    CHECK(i18n_from_system(0) == LANG_JA);
    CHECK(i18n_from_system(1) == LANG_EN);
    CHECK(i18n_from_system(18) == LANG_EN);       // English (UK)
    CHECK(i18n_from_system(2) == LANG_FR);
    CHECK(i18n_from_system(3) == LANG_ES);
    CHECK(i18n_from_system(4) == LANG_DE);
    CHECK(i18n_from_system(7) == LANG_PT_BR);     // Portugal
    CHECK(i18n_from_system(17) == LANG_PT_BR);    // Brazil
    CHECK(i18n_from_system(5) == LANG_EN);        // Italian: no catalog
    CHECK(i18n_from_system(-1) == LANG_EN && i18n_from_system(99) == LANG_EN);
}

int main(void) {
    lookup();
    catalog();
    codes();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
