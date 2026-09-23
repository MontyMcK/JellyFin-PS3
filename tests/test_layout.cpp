// Host-side layout sanity for the XMB chrome and grid.
//
// Compiles the REAL ui_visuals.h macros against stubbed display metrics and
// evaluates the derived layout at every resolution this client runs at. It is
// not a pixel-perfect check -- it is a guard against the class of mistake that
// is invisible in source and obvious on a TV: a chrome band that grows into the
// content, a card height that goes negative, a hints bar that overlaps the grid.
//
// Added when Phase 2 rescaled the chrome band, which shifts the divider (and
// therefore everything below it) on any screen above 720p.  Extended when that
// rescale turned out to have been applied to the chrome and NOT to the content,
// leaving the UI at two scales at once -- see test_one_scale() below, which is
// the direct guard against that happening again.
//
// Build/run:  make -f Makefile.host test_layout && ./test_layout

#include <stdio.h>
#include <string.h>
#include <ppu-types.h>

// ---- stand-ins for the PS3 side ----
u32 display_width  = 1920;
u32 display_height = 1080;

static float s_overscan = 0.0f;
float overscan_frac(void) { return s_overscan; }
void  overscan_set_frac(float f) { s_overscan = f; }
int   overscan_x(void) { return (int)(s_overscan * (float)display_width  + 0.5f); }
int   overscan_y(void) { return (int)(s_overscan * (float)display_height + 0.5f); }

#include "../source/ui/theme.h"
Theme g_theme;                 // values are irrelevant here; layout only
QualityMode g_quality = QUALITY_FULL;

// Normally defined in ui/ui_scale.cpp, which pulls in the LV2 file paths.
// 0 = auto, which is the shipping default and what most of this file exercises.
int g_uis_pct = 0;

// The macros under test come from the REAL header, so if ui_visuals.h changes
// this test changes with it.  That is the whole point: a hand-copied mirror
// would keep passing while the shipping layout drifted away from it.
#include "ui_visuals.h"

// Tab strip, from ui_widgets.cpp
#define TAB_ICON_Y   61
#define TAB_LABEL_Y  96
#define TAB_RULE_Y  110
#define TAB_RULE_H    2

static int g_fail = 0;

static void ck(const char *what, bool ok, const char *detail) {
    if (ok) return;
    printf("  FAIL %-46s %s\n", what, detail ? detail : "");
    g_fail++;
}

static void check_res(const char *name, u32 w, u32 h, float ovs) {
    display_width = w; display_height = h; overscan_set_frac(ovs);

    const int icon_y  = XMB_OY + UIS_H(TAB_ICON_Y);
    const int label_y = XMB_OY + UIS_H(TAB_LABEL_Y);
    const int rule_y  = XMB_OY + UIS_H(TAB_RULE_Y);
    const int rule_b  = rule_y + UIS_H(TAB_RULE_H);
    const int div_y   = XMB_DIVIDER_Y;
    const int card_h  = XMB_CARD_H_FIT;
    const int vis     = XMB_ITEMS_VIS;
    const int hint_y  = (int)display_height - XMB_OY - UIS_H(22);

    char buf[160];
    printf("%-22s %4ux%-4u ovs %.3f  icons %3d  label %3d  rule %3d  div %3d"
           "  card_h %3d  rows %d  hints %4d\n",
           name, w, h, (double)ovs, icon_y, label_y, rule_y, div_y,
           card_h, vis, hint_y);

    // The tab strip must fit entirely inside the chrome band.
    snprintf(buf, sizeof buf, "rule ends %d, divider %d", rule_b, div_y);
    ck("tab underline clears the divider", rule_b <= div_y, buf);
    ck("label sits below the icons", label_y > icon_y, NULL);
    ck("underline sits below the label", rule_y > label_y, NULL);

    // Content below the chrome must still have usable room.
    snprintf(buf, sizeof buf, "card_h %d", card_h);
    ck("cards have positive height", card_h > 40, buf);
    snprintf(buf, sizeof buf, "rows %d", vis);
    ck("list shows at least 3 rows", vis >= 3, buf);

    // The hints bar must not land inside the grid area.
    snprintf(buf, sizeof buf, "hints %d, grid bottom %d",
             hint_y, XMB_GRID_Y0 + XMB_GRID_AVAIL_H);
    ck("hints bar clears the grid", hint_y >= XMB_GRID_Y0 + XMB_GRID_AVAIL_H, buf);

    // And everything must stay on screen.
    ck("divider on screen", div_y > 0 && div_y < (int)display_height, NULL);
    ck("hints bar on screen", hint_y > 0 && hint_y < (int)display_height, NULL);

    // The jump bar's column has to leave room for itself to the left of the
    // grid.  The LETTER size inside it is fixed by measurement in
    // xmb_draw_jumpbar() -- it shrinks the font until "W" fits JBAR_W, so it
    // cannot overflow by construction -- but the column still has to exist.
    snprintf(buf, sizeof buf, "JBAR_W %d, gap %d", JBAR_W, JBAR_GAP);
    ck("jump bar column is at least 8px wide", JBAR_W >= 8, buf);
    ck("jump bar fits beside the content margin",
       JBAR_W + JBAR_GAP * 3 <= XMB_ITEM_PAD + JBAR_W, buf);
}

