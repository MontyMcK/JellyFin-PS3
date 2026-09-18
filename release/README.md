# Download

**[⬇ Download the latest release](https://github.com/vortigauntlet/JellyFin-PS3-LosslessAudio/releases/latest)** — the recommended link.

Direct file: [`JellyFin-PS3.pkg`](https://github.com/vortigauntlet/JellyFin-PS3-LosslessAudio/releases/latest/download/JellyFin-PS3.pkg)
— that link always resolves to the current release, so it cannot go stale the
way a version-pinned one does. This folder mirrors the same build, so the repo
always carries an installable package alongside the source it was made from.

## Install

Either way works; pick whichever is less hassle.

- **USB** — copy the `.pkg` to the root of a USB stick, plug it into the
  console, and install it from the XMB: *Game → Package Manager → Install
  Package Files*.
- **FTP** — drop it in `/dev_hdd0/packages/` and install it from
  **webMAN MOD → Package Manager**. No USB needed.

Installing over an existing copy is fine. Your login and settings live in
`/dev_hdd0/tmp/`, outside the app, so they survive.

## First run

Two settings do almost all the work:

| Setting | Where | Set it to |
|---|---|---|
| **Quality** | Triangle on a title → Quality row | **Max** |
| **Audio Output** | Settings → Audio Output | **5.1** |

The Quality row runs **360p → 480p → 720p → High** (10 Mbps) **→ Very High**
(20) **→ Max** (25), with the bitrate shown beside each name. Your choice is
remembered per title, so a heavy remux and a light episode each keep their own.

**Audio Output** is Stereo / 5.1 / 7.1. 7.1 only appears if your receiver
actually accepts eight channels of LPCM — most soundbars cap at six, and the
app asks yours rather than guessing.

On 5.1 the app also fixes a fault that silences dialogue on some receivers.
The PS3's audio port is 8 channels wide and its HDMI output is 6, and the
console's own 8→6 fold can lose the centre channel — which is where nearly all
the dialogue in a film mix lives. Nothing is compressed to achieve this: the
audio on the wire stays uncompressed LPCM.

**Dialogue still too quiet?** Settings → **Dialogue Boost** (+3/+6/+10 dB).
Film mixes carry full cinema dynamic range and none of these decoders applies
compression, so dialogue can sit well below effects on a compact system.

For actual multichannel output the PS3 itself must also be told your setup can
take it: *Settings → Sound Settings → Audio Output Settings → HDMI → Manual*,
then tick **Linear PCM 5.1 Ch** (and 7.1 if your receiver does 7.1).

Tick **Dolby Digital 5.1 Ch** as well even though nothing here sends Dolby
Digital. The centre-channel fix works by asking the console to treat the output
as 5.1, and it can only ask in a form the chain has said it accepts — the audio
that actually goes down the cable stays uncompressed LPCM either way.

Stuttering? Drop to **Very High** before changing anything else. The console
can only receive about 25 Mbps sustained, which is why **Max** stops there —
see the main [README](../README.md#recommended-settings) for the measurements,
including why asking for 30 actually delivers *less*.

## Verifying the download

The `.self` inside the package is built from the commit this file ships with.
If you would rather build it yourself than trust a binary, see
[Build from source](../README.md#build-from-source) — the toolchain is
ps3dev/PSL1GHT and the whole thing builds with `make && make pkg`.
