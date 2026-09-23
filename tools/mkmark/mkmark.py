#!/usr/bin/env python3
"""Rasterise the design's lockup mark into source/gfx/jfmark_png.h.

The mark in the design bundle's JFChrome is an SVG: a bell path filled with
the theme's ramp, a bevel gradient and an edge stroke over it, an inner shadow
under it, and the PS3 mark PNG clipped into the hole.  The PS3 cannot draw any
of that, and approximating it by hand at 21px looks like an approximation, so
it is rendered once by a real SVG engine and embedded as pixels.

There is no SVG renderer in this toolchain, so the renderer used is a browser:
this script builds a page that draws the SVG into a canvas and POSTs the PNG
back, serves it, and waits.  Open the URL it prints in any browser and the
files land by themselves.

    python3 tools/mkmark/mkmark.py --bundle ../design-import

  --bundle   the design import directory (holds JFChrome.dc.html and assets/).
             assets/ may live in a sibling import -- pass --assets for that.
  --out      where to write the header (default source/gfx/jfmark_png.h)
  --port     local port for the one-shot server (default 8732)

Two variants come out, because the ramp is baked into the pixels: `cool` is the
XMB wave theme's AA5CC3 -> 00A4DC with the soft PS mark, `gold` is Golden Age's
E8B45C -> C8742E with the gold one.  ui_widgets.cpp picks between them by the
warmth of the loaded theme's lk_mark_a.

To recover the PNGs from the header without re-rendering:
    sed -n '/jfmark_cool_png\\[\\]/,/};/p' source/gfx/jfmark_png.h | xxd -r -p
"""
import argparse, base64, http.server, pathlib, re, sys, threading, urllib.parse

# The mark is authored in a 72x72 viewBox and drawn at 21px.  Its drop shadow
# falls outside that box, so the raster covers -8..80 -- 88 units in an 80px
# square -- and the client scales and offsets by the ratios below.
PAD, SPAN, MASTER = 8, 88, 80

VARIANTS = {
    #          bell ramp stop 1, stop 2, cool logo opacity, gold logo opacity
    "cool": ("#aa5cc3", "#00a4dc", 1, 0),
    "gold": ("#e8b45c", "#c8742e", 0, 1),
}

HEADER_DOC = """\
// Brand lockup mark -- the design's SVG, rasterised.
//
// JFChrome's mark is not a glyph and not a flat image: it is a bell path
// filled with the theme's mark ramp, a bevel gradient and an edge stroke over
// it, an inner shadow under it, and the PS3 mark PNG clipped into the hole.
// Nothing here can draw that, and hand-approximating it at 21px would look
// like an approximation, so it is rendered once by a real SVG engine and
// embedded as pixels.  tools/mkmark/mkmark.py is that step, and it reads the
// design bundle directly -- re-run it if the mark is revised.
//
// TWO VARIANTS, because the ramp is baked in: `cool` is the XMB wave theme's
// AA5CC3 -> 00A4DC with the soft PS mark, `gold` is Golden Age's E8B45C ->
// C8742E with the gold one.  ui_widgets.cpp picks between them by the hue of
// the loaded theme's own lk_mark_a, so a hand-written .ini gets whichever of
// the two its mark colour is nearer -- it does not get its own ramp baked.
// That is the one place the lockup is not fully themeable, and it is a
// deliberate trade against a per-pixel gradient composite every frame.
//
// GEOMETRY: the design authors the mark in a 72x72 viewBox and draws it at
// 21px.  Its drop shadow falls outside that box, so this raster covers
// -8..80 -- 88 units in an 80px square, 1.111 px per unit.  A caller that
// wants the BELL 21px wide therefore draws the whole image 21 * 88/72 wide
// and starts it 21 * 8/72 to the left and above.  JFMARK_* below carry those
// numbers so no call site has to restate them.
//
// Decoded at runtime by ui_widgets.cpp, like ps_buttons_png.h.

#define JFMARK_MASTER   %d   // the raster is JFMARK_MASTER square
#define JFMARK_SPAN_U   %d   // ... covering this many viewBox units
#define JFMARK_BELL_U   %d   // ... of which the bell itself is this many
#define JFMARK_PAD_U     %d   // ... inset equally on every side

"""


