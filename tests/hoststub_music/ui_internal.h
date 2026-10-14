#pragma once
// Host stand-in for ui/xmb/ui_internal.h: the JSON helpers music/music_player.cpp's track fetch uses.
bool      xmb_json_str_range(const char *s, int len, const char *key, char *out, int cap);
long long xmb_json_ll_range(const char *s, int len, const char *key, long long def);
int       xmb_json_int_range(const char *s, int len, const char *key, int def);
bool      xmb_json_first_arr_str(const char *s, int len, const char *key, char *out, int cap);
void      decode_unicode_escapes(char *str);
