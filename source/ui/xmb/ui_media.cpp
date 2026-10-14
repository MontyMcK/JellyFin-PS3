// The Media tab: the internal disk's folders and USB drives, played with no server.
//
//   * The tab itself is a list of places: Downloads (the Offline library), the internal disk, and
//     each USB drive that is plugged in -- a drive appears or goes while the tab is open (lfs's
//     generation counter), with the menu sound.
//   * A place opens the folder browser, a screen of its own like the Offline library: folders and
//     video files, folders first in natural order (lfs_list sorts them).  Video rows show the
//     name as a person would say it ("The Movie Name (2019)"), the size, and -- from a worker
//     that probes the rows on screen one at a time -- the length, or "HEVC (can't play on PS3)"
//     for what the console cannot decode.
//   * A video opens its details: what the file is (container, picture, audio tracks), and Play or
//     Resume.  The player is the ordinary one (show_player_file); where it ended goes to the resume
//     table, a file on the console's disk -- the drive is never written to.
//
// Everything that decides something is portable and host-tested: names and sizes
// (local/local_names), the resume rules (local/local_resume), what a file is and whether it can
// be played (local/local_probe).  This file lays it out and forwards.  Music files are not
// listed yet: the browser shows folders and video.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_spine.h"          // spine_frame_id
#include "ui_wave.h"
#include "ui_buffering.h"      // loading_run
#include "ui_sfx.h"
#include "rsxutil.h"
#include "timing.h"
#include "plog.h"
#include "slog.h"
#include "player.h"
#include "jf_paths.h"
#include "surround.h"
#include "audio_bitstream.h"
#include "lfs.h"
#include "lfs_path.h"
#include "local_probe.h"
#include "local_names.h"
#include "local_resume.h"
#include "local_tags.h"
#include "local_audio.h"
#include "local_music_fs.h"
#include "music_screen.h"
#include "dl_library.h"

