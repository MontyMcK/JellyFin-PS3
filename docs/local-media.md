# Local Media

Status: **built for 3.2.**  This started as the scoping document for "can the app play my own BD remuxes and
24-bit FLACs straight off a drive, with no Jellyfin server?"  The answer became yes: the Media tab, the file layer
(`source/local/lfs`, read-only FAT32 / NTFS / exFAT), MKV / .m2ts / .ts playback through the existing player, local
subtitles, and music files in the music screen.  What was built and in which order is in
[release-3.2-plan.md](release-3.2-plan.md) (U1-U5); the sections below are the analysis the work started from, kept
as it was, so "Today" and "not read" in them describe the app before 3.2.

How the pieces fit, now:

| | Where |
|---|---|
| Drives and files, read-only | `source/local/lfs*`, `third_party/mohasi_fs` (NTFS, exFAT) |
| What a video file holds, and whether the PS3 can play it | `source/local/local_probe`, `source/video/mkv_demux`, `h264_sps` |
| Matroska and m2ts played as MPEG-TS | `source/video/mkv_ts`, `source/player/stream/stream_local`, `player/core/player_local` |
| Audio of files: AAC, FLAC, PCM (video) | `source/audio/adec_aac`, `flac_dec`, `adec_flac`, `adec_pcm`, `chan_map`, `resample` |
| Subtitles of Matroska files | `source/local/sub_conv`, `local_subs`, `player/subtitles` |
| Music: tags, decoding, albums | `source/local/local_tags`, `local_audio`, `local_music`, `local_music_fs`; engine in `source/music/music_player` |
| The browser | `source/ui/xmb/ui_media` |

## 1. Why it is worth doing

The server path has a hard ceiling that local files do not. The PS3 receives
about 20–25 Mbps over HTTP (`vquality.h`, measured), which is why the highest
quality setting is a 25 Mbps *copy ceiling*: anything above it is transcoded
down. A Blu-ray remux peaks at 40–48 Mbps. The internal HDD reads several
times faster than that, and the player has already been measured demuxing
30–53 Mbps remuxes (`ts_demux.h`, `adec.h`). From a local drive, a full-bitrate
remux plays **untouched**, with its lossless audio track as-is.

## 2. What already exists

| Piece | Where | Reuse |
|---|---|---|
| Playing a file from the HDD instead of a socket | `stream_open_file()` (Stage 4) | as-is |
| Seek inside a local MPEG-TS (PAT-before-keyframe index) | `stream_local.{h,cpp}` | as-is for .ts/.m2ts |
| MPEG-TS demux, BD stream types (0x81–0x86, 0x8A) | `ts_demux.cpp` | extend |
| H.264 hardware decode, 1080p, Level 4.1 AU size | `vdec.cpp` | as-is |
| Dolby TrueHD, lossless, up to 7.1 (incl. Atmos bed) | `adec_truehd.cpp` | as-is |
| DTS-HD MA lossless (XLL), with a CPU budget that falls back to the core | `adec_dts.cpp` | as-is |
| AC-3, DTS core, MP3 | `adec_ac3.cpp`, `adec_dts.cpp`, minimp3 | as-is |
| 8-channel float output port, bit-exact at 100 % volume | `audio.cpp` | as-is |
| Swappable PCM source for the output port | `audio_set_source()` | music path |
| Offline startup with no server or login | Stage 4/5 | entry point |
| Pure, host-tested screen model + list screens | `dl_ui`, `ui_downloads.cpp` | pattern |
| Atomic key=value records on the HDD | `dl_store` | resume positions |
| libFLAC (BSD) in the toolchain's portlibs | `portlibs/ppu/lib/libFLAC.a` | FLAC decode |

## 3. Formats: what plays, what it costs

### 3a. Blu-ray remuxes

| | Status | Work |
|---|---|---|
| **Containers** | | |
| `.ts` | plays through the Stage 4 path once it can be opened | browser only |
| `.m2ts` (BD 192-byte packets) | not read | small: strip the 4-byte header per packet |
| `.mkv` (what MakeMKV writes; most remuxes) | not read | **large**: new Matroska demuxer (§4b) |
| BDMV folder (playlist of several .m2ts) | not read | medium: `.mpls` playlist, seamless clip joins |
| **Video** | | |
| H.264 up to Level 4.1 (nearly all BDs) | decodes | none |
| MPEG-2 (early BDs, DVD remuxes) | VDEC supports it; player is H.264-only | medium: second VDEC codec path |
| VC-1 (some older BDs) | **not possible**: PSL1GHT's VDEC exposes MPEG-2 and H.264 only, and a 1080p software decode is beyond the PPU | — |
| HEVC / UHD / 10-bit | **not possible**: no decoder on the console | — |
| **Audio** | | |
| TrueHD / Atmos bed | lossless | none |
| DTS-HD MA | lossless while the CPU budget allows, else DTS core | none |
| AC-3, DTS | decodes | none |
| BD LPCM (stream type 0x80) | not decoded | small: it is raw PCM behind a 4-byte header |
| E-AC-3 | not decoded (liba52 is AC-3 only) | medium: new decoder |
| FLAC inside MKV | not decoded | small once §3b's decoder exists |
| **Subtitles** | server burns them in today (`SubtitleMethod=Encode`) | locally there is no renderer: SRT is medium, PGS (BD bitmap subs) is large |

### 3b. Music

| | Status | Work |
|---|---|---|
| FLAC 16/24-bit, up to 8 channels | not decoded locally | medium: libFLAC into the music PCM ring |
| WAV | — | small |
| MP3 | minimp3 already linked | small |
| ALAC / AAC (.m4a) | — | later; faad2 is in portlibs for AAC |
| Album art | — | embedded FLAC `PICTURE`, else `folder.jpg`/`cover.jpg` |

