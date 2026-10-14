// Input: every device folded into one ButtonState per frame, then edge
// detection and nav auto-repeat.
//
// Sources, all ORed together so any screen, the player and the auto-repeat
// take them with no per-screen code:
//
//   * Standard pads on all 7 ports.  That is the DS3, and anything else the
//     firmware itself presents as a pad: a DS4 over USB or registered over
//     Bluetooth, 8BitDo pads in their PS3 / D-input mode, PS3-licensed pads.
//   * The Blu-ray Disc Remote Control.  The firmware reports it as a pad of
//     device type 4 whose data only ioPadGetDataExtra() returns, with the
//     held key as a code in button[25] (0xff = nothing held).
//   * USB and Bluetooth keyboards, read in packet mode so a held arrow key
//     reads as held and auto-repeats like the d-pad.
//
// Media keys (play, pause, stop, scan, skip, subtitle, audio, info) have
// their own ButtonState fields instead of borrowing pad buttons: START means
// "stop" in the player, so a remote's PLAY key cannot simply be START.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <io/kb.h>

#include "ui.h"
#include "timing.h"
#include "ui_sfx.h"
#include "ui_wave.h"

// Each d-pad step also reaches the wave (wave_nav.h), with the cursor sound.
static void nav_step_to_wave(int slot) {
    if      (slot == NAV_up)    wave_nav_event(0, -1);
    else if (slot == NAV_down)  wave_nav_event(0,  1);
    else if (slot == NAV_left)  wave_nav_event(-1, 0);
    else if (slot == NAV_right) wave_nav_event( 1, 0);
}
#include "plog.h"

ButtonState btn_cur  = {0};
ButtonState btn_prev = {0};

static void or_pad(ButtonState *b, const padData *pad) {
    b->up       |= pad->BTN_UP;
    b->down     |= pad->BTN_DOWN;
    b->left     |= pad->BTN_LEFT;
    b->right    |= pad->BTN_RIGHT;
    b->cross    |= pad->BTN_CROSS;
    b->circle   |= pad->BTN_CIRCLE;
    b->square   |= pad->BTN_SQUARE;
    b->triangle |= pad->BTN_TRIANGLE;
    b->start    |= pad->BTN_START;
    b->select   |= pad->BTN_SELECT;
    b->l1       |= pad->BTN_L1;
    b->r1       |= pad->BTN_R1;
    b->l2       |= pad->BTN_L2;
    b->r2       |= pad->BTN_R2;
    b->l3       |= pad->BTN_L3;
    b->r3       |= pad->BTN_R3;
}

void update_buttons(padData *pad) {
    btn_prev = btn_cur;
    memset(&btn_cur, 0, sizeof(btn_cur));
    or_pad(&btn_cur, pad);
}

// The left stick as a d-pad.  One direction at a time -- the dominant axis --
// so a diagonal push does not move a grid both ways at once.  Hysteresis: a
// direction engages past ENGAGE and holds until the stick drops back inside
// RELEASE, so a stick resting near the threshold cannot chatter.  The result
// is ORed into BTN_UP..BTN_RIGHT, so the menus' auto-repeat, the player's
// seeking and everything else that reads the d-pad take it with no changes.
#define STICK_ENGAGE  80   // of 127
#define STICK_RELEASE 50
enum { STK_NONE, STK_UP, STK_DOWN, STK_LEFT, STK_RIGHT };

static int stick_dir(int prev, int dx, int dy) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    // How far the stick still leans the way it was already going.
    int keep = prev == STK_UP ? -dy : prev == STK_DOWN ? dy
             : prev == STK_LEFT ? -dx : prev == STK_RIGHT ? dx : 0;
    int major = ax > ay ? ax : ay;
    int cand = major <= STICK_ENGAGE ? STK_NONE
             : ax > ay ? (dx < 0 ? STK_LEFT : STK_RIGHT)
                       : (dy < 0 ? STK_UP : STK_DOWN);
    // Still leaning the old way and nothing new past ENGAGE: keep it.
    if (prev != STK_NONE && keep > STICK_RELEASE && (cand == STK_NONE || cand == prev))
        return prev;
    return cand;
}