def build_svgs(bundle: pathlib.Path, assets: pathlib.Path) -> dict:
    html = (bundle / "JFChrome.dc.html").read_text(encoding="utf-8", errors="replace")
    i = html.find("jfps3-mark")
    if i < 0:
        sys.exit("no jfps3-mark reference in JFChrome.dc.html -- wrong bundle?")
    svg = html[html.rfind("<svg", 0, i):html.find("</svg>", i) + len("</svg>")]

    # Parsed standalone as an image, the fragment needs its namespace declared.
    svg = svg.replace("<svg ", '<svg xmlns="http://www.w3.org/2000/svg" ', 1)

    # Concrete ids for the bundle's {{ }} placeholders.
    for a, b in (("{{ mkId }}", "mk"), ("{{ mkClip }}", "mkclip"),
                 ("{{ mkBlur }}", "mkblur"), ("{{ mkClipUrl }}", "url(#mkclip)"),
                 ("{{ mkUrl }}", "url(#mk)"), ("{{ mkBlurUrl }}", "url(#mkblur)")):
        svg = svg.replace(a, b)

    # An SVG rendered as an image cannot fetch anything, so the marks go inline.
    for name in ("jfps3-mark-soft-2x.png", "jfps3-mark-gold-2x.png"):
        b64 = base64.b64encode((assets / name).read_bytes()).decode()
        svg = svg.replace('href="assets/%s"' % name,
                          'href="data:image/png;base64,%s"' % b64)

    svg = re.sub(r'viewBox="0 0 72 72"',
                 'viewBox="%d %d %d %d"' % (-PAD, -PAD, SPAN, SPAN), svg, count=1)
    svg = re.sub(r'width="21" height="21"',
                 'width="%d" height="%d"' % (MASTER, MASTER), svg, count=1)
    # The root filter is a CSS var with a drop-shadow default; keep the default
    # and drop the var, which does not resolve outside the component.
    svg = svg.replace("var(--lk-mkglow,", "(").replace(
        "filter:(drop-shadow", "filter:drop-shadow", 1)
    svg = re.sub(r"\)\);\"><defs>", ");\"><defs>", svg, count=1)

    out = {}
    for name, (mk1, mk2, cool, gold) in VARIANTS.items():
        out[name] = (svg.replace("var(--jf-mk1,#aa5cc3)", mk1)
                        .replace("var(--jf-mk2,#00a4dc)", mk2)
                        .replace("var(--lk-cool,1)", str(cool))
                        .replace("var(--lk-gold,0)", str(gold)))
    return out


def page(svgs: dict) -> str:
    def js(s):
        return "`" + s.replace("\\", "\\\\").replace("`", "\\`").replace("$", "\\$") + "`"
    return """<!DOCTYPE html>
<meta charset="utf-8"><title>mark raster</title>
<body style="background:#222;color:#eee;font:14px monospace">
<div id="out">rendering...</div>
<script>
const SVGS = {%s};
const MASTER = %d;
async function raster(svgText) {
  const img = new Image();
  img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svgText)));
  await img.decode();
  const c = document.createElement("canvas");
  c.width = c.height = MASTER;
  c.getContext("2d").drawImage(img, 0, 0, MASTER, MASTER);
  document.body.appendChild(c);
  return c.toDataURL("image/png");
}
(async () => {
  for (const [k, v] of Object.entries(SVGS))
    await fetch("/save?name=" + k, {method: "POST", body: await raster(v)});
  document.getElementById("out").textContent = "done -- you can close this tab";
})();
</script>
""" % (",".join('"%s": %s' % (k, js(v)) for k, v in svgs.items()), MASTER)


def serve_and_collect(html: str, port: int, want: set) -> dict:
    got, done = {}, threading.Event()

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            name = urllib.parse.parse_qs(
                urllib.parse.urlparse(self.path).query).get("name", [""])[0]
            data = self.rfile.read(int(self.headers["Content-Length"])).decode()
            got[name] = base64.b64decode(data.split(",", 1)[-1])
            self.send_response(200); self.send_header("Content-Length", "2")
            self.end_headers(); self.wfile.write(b"ok")
            if want <= set(got): done.set()

        def log_message(self, *a): pass

    srv = http.server.HTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("open http://127.0.0.1:%d/ in a browser -- waiting..." % port)
    if not done.wait(300):
        srv.shutdown(); sys.exit("timed out waiting for the browser")
    srv.shutdown()
    return got


def c_array(name: str, data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 12):
        rows.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    body = "\n".join(rows).rstrip(",")
    return ("unsigned char %s[] = {\n%s\n};\nunsigned int %s_len = %d;\n"
            % (name, body, name, len(data)))


def main():
    ap = argparse.ArgumentParser()
    here = pathlib.Path(__file__).resolve().parent
    ap.add_argument("--bundle", default=str(here.parents[1].parent / "design-import-v1"))
    ap.add_argument("--assets", default="")
    ap.add_argument("--out", default=str(here.parents[1] / "source/gfx/jfmark_png.h"))
    ap.add_argument("--port", type=int, default=8732)
    a = ap.parse_args()

    bundle = pathlib.Path(a.bundle)
    # The PS3 mark PNGs and the fonts are not re-exported every revision, so
    # assets/ can legitimately be in an older import next door.
    assets = pathlib.Path(a.assets) if a.assets else bundle / "assets"
    if not (assets / "jfps3-mark-soft-2x.png").exists():
        alt = bundle.parent / "design-import" / "assets"
        if (alt / "jfps3-mark-soft-2x.png").exists():
            assets = alt
        else:
            sys.exit("no jfps3-mark-soft-2x.png under %s -- pass --assets" % assets)

    svgs = build_svgs(bundle, assets)
    pngs = serve_and_collect(page(svgs), a.port, set(svgs))

    text = HEADER_DOC % (MASTER, SPAN, 72, PAD)
    for name in VARIANTS:
        text += c_array("jfmark_%s_png" % name, pngs[name])
    pathlib.Path(a.out).write_text(text, encoding="utf-8")
    print("wrote %s (%d bytes)" % (a.out, len(text)))


if __name__ == "__main__":
    main()
