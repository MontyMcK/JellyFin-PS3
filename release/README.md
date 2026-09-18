# Download

**[⬇ Download JellyFin-PS3.pkg](https://github.com/vortigauntlet/JellyFin-PS3-LosslessAudio/raw/main/release/JellyFin-PS3.pkg)** — one click, straight to the file.

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
| **Quality** | Triangle on a title → Quality row | **1080p 25** |
| **Surround** | Settings → Surround 5.1 | **HD** |

For actual multichannel output the PS3 itself must also be told your setup can
take it: *Settings → Sound Settings → Audio Output Settings → HDMI → Manual*,
then tick **Dolby Digital 5.1 Ch** and **Linear PCM 5.1 Ch** (and 7.1 if your
receiver does 7.1).

Stuttering? Drop to **1080p 20** before changing anything else. The console can
only pull about 20-25 Mbps over HTTP — see the main
[README](../README.md#recommended-settings) for the measurements.

## Verifying the download

The `.self` inside the package is built from the commit this file ships with.
If you would rather build it yourself than trust a binary, see
[Build from source](../README.md#build-from-source) — the toolchain is
ps3dev/PSL1GHT and the whole thing builds with `make && make pkg`.