// -------------------------------------------------------
// Blu-ray remote
// -------------------------------------------------------
// Key codes as the firmware reports them in button[25].  The remote has its
// own copies of the pad buttons (the PS3 row along the bottom), which map
// straight across; the disc-player keys become the media fields.
#define PAD_TYPE_BD_REMOTE 4
#define BD_KEY_NONE        0xff

static void bd_key(ButtonState *b, u16 code) {
    switch (code) {
    case 0x54: b->up       = 1; break;
    case 0x56: b->down     = 1; break;
    case 0x57: b->left     = 1; break;
    case 0x55: b->right    = 1; break;
    case 0x0b:                              // ENTER
    case 0x5e: b->cross    = 1; break;      // X
    case 0x0e:                              // RETURN
    case 0x5d: b->circle   = 1; break;      // O
    case 0x1a:                              // TOP MENU
    case 0x40:                              // POP UP/MENU
    case 0x5c: b->triangle = 1; break;      // TRIANGLE
    case 0x0f:                              // CLEAR (backspace in search)
    case 0x5f: b->square   = 1; break;      // SQUARE
    case 0x53: b->start    = 1; break;
    case 0x50: b->select   = 1; break;
    case 0x5a: b->l1       = 1; break;
    case 0x5b: b->r1       = 1; break;
    case 0x58: b->l2       = 1; break;
    case 0x59: b->r2       = 1; break;
    case 0x32: b->play     = 1; break;
    case 0x39: b->pause    = 1; break;
    case 0x38: b->stop     = 1; break;
    case 0x34:                              // SCAN >>
    case 0x61: b->ffwd     = 1; break;      // SLOW >
    case 0x33:                              // << SCAN
    case 0x60: b->rew      = 1; break;      // < SLOW
    case 0x31: b->next     = 1; break;
    case 0x30: b->prev     = 1; break;
    case 0x63: b->subtitle = 1; break;
    case 0x64: b->audio    = 1; break;
    case 0x28:                              // TIME
    case 0x70: b->info     = 1; break;      // DISPLAY
    default: {                              // digits, colour keys, eject...
        // Logged (first few) so a remote whose keys arrive as codes this
        // table does not know can be mapped from its player_log.txt.
        static int s_unknown_logged = 0;
        static u16 s_last_unknown = 0xffff;
        if (code != s_last_unknown && s_unknown_logged < 16) {
            char line[64];
            snprintf(line, sizeof line, "input: bd remote key 0x%02x (unmapped)",
                     (unsigned)code);
            plog(line);
            s_unknown_logged++;
        }
        s_last_unknown = code;
        break;
    }
    }
}

// -------------------------------------------------------
// Keyboards
// -------------------------------------------------------
// Packet mode with raw codes: each read that carries data lists every key
// currently held (as HID usage codes), so held keys stay held between
// reports.  A read with nb_keycode == 0 means "no new data", not "nothing
// held" -- releases arrive as a packet whose entries are 0 (no event).  The
// first few packets are logged so that assumption can be checked on a real
// keyboard.
#define KB_PORTS 7

static u16  s_kb_keys[KB_PORTS][MAX_KEYCODES];
static int  s_kb_nkeys[KB_PORTS];
static bool s_kb_ready[KB_PORTS];
static int  s_kb_logged;

static void plogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plogf(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    plog(line);
}

