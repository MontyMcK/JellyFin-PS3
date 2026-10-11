// Picture size adjust mode -- see player_picture.h.

#include <stdio.h>
#include <string.h>

#include "player_picture.h"
#include "player_hud.h"
#include "ui.h"
#include "timing.h"
#include "plog.h"
#include "jf_paths.h"

#define PIC_FILE    "jellyfin_picsize.txt"
#define ASPECT_FILE "jellyfin_aspect.txt"

#define PIC_REPEAT_DELAY_US  350000ULL    // first repeat after a hold
#define PIC_REPEAT_US         45000ULL    // then ~22 steps a second

static PicSize s_pic = { PIC_DEFAULT, PIC_DEFAULT, PIC_FIT };
static bool    s_active = false;
static u64     s_next_us = 0;             // 0 = no direction held
static bool    s_dirty = false;           // changed since the last save

PicSize picture_get(void) { return s_pic; }

static const char *aspect_label(int a) {
    return a == PIC_FILL ? TR("Fill") : a == PIC_STRETCH ? TR("Stretch") : TR("Fit");
}

static void aspect_load(void) {
    FILE *f = fopen(jf_data_path(ASPECT_FILE), "r");
    if (!f) return;
    char line[32] = "";
    if (fgets(line, sizeof line, f)) s_pic.aspect = pic_aspect_parse(line);
    fclose(f);
}

static void aspect_save(void) {
    FILE *f = fopen(jf_data_path(ASPECT_FILE), "w");
    if (!f) return;
    fprintf(f, "%s\n", pic_aspect_name(s_pic.aspect));
    fclose(f);
}

void picture_load(void) {
    static bool s_loaded = false;
    if (s_loaded) return;
    s_loaded = true;
    aspect_load();
    FILE *f = fopen(jf_data_path(PIC_FILE), "r");
    if (!f) return;
    char line[64] = "";
    if (fgets(line, sizeof line, f)) pic_parse(line, &s_pic);
    fclose(f);
}

static void picture_save(void) {
    if (!s_dirty) return;
    s_dirty = false;
    FILE *f = fopen(jf_data_path(PIC_FILE), "w");
    if (!f) return;
    char buf[32];
    pic_format(&s_pic, buf, sizeof buf);
    fputs(buf, f);
    fclose(f);
}

static void show_readout(void) {
    char l1[96], l2[96];
    snprintf(l1, sizeof l1, "%s  %s %d%%   %s %d%%   %s", TR("Picture size"),
             TR("Width"), s_pic.w, TR("Height"), s_pic.h, aspect_label(s_pic.aspect));
    snprintf(l2, sizeof l2, "%s", TR("D-pad adjust  Square fit / fill / stretch  Triangle reset  O done"));
    hud_set_picture_banner(l1, l2);
}

static void leave(void) {
    s_active = false;
    s_next_us = 0;
    hud_clear_picture_banner();
    picture_save();
}

void picture_cycle_aspect(void) {
    s_pic.aspect = pic_aspect_next(s_pic.aspect);
    aspect_save();
    char l1[64];
    snprintf(l1, sizeof l1, "%s: %s", TR("Picture"), aspect_label(s_pic.aspect));
    hud_set_banner(l1, "", timing_get_us() + 2000000ULL);
    char b[48];
    snprintf(b, sizeof b, "picture: aspect %s", pic_aspect_name(s_pic.aspect));
    plog(b);
}

void picture_session_end(void) {
    s_active = false;
    s_next_us = 0;
    picture_save();
}

bool picture_mode_input(void) {
    if (!s_active) {
        if (!BTN_PRESSED(l1)) return false;
        s_active = true;
        s_next_us = 0;
        hud_hide();
        show_readout();
        return true;
    }

    if (BTN_PRESSED(l1) || BTN_PRESSED(cross) || BTN_PRESSED(circle)) {
        leave();
        return true;
    }
    if (BTN_PRESSED(square)) {
        picture_cycle_aspect();
        show_readout();
        return true;
    }
    if (BTN_PRESSED(triangle)) {
        s_pic.w = s_pic.h = PIC_DEFAULT;
        s_dirty = true;
        show_readout();
        return true;
    }

    // Right/Up grow, Left/Down shrink.  One step on the press, then repeat.
    const int dw = (int)(btn_cur.right != 0) - (int)(btn_cur.left != 0);
    const int dh = (int)(btn_cur.up != 0)    - (int)(btn_cur.down != 0);
    if (!dw && !dh) { s_next_us = 0; return true; }
    const u64 now = timing_get_us();
    if (s_next_us == 0)        s_next_us = now + PIC_REPEAT_DELAY_US;   // the press itself
    else if (now >= s_next_us) s_next_us = now + PIC_REPEAT_US;         // a repeat
    else                       return true;
    const PicSize before = s_pic;
    s_pic = pic_step(s_pic, dw, dh);
    if (s_pic.w != before.w || s_pic.h != before.h) {
        s_dirty = true;
        show_readout();
    }
    return true;
}
