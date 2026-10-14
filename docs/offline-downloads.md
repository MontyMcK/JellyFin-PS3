# Offline Downloads — Design & Implementation

Status: **Stages 1–5 implemented**: storage model, queue, state machine,
persistence, resumable transfer, Jellyfin integration, service lifecycle, the
offline library, the offline startup path, local playback through the existing
player, and (Stage 5) the UI: a DOWNLOAD button on the item page, a Downloads
list with progress, and an Offline library (§12). Host-tested and compiled into
the PS3 build; not yet run on a console. What the build does at runtime:

* it starts the download service after `load_config()`. The worker creates
  the store root on the HDD (§4), restores it, and then idles until something
  is queued;
* the player reports its streams to the download manager (§5a);
* the info screen's existing item fetch also parses series/season/episode/
  year/runtime (§7b). No extra request is made;
* Settings gains two rows, **Downloads** and **Offline Library** (§12b);
* a failed sign-in offers the Offline library when it has anything (§12b).

## 1. Goal

From an item's page, **DOWNLOAD FOR OFFLINE** fetches a PS3-playable copy to
the HDD, survives interruptions and reboots, and plays through the
**existing** player from an Offline section that works with the server gone.

## 2. Architecture

```
 item page / Downloads / Offline        (Stage 5 UI -- screens only)
                 |
                 v
       dl_manager.h  -- the one service: queue, control, queries
        |       |        (enqueue/pause/resume/cancel/retry/remove,
        |       |         dl_list, dl_find, dl_media_path)
        |       +--> dl_model.h   state machine, records, error policy (pure)
        |       +--> dl_http.h    URL, Range request, head parser,
        |       |                 streaming chunked decoder (pure)
        |       +--> dl_store.h   on-disk layout, atomic record writes
        v
   dl_platform.h  -- the seam: ~20 calls that differ between PS3 and host
     |                         |
 dl_ps3.cpp (console)      tests/dl_fake.cpp (host tests)
 libnet via http.cpp,      temp dir + scripted fake server
 lv2 fs, lv2 mutex,
 worker thread (dl_service.h)
```

* **One service, not a screen.** The item page, the Downloads list and the
  Offline library all call `dl_manager.h`; none of them owns a transfer.
* **The same code runs on the host.** Everything above the seam is plain C++
  with no PSL1GHT headers. `tests/test_offline.cpp` compiles the *same files*
  the PS3 build does and drives them against a real temporary directory and a
  scripted HTTP server. The console-only file (`dl_ps3.cpp`) only makes direct
  system calls; it contains no logic of its own.
* **No new networking stack.** Resolution and connect come from `http.cpp`
  (the new `http_open_socket()`, under the lock `http_request()` already uses,
  because the PS3 resolver is not reentrant). The download reads the socket
  itself because neither existing reader fits:
  `http_request()` buffers the whole body (384 KB cap), and `stream.cpp`'s
  reader keeps its state in statics the player owns.

## 3. States

```
            START              COMPLETE
  QUEUED ----------> DOWNLOADING ------> COMPLETED   (terminal; only removed)
    ^  ^               |  |  |
    |  +- FAIL_RETRY --+  |  +-- FAIL --> FAILED --RETRY--> QUEUED
    |  +- INTERRUPTED -+  |
    |                     +-- PAUSE ---> PAUSED --RESUME--> QUEUED
  CANCEL (from QUEUED/DOWNLOADING/PAUSED/FAILED) --> CANCELLED --RETRY--> QUEUED
```

| State | Meaning | Partial data |
|---|---|---|
| queued | waiting its turn, or waiting out a retry backoff | kept |
| downloading | the worker is transferring it | growing |
| paused | the user paused it | kept |
| completed | `media.ts` complete and verified | — |
| failed | gave up; the user can retry | kept |
| cancelled | the user cancelled; entry kept so it can be retried | **deleted** |

`dl_next_state()` is the only thing that moves an item between states, and
any transition not in the table is refused as a no-op. A stale button press
therefore cannot corrupt a record. **Remove** is not a transition: the item
and all its files cease to exist.