static void kb_key(ButtonState *b, u16 raw) {
    switch (raw & 0xff) {
    case KB_RAWKEY_UP_ARROW:    b->up       = 1; break;
    case KB_RAWKEY_DOWN_ARROW:  b->down     = 1; break;
    case KB_RAWKEY_LEFT_ARROW:  b->left     = 1; break;
    case KB_RAWKEY_RIGHT_ARROW: b->right    = 1; break;
    case KB_RAWKEY_ENTER:
    case 0x58:                  b->cross    = 1; break;   // keypad Enter
    case KB_RAWKEY_ESC:
    case KB_RAWKEY_BS:          b->circle   = 1; break;
    case KB_RAWKEY_TAB:         b->triangle = 1; break;
    case KB_RAWKEY_DELETE:      b->square   = 1; break;
    case KB_RAWKEY_PAGE_UP:     b->l1       = 1; break;
    case KB_RAWKEY_PAGE_DOWN:   b->r1       = 1; break;
    case 0x2f:                  b->l2       = 1; break;   // [
    case 0x30:                  b->r2       = 1; break;   // ]
    case KB_RAWKEY_SPACE:       b->playpause = 1; break;
    default: break;
    }
}

static void poll_keyboards(ButtonState *b, bool *any) {
    KbInfo ki;
    if (ioKbGetInfo(&ki) != 0) return;
    for (int k = 0; k < KB_PORTS; k++) {
        if (!ki.status[k]) {
            s_kb_ready[k] = false;
            s_kb_nkeys[k] = 0;
            continue;
        }
        if (!s_kb_ready[k]) {
            ioKbSetReadMode(k, KB_RMODE_PACKET);
            ioKbSetCodeType(k, KB_CODETYPE_RAW);
            ioKbClearBuf(k);
            s_kb_ready[k] = true;
            s_kb_nkeys[k] = 0;
            plogf("input: keyboard on port %d", k);
        }
        KbData kd;
        if (ioKbRead(k, &kd) == 0 && kd.nb_keycode > 0) {
            int n = 0;
            for (int i = 0; i < kd.nb_keycode && i < MAX_KEYCODES; i++)
                if (kd.keycode[i] & 0xff) s_kb_keys[k][n++] = kd.keycode[i];
            s_kb_nkeys[k] = n;
            if (s_kb_logged < 12) {
                s_kb_logged++;
                plogf("input: kb%d packet nb=%d held=%d first=0x%04x",
                     k, (int)kd.nb_keycode, n, n ? s_kb_keys[k][0] : 0);
            }
        }
        for (int i = 0; i < s_kb_nkeys[k]; i++) kb_key(b, s_kb_keys[k][i]);
        *any = true;
    }
}

void input_init(void) {
    ioKbInit(KB_PORTS);
}

// Menus only (the XMB loop calls it after poll_buttons): the remote's skip and
// scan keys switch tabs as L1 / R1 do.  A Bluetooth remote with no shoulder
// buttons was otherwise stuck on Home (tester, 2026-09-27).  Not in
// poll_buttons() itself: the player and the music screen give these keys
// their own meaning, and the music screen already treats R1 as NEXT.
void input_media_keys_as_shoulders(void) {
    btn_cur.l1 |= btn_cur.prev | btn_cur.rew;
    btn_cur.r1 |= btn_cur.next | btn_cur.ffwd;
}

// JellyDrop was L1+R1 here (a chord that held every lone shoulder back
// 110 ms).  It is SELECT now, bound where the wave is on screen -- the XMB
// (ui_xmb.cpp) and the music screen -- not here, because the video player's
// Select is "next episode".