// ---------------------------------------------------------------------------
// ONE scale.
//
// This is the regression guard for the bug that prompted the rewrite: the
// chrome was moved onto a scale that tracked the framebuffer while the content
// -- rows, cards, the jump bar, every literal font size -- stayed at the
// authored pixel count.  Everything still fitted, so the checks above passed;
// it just looked wrong, at two scales at once.
//
// Each entry is (authored value at 1280x720, the constant derived from it).
// A constant that was left raw shows up here immediately, because its value
// will not have moved with the screen.
// ---------------------------------------------------------------------------
struct Scaled { const char *name; int authored; int actual; char axis; };

static void test_one_scale(const char *label, u32 w, u32 h) {
    display_width = w; display_height = h; overscan_set_frac(0.0f);

    const Scaled tab[] = {
        // chrome
        { "XMB_TOPBAR_H",     64, XMB_TOPBAR_H,     'h' },
        { "XMB_TABBAR_H",     80, XMB_TABBAR_H,     'h' },
        { "XMB_BOTTOM_PAD",   70, XMB_BOTTOM_PAD,   'h' },
        // content -- the half that got left behind last time
        { "XMB_ITEM_H",       90, XMB_ITEM_H,       'h' },
        // XMB_THUMB_W is a WIDTH defined with the HEIGHT scale.  That predates
        // this test and is left as found: changing it would restretch every
        // list thumbnail on SD, where the pixels are not square, and nothing
        // has reported a problem with how they look.  Asserted as it is so the
        // oddity is recorded rather than silently normalised away.
        { "XMB_THUMB_W",      52, XMB_THUMB_W,      'h' },
        { "XMB_THUMB_H",      74, XMB_THUMB_H,      'h' },
        { "XMB_ROW_H",        88, XMB_ROW_H,        'h' },
        { "XMB_ROW_GAP",      16, XMB_ROW_GAP,      'h' },
        { "XMB_CARD_TEXT_H",  50, XMB_CARD_TEXT_H,  'h' },
        { "XMB_CARD_GAP_X",   24, XMB_CARD_GAP_X,   'w' },
        { "XMB_CARD_W_CAP",  300, XMB_CARD_W_CAP,   'w' },
        { "XMB_MUSIC_SUBTAB_H", 44, XMB_MUSIC_SUBTAB_H, 'h' },
        { "XMB_MUSIC_TEXT_H", 68, XMB_MUSIC_TEXT_H, 'h' },
        { "JBAR_W",           20, JBAR_W,           'w' },
        { "JBAR_GAP",          8, JBAR_GAP,         'w' },
        { "OSK_KEY_H",        44, OSK_KEY_H,        'h' },
        { "OSK_GAP",           8, OSK_GAP,          'w' },
        { "XMB_ROW_RADIUS",    8, XMB_ROW_RADIUS,   'w' },
    };

    printf("-- one scale at %ux%u --\n", w, h);
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        const Scaled &s = tab[i];
        int want = s.axis == 'w'
                 ? s.authored * (int)display_width  / 1280
                 : s.authored * (int)display_height /  720;
        char buf[160];
        snprintf(buf, sizeof buf,
                 "%s (%s): authored %d -> %d, but the screen's scale gives %d",
                 label, s.name, s.authored, s.actual, want);
        ck(s.name, s.actual == want, buf);
    }

    // Type scales with geometry, not independently: one row's height and the
    // type that sits in it have to move together or the text drifts within it.
    float t = UIS_TF(12.0f);
    char buf[160];
    snprintf(buf, sizeof buf, "UIS_TF(12) = %.2f, XMB_ROW_H/88*12 = %.2f",
             (double)t, (double)((float)XMB_ROW_H / 88.0f * 12.0f));
    ck("type scales with the rows it sits in",
       t > (float)XMB_ROW_H / 88.0f * 12.0f - 1.0f &&
       t < (float)XMB_ROW_H / 88.0f * 12.0f + 1.0f, buf);
}