namespace {

const char *MIDDOT = "\xC2\xB7";

// ---------------------------------------------------------------------------
//  Geometry and small drawing helpers
// ---------------------------------------------------------------------------

int list_x(void)      { return XMB_ITEM_PAD; }
int list_w(void)      { return (int)display_width - 2 * XMB_ITEM_PAD; }
int row_h(void)       { return UIS_H(62); }
int row_pitch(void)   { return row_h() + UIS_H(8); }

// The tab's own list sits in the XMB's content band; the standalone screens have a title above theirs.
int tab_top(void)     { return XMB_CONTENT_Y + UIS_H(10); }
int screen_top(void)  { return XMB_OY + UIS_H(112); }
int bottom(void)      { return (int)display_height - XMB_BOTTOM_PAD; }
int rows_in(int top)  {
    const int n = (bottom() - top + UIS_H(8)) / row_pitch();
    return n < 1 ? 1 : n;
}

// Text clipped to max_w with an ellipsis.
void clip_text(int x, int y, const char *text, float px, u32 colour, int max_w, bool bold) {
    char buf[200];
    snprintf(buf, sizeof buf, "%s", text);
    int len = (int)strlen(buf);
    if (ttf_text_width(buf, px, bold) > max_w) {
        while (len > 3 && ttf_text_width(buf, px, bold) > max_w) {
            len--;
            while (len > 3 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;     // not inside a character
            buf[len] = '\0';
        }
        if (len > 3) { buf[len - 1] = '.'; buf[len - 2] = '.'; buf[len - 3] = '.'; }
    }
    drawTTF((u32)x, (u32)y, buf, px, colour, bold);
}

void frame_begin(void) {
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();
}

void draw_row_frame(int x, int y, int w, int h, bool sel) {
    drawRect((u32)x, (u32)y, (u32)w, (u32)h, sel ? XMB_PANEL_HI : XMB_PANEL);
    if (sel) drawRect((u32)(x - UIS_W(4)), (u32)y, UIS_W(3), (u32)h, XMB_ACCENT);
}

void draw_screen_title(const char *title, const char *line) {
    const int x = XMB_ITEM_PAD, y = XMB_OY + UIS_H(28);
    drawTTF((u32)x, (u32)y, title, UIS_TF(28), XMB_TEXT, true);
    if (line && line[0]) clip_text(x, y + UIS_H(42), line, UIS_TF(15), XMB_TEXT_DIM, list_w(), false);
}

void centre_message(const char *m, int y) {
    const int cx = (int)display_width / 2;
    drawTTF((u32)(cx - ttf_text_width(m, UIS_TF(18)) / 2), (u32)y, m, UIS_TF(18), XMB_TEXT_DIM);
}

// A screen waits for the buttons that opened it to be let go before it takes any press.
struct Arm {
    bool armed = false;
    bool ready(void) {
        if (!armed && !btn_cur.cross && !btn_cur.circle && !btn_cur.square && !btn_cur.triangle) armed = true;
        return armed;
    }
};

void screen_begin(void) { rsxSync(); flip(); init_btns(); }

// ---------------------------------------------------------------------------
//  The resume table: a file on the console's disk, never the drive
// ---------------------------------------------------------------------------

LResTable s_res;
bool      s_res_loaded = false;

const char *res_path(void) { return jf_data_path("jellyfin_local_resume.txt"); }

void res_ensure(void) {
    if (s_res_loaded) return;
    s_res_loaded = true;
    lres_load_file(&s_res, res_path());
}

uint64_t res_key_of(const char *path, const lfs_entry &e) { return lres_key(path, e.size, e.mtime); }

// ---------------------------------------------------------------------------
//  The drives
// ---------------------------------------------------------------------------

const int MAX_DRIVES = 9;

lfs_drive s_dr[MAX_DRIVES];
int       s_nd = 0;
uint32_t  s_gen = ~0u;
int       s_sel = 0;
int       s_dl_count = 0;

void drive_sub(const lfs_drive &d, char *out, int cap) {
    if (d.kind == LFS_USB_UNSUPPORTED) { snprintf(out, (size_t)cap, TR("A file system this app cannot read")); return; }
    char tot[24] = "", fr[24] = "";
    if (d.total) local_format_size(d.total, tot, sizeof tot);
    if (d.free)  local_format_size(d.free, fr, sizeof fr);
    int n = snprintf(out, (size_t)cap, "%s", lfs_kind_name(d.kind));
    if (tot[0] && n < cap) n += snprintf(out + n, (size_t)cap - (size_t)n, "  %s  %s", MIDDOT, tot);
    if (fr[0] && n < cap)  snprintf(out + n, (size_t)cap - (size_t)n, TR("  %s  %s free"), MIDDOT, fr);
}

// Rows: Downloads, then the drives.  Re-read when the set of drives changes, and the number of
// downloads about once a second.
void refresh_drives(bool sound) {
    const uint32_t g = lfs_generation();
    if (g != s_gen) {
        const bool first = s_gen == ~0u;
        const int before = s_nd;
        s_gen = g;
        s_nd = lfs_drives(s_dr, MAX_DRIVES);
        if (s_nd < 0) s_nd = 0;
        if (s_sel > s_nd) s_sel = s_nd;
        if (sound && !first && s_nd != before) ui_sfx_play(SFX_OPTION);
    }
    static u64 s_dl_at = 0;
    const u64 now = timing_get_us();
    if (now - s_dl_at > 1000000ULL || s_dl_at == 0) {
        s_dl_at = now;
        static char ids[DL_MAX_ITEMS][DL_ID_MAX];
        s_dl_count = dl_library_ids(ids, DL_MAX_ITEMS);
    }
}

int root_rows(void) { return 1 + s_nd; }

// ---------------------------------------------------------------------------
//  Probing the rows on screen, one file at a time, off the render thread
// ---------------------------------------------------------------------------

struct Probed {
    char     path[256];
    bool     readable;       // the probe could read the file
    bool     playable;
    uint32_t secs;
    char     note[64];       // "HEVC video (can't play on PS3)": for the row, faint
    bool     music;          // a music file: label and note are about its tags and format
    char     label[256];     // a music file's "title - artist" when it has tags
};

const int PC_N = 48;
Probed   pc[PC_N];
int      pc_used = 0, pc_next = 0;

volatile int pw_state = 0;                 // 0 idle, 1 asked, 2 running, 3 done
char         pw_path[256];
Probed       pw_out;
sys_ppu_thread_t pw_tid;
bool             pw_have = false;
volatile bool    pw_run = false;

int probe_rd(void *ctx, uint64_t off, uint8_t *buf, int len) {
    const int got = lfs_read(*(const int *)ctx, off, buf, (uint32_t)len);
    return got < 0 ? -1 : got;
}

const char *leaf_of(const char *path) { return lfs_path_leaf(path); }

// What a probe made of a file, as the browser shows it.
void summarize(const LocalInfo &info, Probed *r) {
    r->secs = info.duration_secs;
    const LocalAudioPrefs prefs = { surround_enabled(), audio_passthrough_wanted() };
    char why[160];
    r->playable = local_can_play(&info, &prefs, why, sizeof why);
    r->note[0] = '\0';
    if (!info.video_ok)
    {
        if (info.video_reason[0]) snprintf(r->note, sizeof r->note, TR("%.40s video (can't play on PS3)"), info.video_reason);
        else                      snprintf(r->note, sizeof r->note, "%s", TR("This video can't play on PS3"));
    }
    else if (!r->playable)
        snprintf(r->note, sizeof r->note, TR("Audio can't be played yet"));
}

// What a music file's tags and format made of it, as the browser shows it.
void summarize_music(const LaMeta &m, Probed *r) {
    r->music = true;
    r->secs = m.duration_secs;
    r->playable = la_can_decode(&m);
    r->label[0] = '\0';
    if (m.title[0] && m.artist[0]) snprintf(r->label, sizeof r->label, "%s - %s", m.title, m.artist);
    else if (m.title[0]) snprintf(r->label, sizeof r->label, "%s", m.title);
    if (r->playable) la_format_line(&m, r->note, sizeof r->note);
    else snprintf(r->note, sizeof r->note, TR("This format can't be played yet"));
}

void pw_main(void *) {
    static LocalInfo info;                 // the worker's own (1.7 KB)
    static LaMeta meta;
    while (pw_run) {
        if (pw_state != 1) { usleep(30000); continue; }
        __sync_synchronize();
        pw_state = 2;
        Probed r;
        memset(&r, 0, sizeof r);
        snprintf(r.path, sizeof r.path, "%s", pw_path);
        int fd = lfs_open(r.path);
        if (fd >= 0) {
            char err[64] = "";
            const uint64_t size = lfs_size(fd);
            const LaKind music = la_kind_of(r.path);
            if (music != LAF_NONE) {
                r.music = true;
                r.readable = la_read_meta(probe_rd, &fd, size, music, &meta);
                lfs_close(fd);
                if (r.readable) summarize_music(meta, &r);
                else snprintf(r.note, sizeof r.note, TR("Can't read this file"));
            } else {
                r.readable = local_probe(probe_rd, &fd, size, leaf_of(r.path), &info, err, sizeof err);
                lfs_close(fd);
                if (r.readable) summarize(info, &r);
                else snprintf(r.note, sizeof r.note, TR("Can't read this file"));
            }
        } else {
            snprintf(r.note, sizeof r.note, TR("Can't read this file"));
        }
        pw_out = r;
        __sync_synchronize();
        pw_state = 3;
    }
    sysThreadExit(0);
}

void pw_start(void) {
    if (pw_have) return;
    pw_state = 0;
    pw_run = true;
    static char name[] = "jf_mprobe";
    if (sysThreadCreate(&pw_tid, pw_main, NULL, 1300, 128 * 1024, THREAD_JOINABLE, name) != 0) {
        pw_run = false;
        plog("media: probe thread did not start");
        return;
    }
    pw_have = true;
}

void pw_stop(void) {
    pw_run = false;
    if (pw_have) { u64 r; sysThreadJoin(pw_tid, &r); pw_have = false; }
    pw_state = 0;
}

// Before the player starts: a probe still reading the drive would compete with it.
void pw_quiesce(void) {
    for (int i = 0; i < 300 && pw_state == 2 && running; i++) usleep(10000);
}

const Probed *pc_get(const char *path) {
    for (int i = 0; i < pc_used; i++) if (!strcmp(pc[i].path, path)) return &pc[i];
    return NULL;
}

void pc_clear(void) { pc_used = pc_next = 0; if (pw_state == 3) pw_state = 0; }

// ---------------------------------------------------------------------------
//  The folder browser
// ---------------------------------------------------------------------------

const int MAX_ENTRIES = 1024;
const int MAX_DEPTH = 24;

struct Level { char path[256]; int sel, top; };

lfs_entry *b_all = NULL;      // the folder's folders and videos
int        b_n = 0;
int        b_total = 0;       // what the folder holds (more than we keep, when it is huge)

struct ListJob { char path[256]; lfs_entry *out; int max; int result; };

void list_work(void *arg) {
    ListJob *j = (ListJob *)arg;
    j->result = lfs_list(j->path, j->out, j->max, 0);
}

// Lists `path` behind the loading screen and keeps the folders and the videos.  A negative result is
// an lfs error code; otherwise b_n entries are held.
int load_dir(const char *path) {
    static ListJob job;
    memset(&job, 0, sizeof job);
    snprintf(job.path, sizeof job.path, "%s", path);
    job.out = b_all;
    job.max = MAX_ENTRIES;
    loading_run(list_work, &job, TR("Loading"), false);
    screen_begin();
    b_n = 0;
    b_total = 0;
    if (job.result < 0) return job.result;
    b_total = job.result;
    const int got = job.result < MAX_ENTRIES ? job.result : MAX_ENTRIES;
    for (int i = 0; i < got; i++) {
        if (b_all[i].is_dir || lfs_media_kind_of(b_all[i].name) != LFS_KIND_OTHER)
            b_all[b_n++] = b_all[i];
    }
    return b_n;
}

bool drive_present(const char *drive_id) {
    for (int i = 0; i < s_nd; i++) if (!strcmp(s_dr[i].id, drive_id)) return true;
    return false;
}

// "USB drive 1 / Films / Marvel" from "usb0:/Films/Marvel".
void pretty_path(const char *path, const char *drive_label, char *out, int cap) {
    char drive[16], rest[256];
    if (!lfs_path_split(path, drive, sizeof drive, rest, sizeof rest)) { snprintf(out, (size_t)cap, "%s", path); return; }
    int n = snprintf(out, (size_t)cap, "%s", drive_label);
    for (const char *p = rest; *p && n < cap - 4; p++) {
        if (*p == '/') {
            if (p[1]) { out[n++] = ' '; out[n++] = '/'; out[n++] = ' '; }
        } else out[n++] = *p;
    }
    out[n] = '\0';
}

// ---- a file's details ----

struct DetailJob { char path[256]; LocalInfo *info; bool ok; char err[64]; };

void detail_work(void *arg) {
    DetailJob *j = (DetailJob *)arg;
    int fd = lfs_open(j->path);
    if (fd < 0) { j->ok = false; snprintf(j->err, sizeof j->err, fd == LFS_E_REMOVED ? TR("The drive was removed.") : TR("The file could not be opened.")); return; }
    const uint64_t size = lfs_size(fd);
    j->ok = local_probe(probe_rd, &fd, size, leaf_of(j->path), j->info, j->err, sizeof j->err);
    lfs_close(fd);
    if (!j->ok && !j->err[0]) snprintf(j->err, sizeof j->err, TR("This file could not be read."));
}

void play_file(const char *path, const lfs_entry &e, const char *title, u32 resume_secs, uint32_t total_secs) {
    pw_quiesce();
    char why[160] = "";
    if (!show_player_file(path, title, resume_secs, why, sizeof why)) {
        xmb_dl_notice(TR("Can't play this file"), why[0] ? why : TR("The player could not start it."));
        return;
    }
    // where it ended: remembered, or forgotten when it was watched through
    res_ensure();
    lres_update(&s_res, res_key_of(path, e), player_last_position_secs(), total_secs);
    if (!lres_save_file(&s_res, res_path())) plog("media: the resume file could not be written");
    init_btns();
}

// ---- a folder's music ----

struct MusicJob {
    char       dir[256];
    lfs_entry *entries;
    int        n;
    LmTrack   *tracks;
    int        count, skipped;
    char       cover[32];
    bool       has_cover;
};

void music_work(void *arg) {
    MusicJob *j = (MusicJob *)arg;
    j->count = lm_scan_folder(j->dir, j->entries, j->n, j->tracks, LM_MAX_TRACKS, &j->skipped);
    j->has_cover = j->count > 0 && lm_register_cover(j->dir, j->tracks, j->count, j->cover, sizeof j->cover);
}

// The music of the folder `dir` (the entries b_all holds), from the file that was chosen: an album is the whole folder,
// in its order; a folder with more files than the queue holds starts at the chosen one.  Returns when the music stops.
void play_music(const char *dir, const lfs_entry &chosen) {
    static MusicJob job;
    static LmTrack tracks[LM_MAX_TRACKS];
    static MusicTrack queue[MUSIC_QUEUE_MAX];
    int music_files = 0, chosen_at = 0;
    for (int i = 0; i < b_n; i++) {
        if (b_all[i].is_dir || lfs_media_kind_of(b_all[i].name) != LFS_KIND_MUSIC) continue;
        if (!strcmp(b_all[i].name, chosen.name)) chosen_at = i;
        music_files++;
    }
    memset(&job, 0, sizeof job);
    snprintf(job.dir, sizeof job.dir, "%s", dir);
    job.entries = b_all + (music_files > LM_MAX_TRACKS ? chosen_at : 0);
    job.n = b_n - (music_files > LM_MAX_TRACKS ? chosen_at : 0);
    job.tracks = tracks;
    pw_quiesce();
    loading_run(music_work, &job, TR("Loading"), false);
    screen_begin();
    if (job.count == 0) {
        xmb_dl_notice(TR("Can't play this"), job.skipped ? TR("None of the music files here could be read or played.")
                                                     : TR("There is no music here."));
        return;
    }
    char want[256];
    int start = 0;
    if (lfs_path_join(dir, chosen.name, want, sizeof want))
        for (int i = 0; i < job.count; i++) if (!strcmp(tracks[i].path, want)) { start = i; break; }
    for (int i = 0; i < job.count; i++) {
        MusicTrack &q = queue[i];
        memset(&q, 0, sizeof q);
        char name[128];
        lm_display_title(&tracks[i], name, sizeof name);
        snprintf(q.id, sizeof q.id, "file:%d", i);
        snprintf(q.name, sizeof q.name, "%s", name);
        snprintf(q.artist, sizeof q.artist, "%s", tracks[i].artist);
        snprintf(q.art_id, sizeof q.art_id, "%s", job.cover);
        q.duration_secs = tracks[i].duration_secs;
        q.track_num = tracks[i].track_no;
        snprintf(q.path, sizeof q.path, "%s", tracks[i].path);
    }
    char album[128];
    lm_album_name(tracks, job.count, lfs_path_leaf(dir), album, sizeof album);
    char msg[160];
    snprintf(msg, sizeof msg, "media: music %d track(s) from %.60s, %d skipped", job.count, dir, job.skipped);
    plog(msg);
    music_screen_open_local(queue, job.count, start, album);
    init_btns();
}

void wrap_lines(int x, int &y, const char *text, float px, u32 colour, int max_w, int pitch) {
    clip_text(x, y, text, px, colour, max_w, false);
    y += pitch;
}

// A video's page.  Returns when the user backs out (after playing, too).
void show_details(const char *path, const lfs_entry &e) {
    static LocalInfo info;
    static DetailJob job;
    memset(&info, 0, sizeof info);
    memset(&job, 0, sizeof job);
    snprintf(job.path, sizeof job.path, "%s", path);
    job.info = &info;
    loading_run(detail_work, &job, TR("Loading"), false);
    screen_begin();

    LocalTitle lt;
    local_clean_name(e.name, &lt);
    char title[160];
    local_title_line(&lt, title, sizeof title);

    const LocalAudioPrefs prefs = { surround_enabled(), audio_passthrough_wanted() };
    char why[160] = "";
    const bool can = job.ok && local_can_play(&info, &prefs, why, sizeof why);

    res_ensure();
    uint32_t res_secs = 0, res_total = 0;
    const bool have_resume = job.ok && lres_find(&s_res, res_key_of(path, e), &res_secs, &res_total) && can;

    // the buttons
    char b_resume[48] = "";
    const char *labels[2];
    int n_labels = 0;
    if (can) {
        if (have_resume) { local_format_resume(res_secs, b_resume, sizeof b_resume); labels[n_labels++] = b_resume; labels[n_labels++] = TR("Play from the start"); }
        else labels[n_labels++] = TR("Play");
    }
    int sel = 0;
    Arm arm;

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        if (arm.ready()) {
            if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); break; }
            if (BTN_PRESSED(up) && sel > 0)                 { sel--; ui_sfx_play(SFX_CURSOR); }
            if (BTN_PRESSED(down) && sel < n_labels - 1)    { sel++; ui_sfx_play(SFX_CURSOR); }
            if (BTN_PRESSED(cross)) {
                if (n_labels == 0) { ui_sfx_play(SFX_CANCEL); break; }
                ui_sfx_play(SFX_DECIDE);
                const u32 from = (have_resume && sel == 0) ? res_secs : 0;
                play_file(path, e, title, from, info.duration_secs);
                break;                                         // back to the folder, whose row shows the new resume point
            }
        }

