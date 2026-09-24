#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Subtitle colour
// -------------------------------------------------------------------------
//  Three choices, each a complete look rather than a colour swatch: fill,
//  outline and outline weight move together, because they have to. A pale
//  fill needs a heavier outline than a saturated one to hold its edge over a
//  bright scene, and a translucent fill needs a thin outline or the outline
//  becomes the brightest thing on screen.
//
//    White        opaque white, heavy outline. The broadcast default and what
//                 every player falls back to.
//    Soft Yellow  the warm yellow DVDs and television subtitling used for
//                 decades. It sits away from the grey-white of most film
//                 imagery, so it separates from the picture without the glare
//                 of pure white -- easier on the eyes in a dark room.
//    Soft Grey    a slightly translucent off-white with a THIN outline. The
//                 least intrusive of the three: present when you look for it,
//                 much quieter over a bright frame.
//
//  Colours are 0xRRGGBBAA, matching drawTTF().
//
//  Persisted as a digit beside the other settings files.

typedef enum {
    SUBCOLOR_WHITE  = 0,
    SUBCOLOR_YELLOW = 1,
    SUBCOLOR_GREY   = 2,
    SUBCOLOR_COUNT  = 3,
} subcolor_t;

void        subcolor_load(void);
void        subcolor_cycle(void);
subcolor_t  subcolor_get(void);
const char *subcolor_label(void);

// The look for the current choice.  `outline_px` is the half-width of the
// outline in pixels at the given picture height -- 0 would mean no outline,
// which none of these use, because subtitles sit over arbitrary imagery.
u32 subcolor_fill(void);
u32 subcolor_outline(void);
int subcolor_outline_px(u32 display_height);