Control on the **active** item is posted to the worker and takes effect
within about a second (the socket's receive timeout). Requests only escalate
(pause < cancel < remove), so a pause pressed after a cancel cannot rescue the
download. A resume pressed before a pending pause lands withdraws the pause.

**Suspension** (`dl_set_suspended`) is for stopping the service. It sends the
active item back to QUEUED with its data, recorded as an interruption rather
than an error or a pause, and nothing starts until suspension lifts. Playback
has its own gate (§5a), and ending playback never lifts a suspension.

## 4. Storage

```
<root>/
  layout.txt              "jfdl-layout 1"
  items/<item id>/
    state.txt             DlRecord  -- queue + transfer bookkeeping
    meta.txt              DlMeta    -- identify, show and play it offline
    media.ts.part         while downloading
    media.ts              once complete and length-verified
    poster.jpg            artwork (optional)
    backdrop.jpg
```

**Root.** The first root that can be created *and written* wins. The write
probe is the layout marker; mkdir alone can succeed on a read-only mount.

| Target | Tried first | Fallback |
|---|---|---|
| Hardware | `/dev_hdd0/jellyfin_offline` | `/dev_hdd0/tmp/jellyfin_offline` |
| RPCS3 | `/dev_hdd0/game/JFPS30000/USRDIR/jellyfin_offline` | same `tmp` path |

Not `/dev_hdd0/tmp` by choice, since that is scratch space. Not the game
USRDIR on hardware, because `jf_paths.cpp` records that it is not reliably
writable while the title runs. The `tmp` fallback exists only because it is
the one place this app has proven writable on hardware. **To verify on
hardware:** the root in use is logged as `dl: root ... free=...MB`.

**Why per item.** An item is the unit everything happens to (enqueue, cancel,
delete, restore). With its files together, deleting it means removing a fixed
list of known names and then the directory. The store never recursively
deletes whatever it finds, so a wrong root cannot eat the HDD. Unknown files a
user drops into an item directory are left alone.

**Records** are `key=value` text, with enums written as *names*. The settings
files in this app learned that a persisted digit pins an enum's numbering
forever. Values escape `\`, newline and CR, so a title cannot forge a key.
Each file has a magic/version first line and an `end` last line, so a torn
write is detected rather than half-loaded. Unknown keys are ignored, so a newer
build's files still load in an older one.

Writes are **atomic**: they go to `<file>.tmp`, remove the old file, then
rename. lv2's rename does not promise to replace an existing file. A crash at
any point leaves either the old or the new version readable, and load falls
back to `.tmp`.

**No token is ever written to disk.** The URL is stored without credentials.
The auth header lives only in memory and is rebuilt from the session
(`dl_service_refresh_auth`).

## 5. Transfer

A plain HTTP/1.1 GET, streamed to `media.ts.part` through **one fixed 512 KB
buffer**, so the file never passes through RAM. When bytes are already on disk
the request carries `Range: bytes=N-`, where N is the partial's actual size on
disk rather than the record's, because the record can lag by one checkpoint.

| Server answer | Action |
|---|---|
| `206` from N, same total | append; a real resume |
| `200` | the Range was ignored (a live transcode does this): truncate, write from 0 |
| `206` from another offset, or a different total | discard and retry; appending would splice two files |
| `416`, `*/N` with N bytes already here | already complete, so verify and finish |
| `416` otherwise | discard, retry from 0 |
| `text/*` / JSON with 200 | refused: an error page must not be saved as a film |
| `401` | `auth`: the item goes back to **queued** (not failed) and the whole queue **holds** until the next login (§7d) |
| `403` | failed: `http` (this account may not have the item; no retry or re-login changes that) |
| `404`/`410` | failed: `not_found` |
| other `4xx` | failed: `http` |
| `5xx`, `408`, `429` | retried: `server` |

Other failures:

| Condition | Error | Retried? |
|---|---|---|
| resolve/connect fails (server down, no network) | `unreachable` | yes |
| no head within 120 s (same as the player's transcode wait) / no body bytes for 30 s | `timeout` | yes |
| reset mid-body | `network` | yes |
| closed before Content-Length / before the last chunk | `partial` | yes |
| malformed head, bad chunk framing, more body than declared | `bad_response` | yes |
| `https://` (no TLS on this path, same as streaming) | `unsupported` | no |
| free space below need + reserve (1 GB), checked before, after the head, and every 64 MB | `no_space` | no |
| write fails | `disk`, or `no_space` if the disk is now full | no |

**Retry policy.** Backoff is 2 s, 4 s, 8 s … capped at 5 minutes. After 8
consecutive attempts that moved no bytes, the item goes to FAILED. **Progress
resets the count**, so a flaky link that keeps advancing never runs out of
retries. Backoff is not persisted: after a reboot a queued item is tried at
once.

**Checkpoints.** The record is saved every 8 MB or 2 s, and the file is fsynced
at the same time. In-memory progress, which the UI will read, updates on every
buffer.

**Completion.** The partial is renamed to `media.ts` only once its length
matches Content-Length/Content-Range, or the terminating chunk arrived. A
close-delimited body with no length is the one case where a close means done.
Downloads requested through Stage 3 are then also validated as MPEG-TS (§7c).

### Write batching (from pkgi-ps3)

[pkgi-ps3](https://github.com/bucanero/pkgi-ps3) is the reference for moving
large files onto this console quickly. Its download path is libcurl with a
resume offset, writing through newlib's *buffered* `fwrite`. It keeps its
per-read bookkeeping near zero: free space is queried once per 512 writes, and
progress is redrawn every 500 ms. Its very fast mode is the XMB's own
background downloader (`/dev_hdd0/vsh/task`). That mode only installs `.pkg`
files and only runs from the XMB, so it cannot carry media for this app.

What carries over:

* **Batch the HDD writes.** Sockets are read straight into the free tail of
  the one 512 KB buffer. The payload is written only when fewer than 64 KB
  remain, at a checkpoint, and at the end. Every lv2 fs write is a syscall into
  an encrypted HDD, and one per network read costs far more than one per
  roughly 450 KB. Measured on the host: a 3 MB transfer takes 398 network reads
  and 7 disk writes. Whatever arrived is still flushed when a transfer stops
  for any reason other than a failing disk, so resume never loses bytes that
  were received.
* **Large reads.** Reads are always at least 64 KB, in the spirit of
  stream.cpp's 256 KB request after Movian. This complements the 512 KB
  `SO_RCVBUF` that stream.cpp measured as a win on this console. pkgi (curl)
  sets neither.
* **Cheap bookkeeping.** In-memory progress is published every 250 ms, free
  space is checked every 64 MB, and the record is saved every 8 MB or 2 s.
* **Not taken:** libcurl itself (this app has no curl portlib, and owning the
  socket is what allows the receive buffer and pacing), and pkgi's separate
  `HEAD` request for the size. The size is read from the GET's own response
  head and checked against free space before any body byte is written, which
  saves a round trip.

## 5a. Playback gets the network

The console pulls about 25 Mbps over HTTP in total (vquality.h has the
measurements), so a download and a stream compete for the same pipe. The
player tells the manager about every stream it opens:
`dl_playback_begin(url)` on the initial open, on every seek and on every track
change, and `dl_playback_end()` on every exit path through a scope guard.

| Stream | Downloads |
|---|---|
| **Heavy:** above 480p, direct play (no bitrate ceiling), any HD audio stream copy | **stopped.** The active one goes back to QUEUED with its data, nothing starts until playback ends, then it resumes by Range. |
| **Light:** 480p or 360p, a VideoBitrate ceiling of at most 1.5 Mbps, no HD audio copy, audio at most 640 kbps | continue, **paced to 12 Mbps** |

The decision comes from the **exact URL the player built**
(`dl_stream_is_light`), so it cannot disagree with what was requested, and
there is no second capability engine. Anything missing or unparseable counts as
heavy: when in doubt, the stream wins. A track change to a TrueHD copy
re-decides and stops a paced download at once.

Pacing sleeps between reads. The socket buffer fills and TCP flow control slows
the server, so no data is dropped. Time spent pacing is not counted as the
server being idle. The 12 Mbps share leaves the ~2.2 Mbps light stream plenty of
headroom for its read-ahead ring to refill after a server hiccup, and that
refill is what stutters, not the average.

Thread priority already favours playback. The download worker runs at 1500 and
the player's threads at 700–1100; on lv2 a lower number means higher priority.

The music player is not gated: its streams are audio-only.

## 6. Restore (offline startup)

`dl_manager_init` reads only the HDD and makes no network call. For each item
directory whose name is a valid id:

| Found | Result |
|---|---|
| record says `downloading` | back to `queued`; resumes from the partial |
| record says `completed`, media present at the recorded size | completed |
| record says `completed`, media missing or wrong size | `failed`/`corrupt`; retry re-downloads |
| record not `completed` but `media.ts` present at the right size | completed (stopped between rename and record save) |
| record damaged, meta + media present | record **rebuilt**, completed |
| record damaged, no media | `failed`/`corrupt`, visible with its title (or id) so it can be removed; re-enqueueing starts clean |
| record filed under another item's directory | treated as damaged |
| directory name not a valid id | ignored |

Damaged items are never silently dropped, because they still hold HDD space.

## 7. Stage 3: Jellyfin integration

```
 item page (info screen)                  already loaded: item, detail, versions
        |  dl_download_item(item, detail, version)          [dl_ps3.cpp]
        v
 dl_request_build()                                          [dl_request.cpp]
   selection = version + stream_select_initial()   <- the player's track rule
   decision  = stream_request_resolve(prefs, sel)  <- the player's decision
   url       = stream_url_build(decision, StartTimeTicks=0, no PlaySessionId)
   meta      = item + identity + detail + what the decision makes the file
   extras    = container "ts", runtime, artwork URLs
        |  dl_enqueue(meta, url, 0, extras)   disk writes only; returns at once
        v
 worker: artwork (best effort) -> media.ts.part -> length + TS validation
        -> media.ts, COMPLETED
```

### 7a. One stream decision, shared

`build_stream_url()` used to hold the whole stream decision inline, reading
the player's state and the settings globals. That decision now lives in
**`player/stream/stream_request.{h,cpp}`**. It was moved rather than
rewritten: same code, same comments, and it stays pure, with no globals or I/O.

| Step | Function | Used by |
|---|---|---|
| settings snapshot | `stream_prefs_current()` (player_session.cpp): `vquality_get()`, `hd1080_enabled()`, `surround_enabled()`, `surround_hd_preferred()`, display size | player, downloader |
| initial tracks | `stream_select_initial()`: the version's default audio track, subtitles off | player (show_player), downloader |
| decision | `stream_request_resolve()`: frame ceiling, profile/level, bitrate ceiling (0 = direct copy), AC-3/MP3 transcode or DTS/TrueHD stream copy, stream indices, MediaSourceId, LiveStreamId | player, downloader |
| URL | `stream_url_build()`: the decision plus server, DeviceId, **PlaySessionId** and **StartTimeTicks** | player, downloader |

`build_stream_url(ps, ticks)` is now a thin adapter. It snapshots the settings,
passes the player's selection, keeps the frame ceiling its jitter buffer was
sized for, and adds its session and offset. The downloader's adapter
(`dl_request_build`) passes the same things with `StartTimeTicks=0` and no
`PlaySessionId`. The server therefore makes the same original, remux or
transcode choice, and `media.ts` is exactly what Play would have streamed from
the start.

**Proof, both ways:**

* *Playback unchanged.* The pre-refactor `build_stream_url` was compiled on the
  host, with its globals stubbed, as an oracle. The refactored path produced
  byte-identical URLs on all **31,104** combinations of quality step, 1080p
  toggle, surround mode (stereo / 5.1 / 5.1 with HD copy), display size, audio
  track kind (DTS-HD, TrueHD, AC-3, E-AC-3, AAC), subtitle, version (none /
  chosen / live), session and offset. Eleven of those URLs are frozen as
  goldens in the test suite.
* *Download = playback.* Over 1,620 combinations (quality × 1080p × surround ×
  display × default-track kind × live source), the download's decision matches
  Play's in every field, and its URL has the same path and parameters. The
  only difference is that PlaySessionId is absent, and StartTimeTicks is 0 in
  both. A resuming playback differs only in StartTimeTicks.

**No playback session is created.** A download makes no PlaybackInfo call,
because that would mint a PlaySessionId and open live streams just to
download. The version list comes from the item DTO that the info screen
already fetches with `jellyfin_fetch_media_sources`. It carries the same
MediaSources/MediaStreams, parsed by the same `jellyfin_parse_media_sources`
the player's PlaybackInfo response goes through. `stream.ts` without a
PlaySessionId is a path the player already relies on when PlaybackInfo fails.
Because `jellyfin_stop_transcode()` only acts on a PlaySessionId, a player
seek can never kill a download's transcode.

**Nothing is guessed.** `dl_request_build` refuses to build a request without
a version. That version's tracks decide the audio track and the HD-copy
choice, so guessing would silently differ from Play.

Consistent with Play at the same moment: the quality is whatever
`vquality_get()` holds, which is the per-title value once the info screen has
restored it. A per-session HD veto (a coreless DTS track found earlier)
applies to both, because the player's first URL is also built before it
resets the veto.

### 7b. Metadata and artwork

`meta.txt` holds id, type, title, series and series id, season, episode,
year, runtime, overview, and the item page's video/audio descriptions. It also
holds what the **file** is: `ts`/`h264`, the audio codec actually requested
(`mp3`/`ac3`, or `dts`/`truehd` when copied), the frame ceiling, the bitrate
ceiling, MediaSourceId, AudioStreamIndex, the quality label, and the label of
the track downloaded.

Series, season, episode, year, runtime and backdrop source are parsed by
`jellyfin_parse_item_identity()` (jellyfin_api.cpp, pure) from the item DTO
that `jellyfin_fetch_item_detail()` already downloads for the info screen. They
land in `XMBItemDetail.identity`, at no extra request. Missing fields keep
"unknown" values (-1 / 0 / ""). The runtime falls back to the version's.

Artwork is fetched by the worker, not the UI, as the first step of a transfer
attempt:

* **What:** the item's `Primary` image (`maxWidth=400&maxHeight=600`, aspect
  kept), and a backdrop at `maxWidth=1280`: the item's own, or for an episode
  the series backdrop it inherits (`ParentBackdropItemId`).
* **How:** a whole-body GET into the worker's 512 KB buffer, sent with the
  session header like every request. It is accepted only as `200` with
  `image/*` and JPEG/PNG magic bytes, then written with the same atomic
  `.tmp` + rename as records, as `poster.jpg`/`backdrop.jpg`. `meta.txt`
  names an image only once its file exists. Stored as JPEG bytes and not
  decoded: decoding is for whatever screen shows it (Stage 4/5, through the
  existing image path).
* **Once:** an image already on disk is never requested again, including when
  a crash left its URL pending. A `404`/`410` means the server has none, and
  it is not asked again. Other failures are retried on later attempts, **3
  failed fetches in total** (persisted), and then given up.
* **Never fatal:** artwork failures do not affect the media download.

### 7c. Completion validation

A live transcode is chunked with no length. When the server's ffmpeg dies
part way, the response can still end cleanly, so "all promised bytes arrived"
proves nothing. Every byte written goes through a streaming MPEG-TS scan
(`dl_ts.cpp`, fed at each batched flush). The scan reads a few header bytes per
188-byte packet and never re-reads the file. At completion:

* size must be whole packets;
* when the scan saw the whole file (the transfer started at byte 0; a
  transcode always restarts there), every packet must start with `0x47` and
  the video PES timestamps must be present;
* the video PTS span must cover at least 90 % of the runtime, minus 10 s.
  This is deliberately loose: its job is catching a transcode that died, and
  a false alarm would re-download a whole film. 33-bit PTS wrap is handled.
  Audio PES do not count.

A failure is `bad_media` ("Server sent an incomplete video"). The partial is
discarded and the item is retried **once**; after that it is FAILED for the
user to decide. Unlike other errors, progress does not reset this count: a
validation failure means a whole film was fetched, and that is exactly the
attempt not to repeat freely. A byte-resumed file, which the scan did not see
whole, gets the packet-size check only. This is not a checksum: Jellyfin
provides none, and two transcodes of the same request are not
byte-identical.

### 7d. Service lifecycle

The lifecycle is portable (`dl_service.cpp`, host-tested) and wired in
`main.cpp`:

| When | Call | Effect |
|---|---|---|
| after `load_config()` | `dl_service_start()` | starts the **one** worker thread (idempotent). The worker restores every item from the HDD: no network, off the UI thread, so the first frame never waits. Completed items are usable whatever the server's state; an interrupted item is back in QUEUED with its bytes. |
| saved login, or login succeeded (before the main menu) | `dl_service_refresh_auth()` | installs the session header, and the queue runs |
| main menu returns (logout / revoked) | `dl_service_refresh_auth()` | empty token, so the queue holds |
| a download gets `401` | — | item back to QUEUED (`auth`), queue held until the next `refresh_auth` |
| app exit (before `http_end`) | `dl_service_stop()` | worker told to stop. The active item parks in QUEUED with its data; the worker is joined and the manager shut down |

Nothing transfers without a session. That is enforced in the manager itself
(no auth header means no attempt), not only by the service. Roots are tried
on the worker in order, and the first that proves writable wins (§4). With
none, the service idles once a second and every `dl_*` call reports
`DL_E_NOT_READY`.

The worker runs at thread priority 1500, below every playback thread
(700–1100). Playback gating (§5a) is unchanged and independent of all this.
Heavy streams stop downloads and light ones pace them, now verified with URLs
from the shared builder: light at 480p/360p only, and heavy at every step when
an HD track is copied.

### 7e. Resume and transcodes

A stream-copied or transcoded `stream.ts` is produced live, so Jellyfin serves
it as a `200` with no Range support. An interrupted download of one restarts
from zero, which the transfer handles correctly (§5, tested). Byte-exact
resume applies when the server honours Range. Time-offset resume
(`StartTimeTicks` plus TS splicing) or HLS segments would change this; it is
left as a measured decision for later, not assumed.

## 8. Stages

| Stage | Scope | Status |
|---|---|---|
| 1 | persistent storage model, states, queue, bookkeeping, tests | **done** |
| 2 | network transfer, progress, retry, resume, cancellation, yielding to playback, batched writes | **done** (service built, not started; player gate live) |
| 3 | Jellyfin integration: shared stream decision, request builder, metadata + artwork, TS validation, service lifecycle, session hold | **done** (no UI entry point yet) |
| 4 | offline library, offline startup path, playback of `media.ts` through the existing player (`stream_open` on a local file) | **done** (§11; nothing launches it until Stage 5) |
| 5 | UI: item-page action, Downloads list with progress, Offline section | **done** (§12) |

## 9. Tests

```
cd tests && make -f Makefile.host test_offline && ./test_offline   # -v logs
```

The suite covers the state machine (every legal and illegal transition),
record and metadata round-trips, and malformed input of every kind: torn,
traversal, overflow, bad numbers. It also covers URL, request, head and chunked
parsing across every split point, and against the fake server:
complete, queue order and limits, progress/checkpoints, drop plus Range
resume, reset, idle and head timeouts, Range ignored, inconsistent 206, 416,
chunked transcode, close-delimited bodies, pause/resume, cancel (including
racing a pause), retry/backoff/exhaustion, progress resetting retries,
permanent HTTP errors, invalid responses, unreachable server, playback
suspension, the light/heavy stream classifier across every quality step and
audio mode, heavy streams stopping and resuming downloads, a track change to an
HD copy, pacing (held to the share, lifted when playback ends, pause noticed
mid-sleep, long pacing waits not mistaken for an idle server), write batching,
deletion (including the active item and unknown files), storage
limits (up front, from the server's size, disk full mid-write, space draining
mid-transfer), crash/restart recovery, damaged state, and offline startup.
The suite runs clean under ASan and UBSan.

Stage 3 adds:

* **Stream selection:**
  * goldens against the pre-refactor playback URL;
  * download-equals-playback across 1,620 combinations (decision fields and
    URL parameters);
  * StartTimeTicks=0 with no PlaySessionId, and no token in the URL;
  * refusal without a version, with a bad id, with a missing server, or when
    the URL overflows;
  * https refused at enqueue;
  * gating classification of builder URLs at every quality.
* **Metadata:**
  * identity parsing, including "ParentIndexNumber is not IndexNumber";
  * request metadata for an episode, and for a movie with optional fields
    missing;
  * a `meta.txt` round trip.
* **Artwork:**
  * saved and linked in meta, with the session header;
  * 500, HTML and not-an-image responses are non-fatal;
  * a 404 is not retried;
  * oversized images are rejected;
  * gives up after 3 tries, persisted across a restart;
  * never re-fetched once on disk, including after a crash left the URL
    pending.
* **TS validation:**
  * the scanner, fed in odd splits;
  * garbage, no-PTS, audio-only PTS and PTS-wrap cases;
  * a complete transcode is kept;
  * a transcode that died part way becomes `bad_media`, is retried once, then
    FAILED, and a retry succeeds;
  * non-TS bodies and torn packets are rejected;
  * unknown runtime;
  * byte-resumed files, including one with a torn tail;
  * a restart after the Range was ignored;
  * a 500 is retried.
* **Playback:** heavy playback parks a requested download, persisted, and it
  resumes by Range afterwards.
* **Session:**
  * no session means no transfer;
  * a 401 holds the queue (no other item is tried) and a fresh login releases
    it;
  * the auth header format.
* **Lifecycle:**
  * start once;
  * restore happens on the worker, falls back past an unwritable root, and
    touches no network;
  * with the server down at boot, completed items stay usable and an
    interrupted one is restored;
  * no session means an idle worker;
  * a download resumes when the server returns;
  * logout holds the queue;
  * stop suspends the worker while joining, is harmless twice, and can be
    followed by a restart;
  * thread start failure;
  * no writable root.

Mutation testing covered 38 deliberate breaks of Stage 3 logic (stream decision,
request builder, metadata, artwork, TS scan and rule, bad-media limit, auth
hold, service start/restore/stop, identity parser). 36 are caught. The other 2
are equivalent, meaning the behaviour cannot change:

* dropping the bad-media exclusion from the progress reset: that outcome never
  reports progress;
* dropping the no-root idle: the manager's own idle returns the same 1000 ms.

**Build:** clean PS3 build with 0 errors. The warning set is identical to
`main`'s (45).

## 10. Remaining work

* **UI follow-ups** (deliberately left out of Stage 5):
  * posters in the Downloads and Offline lists. `poster.jpg` is on disk, but
    decoding it on the UI thread would stall a frame per row, so the lists are
    text until a background decode (like the library grid's) is wired to them;
  * a Downloads/Offline entry on the tab bar. Stage 5 uses Settings rows so
    the XMB spine is untouched (§12b);
  * "download the whole season" from a series page.
* **To verify on hardware** (nothing here has run on a console yet):
  * that `/dev_hdd0/jellyfin_offline` is writable (the log says which root
    won);
  * that a 25 Mbps-class download sustains near the measured ceiling;
  * Jellyfin's handling of a `stream.ts` with no PlaySessionId running beside
    a light playback from the same DeviceId;
  * plugin "live" sources, which normally need PlaybackInfo to open a
    LiveStreamId and so may refuse a download (surfaced as an HTTP failure).
  * Stage 4 on the console:
    * local playback startup and seek feel on the real HDD (a seek costs 4–12
      reads of 256 KB);
    * VDEC entering at the PAT and keyframe found by the index;
    * the frame-buffer re-grab when a file's ceiling is larger than the
      current quality setting's.
  * Stage 5 on the console:
    * the item page's Play + Download row at 480p/576p output (the button is
      a fixed 250 px beside Play);
    * the whole path end to end: DOWNLOAD, the Downloads list during a stream,
      then Offline playback with the network unplugged;
    * the sign-in-failure offer with the server down.

## 11. Stage 4: offline library, startup path, local playback

### 11a. The offline library (`dl_library.{h,cpp}`)

This is a query layer over the records the manager already keeps. There is no
second index and no new files on disk. An item is in the library only when
**all** of these hold. They are checked every time it is asked for, because
the file can change under a running app (FTP, a full disk, a crash):

* its record is **COMPLETED**. Queued, downloading (active), paused, failed,
  cancelled and partial items are never listed and never playable;
* `media.ts` exists at **exactly** the size the record verified;
* a TS download is still whole packets. Size % 188 is checked even when the
  file and the record agree, since a hand-edited record is still not a TS.

| API | Returns |
|---|---|
| `dl_library_ids(ids, max)` | playable ids in download order (cheap: no metadata read, no 64-entry snapshot array) |
| `dl_library_get(id, &entry)` | one fully verified entry: `DlMeta`, `meta_ok`, media path, poster/backdrop paths, bytes, seq |
| `dl_library_has(id)` | does an online item have a playable local copy |
| `dl_library_plan(entry, &plan)` | how to set the player up: frame ceiling, runtime, download gate |

The library adds one manager query, `dl_completed_ids()`: ids only, in queue
order.

**Stale metadata.** If `meta.txt` is missing, torn or belongs to another item,
the entry still plays, using what the record knows (id, title, verified
container) with `meta_ok = false`. A stale entry is treated conservatively:
the frame ceiling becomes 1080p (it holds any smaller frame), and the file is
treated as heavy for download gating.

**Missing artwork.** A poster or backdrop path is set only while the image file
exists with a non-zero size. It is re-checked on every lookup, so an image
deleted since is simply absent.

**After a restart,** Stage 1's restore already turns a COMPLETED record with
missing or wrong-size media into FAILED/corrupt, so the library and the
records never disagree about it.

### 11b. Offline startup path

The library needs no server and no login. The worker restores it from disk on
its own thread right after `load_config()` (§7d). The one thing a startup
screen cannot tell from an empty list is "not restored yet", so Stage 4 adds:

* `dl_svc_restored()`: true once the worker has finished its restore
  attempt. It is published only after the store has been read, and a stopped
  service reports false;
* `dl_service_wait_restored(timeout_ms)` (console): a bounded wait, true when
  the library can be queried.

Before restore, the library reports nothing: an empty list and no item
playable.

### 11c. Local playback through the existing player

`show_player_offline(item_id, resume_secs)` (player.h) plays a library item
with the **same player**. It uses the same decoder, jitter buffer, HUD,
threads and teardown; `show_player()` and it share one body
(`show_player_run`). The online path passes `local = NULL` and runs exactly
the statements it always ran. Every local difference is an explicit `if
(local)`:

| Online | Local file |
|---|---|
| PlaybackInfo, track fetch | none: one audio track (the downloaded one), so the AUDIO/CC menus are inert |
| `build_stream_url()`, `stream_open(url)` | `stream_open_file(path, offset)`, an explicit local source in `stream.cpp` |
| frame ceiling from the quality setting | the **file's** ceiling (`meta.txt`), since it was downloaded at some quality |
| runtime from the server | **measured from the file** (first keyframe PTS to last video PTS), so stale metadata cannot mislead the HUD |
| report playing / progress / stopped, stop transcode | none. A report to an unreachable server would block for its connect timeout |
| HD-audio fallback reopen as AC-3 | disabled (the audio is what was downloaded) |
| seek: new transcode at StartTimeTicks | seek **in the file** (§11d) |
| download gate from the URL | `dl_playback_begin_local(plan.light)`: the file's own weight by the same `DL_LIGHT_*` thresholds; heavy stops downloads, light paces them |

**The stream layer.** `stream.cpp` gains a local-file source. The handle is
tagged (`0x40000000`) so it can never be mistaken for a socket, and
`stream_read()` serves it through the same 256 KB buffer. `stream_close()` and
`stream_set_timeout()` replace the player's direct `netClose()`/`setsockopt()`
calls and do exactly what those did for a socket. Nothing else in the online
path changed.

**Verification at play time.** `show_player_offline` asks the library again at
the moment of playing, so a file removed or truncated since a list was drawn
never reaches the decoder. A file with no enterable video is refused before
the player is touched.

### 11d. Entering and seeking a local TS (`stream_local.{h,cpp}`)

The player resets its demuxer on every seek, forgetting the PAT/PMT. So a seek
must land on a **PAT that precedes a keyframe**, which is exactly where
ffmpeg's mpegts muxer writes its tables. The random-access flag marks the
keyframe; without one, "a video PES right after a PAT" is the keyframe. With
flags but no repeated PAT (another muxer), the keyframe itself is the entry.

A seek is an interpolation search over (offset, PTS), with bisection once
interpolation stops paying. It finishes with a forward scan to the last entry
at or before the target, allowing up to 0.5 s past it.
Each probe is one 256 KB read, using the stream's own buffer as scratch (no
allocation) through a separate descriptor. On an hour of VBR test media, the
worst landing was 1 s before the target in 4 reads.

The clock stays exact: the entry's PTS minus the file's first PTS becomes
`play_base_us`, and avsync re-latches on the first frame, as it does online.
33-bit PTS wrap, streams not starting at PTS 0, B-frame reordering at the end,
and audio PES (ignored) are handled. The PTS parsing is `dl_ts`'s, exposed as
`dl_ts_packet_info()` rather than copied.

### 11e. Stage 4 tests

* **Index:**
  * duration measured from the file;
  * streams starting at a non-zero PTS;
  * a PTS wrap;
  * B-frames at the end;
  * refusal of garbage, empty and unreadable files.
* **Seek:**
  * on an hour of VBR: lands on a PAT-before-keyframe at the exact reported
    position, never more than 0.5 s past the target and within a GOP or two
    before it, in 12 reads or fewer;
  * clamps past the end;
  * deterministic;
  * files without keyframe flags;
  * files with flags but no repeated PAT;
  * a search starting between a PAT and its keyframe;
  * short GOPs with minimum-size reads;
  * positions across a wrap;
  * the minimum-scratch guard.
* **Library:**
  * only COMPLETED items with verified media;
  * queued, paused, failed, cancelled, partial and unknown ids refused;
  * download order;
  * artwork present, absent, and deleted since;
  * media truncated, removed, or torn;
  * a torn TS that matches its record;
  * stale metadata (torn, other item's, missing);
  * restart consistency;
  * empty before restore.
* **Plan:**
  * the file's frame ceiling;
  * the light rule's every branch;
  * stale-meta fallbacks;
  * light/heavy agreement with the URL classifier for every quality.
* **Local playback gate:** heavy stops, light paces, and heavy starting
  mid-download parks the transfer.
* **Startup path:**
  * restored only after the worker ran, with no network and no login;
  * a stopped service is not "restored";
  * no-root restore reports restored with nothing to show.

Mutation testing covered 30 planted breaks of Stage 4 logic (library
verification, light rule, plan, entry/seek/index, packet parse, local gate,
completed-id query, restore publication):

* 26 are caught.
* 3 are equivalent (the behaviour cannot change):
  * the COMPLETED test in `verify`, since `dl_media_path` already requires
    COMPLETED;
  * the `size <= 0` guard, since a completed record always has a verified
    non-zero size;
  * the past-the-end clamp, since the interpolation fraction is clamped too.
* 1 survives: the finish scan's continuation past its first window. The
  bracket already ends within two windows of the target on every fixture.
  Removing the finish scan altogether is caught.

**Online playback unchanged:** the Stage 3 goldens (byte-identical playback
URLs) still pass, and the player diff substitutes only `stream_close` and
`stream_set_timeout` on the online path.

## 12. Stage 5: the UI

Three entry points, all drawn by existing overlay patterns (a blocking loop
over the wave background that owns input until Circle, like
`xmb_resume_choice` and the info page). The XMB spine, the tab bar and
JellyWave are untouched.

### 12a. One pure model (`dl_ui.{h,cpp}`)

Every word the screens show and every decision about what a button press does
lives in `dl_ui`, which has no PS3 dependency and is pinned by the host tests.
The screens only draw what it returns and forward the resulting `DlUiAction`
to the manager (`dl_pause`, `dl_resume`, `dl_retry`, `dl_cancel`,
`dl_remove`), to `dl_download_item`, or to `show_player_offline`.

* `DlUiContext {playback_block, auth_held, ready}` is read once per refresh
  from `dl_playback_blocking()`, `dl_auth_held()` and `dl_manager_ready()`.
* **Item page:** `dl_ui_item_action` / `dl_ui_item_label` map the item's
  record (or none) to one action and one label: Download, Queued, Queued
  (sign in), Queued (streaming), Retrying..., Downloading 42%, Paused 42%,
  Play offline, Retry download, Can't download, Downloads unavailable.
* **Downloads list:** `dl_ui_row` gives a row its title, status line
  ("Downloading", "Waiting...", "Retrying in 8s -- Server unreachable",
  "Waiting for sign-in", "Paused while streaming", "Paused", "Downloaded",
  "Failed -- Not enough HDD space", "Cancelled"), size ("1.2 GB of 2.9 GB"),
  progress (or no bar for a transcode of unknown length) and colour.
  `dl_ui_row_primary` (Cross) and `dl_ui_row_secondary` (Square) give the
  actions. A failed item rebuilt from damaged files has no request to retry,
  so it offers no Retry. It must be downloaded again from its page.
* **Confirmation:** `dl_ui_action_needs_confirm` asks before deleting a
  finished download and before cancelling one that has bytes on disk. Nothing
  else asks. The dialog's default is the safe option.
* **Offline library:** `dl_ui_offline_lines` gives "Pilot" /
  "The Expanse  S1 E1  ·  47 min  ·  1.2 GB" and falls back to the title and
  size for an entry whose metadata is stale.
* Helpers: `dl_ui_format_bytes`, `dl_ui_clamp_selection` (keeps the selection
  and scroll window valid when the list shrinks under it).

Two small manager queries were added for the screens, with no new state:
`dl_ids` (every id, queue order) and `dl_counts` (active / completed /
failed). `dl_completed_ids` now shares their implementation.

### 12b. Where the screens live

* **Item page** (`ui_info.cpp`): a DOWNLOAD button sits beside Play on the
  same row (Right from Play, Left back). It shows for Movie, Episode and Video
  items. It downloads the version on screen (`versions.source[version_sel]`),
  the one Play would stream, through the shared stream decision (§7a). The
  button shows the label, a progress bar while downloading, and a 3 s toast
  under it ("Added to Downloads (Settings > Downloads)", or why not). The
  record is polled with `dl_find` every 250 ms; nothing else is fetched.
  Play offline goes through the existing resume prompt.
* **Settings** (`ui_settings.cpp`, `ui_nav.cpp`): two rows, **Downloads**
  ("2 active", "1 failed", "None") and **Offline Library** ("3", "Empty"),
  both "Unavailable" when there is no writable store. Settings was chosen over
  a new tab so the spine is not touched, and because it is reachable when the
  server is not.
* **Sign-in failure** (`main.cpp`): if `do_login()` fails and the library is
  not empty (after up to 1.5 s for the restore, §6), a dialog offers
  "Try again" (default, the old flow exactly) or "Open Offline". Leaving the
  Offline list returns to sign-in with the server URL kept.

### 12c. The screens (`ui/xmb/ui_downloads.cpp`)

* **Downloads:** up to as many rows as fit above the hints bar, with a
  scroll indicator past that. A banner under the title says when the queue as a
  whole is held ("Downloads pause while you stream -- they resume
  afterwards", "Sign in to continue downloads"). The list is re-read about
  four times a second, and only the visible rows are fetched (`dl_find`).
  Refresh runs after input, and an action pressed in the same frame as a
  scroll is ignored, so a press never acts on a row the user has not seen.
* **Offline:** the completed, verified items (`dl_library_ids`). Metadata is
  read (`dl_library_get`) only when the list or scroll window changes. Cross
  plays through `show_player_offline` (§11c); Square deletes, after a confirm.
  An empty library says how to add something.
* No network work, and no per-frame allocation. Posters are not drawn (§10).

Host previews of every screen are in `tools/ui_preview` (frames 8–16,
`preview_offline.cpp`, which renders from the real `dl_ui` model).

### 12d. Stage 5 tests

* **Item button:** each state with and without a downloadable version, in
  each context (not ready, signed out, streaming, retry pending): its label
  and its action; and the result text for every error code.
* **Rows:** each state's status, size, bar and colour; transcodes with no
  total; the retry countdown (rounded up); the held-queue variants; every
  recorded error reading as words; primary and
  secondary actions; Retry hidden for a record with no request; which actions
  confirm; the queue banner.
* **Offline lines and helpers:** episodes, films, stale metadata, runtimes
  under and over an hour, a missing season; byte formatting at each unit
  boundary; selection clamping when scrolling, when the list shrinks or
  empties, and with a zero-row window.
* **Flow through the real manager:** DOWNLOAD from the item page, progress
  and pause/resume as the list shows them, completion into the Offline
  library, delete, and `dl_ids` / `dl_counts` agreeing with the list at each
  step.

Totals: 45,815 checks pass. ASan/UBSan are clean, and a strict clang pass
(`-Wshadow -Wconversion`) gives 0 warnings. Mutation testing planted 20 breaks
in `dl_ui` and the new manager queries; all 20 are caught.

**Build:** clean PS3 build with 0 errors. The warning set is identical to
`main`'s (45).
