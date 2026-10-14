#pragma once
// UI scale — how the XMB's 1280x720 authored numbers map onto the framebuffer.
//
// The value itself and the accessors live in ui_visuals.h, beside the macros
// that use them, so every UI translation unit gets them inline.  This header is
// only the boot-time load, which main.cpp calls.

// Read jellyfin_uiscale.txt.  Missing or unparseable leaves the default (auto).
// Call AFTER plog_load_setting(), or its one log line is discarded.
void ui_scale_load(void);

// 0 = auto (proportional to the framebuffer), else the percentage in force.
int  ui_scale_pct(void);
