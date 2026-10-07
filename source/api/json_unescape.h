// One JSON string decoder for every field reader in the client.
//
// WHY.  Jellyfin serialises with System.Text.Json, whose default encoder
// escapes far more than JSON requires: an apostrophe arrives as ', a
// plus as +, <, > and & as < / > / &, and any non-ASCII
// character as \uXXXX (a surrogate pair above the BMP).  The old readers
// copied the raw bytes between the quotes, so a synopsis read "don't"
// on screen, codec words picked up escape digits between their letters, and
// an escaped quote (\") ended the string early.  This decodes every escape
// to UTF-8, which the text renderer already draws (utf8_next in
// render/ui_text.cpp) -- so the fix needs no font change.
//
// Pure C, no allocation; host-tested in tests/test_json_unescape.c.

#ifndef JF_JSON_UNESCAPE_H
#define JF_JSON_UNESCAPE_H

#ifdef __cplusplus
extern "C" {
#endif

static inline int json_hex4(const char *p, const char *end, unsigned *out) {
    if (end - p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

// Append one codepoint as UTF-8 if it fits whole.  Returns the new length.
static inline int json_put_utf8(char *out, int i, int cap, unsigned cp) {
    char b[4]; int n;
    if (cp < 0x80)         { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (char)(0xC0 | (cp >> 6));  b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                             b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (i + n > cap - 1) return -1;          // never split a character
    for (int k = 0; k < n; k++) out[i + k] = b[k];
    return i + n;
}

// Decode the JSON string body starting at p (just AFTER the opening quote),
// reading no further than end.  Writes at most cap-1 bytes plus a NUL; a
// character that does not fit whole is dropped along with the rest (the
// output is truncated, never split mid-character).  Returns a pointer just
// past the closing quote (or end, if the string is unterminated).
static inline const char *json_unescape(const char *p, const char *end,
                                        char *out, int cap) {
    int i = 0, full = 0;
    if (cap <= 0) out = 0;
    while (p < end && *p != '"') {
        unsigned cp;
        if (*p == '\\') {
            if (p + 1 >= end) { p = end; break; }       // escape cut off by end
            const char e = p[1];
            p += 2;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u':
                if (!json_hex4(p, end, &cp)) { cp = '?'; break; }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate
                    unsigned lo;
                    if (end - p >= 6 && p[0] == '\\' && p[1] == 'u' &&
                        json_hex4(p + 2, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;                              // lone low surrogate
                }
                break;
            default: cp = (unsigned char)e; break;           // \" \\ \/ and anything else
            }
            if (!full && out) {
                const int ni = json_put_utf8(out, i, cap, cp);
                if (ni < 0) full = 1; else i = ni;
            }
            continue;
        }
        // A raw byte (UTF-8 passes straight through).  Stop cleanly at a
        // character boundary when the buffer fills.
        if (!full && out) {
            const unsigned char c = (unsigned char)*p;
            int len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)));
            if (i + len > cap - 1) full = 1;
            else {
                for (int k = 0; k < len && p + k < end && p[k] != '"'; k++) out[i++] = p[k];
                p += len - 1;
            }
        }
        p++;
    }
    if (out) out[i] = '\0';
    return p < end ? p + 1 : end;
}

#ifdef __cplusplus
}
#endif

#endif
