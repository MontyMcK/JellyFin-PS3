#include "lang_names.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// code (any of its spellings, '|' separated) -> name
static const struct { const char *codes; const char *name; } LANGS[] = {
    { "eng|en", "English" },        { "fre|fra|fr", "French" },     { "ger|deu|de", "German" },
    { "spa|es", "Spanish" },        { "ita|it", "Italian" },        { "por|pt", "Portuguese" },
    { "dut|nld|nl", "Dutch" },      { "rus|ru", "Russian" },        { "pol|pl", "Polish" },
    { "swe|sv", "Swedish" },        { "nor|no|nob|nb|nno|nn", "Norwegian" }, { "dan|da", "Danish" },
    { "fin|fi", "Finnish" },        { "jpn|ja", "Japanese" },       { "chi|zho|zh|cmn|yue", "Chinese" },
    { "kor|ko", "Korean" },         { "ara|ar", "Arabic" },         { "heb|he", "Hebrew" },
    { "tur|tr", "Turkish" },        { "gre|ell|el", "Greek" },      { "hun|hu", "Hungarian" },
    { "cze|ces|cs", "Czech" },      { "slo|slk|sk", "Slovak" },     { "rum|ron|ro", "Romanian" },
    { "bul|bg", "Bulgarian" },      { "ukr|uk", "Ukrainian" },      { "hrv|hr", "Croatian" },
    { "srp|sr", "Serbian" },        { "slv|sl", "Slovenian" },      { "hin|hi", "Hindi" },
    { "tha|th", "Thai" },           { "vie|vi", "Vietnamese" },     { "ind|id", "Indonesian" },
    { "may|msa|ms", "Malay" },      { "per|fas|fa", "Persian" },    { "cat|ca", "Catalan" },
    { "ice|isl|is", "Icelandic" },  { "est|et", "Estonian" },       { "lav|lv", "Latvian" },
    { "lit|lt", "Lithuanian" },     { "tam|ta", "Tamil" },          { "tel|te", "Telugu" },
    { "wel|cym|cy", "Welsh" },      { "gle|ga", "Irish" },          { "baq|eus|eu", "Basque" },
    { "glg|gl", "Galician" },       { "lat|la", "Latin" },          { "afr|af", "Afrikaans" },
};

static bool eq_code(const char *list, const char *code) {
    const size_t n = strlen(code);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, '|');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && strncmp(p, code, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

void lang_name(const char *code, char *out, size_t cap) {
    if (cap == 0) return;
    char c[8];
    size_t n = 0;
    for (; code && code[n] && n < sizeof c - 1; n++) c[n] = (char)tolower((unsigned char)code[n]);
    c[n] = '\0';
    // "pt-BR", "en_US": the language part
    for (size_t i = 0; i < n; i++) if (c[i] == '-' || c[i] == '_') { c[i] = '\0'; break; }
    if (!c[0] || !strcmp(c, "und") || !strcmp(c, "mis") || !strcmp(c, "zxx") || !strcmp(c, "mul")) {
        snprintf(out, cap, "Unknown");
        return;
    }
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) {
        if (eq_code(LANGS[i].codes, c)) { snprintf(out, cap, "%s", LANGS[i].name); return; }
    }
    size_t i = 0;
    for (; c[i] && i + 1 < cap; i++) out[i] = (char)toupper((unsigned char)c[i]);
    out[i] = '\0';
}
