#pragma once

// -------------------------------------------------------------------------
//  Interface language
// -------------------------------------------------------------------------
//  Every user-facing string in the app is written in English at its use and
//  looked up here by that English text:
//
//      drawTTF(x, y, TR("Continue Watching"), ...);
//      snprintf(buf, n, TR("%d episodes"), count);
//
//  TR() returns the current language's text, or the English itself when the
//  language is English or the catalog has no entry.  TRN() marks a literal
//  that cannot be translated where it is written (a static table, a
//  compile-time initialiser): it is the literal unchanged, and the code that
//  reads the table calls tr() on it.  tools/i18n.py check finds both markers,
//  and fails when a marked string has no complete catalog row.
//
//  The catalog is source/i18n/catalog.json; tools/gen_i18n.py turns it into
//  i18n_tables.c.  A format string keeps its conversions, in the same order,
//  in every language (the checker enforces that).
//
//  Pure C with no PS3 includes, so tests/test_i18n.c compiles this exact file.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_EN, LANG_JA, LANG_PT_BR, LANG_DE, LANG_FR, LANG_ES,
    LANG__COUNT
} i18n_lang;

// The stored preference: a language, or "Auto" = follow the console.
#define LANG_PREF_AUTO  ((int)LANG__COUNT)
#define LANG_PREF_COUNT ((int)LANG__COUNT + 1)

#define TR(s)   tr(s)
#define TRN(s)  (s)

// The text in the current language; `en` itself when there is none.  Never NULL
// for a non-NULL argument.  Cheap enough to call every frame.
const char *tr(const char *en);

// The language tr() answers in.
void      i18n_set(i18n_lang lang);
i18n_lang i18n_current(void);

// The language's own name ("Deutsch"), the file code ("de", "pt-BR") and back.
// An unknown code gives -1.
const char *i18n_native_name(i18n_lang lang);
const char *i18n_code(i18n_lang lang);
int         i18n_from_code(const char *code);   // i18n_lang, or -1

// The language for a console whose system language is `sysutil_lang` (the
// SYSUTIL_LANG_* numbers).  Languages the app has no catalog for give English;
// both Portuguese variants give Brazilian Portuguese.
i18n_lang i18n_from_system(int sysutil_lang);

// Catalog access, for the tests and the checker.
int         i18n_catalog_size(void);
const char *i18n_catalog_key(int i);
const char *i18n_catalog_text(i18n_lang lang, int i);   // NULL for LANG_EN

#ifdef __cplusplus
}
#endif