// ---------------------------------------------------------------------------
// The override.  jellyfin_uiscale.txt exists so that a scale that reads badly
// on a given TV can be corrected without a rebuild, so the two values that
// matter most are worth pinning: 100 must reproduce the authored numbers
// EXACTLY (that is the "put it back how it was" setting), and an explicit
// percentage must beat the automatic one.
// ---------------------------------------------------------------------------
static void test_override(void) {
    puts("-- the jellyfin_uiscale.txt override --");
    display_width = 1920; display_height = 1080; overscan_set_frac(0.0f);

    g_uis_pct = 100;
    char buf[160];
    snprintf(buf, sizeof buf, "topbar %d, rows %d, type %.1f",
             XMB_TOPBAR_H, XMB_ROW_H, (double)UIS_TF(12.0f));
    ck("uiscale=100 gives the authored pixel counts",
       XMB_TOPBAR_H == 64 && XMB_ROW_H == 88 && XMB_CARD_W_CAP == 300 &&
       JBAR_W == 20 && UIS_TF(12.0f) == 12.0f, buf);

    g_uis_pct = 150;
    snprintf(buf, sizeof buf, "topbar %d, rows %d", XMB_TOPBAR_H, XMB_ROW_H);
    ck("uiscale=150 matches auto at 1080p",
       XMB_TOPBAR_H == 96 && XMB_ROW_H == 132, buf);

    // An explicit percentage ignores the framebuffer, which is the point: the
    // same file has to mean the same thing on a 720p set and a 1080p one.
    display_height = 720; display_width = 1280;
    snprintf(buf, sizeof buf, "topbar %d at 720p", XMB_TOPBAR_H);
    ck("an explicit percentage does not track the screen",
       XMB_TOPBAR_H == 96, buf);

    g_uis_pct = 0;
}

int main(void) {
    printf("%-22s %-10s %-10s\n", "resolution", "", "derived layout");
    check_res("1080p",            1920, 1080, 0.00f);
    check_res("1080p + overscan", 1920, 1080, 0.05f);
    check_res("720p",             1280,  720, 0.00f);
    check_res("720p + overscan",  1280,  720, 0.08f);
    check_res("576i",              720,  576, 0.00f);
    check_res("480p",              720,  480, 0.00f);
    check_res("480p + overscan",   720,  480, 0.08f);

    putchar('\n');
    test_one_scale("1080p", 1920, 1080);
    test_one_scale("720p",  1280,  720);
    test_one_scale("480p",   720,  480);
    putchar('\n');
    test_override();

    if (g_fail) { printf("\ntest_layout: %d FAILURES\n", g_fail); return 1; }
    printf("\ntest_layout: layout sane at every resolution, one scale throughout\n");
    return 0;
}