        frame_begin();
        const int x = XMB_ITEM_PAD, w = (int)display_width - 2 * XMB_ITEM_PAD;
        int y = XMB_OY + UIS_H(28);
        clip_text(x, y, title, UIS_TF(28), XMB_TEXT, w, true);
        y += UIS_H(44);
        clip_text(x, y, e.name, UIS_TF(13), XMB_TEXT_FAINT, w, false);
        y += UIS_H(34);
        if (!job.ok) {
            wrap_lines(x, y, job.err, UIS_TF(16), XMB_ACCENT_ALT, w, UIS_H(26));
        } else {
            char sz[24], du[32], line[240];
            local_format_size(e.size, sz, sizeof sz);
            local_format_duration(info.duration_secs, du, sizeof du);
            const char *cont = info.container == LM_MKV ? TR("Matroska") : info.container == LM_M2TS ? TR("Blu-ray m2ts") : TR("MPEG-TS");
            int n = snprintf(line, sizeof line, "%s  %s  %s", cont, MIDDOT, sz);
            if (du[0] && n < (int)sizeof line) n += snprintf(line + n, sizeof line - (size_t)n, "  %s  %s", MIDDOT, du);
            wrap_lines(x, y, line, UIS_TF(16), XMB_TEXT_DIM, w, UIS_H(26));
            wrap_lines(x, y, info.video_desc, UIS_TF(16), info.video_ok ? XMB_TEXT_DIM : XMB_ACCENT_ALT, w, UIS_H(34));

            drawTTF((u32)x, (u32)y, TR("Audio"), UIS_TF(15), XMB_TEXT, true);
            y += UIS_H(28);
            for (int i = 0; i < info.n_audio; i++) {
                char a[120];
                snprintf(a, sizeof a, "%s%s", info.audio[i].label, info.audio[i].decodable ? "" : TR("  (can't be decoded yet)"));
                wrap_lines(x + UIS_W(14), y, a, UIS_TF(14), info.audio[i].decodable ? XMB_TEXT_DIM : XMB_TEXT_FAINT, w - UIS_W(14), UIS_H(24));
            }
            if (info.n_audio == 0) wrap_lines(x + UIS_W(14), y, TR("None"), UIS_TF(14), XMB_TEXT_FAINT, w, UIS_H(24));
            y += UIS_H(8);
            if (info.n_subs > 0) {
                int shown = 0;
                for (int i = 0; i < info.n_subs; i++) shown += info.subs[i].usable ? 1 : 0;
                char s[96];
                if (shown == info.n_subs)
                    snprintf(s, sizeof s, shown == 1 ? TR("Subtitles: %d track") : TR("Subtitles: %d tracks"), shown);
                else if (shown == 0)
                    snprintf(s, sizeof s, info.n_subs == 1 ? TR("Subtitles: %d track in the file, none the player can show")
                                                         : TR("Subtitles: %d tracks in the file, none the player can show"), info.n_subs);
                else
                    snprintf(s, sizeof s, TR("Subtitles: %d of %d tracks can be shown"), shown, info.n_subs);
                wrap_lines(x, y, s, UIS_TF(14), XMB_TEXT_FAINT, w, UIS_H(30));
            }
            if (!can) {
                wrap_lines(x, y, why, UIS_TF(16), XMB_ACCENT_ALT, w, UIS_H(30));
            }
        }
        // the buttons, at the foot
        const int bh = UIS_H(48), bw = UIS_W(420);
        int by = bottom() - n_labels * (bh + UIS_H(8)) - UIS_H(10);
        for (int i = 0; i < n_labels; i++) {
            draw_row_frame(x, by, bw, bh, i == sel);
            drawTTF_vcentered((u32)(x + UIS_W(18)), by + bh / 2, labels[i], UIS_TF(19), i == sel ? XMB_WHITE : XMB_TEXT_DIM);
            by += bh + UIS_H(8);
        }
        { Hint h[2]; int nh = 0;
          if (n_labels) { h[nh].glyph = 'X'; h[nh].label = TRN("Select"); nh++; }
          h[nh].glyph = 'C'; h[nh].label = TRN("Back"); nh++;
          draw_hints_bar(h, nh); }
        flip();
    }
    init_btns();
}

