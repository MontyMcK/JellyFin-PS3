# Changelog

## 3.2

The biggest update yet: **Live TV, downloads, playing straight from USB drives,
six languages, Quick Connect sign-in** and a new Settings screen, plus a long
list of fixes.

### New
- **Quick Connect sign-in.** No more typing passwords with a controller: the
  console shows a code, you approve it in the Jellyfin app on your phone
  (Settings → Quick Connect), and you're in. Username and password still work.
- **Live TV.** If your server has Live TV (a tuner or an IPTV playlist), a Live
  TV tab lists your channels with what's on now and a TV guide. `↑`/`↓` change
  channel while watching. *My Channels* and *Recently watched* pages come
  first, and `Start` lets you hide the categories you don't use.
- **Downloads.** Save a film, an episode or a whole season (`△` on a season) to
  the PS3's hard drive or an **NTFS USB drive**, and watch it with no server.
  Downloads pause while you stream, resume after a restart, and always leave at
  least 10 GB free on the hard drive.
- **Play from USB drives, no server needed.** The Media tab plays MKV, .m2ts
  and .ts films from NTFS, exFAT or FAT32 drives, with lossless TrueHD /
  DTS-HD MA audio and subtitles, plus **FLAC, MP3 and WAV music** as albums.
- **Six languages:** English, 日本語, Português (Brasil), Deutsch, Français and
  Español. It follows your PS3's language, or pick one in Settings → Display.
- **A new Settings screen** laid out like the PS3's own: six categories
  (Playback, Audio, Subtitles, Downloads, Display, System), each with a short
  description of every setting.
- **Fit, Fill or Stretch** (`Select` in the player): Fill zooms a 16:9 film to
  fill a 4:3 TV.
- **Chapters** (`R1` in the player), for films whose server has chapter data.
- **Picture size** (`L1` in the player), for CRTs that cut off the edges.
- **Favourites**, with their own row on Home.
- **Dolby Digital output** (Settings → Audio → Audio Output), for soundbars on
  a TV's ARC port that lose the centre channel in 5.1.
- **Wake the server** with Wake-on-LAN (Settings → System). The console also
  wakes it by itself when it can't reach it.
- **The app finds your server again** if its address changes (for example the
  PC moving from Wi-Fi to Ethernet).
- **Send Log to Server** (Settings → System) for bug reports, no FTP needed.
- **HTTPS server addresses** for sign-in and browsing (experimental).
- **Pinned to the top of the Game column** in the XMB, above your games, and
  the XMB now shows the real version (03.20).
- Smaller touches: `←`/`→` skip 10 s with the player bar hidden, and clicking
  a stick shows the player stats.

### Fixes
- **Films start straight after the 24 Hz switch** again, with no second
  buffering screen in between.
- **The version list showed the same version over and over** on episodes a
  debrid plugin had listed many times. Repeats are now shown once.
- **Random freezes and missing artwork** when opening pages quickly: two
  internal locks were never actually created, so parts of the app could read
  each other's network replies. Fixed, along with **Software Update**, which
  could not run in the previous build.
- Resuming a film from its page started it over instead of where you stopped.
- Pressing the PS button while paused could make the player look frozen.
- Some pages opened empty and hung on retry when the server's reply was framed
  unusually.
- The menu froze for about five seconds at a time while the server was
  unreachable, and startup waited on a server that wasn't there.
- Small text on standard-definition (480/576) TVs is now readable.
- Oversized cover images and failed video-memory allocations no longer crash
  the app; a failed seek could close the stream twice; JellyDrop jumped once an
  hour.

### Buffering on a wired server PC? Check your router
If your Jellyfin PC is plugged into the router with a cable and you stream
through a **debrid / Stremio-style plugin** (Gelato and similar), the server can
download a film from the internet as fast as your connection allows when it
starts. Some routers can't keep up with that and stop passing traffic between
your PC and the PS3 for a few seconds at a time, so the film buffers and stalls
over and over even though everything is on a fast network. Two ways out:

- **A cheap gigabit switch** between the router, the PC and the PS3, so their
  traffic never goes through the router. This also protects other streaming,
  such as Moonlight.
- **Make the server download at film speed.** This release includes
  `ffwrap`, a small stand-in for Jellyfin's ffmpeg that limits how fast it
  reads from the internet (to 1.25× the film's own bitrate). See
  [`tools/ffwrap/README.md`](tools/ffwrap/README.md).

The app also notices when a stream arrives in bursts with long gaps and
reconnects by itself.

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
