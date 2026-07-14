// On-screen keyboard for login / free text entry (get_input).
//
// Matches the look of the XMB search bar OSK, but adds an on-screen SHIFT key
// (iOS/Android style) for upper/lower case plus a #+= key for symbols, so any
// username, password, or URL — including special characters — can be typed.

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_wave.h"
#include "timing.h"

namespace {

enum OKind { OK_CHAR, OK_SHIFT, OK_SYM, OK_BACK, OK_SPACE, OK_ENTER };

struct OKey {
    OKind       kind;
    char        ch;     // OK_CHAR: the already-cased character to insert
    const char *label;  // non-char keys: button caption
    int         cols;   // width in OSK step units
};

struct ORow {
    OKey keys[12];
    int  n;
};

const int OSK_MAX_ROWS = 6;         // letters use 5 rows, symbols use 6

// Character-key label size follows the key height (31.5px on 44px keys at
// 720p, proportionally smaller on SD framebuffers).  Not a namespace const:
// display_height isn't known until init_screen has run.
static float osk_lbl_px(void) { return (float)OSK_KEY_H * 0.72f; }

OKey okey(OKind kind, char ch, const char *label, int cols) {
    OKey k; k.kind = kind; k.ch = ch; k.label = label; k.cols = cols;
    return k;
}

// Build the keyboard layout for the current mode (symbols vs letters) and case.
// Returns the number of rows used (including the bottom action row).
int osk_build(ORow rows[OSK_MAX_ROWS], bool sym, bool caps) {
    for (int i = 0; i < OSK_MAX_ROWS; i++) rows[i].n = 0;

    int nr;   // number of content rows (action row is appended after)
    if (!sym) {
        static const char *R0 = "1234567890";
        static const char *R1 = "qwertyuiop";
        static const char *R2 = "asdfghjkl";
        static const char *R3 = "zxcvbnm";
        for (const char *p = R0; *p; p++) rows[0].keys[rows[0].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R1; *p; p++) rows[1].keys[rows[1].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        for (const char *p = R2; *p; p++) rows[2].keys[rows[2].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        rows[3].keys[rows[3].n++] = okey(OK_SHIFT, 0, "Caps", 2);
        for (const char *p = R3; *p; p++) rows[3].keys[rows[3].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        rows[3].keys[rows[3].n++] = okey(OK_BACK, 0, "<", 2);
        nr = 4;
    } else {
        static const char *R0 = "1234567890";   // numbers row on the symbols page too
        static const char *R1 = "!@#$%^&*()";
        static const char *R2 = "-_=+[]{}|\\";
        static const char *R3 = ":;\"'`~<>?";
        static const char *R4 = ".,/?";
        for (const char *p = R0; *p; p++) rows[0].keys[rows[0].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R1; *p; p++) rows[1].keys[rows[1].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R2; *p; p++) rows[2].keys[rows[2].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R3; *p; p++) rows[3].keys[rows[3].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R4; *p; p++) rows[4].keys[rows[4].n++] = okey(OK_CHAR, *p, 0, 1);
        rows[4].keys[rows[4].n++] = okey(OK_BACK, 0, "<", 2);
        nr = 5;
    }
    rows[nr].keys[rows[nr].n++] = okey(OK_SYM,   0, sym ? "ABC" : "#+=", 2);
    rows[nr].keys[rows[nr].n++] = okey(OK_SPACE, 0, "Space", 5);
    rows[nr].keys[rows[nr].n++] = okey(OK_ENTER, 0, "Enter", 3);
    return nr + 1;
}

int orow_units(const ORow *r) {
    int u = 0;
    for (int i = 0; i < r->n; i++) u += r->keys[i].cols;
    return u;
}

void osk_draw(const char *prompt, const char *input, bool is_password,
              const ORow rows[OSK_MAX_ROWS], int nrows, int sr, int sc, bool caps) {
    int W       = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int barx    = (W - total_w) / 2;
    int y0      = XMB_CONTENT_Y + UIS_H(80);
    const int field_h = 40;

    waitflip();
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();

    xmb_draw_divider();

    // Input field: dark well with an accent underline.
    drawRect((u32)barx, (u32)(XMB_CONTENT_Y + 8), (u32)total_w, field_h,
             0x00131830UL);
    drawRect((u32)barx, (u32)(XMB_CONTENT_Y + 8 + field_h - 2), (u32)total_w,
             2, XMB_ACCENT);

    // Key cells (CPU rects).
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        int ry = y0 + r * OSK_STEP_Y;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int kw  = k->cols * OSK_STEP_X - OSK_GAP;
            u32 col = (r == sr && c == sc) ? XMB_KEY_SEL : XMB_KEY_NORMAL;
            if (k->kind == OK_SHIFT && caps && !(r == sr && c == sc))
                col = XMB_ACCENT_DEEP;
            drawRect((u32)cx, (u32)ry, (u32)kw, OSK_KEY_H, col);
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Brand + prompt.
    drawTTF(XMB_ITEM_PAD, 20, "Jellyfin", 22, XMB_TEXT, true);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", 22, true) + 8, 27, "PS3",
            13, XMB_ACCENT, true);
    {
        int pw = ttf_text_width(prompt, 18);
        int px = W / 2 - pw / 2;
        if (px < (int)XMB_ITEM_PAD) px = (int)XMB_ITEM_PAD;
        drawTTF((u32)px, (u32)(XMB_DIVIDER_Y - 34), prompt, 18, XMB_TEXT_DIM);
    }

    // Current text (masked for passwords) with a blinking cursor.
    {
        char shown[80];
        int  ilen = strlen(input);
        if (is_password) {
            int n = ilen > 60 ? 60 : ilen;
            memset(shown, '*', n);
            shown[n] = '\0';
        } else {
            const char *src = ilen > 60 ? input + ilen - 60 : input;
            snprintf(shown, sizeof(shown), "%s", src);
        }
        bool cur = ((timing_get_us() / 500000) & 1) == 0;
        char disp[96];
        snprintf(disp, sizeof(disp), "%s%s", shown, cur ? "_" : " ");
        const float typed_px = 22.0f;
        int tw = ttf_text_width(disp, typed_px);
        int tx = W / 2 - tw / 2;
        if (tx < barx + 14) tx = barx + 14;
        drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 8 + (field_h - (int)typed_px) / 2),
                disp, typed_px, XMB_TEXT);
    }

    // Key labels — dark on the selected (white) key, soft white elsewhere.
    // Action keys (Caps/Enter/etc.) use a smaller size than characters.
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int  kw = k->cols * OSK_STEP_X - OSK_GAP;
            bool sel = (r == sr && c == sc);
            int  ry  = y0 + r * OSK_STEP_Y;
            u32  clr = sel ? XMB_KEY_LABEL_SEL : XMB_TEXT;
            if (k->kind == OK_BACK) {
                drawIcon((u32)(cx + (kw - 22) / 2),
                         (u32)(ry + (OSK_KEY_H - 22) / 2), ICON_BACKSPACE, 22.0f, clr);
            } else {
                char chbuf[2] = { k->ch, '\0' };
                const char *lbl = (k->kind == OK_CHAR) ? chbuf : k->label;
                float lbl_px = (k->kind == OK_CHAR) ? osk_lbl_px() : 18.0f;
                int lw = ttf_text_width(lbl, lbl_px);
                int lx = cx + (kw - lw) / 2;
                int ly = ry + (OSK_KEY_H - (int)lbl_px) / 2;
                drawTTF((u32)lx, (u32)ly, lbl, lbl_px, clr);
            }
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Hints.  Square = backspace, Start = confirm, Circle = cancel.
    static const Hint hints[] = {{'X',"Select"},{'S',"Delete"},
                                 {'A',"Done"},{'C',"Cancel"}};
    draw_hints_bar(hints, 4);

    flip();
}

} // namespace