// Chooses the next file for the worker: the selected row first, then the others on screen.
void probe_next(int sel, int top, int vis, const char *dir) {
    if (pw_state != 0 || !pw_have) return;
    int order[64]; int n = 0;
    order[n++] = sel;
    for (int i = 0; i < vis && n < 63; i++) if (top + i != sel) order[n++] = top + i;
    for (int k = 0; k < n; k++) {
        const int i = order[k];
        if (i < 0 || i >= b_n || b_all[i].is_dir) continue;
        char path[256];
        if (!lfs_path_join(dir, b_all[i].name, path, sizeof path)) continue;
        if (pc_get(path)) continue;
        snprintf(pw_path, sizeof pw_path, "%s", path);
        __sync_synchronize();
        pw_state = 1;
        return;
    }
}

void probe_collect(void) {
    if (pw_state != 3) return;
    __sync_synchronize();
    pc[pc_next] = pw_out;
    pc_next = (pc_next + 1) % PC_N;
    if (pc_used < PC_N) pc_used++;
    __sync_synchronize();
    pw_state = 0;
}

// A drive's (or the disk's) folders, from its root, until the user backs out to the list of places.
void browse(const lfs_drive &drive) {
    b_all = (lfs_entry *)malloc(sizeof(lfs_entry) * MAX_ENTRIES);
    if (!b_all) { xmb_dl_notice(TR("Not enough memory"), TR("The folder could not be listed.")); return; }
    pw_start();
    pc_clear();
    res_ensure();

    Level stack[MAX_DEPTH];
    int depth = 0;
    char cur[256];
    snprintf(cur, sizeof cur, "%s:/", drive.id);
    char drive_id[16];
    snprintf(drive_id, sizeof drive_id, "%s", drive.id);
    char drive_label[48];
    snprintf(drive_label, sizeof drive_label, "%s", drive.label);

    int sel = 0, top = 0;
    int status = load_dir(cur);
    uint32_t seen_gen = lfs_generation();
    Arm arm;
    bool leave = false;

    while (running && !leave) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        refresh_drives(true);

        // the drive was pulled
        if (lfs_generation() != seen_gen) {
            seen_gen = lfs_generation();
            if (!drive_present(drive_id)) {
                xmb_dl_notice(TR("The drive was removed"), TR("Plug it back in to browse it again."));
                break;
            }
        }
        if (status == LFS_E_REMOVED) {
            xmb_dl_notice(TR("The drive was removed"), TR("Plug it back in to browse it again."));
            break;
        }

        const int vis = rows_in(screen_top());
        probe_collect();
        probe_next(sel, top, vis, cur);

        if (arm.ready()) {
            const int was = sel;
            if (BTN_REPEAT(up)   && sel > 0)       sel--;
            if (BTN_REPEAT(down) && sel < b_n - 1) sel++;
            if (BTN_PRESSED(l2)) sel = sel - vis < 0 ? 0 : sel - vis;
            if (BTN_PRESSED(r2)) sel = sel + vis > b_n - 1 ? (b_n > 0 ? b_n - 1 : 0) : sel + vis;
            if (sel != was) ui_sfx_play(SFX_CURSOR);
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
            if (top > b_n - vis) top = b_n - vis;
            if (top < 0) top = 0;

            if (BTN_PRESSED(circle)) {
                ui_sfx_play(SFX_CANCEL);
                if (depth == 0) break;                         // the drive's root: back to the places
                depth--;
                snprintf(cur, sizeof cur, "%s", stack[depth].path);
                sel = stack[depth].sel; top = stack[depth].top;
                status = load_dir(cur);
                if (sel > b_n - 1) sel = b_n > 0 ? b_n - 1 : 0;
                arm.armed = false;
                continue;
            }
            if (BTN_PRESSED(cross) && b_n > 0 && sel < b_n) {
                ui_sfx_play(SFX_DECIDE);
                const lfs_entry ent = b_all[sel];
                char next[256];
                if (!lfs_path_join(cur, ent.name, next, sizeof next)) {
                    xmb_dl_notice(TR("Can't open this"), TR("The path is too long."));
                } else if (ent.is_dir) {
                    if (depth < MAX_DEPTH) {
                        snprintf(stack[depth].path, sizeof stack[depth].path, "%s", cur);
                        stack[depth].sel = sel; stack[depth].top = top;
                        depth++;
                        snprintf(cur, sizeof cur, "%s", next);
                        status = load_dir(cur);
                        sel = 0; top = 0;
                        pc_clear();
                    }
                } else if (lfs_media_kind_of(ent.name) == LFS_KIND_MUSIC) {
                    play_music(cur, ent);
                    arm.armed = false;
                    pc_clear();
                } else {
                    show_details(next, ent);
                    arm.armed = false;
                }
                continue;
            }
        }

        // ---- draw ----
        frame_begin();
        char where[200];
        pretty_path(cur, drive_label, where, sizeof where);
        draw_screen_title(TR("Media"), where);
        const int x = list_x(), w = list_w();
        if (status < 0) {
            centre_message(status == LFS_E_REMOVED ? TR("The drive was removed") : TR("This folder could not be read"),
                           screen_top() + UIS_H(60));
        } else if (b_n == 0) {
            centre_message(TR("No videos, music or folders here"), screen_top() + UIS_H(60));
        }
        for (int r = 0; r < vis && top + r < b_n; r++) {
            const int i = top + r;
            const lfs_entry &ent = b_all[i];
            const int y = screen_top() + r * row_pitch();
            const bool s = (i == sel);
            draw_row_frame(x, y, w, row_h(), s);
            const bool is_music = !ent.is_dir && lfs_media_kind_of(ent.name) == LFS_KIND_MUSIC;
            drawIcon((u32)(x + UIS_W(16)), (u32)(y + (row_h() - UIS_H(22)) / 2),
                     ent.is_dir ? ICON_COLLECTIONS : is_music ? ICON_MUSIC : ICON_MOVIE, UIS_TF(22.0f), s ? XMB_ACCENT_ALT : XMB_TEXT_DIM);
            const int tx = x + UIS_W(64);
            const int right = x + w - UIS_W(20);
            if (ent.is_dir) {
                clip_text(tx, y + (row_h() - UIS_H(19)) / 2 - UIS_H(1), ent.name, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, right - tx, s);
                continue;
            }
            char path[256] = "";
            lfs_path_join(cur, ent.name, path, sizeof path);
            char name[160];
            const Probed *pr = path[0] ? pc_get(path) : NULL;
            if (is_music) {
                if (pr && pr->label[0]) snprintf(name, sizeof name, "%s", pr->label);
                else lm_stem_title(ent.name, name, sizeof name);
            } else {
                LocalTitle lt;
                local_clean_name(ent.name, &lt);
                local_title_line(&lt, name, sizeof name);
            }
            // size, then what the worker found out
            char sz[24], line[160];
            local_format_size(ent.size, sz, sizeof sz);
            int n = snprintf(line, sizeof line, "%s", sz);
            u32 sub_clr = XMB_TEXT_DIM;
            if (pr && pr->readable && pr->secs) {
                char du[32];
                local_format_duration(pr->secs, du, sizeof du);
                n += snprintf(line + n, sizeof line - (size_t)n, "  %s  %s", MIDDOT, du);
            }
            if (pr && pr->note[0]) {
                n += snprintf(line + n, sizeof line - (size_t)n, "  %s  %s", MIDDOT, pr->note);
                sub_clr = XMB_TEXT_FAINT;
            }
            uint32_t rs = 0;
            if (!is_music && path[0] && lres_find(&s_res, res_key_of(path, ent), &rs, NULL) && (!pr || pr->playable || !pr->readable)) {
                char rt[40];
                local_format_resume(rs, rt, sizeof rt);
                snprintf(line + n, sizeof line - (size_t)n, "  %s  %s", MIDDOT, rt);
            }
            clip_text(tx, y + UIS_H(9), name, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, right - tx, s);
            clip_text(tx, y + UIS_H(37), line, UIS_TF(13), sub_clr, right - tx, false);
        }
        if (b_n > vis) {
            char pos[32];
            snprintf(pos, sizeof pos, "%d / %d", sel + 1, b_n);
            const int pw = ttf_text_width(pos, UIS_TF(14));
            drawTTF((u32)(x + w - pw), (u32)(XMB_OY + UIS_H(70)), pos, UIS_TF(14), XMB_TEXT_FAINT);
        }
        if (b_total > MAX_ENTRIES) {
            char m[64];
            snprintf(m, sizeof m, TR("The first %d of %d entries"), MAX_ENTRIES, b_total);
            const int mw = ttf_text_width(m, UIS_TF(13));
            drawTTF((u32)(x + w - mw), (u32)(XMB_OY + UIS_H(90)), m, UIS_TF(13), XMB_TEXT_FAINT);
        }
        { Hint h[5]; int nh = 0;
          if (b_n > 0) { h[nh].glyph = 'X'; h[nh].label = TRN("Open"); nh++; }
          if (b_n > vis) { h[nh].glyph = 'L'; h[nh].label = ""; nh++; h[nh].glyph = 'R'; h[nh].label = TRN("Page"); nh++; }
          h[nh].glyph = 'C'; h[nh].label = TRN("Back"); nh++;
          draw_hints_bar(h, nh); }
        flip();
    }
    pw_stop();
    free(b_all);
    b_all = NULL;
    init_btns();
}

