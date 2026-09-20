#pragma once
// Host stand-in: ui.h includes <io/pad.h> for padData, which none of the text
// code touches -- it only needs to be a type so the declaration parses.
#include <ppu-types.h>
typedef struct { u16 len; u16 button[12]; } padData;
