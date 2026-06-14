# UI preview renderer

Host-side renderer that mirrors the XMB draw code (palette, layout constants,
and the drawRect/drawTTF/drawIcon primitives from `source/ui/`) so UI changes
can be eyeballed as 1280x720 PNGs without deploying to a PS3.

```bash
gcc -O2 preview.c -lm -o preview
./preview                          # writes *.ppm frames
magick mogrify -format png *.ppm   # optional: convert to PNG
```

It renders mock data (fake posters, a fixed clock) — it is a design proof,
not an emulator. When you change colors or layout in `source/ui/ui_visuals.h`
or the render code, mirror the change here to preview it.
