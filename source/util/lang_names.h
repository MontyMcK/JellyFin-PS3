#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// "eng", "en", "ENG", "fre", "fra", "fr" -> "English", "French".  ISO 639-2 (both the bibliographic
// and the terminology codes) and the two-letter ISO 639-1 codes of the common languages.  "und", "mis",
// "zxx", "" -> "Unknown".  A code not in the table comes back upper-cased ("TLH").
void lang_name(const char *code, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
