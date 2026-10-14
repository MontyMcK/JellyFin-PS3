#include "h264_sps.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    int  n;            // bytes
    int  bit;          // bit position
    bool err;
} Bits;

static unsigned get1(Bits *b) {
    if (b->bit >= b->n * 8) { b->err = true; return 0; }
    const unsigned v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1u;
    b->bit++;
    return v;
}

static uint32_t getn(Bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | get1(b);
    return v;
}

static uint32_t get_ue(Bits *b) {
    int zeros = 0;
    while (!get1(b) && !b->err) {
        if (++zeros > 31) { b->err = true; return 0; }
    }
    if (zeros == 0) return 0;
    return ((1u << zeros) - 1u) + getn(b, zeros);
}

static int32_t get_se(Bits *b) {
    const uint32_t k = get_ue(b);
    return (k & 1u) ? (int32_t)((k + 1u) >> 1) : -(int32_t)(k >> 1);
}

static void skip_scaling_list(Bits *b, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0) {
            next = (last + get_se(b) + 256) % 256;
        }
        last = next ? next : last;
    }
}

bool h264_parse_sps(const uint8_t *nal, int len, H264SpsInfo *out) {
    memset(out, 0, sizeof *out);
    if (len < 5 || (nal[0] & 0x1F) != 7) return false;

    // unescape (00 00 03 -> 00 00), after the NAL header byte
    uint8_t rbsp[512];
    int n = 0, zeros = 0;
    for (int i = 1; i < len && n < (int)sizeof rbsp; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        rbsp[n++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    Bits b = { rbsp, n, 0, false };

    out->profile_idc = (int)getn(&b, 8);
    getn(&b, 8);                                   // constraint flags + reserved
    out->level_idc = (int)getn(&b, 8);
    get_ue(&b);                                    // seq_parameter_set_id
    out->chroma_format_idc = 1;
    out->bit_depth = 8;
    bool separate_planes = false;
    switch (out->profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
        out->chroma_format_idc = (int)get_ue(&b);
        if (out->chroma_format_idc == 3) separate_planes = get1(&b) != 0;
        out->bit_depth = 8 + (int)get_ue(&b);
        get_ue(&b);                                // chroma bit depth
        get1(&b);                                  // qpprime_y_zero_transform_bypass
        if (get1(&b)) {                            // seq_scaling_matrix_present
            const int lists = out->chroma_format_idc != 3 ? 8 : 12;
            for (int i = 0; i < lists && !b.err; i++)
                if (get1(&b)) skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
        break;
    }
    default: break;
    }
    get_ue(&b);                                    // log2_max_frame_num_minus4
    const uint32_t poc_type = get_ue(&b);
    if (poc_type == 0) {
        get_ue(&b);                                // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        get1(&b);
        get_se(&b); get_se(&b);
        const uint32_t cycle = get_ue(&b);
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle && !b.err; i++) get_se(&b);
    }
    out->max_num_ref_frames = (int)get_ue(&b);
    get1(&b);                                      // gaps_in_frame_num_value_allowed
    const uint32_t mbs_w = get_ue(&b) + 1u;
    const uint32_t map_h = get_ue(&b) + 1u;
    out->frame_mbs_only = get1(&b) != 0;
    if (!out->frame_mbs_only) get1(&b);            // mb_adaptive_frame_field
    get1(&b);                                      // direct_8x8_inference
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (get1(&b)) {
        crop_l = get_ue(&b); crop_r = get_ue(&b); crop_t = get_ue(&b); crop_b = get_ue(&b);
    }
    if (b.err || mbs_w == 0 || mbs_w > 1024 || map_h > 1024) return false;

    int cux = 1, cuy = 1;                          // crop units (4:2:0 frame coded: 2x2)
    const int chroma = separate_planes ? 0 : out->chroma_format_idc;
    if (chroma == 1) { cux = 2; cuy = 2; } else if (chroma == 2) { cux = 2; cuy = 1; }
    if (!out->frame_mbs_only) cuy *= 2;
    out->width  = (int)(mbs_w * 16u) - (int)((crop_l + crop_r) * (uint32_t)cux);
    out->height = (int)(map_h * 16u * (out->frame_mbs_only ? 1u : 2u)) - (int)((crop_t + crop_b) * (uint32_t)cuy);

    if (get1(&b)) {                                // vui_parameters_present
        if (get1(&b)) {                            // aspect_ratio_info_present
            if (getn(&b, 8) == 255) { getn(&b, 16); getn(&b, 16); }
        }
        if (get1(&b)) get1(&b);                    // overscan
        if (get1(&b)) {                            // video_signal_type_present
            getn(&b, 4);
            if (get1(&b)) getn(&b, 24);            // colour description
        }
        if (get1(&b)) { get_ue(&b); get_ue(&b); }  // chroma_loc_info
        if (get1(&b)) {                            // timing_info_present
            out->num_units_in_tick = getn(&b, 32);
            out->time_scale = getn(&b, 32);
            if (!b.err && out->num_units_in_tick != 0 && out->time_scale != 0) {
                out->has_timing = true;
                out->fps = (double)out->time_scale / (2.0 * (double)out->num_units_in_tick);
            }
        }
    }
    b.err = false;                                 // running out of bits inside the VUI is not fatal
    return out->width > 0 && out->height > 0;
}

bool h264_playable(const H264SpsInfo *s, char *reason, int cap) {
    const char *why = NULL;
    char buf[40];
    switch (s->profile_idc) {
    case 66: case 77: case 88: case 100: break;
    case 110: why = "10-bit"; break;
    case 122: why = "4:2:2"; break;
    case 244: case 44: why = "4:4:4"; break;
    default:
        snprintf(buf, sizeof buf, "profile %d", s->profile_idc); why = buf; break;
    }
    if (!why && s->bit_depth != 8) why = "10-bit";
    if (!why && s->chroma_format_idc != 1) why = "not 4:2:0";
    if (!why && (s->width > 1920 || s->height > 1088)) why = s->width >= 3840 ? "4K" : "bigger than 1080p";
    if (!why && s->level_idc > 42) {
        snprintf(buf, sizeof buf, "Level %d.%d", s->level_idc / 10, s->level_idc % 10);
        why = buf;
    }
    if (reason && cap > 0) snprintf(reason, (size_t)cap, "%s", why ? why : "");
    return why == NULL;
}
