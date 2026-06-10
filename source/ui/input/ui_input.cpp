// Pad input — merged multi-pad polling, edge detection, nav auto-repeat.

#include <string.h>

#include "ui.h"
#include "timing.h"

ButtonState btn_cur  = {0};
ButtonState btn_prev = {0};

void update_buttons(padData *pad) {
    btn_prev         = btn_cur;
    btn_cur.up       = pad->BTN_UP;
    btn_cur.down     = pad->BTN_DOWN;
    btn_cur.left     = pad->BTN_LEFT;
    btn_cur.right    = pad->BTN_RIGHT;
    btn_cur.cross    = pad->BTN_CROSS;
    btn_cur.circle   = pad->BTN_CIRCLE;
    btn_cur.square   = pad->BTN_SQUARE;
    btn_cur.triangle = pad->BTN_TRIANGLE;
    btn_cur.start    = pad->BTN_START;
    btn_cur.select   = pad->BTN_SELECT;
    btn_cur.l1       = pad->BTN_L1;
    btn_cur.r1       = pad->BTN_R1;
    btn_cur.l2       = pad->BTN_L2;
    btn_cur.r2       = pad->BTN_R2;
}

bool poll_buttons(void) {
    padInfo pi;
    ioPadGetInfo(&pi);
    padData merged; memset(&merged, 0, sizeof(merged));
    bool any = false;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pi.status[i]) continue;
        padData pd;
        ioPadGetData(i, &pd);
        if (!pd.len) continue;
        merged.BTN_UP       |= pd.BTN_UP;
        merged.BTN_DOWN     |= pd.BTN_DOWN;
        merged.BTN_LEFT     |= pd.BTN_LEFT;
        merged.BTN_RIGHT    |= pd.BTN_RIGHT;
        merged.BTN_CROSS    |= pd.BTN_CROSS;
        merged.BTN_CIRCLE   |= pd.BTN_CIRCLE;
        merged.BTN_SQUARE   |= pd.BTN_SQUARE;
        merged.BTN_TRIANGLE |= pd.BTN_TRIANGLE;
        merged.BTN_START    |= pd.BTN_START;
        merged.BTN_SELECT   |= pd.BTN_SELECT;
        merged.BTN_L1       |= pd.BTN_L1;
        merged.BTN_R1       |= pd.BTN_R1;
        merged.BTN_L2       |= pd.BTN_L2;
        merged.BTN_R2       |= pd.BTN_R2;
        any = true;
    }
    // If no pad delivered a fresh packet this frame, keep the last known button
    // state (the controller simply hasn't sent new data yet) — exactly like a game
    // does, so a held trigger reads as continuously held instead of flickering.
    // Still advance btn_prev so the BTN_PRESSED edge doesn't re-fire every idle
    // frame (which would spam taps/pause for any held button).
    if (!any) { btn_prev = btn_cur; return false; }
    merged.len = 1;
    update_buttons(&merged);
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
        return true;
    }
    if (now >= next_us[slot]) {                // repeat tick
        next_us[slot] = now + NAV_REPEAT_US;
        return true;
    }
    return false;
}
