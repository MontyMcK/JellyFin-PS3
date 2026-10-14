#pragma once

// The Language setting: Auto (follow the console's system language) or one of
// the app's languages.  Stored in jellyfin_language.txt as "auto" or the code
// ("ja", "pt-BR").  Console-only half of i18n.h.

#include "i18n.h"

void        i18n_load(void);              // once at startup: read the file, apply the language
int         i18n_pref(void);              // 0..LANG__COUNT-1 or LANG_PREF_AUTO
void        i18n_set_pref(int pref);      // apply + persist now
void        i18n_step_pref(int dir);      // Left / Right, wrapping
const char *i18n_pref_label(int pref);    // "Auto" (translated) or the language's own name
