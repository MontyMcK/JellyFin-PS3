// Audio track label → codec question.  See track_codec.h.

#include "track_codec.h"

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// A codec name in a DisplayTitle is a standalone token: it is bounded by
// separators (" - ", spaces, commas, brackets) or by the string ends.
// Requiring that boundary is what keeps "DTS" from matching inside an
// unrelated word — a track named for a band or a dub studio, say — while
// still matching "DTS-HD MA", "DTS:X" and "DTS-ES", where what follows is
// punctuation rather than a letter.
static bool alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static bool has_token(const char *s, const char *tok) {
    if (!s || !tok || !tok[0]) return false;
    for (int i = 0; s[i]; i++) {
        int j = 0;
        while (tok[j] && lower(s[i + j]) == lower(tok[j])) j++;
        if (tok[j]) continue;                       // no match here
        if (i > 0 && alnum(s[i - 1])) continue;     // starts mid-word
        if (alnum(s[i + j])) continue;              // ends mid-word
        return true;
    }
    return false;
}

bool track_label_is_truehd(const char *label) {
    // Jellyfin renders this codec as "TrueHD"; MediaInfo-derived names and
    // some servers use "MLP" (the format TrueHD is built on) or spell it
    // "TRUE-HD".  An Atmos track is a TrueHD track — its label names both,
    // and it decodes here as its TrueHD bed — but "Atmos" alone is NOT
    // accepted: Dolby Digital Plus can also carry Atmos, and nothing here
    // decodes E-AC-3.
    return has_token(label, "truehd") || has_token(label, "true-hd") ||
           has_token(label, "mlp");
}

bool track_label_is_hd_audio(const char *label) {
    return track_label_is_dts(label) || track_label_is_truehd(label);
}

bool track_label_is_dts(const char *label) {
    // "DTS" covers DTS, DTS-HD HRA, DTS-HD MA, DTS-ES, DTS Express and DTS:X,
    // because Jellyfin renders the codec and the profile as separate tokens
    // ("DTS-HD MA" begins with the DTS token).  "DCA" is the same codec under
    // ffmpeg's internal name, which some servers surface instead.
    return has_token(label, "dts") || has_token(label, "dca");
}
