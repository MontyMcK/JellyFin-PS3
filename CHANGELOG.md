# Changelog

## 3.2

### New
- **Live TV.** If your Jellyfin server has Live TV set up (a tuner, or an IPTV
  M3U playlist), a Live TV tab lists your channels with what's on now. Press
  `→` for the TV guide. While watching, `↑`/`↓` change channel. Favourite
  channels (`△`) come first.
- **Downloads.** *Download* on a film or episode's page saves a copy to the
  PS3's hard drive. It pauses while you stream and picks up where it left off,
  even after a restart. Watch downloads from Settings › Downloads › Offline
  Library, or from the Downloaded row on Home. When the server can't be
  reached at startup, the app offers your downloads instead.
- **Download a whole season**: `△` on a season.
- Downloads always leave at least **10 GB free** on the PS3's hard drive, and
  pause rather than fill it. If the app can't tell how much room there is, it
  doesn't download.
- **Play from USB drives, no server needed.** The new Media tab plays your own
  MKV, .m2ts and .ts films from USB drives formatted **NTFS, exFAT or FAT32**,
  or from the PS3's hard drive. Blu-ray remuxes play at full bitrate with
  their lossless TrueHD / DTS-HD MA audio (Dolby Digital, DTS, MP3, AAC,
  FLAC and PCM work too), you can switch audio tracks and subtitles while
  watching, and the drive's files show their length and where you stopped.
  Drives are only ever read, never written to. Limits, which the file's page
  tells you about: H.264 video only (HEVC, 4K, 10-bit and VC-1 files can't
  play on a PS3), files whose only audio is Dolby Digital Plus or Vorbis can't
  play yet, and subtitles come from MKV files only (text and PGS).
- **Music from your drives.** FLAC, MP3 and WAV files in the Media tab play in
  the music player: the folder is the album, in track order (from the files'
  tags), with the cover art (inside the files, or a cover.jpg / folder.jpg
  beside them), and the tracks run into each other without a gap. FLAC up to
  24-bit and 192 kHz is converted to the console's 48 kHz with a proper filter,
  not squeezed; MP3s with a LAME header are cut to their exact length.
- **Languages.** The menus, messages and button hints are now available in
  **English, 日本語, Português (Brasil), Deutsch, Français and Español.** The
  app follows your PS3's own language setting (Japanese, French, Spanish,
  German and both Portuguese variants map to their language; anything else
  stays English). To choose one yourself: Settings → Display → Language
  (Auto, or one of the six, each named in its own language so you can always
  find your way back). Japanese titles and subtitles from your server now
  draw too: the kana and the common kanji are built in. Track names (audio
  and subtitle labels), codec names and anything your server supplies, such
  as titles and descriptions, are shown as the server sends them.
- **Favourites.** Star films and shows on their page; they get their own row
  on Home.
- **Dolby Digital output.** Settings → Audio → Audio Output has a new
  *Dolby Digital* choice. It sends the film's Dolby Digital track to your
  soundbar or receiver untouched (other tracks are converted to Dolby Digital
  by your server). Use it if your soundbar is connected through the TV's
  ARC port and the centre channel — where the dialogue is — goes missing in 5.1.
  Volume is then set on the soundbar.
- **Send Log to Server.** Settings → System → Send Log to Server uploads the
  diagnostic log to your Jellyfin server (Dashboard → Logs), so you can attach
  it to a bug report without FTP. Diagnostic logging is now on by default.
- **Picture size.** `L1` in the player resizes the picture, width with `←`/`→`
  and height with `↑`/`↓`, each on its own (`△` resets, `O` or `L1` done). For
  a CRT that overscans: shrink the picture until the whole frame is on screen.
  A flat-panel TV can leave it alone. The setting is kept for every video.
- **Skip with the D-pad.** With the player bar hidden, `←`/`→` jump 10 s back
  or ahead.
- **Player stats from the pad.** Click either stick in the player to show or
  hide the stats overlay.
- **Settings has sections** — Playback, Audio, Subtitles, Downloads, Display and
  System. `L2`/`R2` jump between sections, and `←`/`→` change a value in either
  direction (`X` still steps forward).

### Fixes
- Leaving a film part way and opening it again from its page started it over
  instead of where you stopped; it now resumes.
- Square put the player bar away only if you pressed it very quickly: held for
  a moment, the bar came straight back. A press now hides it for good.
- Pressing the PS button while a video was paused could leave the player
  looking frozen; the system menu now appears.
- Some items opened to an empty details page and then hung on retry when the
  server's reply was framed unusually.
- A downloaded film entered part way through (after a seek) showed a time that
  counted the position twice.

## 3.1

**The new interface is now what everyone gets.** In 3.0 (and the beta before it),
the layered interface, JellyWave, and the GPU-drawn posters and text were all
switched off unless a setting file existed on the console, and nothing created
those files. A fresh install showed the old tab strip, a flat three-band wave,
and slower CPU-drawn menus. They are all on by default now. If you installed 3.0,
installing 3.1 over it is all you need to do.

### New
- **Settings → Software Update.** Shows the result of the launch check (up to
  date, a newer version available, or couldn't check). `X` checks again, or
  reopens the update popup when there is a new version.
- The update popup now says where to get the new `.pkg`.

### Controls
- **`O` stops playback**, the same as `Start`, whether or not the player bar is
  showing. If a track menu or the volume slider is open, `O` closes that first.
- **`□` hides the player bar** without waiting for it to time out. The player
  stats overlay is still on the remote's `DISPLAY` key and in Settings.
- **Remotes without `L1`/`R1`:** the Blu-ray remote's `|<<` `>>|` and `<<` `>>`
  keys now switch tabs in the menus, so a Bluetooth remote can reach every tab.
  Unrecognised remote keys are written to the debug log so they can be mapped.

### Fixes
- **Home posters on the selected row.** The poster cache has a fixed number of
  slots, and when Home wanted more than fit, the row you were looking at could
  lose out: some of its posters stayed blank until you moved off it. The
  selected row now loads first.
- JellyWave moves at the speed it was tuned to on a TV (a little calmer than 3.0's
  default).

### Turning things back off
Each of these still has a file in `/dev_hdd0/tmp/`; put `0` in it to switch that
feature off: `jellyfin_spine.txt` (layered interface), `jellyfin_gpuwave.txt`
(JellyWave), `jellyfin_gpucards.txt` and `jellyfin_gputext.txt` (GPU posters and
text). `jellyfin_jwspeed.txt` sets the JellyWave speed (default 14).
