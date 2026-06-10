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

const int   OSK_MAX_ROWS = 6;       // letters use 5 rows, symbols use 6
const float OSK_LBL_PX   = 31.5f;   // 150% of the 21px search-bar OSK labels

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
        rows[3].keys[rows[3].n++] = okey(OK_SHIFT, 0, "CAPS", 2);
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
    rows[nr].keys[rows[nr].n++] = okey(OK_SPACE, 0, "SPACE", 5);
    rows[nr].keys[rows[nr].n++] = okey(OK_ENTER, 0, "ENTER", 3);
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
    int y0      = XMB_CONTENT_Y + 58;

    waitflip();
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();

    // Divider under the title bar.
    u32 *div = color_buffer[curr_fb] + (u32)XMB_DIVIDER_Y * display_width;
    for (u32 x = 0; x < display_width; x++) div[x] = XMB_DIVIDER_CLR;

    // Input field background.
    drawRect((u32)barx, (u32)(XMB_CONTENT_Y + 8), (u32)total_w, 40, 0x001A1040);

    // Key cells (CPU rects).
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        int ry = y0 + r * OSK_STEP_Y;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int kw  = k->cols * OSK_STEP_X - OSK_GAP;
            u32 col = (r == sr && c == sc) ? XMB_KEY_SEL : XMB_KEY_NORMAL;
            if (k->kind == OK_SHIFT && caps) col = XMB_ACCENT;
            drawRect((u32)cx, (u32)ry, (u32)kw, OSK_KEY_H, col);
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Title + prompt (RSX).
    drawTTF(XMB_ITEM_PAD, 16, "JELLYFIN-PS3", 28, XMB_ACCENT);
    {
        int pw = ttf_text_width(prompt, 24);
        int px = W / 2 - pw / 2;
        if (px < (int)XMB_ITEM_PAD) px = (int)XMB_ITEM_PAD;
        drawTTF((u32)px, (u32)(XMB_DIVIDER_Y + 14), prompt, 24, 0x00FFFFFF);
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
        const float typed_px = 27.0f;   // 150% of the original 18px
        int tw = ttf_text_width(disp, typed_px);
        int tx = W / 2 - tw / 2;
        if (tx < barx + 6) tx = barx + 6;
        drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 12), disp, typed_px, 0x00FFFFFF);
    }

    // Key labels (RSX).
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        int ry = y0 + r * OSK_STEP_Y + (OSK_KEY_H - (int)OSK_LBL_PX) / 2;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int  kw = k->cols * OSK_STEP_X - OSK_GAP;
            char chbuf[2] = { k->ch, '\0' };
            const char *lbl = (k->kind == OK_CHAR) ? chbuf : k->label;
            int lw = ttf_text_width(lbl, OSK_LBL_PX);
            int lx = cx + (kw - lw) / 2;
            drawTTF((u32)lx, (u32)ry, lbl, OSK_LBL_PX, 0x00FFFFFF);
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Hints.
    static const Hint hints[] = {{'D',"MOVE"},{'X',"SELECT"},{'C',"CANCEL"}};
    draw_hints_bar(hints, 3);

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

        // Button shortcuts: Square = backspace, Start = confirm, Select/Circle = cancel.
        if (BTN_PRESSED(square)) { int l = strlen(out); if (l > 0) out[l-1] = '\0'; }
        if (BTN_PRESSED(start))  return 1;
        if (BTN_PRESSED(select) || BTN_PRESSED(circle)) return -1;

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
            case OK_SYM:   sym = !sym; sc = 0; break;
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
