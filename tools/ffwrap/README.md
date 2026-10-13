# ffwrap: stop Jellyfin flooding your router

**Who needs this:** your Jellyfin server PC is wired to the router, you stream
through a debrid / Stremio-style plugin (Gelato and similar), and films on the
PS3 buffer and stall over and over.

**Why it happens:** when a film starts, the server's ffmpeg downloads it from
the internet as fast as your connection allows. Some routers can't route that
and still pass traffic between your PC and the PS3, so the PS3's stream stops
for seconds at a time. (A gigabit switch between the router, PC and PS3 fixes
it too, and also protects Moonlight.)

**What ffwrap does:** it stands in for Jellyfin's `ffmpeg.exe`. When the input
is an internet address it adds `-readrate 1.25 -readrate_initial_burst 20`, so
ffmpeg reads at 1.25× the film's own bitrate after a 20-second head start.
Everything else goes to the real ffmpeg untouched. Windows only.

## Install (Windows)

1. Build it with the C# compiler that comes with Windows (no downloads needed):

   ```
   mkdir C:\ProgramData\Jellyfin\ffwrap
   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /platform:x64 /out:C:\ProgramData\Jellyfin\ffwrap\ffmpeg.exe ffwrap.cs
   copy "C:\Program Files\Jellyfin\Server\ffprobe.exe" C:\ProgramData\Jellyfin\ffwrap\
   ```

   Or use the prebuilt `ffwrap.zip` from the release: unzip `ffmpeg.exe` into
   `C:\ProgramData\Jellyfin\ffwrap\` and copy `ffprobe.exe` next to it as above.

2. Close Jellyfin. In `C:\ProgramData\Jellyfin\Server\config\encoding.xml` set

   ```
   <EncoderAppPath>C:\ProgramData\Jellyfin\ffwrap\ffmpeg.exe</EncoderAppPath>
   ```

   (add the line next to `<EncoderAppPathDisplay>` if it isn't there).

3. Start Jellyfin. Its log should say
   `FFmpeg: "C:\ProgramData\Jellyfin\ffwrap\ffmpeg.exe"`. Each film start that
   gets limited adds a line to `C:\ProgramData\Jellyfin\ffwrap\ffwrap.log`.

The real ffmpeg is expected at `C:\Program Files\Jellyfin\Server\ffmpeg.exe`;
set the `FFWRAP_REAL` environment variable if yours is elsewhere.

**To undo:** remove the `EncoderAppPath` line and restart Jellyfin. A Jellyfin
update may reset it, so check it after upgrading.
