#!/usr/bin/env python3
"""Build a self-contained HTML player for the cold-boot animation.

The frames come from dump_frames.c, i.e. from the REAL timeline in
source/ui/render/boot_seq.h -- this page only draws what that computed.  The
mark and the wordmark face are extracted from the repo's own embedded
headers, so they are the pixels and glyphs the console uses.

What this is NOT: a render of the console.  The backdrop is a still
screenshot (the XMB behind the veil does not move here), the halo is a
canvas radial gradient rather than the RSX's 12-segment fan, and the
wordmark uses the browser's text engine, so kerning can differ by a pixel.
It exists to judge CHOREOGRAPHY -- timing, easing, overlap, where things go
-- before a TV is involved.

    python3 mkpreview.py --frames normal=a.json slow=b.json skip=c.json \\
                         --backdrop xmb_1080p.png --out preview.html

--backdrop is any 1920x1080 XMB screenshot; the top-left lockup area is
painted over with the background colour so the animation's lockup replaces
whatever lockup the screenshot has.  Without it the XMB is a flat gradient.
"""
import argparse, base64, json, pathlib, re

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def header_bytes(path, symbol):
    txt = (ROOT / path).read_text(encoding="utf-8", errors="replace")
    m = re.search(re.escape(symbol) + r"\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise SystemExit("no %s in %s" % (symbol, path))
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


PAGE = r"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Boot animation preview</title>
<style>
@font-face { font-family: "Mata"; src: url(data:font/otf;base64,__FONT__); }
:root { --bg: #0b0b12; --panel: #15151f; --ink: #e6e6f0; --dim: #9a9ab0; --acc: #aa5cc3; }
html, body { margin: 0; background: var(--bg); color: var(--ink);
  font: 14px/1.4 system-ui, sans-serif; }
main { max-width: 1280px; margin: 0 auto; padding: 16px; }
canvas { width: 100%; height: auto; display: block; background: #000;
  border-radius: 6px; }
.bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin: 10px 0; }
button, select { background: var(--panel); color: var(--ink); border: 1px solid #2a2a3a;
  border-radius: 6px; padding: 6px 12px; font: inherit; cursor: pointer; }
button:hover { border-color: var(--acc); }
input[type=range] { flex: 1 1 300px; accent-color: var(--acc); }
#info { font-variant-numeric: tabular-nums; color: var(--dim); min-width: 260px; }
p.note { color: var(--dim); max-width: 70ch; }
</style></head><body><main>
<canvas id="c" width="1920" height="1080"></canvas>
<div class="bar">
  <button id="play">Pause</button>
  <button id="restart">Restart</button>
  <select id="scn"></select>
  <select id="speed"><option value="1">1x</option><option value="0.5">0.5x</option>
    <option value="0.25">0.25x</option></select>
  <input id="scrub" type="range" min="0" max="1000" value="0">
  <span id="info"></span>
</div>
<p class="note">Frames are computed by the real timeline (boot_seq.h) via
dump_frames.c. The mark and the Mata wordmark are the embedded assets. The
backdrop is a still, and the halo and text are drawn by the browser, so use this
to judge timing and motion, not exact pixels.</p>
</main>
<script>
const DATA = __DATA__;
const BACKDROP = __BACKDROP__;
const MARK = "data:image/png;base64,__MARK__";
const RAMP = ["#00A4DC", "#4189D3", "#7A70CA", "#AA5CC3"];
const WORD = "JELLYFIN";
const MARK_SPAN = 88 / 72;

const cv = document.getElementById("c"), g = cv.getContext("2d");
const mark = new Image(); mark.src = MARK;
let back = null;
if (BACKDROP) { back = new Image(); back.src = BACKDROP; }

const scn = document.getElementById("scn");
for (const k of Object.keys(DATA)) {
  const o = document.createElement("option"); o.value = k; o.textContent = k; scn.appendChild(o);
}
let cur = DATA[scn.value], playing = true, t0 = performance.now(), tpos = 0;

function frameAt(t) {
  const fr = cur.frames;
  let lo = 0, hi = fr.length - 1;
  while (lo < hi) { const m = (lo + hi + 1) >> 1; if (fr[m].t <= t) lo = m; else hi = m - 1; }
  return fr[lo];
}

function drawXmb() {
  if (back && back.complete) {
    g.drawImage(back, 0, 0, 1920, 1080);
    // paint out the screenshot's own lockup: the animation brings its own
    const px = g.getImageData(260, 46, 1, 1).data;
    g.fillStyle = "rgb(" + px[0] + "," + px[1] + "," + px[2] + ")";
    g.fillRect(40, 18, 200, 60);
  } else {
    const gr = g.createLinearGradient(0, 0, 0, 1080);
    gr.addColorStop(0, "#1b1a3a"); gr.addColorStop(1, "#0d0c1c");
    g.fillStyle = gr; g.fillRect(0, 0, 1920, 1080);
  }
}

function drawVeil(rows) {
  for (let i = 0; i + 1 < rows.length; i++) {
    const [y0, a0] = rows[i], [y1, a1] = rows[i + 1];
    if (y1 <= y0) continue;
    const gr = g.createLinearGradient(0, y0, 0, y1);
    gr.addColorStop(0, "rgba(0,0,0," + a0 / 255 + ")");
    gr.addColorStop(1, "rgba(0,0,0," + a1 / 255 + ")");
    g.fillStyle = gr; g.fillRect(0, y0, 1920, y1 - y0 + 0.5);
  }
}

function drawWord(f) {
  const L = DATA[scn.value].lockup, px = L.word_px, track = px * 0.02;
  g.font = px + "px Mata";
  const m = g.measureText(WORD);
  const ink = m.actualBoundingBoxAscent - m.actualBoundingBoxDescent;
  const base = L.word_cy + ink / 2;
  let x = L.word_x;
  const homes = [];
  for (let i = 0; i < WORD.length; i++) {
    const w = g.measureText(WORD[i]).width;
    homes.push([x, w]); x += w + track;
  }
  const total = x - track - L.word_x;
  for (let pass = 0; pass < 2; pass++) {
    for (let i = 0; i < WORD.length; i++) {
      const [r, a] = f.word[i];
      if (a <= 0) continue;
      const [hx, w] = homes[i];
      const d = Math.min(hx - L.word_x, L.slide || 1e9);
      const px0 = hx - d * (1 - r);
      const k = Math.min(0.9999, Math.max(0, (hx - L.word_x + w / 2) / total)) * 3;
      const c = RAMP[Math.floor(k) + (k % 1 > 0.5 ? 1 : 0)] || RAMP[3];
      g.globalAlpha = a;
      g.fillStyle = pass ? c : "#000";
      g.fillText(WORD[i], px0, base + (pass ? 0 : 1.5));
    }
  }
  g.globalAlpha = 1;
}

// The glint: boot_glint_at() over the mark's coverage at 128px, added --
// the same thing boot_anim.cpp writes into its glint texture.
const GL = 128, gc = document.createElement("canvas"); gc.width = gc.height = GL;
const gx = gc.getContext("2d");
let cov = null;
function coverage() {
  if (cov || !mark.complete) return cov;
  gx.clearRect(0, 0, GL, GL); gx.drawImage(mark, 0, 0, GL, GL);
  const d = gx.getImageData(0, 0, GL, GL).data;
  cov = new Uint8Array(GL * GL);
  for (let i = 0; i < GL * GL; i++) cov[i] = d[i * 4 + 3];
  return cov;
}
function glintAt(u, v, pos) {
  const sc = (u + 0.45 * v) / 1.45, d = (sc - pos) / 0.08, b = 1 - d * d;
  return b > 0 ? b * b : 0;
}
function drawGlint(f, box) {
  const c = coverage(); if (!c) return;
  const img = gx.createImageData(GL, GL);
  for (let y = 0; y < GL; y++) for (let x = 0; x < GL; x++) {
    const i = y * GL + x; if (!c[i]) continue;
    const k = Math.min(1, glintAt((x + .5) / GL, (y + .5) / GL, f.gpos) * 0.55 * f.glint * c[i] / 255);
    img.data[i * 4] = 255; img.data[i * 4 + 1] = 245; img.data[i * 4 + 2] = 250;
    img.data[i * 4 + 3] = 255 * k;
  }
  gx.putImageData(img, 0, 0);
  g.globalCompositeOperation = "lighter";
  g.drawImage(gc, f.mark.cx - box / 2, f.mark.cy - box / 2, box, box);
  g.globalCompositeOperation = "source-over";
}
function star(x, y, arm, waist, rot, a) {
  const gr = g.createRadialGradient(x, y, 0, x, y, arm);
  gr.addColorStop(0, "rgba(255,248,255," + a + ")");
  gr.addColorStop(1, "rgba(255,248,255,0)");
  g.fillStyle = gr; g.beginPath();
  for (let k = 0; k < 8; k++) {
    const ang = rot + k * Math.PI / 4, r = (k & 1) ? waist : arm;
    g.lineTo(x + r * Math.cos(ang), y + r * Math.sin(ang));
  }
  g.closePath(); g.fill();
}
function drawSpark(x, y, bell, s, rot) {
  const size = bell * 0.38 * (0.55 + 0.45 * s);
  g.globalCompositeOperation = "lighter";
  const gr = g.createRadialGradient(x, y, 0, x, y, size * 0.45);
  gr.addColorStop(0, "rgba(255,248,255," + 0.40 * s + ")");
  gr.addColorStop(1, "rgba(255,248,255,0)");
  g.fillStyle = gr; g.fillRect(x - size, y - size, size * 2, size * 2);
  star(x, y, size, size * 0.11, rot, s);
  star(x, y, size * 0.5, size * 0.08, rot + Math.PI / 4, s * 0.7);
  g.globalCompositeOperation = "source-over";
}

function draw(f) {
  g.globalAlpha = 1;
  if (f.xmb || (f.phase === "DONE" && f.mark.owner === "static")) drawXmb();
  else { g.fillStyle = "#000"; g.fillRect(0, 0, 1920, 1080); }
  drawVeil(f.veil);
  const box = f.mark.bell * MARK_SPAN;
  if (f.halo > 0) {
    const gr = g.createRadialGradient(f.mark.cx, f.mark.cy, 0, f.mark.cx, f.mark.cy,
                                      f.mark.bell * 1.55);
    gr.addColorStop(0, "rgba(170,92,195," + (f.halo * (f.xmb ? 1 : f.op)) + ")");
    gr.addColorStop(1, "rgba(170,92,195,0)");
    g.fillStyle = gr; g.fillRect(0, 0, 1920, 1080);
  }
  if (f.mark.owner !== "none" && mark.complete) {
    g.globalAlpha = f.xmb ? 1 : f.op;
    g.drawImage(mark, f.mark.cx - box / 2, f.mark.cy - box / 2, box, box);
    g.globalAlpha = 1;
    if (f.glint > 0) drawGlint(f, box);
  }
  if (f.spark > 0) drawSpark(f.mark.cx, f.mark.cy - 0.47 * f.mark.bell,
                             f.mark.bell, f.spark, f.srot);
  if (f.status > 0) {
    g.font = "22px system-ui, sans-serif"; g.textAlign = "center";
    g.fillStyle = "rgba(150,150,175," + f.status + ")";
    g.fillText("Connecting to server", f.mark.cx, f.mark.cy + box / 2 + 0.045 * 1080 + 16);
    g.textAlign = "start";
  }
  drawWord(f);
  document.getElementById("info").textContent =
    f.phase + "  t=" + f.t.toFixed(0) + " ms  mark: " + f.mark.owner;
}

function tick(now) {
  const end = cur.frames[cur.frames.length - 1].t;
  if (playing) {
    tpos = (now - t0) * parseFloat(document.getElementById("speed").value);
    if (tpos > end + 800) { t0 = now; tpos = 0; }
    document.getElementById("scrub").value = Math.round(1000 * Math.min(tpos, end) / end);
  }
  draw(frameAt(tpos));
  requestAnimationFrame(tick);
}
document.getElementById("play").onclick = (e) => {
  playing = !playing; e.target.textContent = playing ? "Pause" : "Play";
  t0 = performance.now() - tpos / parseFloat(document.getElementById("speed").value);
};
document.getElementById("restart").onclick = () => { t0 = performance.now(); tpos = 0; };
document.getElementById("speed").onchange = () => {
  t0 = performance.now() - tpos / parseFloat(document.getElementById("speed").value);
};
scn.onchange = () => { cur = DATA[scn.value]; t0 = performance.now(); tpos = 0; };
document.getElementById("scrub").oninput = (e) => {
  playing = false; document.getElementById("play").textContent = "Play";
  const end = cur.frames[cur.frames.length - 1].t;
  tpos = end * e.target.value / 1000;
};
document.fonts.load("21px Mata").then(() => requestAnimationFrame(tick));
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", nargs="+", required=True,
                    help="name=path.json, one per scenario")
    ap.add_argument("--backdrop", default="")
    ap.add_argument("--variant", default="cool", choices=("cool", "gold"))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    data = {}
    for spec in a.frames:
        name, path = spec.split("=", 1)
        data[name] = json.loads(pathlib.Path(path).read_text())
    mark = header_bytes("source/gfx/jfmark_hd_png.h", "jfmark_hd_%s_png" % a.variant)
    font = header_bytes("source/gfx/mata_bold.h", "Mata_Bold_otf")
    back = "null"
    if a.backdrop:
        back = json.dumps("data:image/png;base64," +
                          base64.b64encode(pathlib.Path(a.backdrop).read_bytes()).decode())
    html = (PAGE.replace("__DATA__", json.dumps(data, separators=(",", ":")))
                .replace("__BACKDROP__", back)
                .replace("__MARK__", base64.b64encode(mark).decode())
                .replace("__FONT__", base64.b64encode(font).decode()))
    pathlib.Path(a.out).write_text(html, encoding="utf-8")
    print("wrote %s (%d KB)" % (a.out, len(html) // 1024))


if __name__ == "__main__":
    main()