// ---------------------------------------------------------------------------
//  The list of places
// ---------------------------------------------------------------------------

void open_root_row(int row) {
    if (row == 0) { xmb_show_offline(); init_btns(); return; }
    const int di = row - 1;
    if (di < 0 || di >= s_nd) return;
    const lfs_drive d = s_dr[di];
    if (d.kind == LFS_USB_UNSUPPORTED) {
        xmb_dl_notice(TR("Can't read this drive"),
                      TR("It uses a file system this app cannot read. NTFS, exFAT and FAT32 work."));
        return;
    }
    browse(d);
}

// One place's row: panel in the CPU phase, text in the text phase.
void draw_root_panels(int top, int x, int w) {
    const int vis = rows_in(top);
    for (int r = 0; r < vis && r < root_rows(); r++)
        draw_row_frame(x, top + r * row_pitch(), w, row_h(), r == s_sel);
}

void draw_root_text(int top, int x, int w) {
    const int vis = rows_in(top);
    for (int r = 0; r < vis && r < root_rows(); r++) {
        const int y = top + r * row_pitch();
        const bool s = (r == s_sel);
        char title[64], sub[120];
        int icon = ICON_COLLECTIONS;
        if (r == 0) {
            snprintf(title, sizeof title, TR("Downloads"));
            if (s_dl_count > 0) snprintf(sub, sizeof sub, TR("%d on this console, ready to play offline"), s_dl_count);
            else snprintf(sub, sizeof sub, TR("Nothing downloaded yet"));
            icon = ICON_PLAY;
        } else {
            const lfs_drive &d = s_dr[r - 1];
            snprintf(title, sizeof title, "%s", d.label);
            drive_sub(d, sub, sizeof sub);
        }
        drawIcon((u32)(x + UIS_W(18)), (u32)(y + (row_h() - UIS_H(24)) / 2), icon, UIS_TF(24.0f),
                 s ? XMB_ACCENT_ALT : XMB_TEXT_DIM);
        const int tx = x + UIS_W(68);
        clip_text(tx, y + UIS_H(9), title, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, w - UIS_W(90), s);
        clip_text(tx, y + UIS_H(37), sub, UIS_TF(13), XMB_TEXT_DIM, w - UIS_W(90), false);
    }
}

