// UI lifecycle — one-time setup, RSX state restore, teardown.
// The rest of the UI lives in input/, osk/, xmb/, and render/.

#include <rsx/rsx.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_wave.h"

extern void crash_log(const char *msg);

void ui_init(void) {
    crash_log("6.1 blend state");
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    crash_log("6.2 setRenderTarget");
    setRenderTarget(curr_fb);
    crash_log("6.3 ttf_init");
    ttf_init();
    crash_log("6.4 wave_init");
    wave_init();
    crash_log("6.5 ui_init done");
}

void ui_restore_rsx_state(void) {
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    setRenderTarget(curr_fb);
}

void ui_cleanup(void) {
    visuals_cleanup();
}