bool poll_buttons(void) {
    // Per-port last report.  A read with len == 0 means "nothing changed",
    // so the port keeps its previous state -- per port, which is what lets a
    // held pad button stay held while a keyboard or remote is also sending.
    static padData s_last[MAX_PORT_NUM];
    static u8 s_stick[MAX_PORT_NUM];
    static u8 s_seen_type[MAX_PORT_NUM];
    padInfo2 pi;
    ButtonState merged; memset(&merged, 0, sizeof(merged));
    bool any = false;
    if (ioPadGetInfo2(&pi) == 0) {
        for (int i = 0; i < MAX_PORT_NUM; i++) {
            if (!(pi.port_status[i] & 1)) {
                s_seen_type[i] = 0;
                memset(&s_last[i], 0, sizeof(s_last[i]));
                continue;
            }
            const u32 type = pi.device_type[i];
            if (s_seen_type[i] != type + 1) {
                s_seen_type[i] = (u8)(type + 1);
                memset(&s_last[i], 0, sizeof(s_last[i]));
                plogf("input: port %d device_type=%u cap=0x%x", i,
                     (unsigned)type, (unsigned)pi.device_capability[i]);
            }
            padData pd;
            if (type == PAD_TYPE_BD_REMOTE) {
                u32 t = PAD_TYPE_BD_REMOTE;
                if (ioPadGetDataExtra(i, &t, &pd) == 0 && pd.len > 0) s_last[i] = pd;
                if (s_last[i].len > 0 && s_last[i].button[25] != BD_KEY_NONE)
                    bd_key(&merged, s_last[i].button[25]);
                any = true;
                continue;
            }
            if (ioPadGetData(i, &pd) == 0 && pd.len > 0) s_last[i] = pd;
            const padData *cur = &s_last[i];
            if (cur->len <= 0) continue;
            or_pad(&merged, cur);
            // Sticks are words 4..7; a pad that sends fewer has none.
            if (cur->len >= 8) {
                s_stick[i] = (u8)stick_dir(s_stick[i], (int)cur->ANA_L_H - 128,
                                           (int)cur->ANA_L_V - 128);
                merged.up    |= s_stick[i] == STK_UP;
                merged.down  |= s_stick[i] == STK_DOWN;
                merged.left  |= s_stick[i] == STK_LEFT;
                merged.right |= s_stick[i] == STK_RIGHT;
            }
            any = true;
        }
    }
    poll_keyboards(&merged, &any);
    btn_prev = btn_cur;
    btn_cur  = merged;
    // XMB sounds on the button edges.  The cursor sound for the d-pad is in
    // btn_nav_repeat() instead, so it ticks with each step of a held scroll.
    // During video playback the effects port is suspended and these no-op.
    if (BTN_PRESSED(cross))                       ui_sfx_play(SFX_DECIDE);
    else if (BTN_PRESSED(circle))                 ui_sfx_play(SFX_CANCEL);
    else if (BTN_PRESSED(triangle))               ui_sfx_play(SFX_OPTION);
    else if (BTN_PRESSED(l1) || BTN_PRESSED(r1))  ui_sfx_play(SFX_CURSOR);
    return any;
}

void init_btns(void) {
    poll_buttons();
    btn_prev = btn_cur;
}

// Auto-repeat for held navigation buttons.  Returns true on the initial press,
// then every NAV_REPEAT_US after an initial NAV_DELAY_US for as long as the button
// is held — giving menus a steady, controllable scroll instead of one-per-tap.
bool btn_nav_repeat(bool held, int slot) {
    static u64  next_us[NAV_REPEAT_SLOTS] = { 0 };
    static bool active[NAV_REPEAT_SLOTS]  = { false };
    if (slot < 0 || slot >= NAV_REPEAT_SLOTS) return false;
    if (!held) { active[slot] = false; return false; }
    u64 now = timing_get_us();
    if (!active[slot]) {                       // first press
        active[slot]  = true;
        next_us[slot] = now + NAV_DELAY_US;
        if (slot <= NAV_right) { ui_sfx_play(SFX_CURSOR); nav_step_to_wave(slot); }
        return true;
    }
    if (now >= next_us[slot]) {                // repeat tick
        next_us[slot] = now + NAV_REPEAT_US;
        if (slot <= NAV_right) { ui_sfx_play(SFX_CURSOR); nav_step_to_wave(slot); }
        return true;
    }
    return false;
}
