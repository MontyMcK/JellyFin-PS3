#!/bin/bash
# Renders the Settings screen (the real drawing code) to PPM/PNG frames here.
set -e
cd "$(dirname "$0")"
python3 - <<'PY'
src = open("../../source/ui/render/ui_settings.cpp", encoding="utf-8").read()
a = src.index("// -------------------------------------------------------------------------\n//  The screen\n")
b = src.index("// -------------------------------------------------------\n// Overscan calibration screen")
open("settings_block.inc", "w", encoding="utf-8").write(src[a:b])
PY
g++ -O2 -w -fpermissive -I../../source/ui -I../../source/i18n -I../../source/net \
    -o preview_settings preview_settings.cpp \
    ../../source/ui/settings_model.c ../../source/i18n/i18n.c ../../source/i18n/i18n_tables.c -lm
./preview_settings
python3 - <<'PY'
import glob, struct, zlib
for f in sorted(glob.glob("settings_*.ppm")):
    d = open(f, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(f.replace(".ppm", ".png"), "wb").write(png)
PY
rm -f settings_*.ppm
ls settings_*.png