**Sample rates — the honest limit.** The PS3's audio port runs at a fixed
48 kHz. A 24-bit/48 kHz file plays bit-exact: float32 holds a 24-bit sample
exactly, and the output stage is bit-exact at 100 % volume. A 44.1, 88.2, 96
or 192 kHz file must be resampled to 48 kHz. The music player's current
resampler is linear interpolation (`frame_to_48k`), which is fine for MP3 but
not for a lossless library, so this needs a proper windowed-sinc/polyphase
resampler. 96→48 stereo is cheap on the PPU; 192 kHz multichannel needs
measuring. Whether HDMI then carries 24 bits depends on the console's system
audio output settings, to be verified on hardware.

## 4. The hard problems

### 4a. Drives and filesystems

| Drive | Natively readable | File size limit | Notes |
|---|---|---|---|
| Internal HDD (`/dev_hdd0`) | yes | none | files arrive over FTP (webMAN MOD, multiMAN) |
| USB, FAT32 (`/dev_usb000`–`007`) | yes | **4 GB** | a remux will not fit; FLAC libraries will |
| USB, NTFS / ext | **no** | none | needs a vendored filesystem library (below) |
| USB, exFAT | **no** | none | no known PS3 library; needs research |

NTFS (and ext2/3/4) is what homebrew such as IRISMAN and webMAN use through
Estwald's `libntfs_ext`: ntfs-3g plus an ext reader over raw USB sector access
(lv2 storage syscalls, which PSL1GHT has no headers for). It is GPLv2-or-later,
compatible with this project's GPL-3. Vendoring it is mostly integration, but
it is the riskiest piece, because a filesystem bug can only corrupt a drive if
it writes. **The app would mount these drives read-only.**

Until this lands, remuxes have to live on the internal HDD.

### 4b. Matroska

MKV is the one large new component. It is a demuxer that hands the existing
decoders the same access units the TS demux does, so neither decoder changes:

* EBML parse: Segment, Info (timestamp scale), Tracks, Cues, Clusters,
  SimpleBlock/BlockGroup, and all three lacing modes (audio uses them);
* H.264 from `V_MPEG4/ISO/AVC`: length-prefixed NAL units rewritten to Annex B,
  with SPS/PPS from `CodecPrivate` (avcC) re-sent before each keyframe;
* audio IDs `A_TRUEHD`, `A_DTS`, `A_AC3`, `A_PCM/INT/LIT`, `A_FLAC`;
* timestamps to the 90 kHz clock the player's A/V sync already uses;
* seek through Cues, or a bounded cluster scan when a file has none;
* no per-frame allocation, and the 1.5 MB AU ceiling `ts_demux.h` derives from
  Level 4.1.

It is host-testable the way `dl_ts` and `stream_local` are: a fixed set of
small MKV fixtures, with goldens of the access units it emits.

### 4c. Track choice

On the server path, Jellyfin picks the audio track and burns in subtitles.
Locally the app must do it: list the audio tracks with language and codec
(TS PMT ISO-639 descriptors, MKV `Language`), default to the best decodable
one (lossless first), and offer a picker in the player menu. Today the TS
demux takes the first decodable audio PID.

### 4d. Library and state

* **Browsing:** a folder browser over the known drives, which is cheap, exact,
  and needs no index. Titles come from file names. Music folders read as
  albums, with tags and art read only for the rows on screen (same rule as the
  Downloads list).
* **Resume:** a small local store keyed by path + size + mtime, written with
  the `dl_store` atomic writer.
* **Later:** a scanned index (search, artists, "recently added") is a separate
  step and is not needed for the first stages.

## 5. Plan

Each stage is shippable, host-tested where pure, and leaves JellyWave, the
RSX/strobe code, the XMB spine and the server playback path untouched.

| Stage | Scope | Size | What you can do after it |
|---|---|---|---|
| **L1** | Drive list (internal HDD, USB FAT32), folder browser screen, `.ts`/`.m2ts` playback through the Stage 4 player, BD LPCM, audio-track picker, local resume. Entry: a "This PS3" row in Settings and the sign-in-failure offer | medium | play `.m2ts` remuxes from the internal HDD at full bitrate, with TrueHD / DTS-HD MA / LPCM |
| **L2** | Matroska demuxer (§4b), Cues seek, MKV track picker | large | play MakeMKV `.mkv` remuxes |
| **L3** | Local music: libFLAC + WAV + MP3 behind a source switch in the music player, a quality resampler, folder-as-album with tags and art, gapless | medium | play a FLAC library (24/48 bit-exact; other rates resampled) |
| **L4** | NTFS/ext USB drives, read-only (§4a); exFAT investigated | medium, highest risk | play from an external NTFS drive without copying to the PS3 |
| **L5** | SRT subtitles, then PGS; BDMV playlists; MPEG-2 video; E-AC-3; scanned index | each medium–large | — |

Ordering: L1 first, because it is mostly reuse and proves the local path end
to end on hardware. L2 is the biggest piece but what most remux owners need.
If your files are on NTFS drives you would rather not copy, L4 can come before
L2/L3, since nothing else helps until the drive can be read.

## 6. Decisions needed before L1

1. **Where are the files?** Internal HDD, FAT32 USB, or NTFS/exFAT external
   drives. This decides whether L4 moves up.
2. **Containers:** mostly `.mkv`, or `.m2ts`/BDMV folders? This decides
   whether L2 moves ahead of L1's m2ts work.
3. **Music sample rates:** mostly 44.1/48 kHz, or 96/192 kHz hi-res? This sets
   how much the resampler's quality and cost matter.
4. **Subtitles:** needed for the first usable version, or later?
