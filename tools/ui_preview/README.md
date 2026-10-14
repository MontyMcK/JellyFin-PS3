# UI preview renderer

Host-side renderer that mirrors the XMB draw code (palette, layout constants,
and the drawRect/drawTTF/drawIcon primitives from `source/ui/`) so UI changes
can be eyeballed as 1280x720 PNGs without deploying to a PS3.

```bash
gcc -O2 preview.c -lm -o preview
./preview                          # writes *.ppm frames
magick mogrify -format png *.ppm   # optional: convert to PNG
```

The offline-download screens (frames 8–16: Downloads list, Offline library,
the item page's Download button, the delete confirm) have their own entry
point, which draws from the real `source/offline/dl_ui` model rather than
copies of its strings:

```bash
g++ -O2 -w -fpermissive -I. -I../../source/offline preview_offline.cpp \
    ../../source/offline/dl_ui.cpp ../../source/offline/dl_model.cpp \
    -lm -o preview_offline
./preview_offline                  # writes 8_*.ppm .. 16_*.ppm
```

It renders mock data (fake posters, a fixed clock) — it is a design proof,
not an emulator. When you change colors or layout in `source/ui/ui_visuals.h`
or the render code, mirror the change here to preview it.