// Input for the list, shared by the tab and the standalone screen.  True when a place was opened
// (the caller then has to treat its frame state as stale).
void root_move(void) {
    const int was = s_sel;
    if (BTN_REPEAT(up)   && s_sel > 0)               s_sel--;
    if (BTN_REPEAT(down) && s_sel < root_rows() - 1) s_sel++;
    if (s_sel != was) ui_sfx_play(SFX_CURSOR);
}

}   // namespace

// ---------------------------------------------------------------------------
//  The tab, as the XMB drives it
// ---------------------------------------------------------------------------

void xmb_media_on_enter(void) {
    s_gen = ~0u;
    refresh_drives(false);
    if (s_sel >= root_rows()) s_sel = 0;
}

bool xmb_media_at_top(void) { return s_sel <= 0; }

bool xmb_input_media(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    refresh_drives(true);
    root_move();
    if (BTN_PRESSED(cross)) { ui_sfx_play(SFX_DECIDE); open_root_row(s_sel); }
    return false;
}

void xmb_media_hints(void) {
    Hint h[2]; int n = 0;
    h[n].glyph = 'X'; h[n].label = TRN("Open"); n++;
    // Every bar but Search's ends with Square and the visualiser it switches to.
    h[n].glyph = 'S'; h[n].label = wave_vis_next_label(); n++;
    draw_hints_bar(h, n);
}

