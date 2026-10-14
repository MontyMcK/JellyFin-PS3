// Matroska subtitle blocks: see sub_conv.h.

#include "sub_conv.h"

#include <stdbool.h>
#include <string.h>

// Drops a character the cut left unfinished at the end of s[0..n).
static int whole_chars(const char *s, int n) {
    if (n <= 0) return 0;
    int i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    const unsigned char lead = (unsigned char)s[i];
    const int want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return i + want > n ? i : n;
}

// Drops trailing spaces and newlines.
static int trim_end(char *s, int n) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t')) n--;
    s[n] = '\0';
    return n;
}

int sub_srt_block_text(const uint8_t *data, int len, char *out, int cap) {
    if (cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < len && n < cap - 1; i++) {
        const char c = (char)data[i];
        // a tag (<i>, </i>, <font ...>, or an override block that starts with a backslash) is skipped whole when it closes;
        // "a < b" is text
        if ((c == '<' && i + 1 < len && (((char)data[i + 1] >= 'a' && (char)data[i + 1] <= 'z') || ((char)data[i + 1] >= 'A' && (char)data[i + 1] <= 'Z') ||
                                              (char)data[i + 1] == '/' || (char)data[i + 1] == '!')) ||
            (c == '{' && i + 1 < len && (char)data[i + 1] == '\\')) {
            const char close = c == '<' ? '>' : '}';
            int j = i + 1;
            while (j < len && (char)data[j] != close && data[j] != '\n') j++;
            if (j < len && (char)data[j] == close) { i = j; continue; }
        }
        if (c == '\r') continue;
        if (c == '\0') continue;
        // blank lines inside a cue collapse to one break
        if (c == '\n' && (n == 0 || out[n - 1] == '\n')) continue;
        out[n++] = c;
    }
    out[n] = '\0';
    n = trim_end(out, whole_chars(out, n));
    return n;
}

int sub_ass_block_text(const uint8_t *data, int len, char *out, int cap) {
    if (cap <= 0) return 0;
    // the eight fields before the text
    int i = 0, commas = 0;
    while (i < len && commas < 8) { if (data[i] == ',') commas++; i++; }
    if (commas < 8) { out[0] = '\0'; return 0; }
    int n = 0;
    bool drawing = false;
    for (; i < len && n < cap - 1; i++) {
        const char c = (char)data[i];
        if (c == '{') {
            int j = i + 1;
            while (j < len && (char)data[j] != '}') j++;
            if (j < len) {
                // inside the block: \p<digits> turns vector drawing on (a non-zero scale) or off (0)
                for (int k = i + 1; k + 1 < j; k++) {
                    if ((char)data[k] == '\\' && (char)data[k + 1] == 'p' && k + 2 < j && data[k + 2] >= '0' && data[k + 2] <= '9') {
                        int m = k + 2;
                        int v = 0;
                        while (m < j && data[m] >= '0' && data[m] <= '9') { v = v * 10 + (data[m] - '0'); m++; }
                        drawing = v != 0;
                    }
                }
                i = j;
                continue;
            }
        }
        if (drawing) continue;
        if (c == '\\' && i + 1 < len) {
            const char e = (char)data[i + 1];
            if (e == 'N' || e == 'n') { if (n > 0 && out[n - 1] != '\n') out[n++] = '\n'; i++; continue; }
            if (e == 'h') { out[n++] = ' '; i++; continue; }
        }
        if (c == '\r' || c == '\0') continue;
        if (c == '\n') { if (n > 0 && out[n - 1] != '\n') out[n++] = '\n'; continue; }
        out[n++] = c;
    }
    out[n] = '\0';
    // leading blank space too
    int lead = 0;
    while (lead < n && (out[lead] == ' ' || out[lead] == '\n')) lead++;
    if (lead) { memmove(out, out + lead, (size_t)(n - lead) + 1); n -= lead; }
    n = trim_end(out, whole_chars(out, n));
    return n;
}

int sub_pgs_block_to_sup(const uint8_t *data, int len, uint32_t pts90, uint8_t *out, int cap) {
    int i = 0, w = 0;
    while (i + 3 <= len) {
        const int size = (data[i + 1] << 8) | data[i + 2];
        if (i + 3 + size > len) break;                       // a segment cut short: the rest is dropped
        if (w + 13 + size > cap) return -1;
        out[w + 0] = 'P'; out[w + 1] = 'G';
        out[w + 2] = (uint8_t)(pts90 >> 24); out[w + 3] = (uint8_t)(pts90 >> 16); out[w + 4] = (uint8_t)(pts90 >> 8); out[w + 5] = (uint8_t)pts90;
        out[w + 6] = out[w + 7] = out[w + 8] = out[w + 9] = 0;
        out[w + 10] = data[i];
        out[w + 11] = (uint8_t)(size >> 8); out[w + 12] = (uint8_t)size;
        memcpy(out + w + 13, data + i + 3, (size_t)size);
        w += 13 + size;
        i += 3 + size;
    }
    return w;
}