int get_input(char *out, int max_len, const char *prompt, bool is_password) {
    out[0] = '\0';

    int  sr = 1, sc = 0;            // start on the top letter row
    bool caps = false, sym = false;

    init_btns();

    while (running) {
        sysUtilCheckCallback();
        poll_buttons();

        ORow rows[OSK_MAX_ROWS];
        int  nrows = osk_build(rows, sym, caps);

        // Navigation.
        if (BTN_REPEAT(up)) {
            sr = (sr - 1 + nrows) % nrows;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
        }
        if (BTN_REPEAT(down)) {
            sr = (sr + 1) % nrows;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
        }
        if (BTN_REPEAT(left))  sc = (sc - 1 + rows[sr].n) % rows[sr].n;
        if (BTN_REPEAT(right)) sc = (sc + 1) % rows[sr].n;

        // Button shortcuts: Square = backspace, Start = confirm, Circle = cancel.
        // Cancel is Circle only — the hints bar advertises just Circle, and on
        // RPCS3 the default keyboard handler maps Select to the spacebar, so
        // also cancelling on Select made the app quit the moment the user hit
        // space while typing a URL (which needs the symbols page for : and /).
        if (BTN_PRESSED(square)) { int l = strlen(out); if (l > 0) out[l-1] = '\0'; }
        if (BTN_PRESSED(start))  return 1;
        if (BTN_PRESSED(circle)) return -1;

        // Activate the highlighted key.
        if (BTN_PRESSED(cross)) {
            const OKey *k = &rows[sr].keys[sc];
            switch (k->kind) {
            case OK_CHAR: {
                int l = strlen(out);
                if (l < max_len - 1) { out[l] = k->ch; out[l+1] = '\0'; }
                break;
            }
            case OK_SHIFT: caps = !caps; break;
            case OK_SYM:
                // Switch letters<->symbols and keep the cursor on the toggle key
                // itself (first key of the action row, which is the last row)
                // so repeated presses flip in place instead of jumping the
                // selection into the middle of the new layout.
                sym = !sym;
                sr  = osk_build(rows, sym, caps) - 1;
                sc  = 0;
                break;
            case OK_BACK:  { int l = strlen(out); if (l > 0) out[l-1] = '\0'; break; }
            case OK_SPACE: { int l = strlen(out); if (l < max_len - 1) { out[l] = ' '; out[l+1] = '\0'; } break; }
            case OK_ENTER: return 1;
            }
            // The layout (and row count) may have changed; re-clamp selection.
            nrows = osk_build(rows, sym, caps);
            if (sr >= nrows)      sr = nrows - 1;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
            if (sc < 0)           sc = 0;
        }

        nrows = osk_build(rows, sym, caps);
        osk_draw(prompt, out, is_password, rows, nrows, sr, sc, caps);
    }
    return -1;
}