void xmb_cpu_draw_media(void) {
    static unsigned frame = ~0u;
    if (spine_frame_id() != frame) { frame = spine_frame_id(); refresh_drives(true); }
    draw_root_panels(tab_top(), (int)(((int)display_width - XMB_LIST_W) / 2), XMB_LIST_W);
}

void xmb_draw_media(void) {
    draw_root_text(tab_top(), (int)(((int)display_width - XMB_LIST_W) / 2), XMB_LIST_W);
}

// The same list as a screen of its own.
void xmb_show_media(void) {
    screen_begin();
    s_gen = ~0u;
    refresh_drives(false);
    s_sel = 0;
    Arm arm;
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        refresh_drives(true);
        if (arm.ready()) {
            if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); break; }
            root_move();
            if (BTN_PRESSED(cross)) {
                ui_sfx_play(SFX_DECIDE);
                open_root_row(s_sel);
                arm.armed = false;
            }
        }
        frame_begin();
        draw_screen_title(TR("Media"), TR("Plays from this console or a USB drive -- no server needed"));
        draw_root_panels(screen_top(), list_x(), list_w());
        draw_root_text(screen_top(), list_x(), list_w());
        { static const Hint h[] = {{'X', TRN("Open")}, {'C', TRN("Back")}};
          draw_hints_bar(h, 2); }
        flip();
    }
    init_btns();
}
