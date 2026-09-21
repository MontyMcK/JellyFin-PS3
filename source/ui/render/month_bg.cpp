#include "month_bg.h"

void month_bg_load(void)
{
    /* Reserved for a future cached calendar/background table. */
}

bg_quad month_bg_current(u32 top, u32 bot)
{
    /*
     * Deliberately deterministic: wave_draw() can be called multiple times per
     * frame, and this function must return the same background for every call.
     */
    return bg_from_two(top, bot);
}
