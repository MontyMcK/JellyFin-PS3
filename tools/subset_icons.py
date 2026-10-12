"""Re-subset Tabler Icons v3.44 for the PS3 app: the glyphs in source/ui/icons.h.

    python subset_icons.py <tabler-icons.ttf> <icons.h> <out tabler_icons.h>

Reads every `#define ICON_x 0xNNNN` from icons.h, subsets the TTF to exactly
those codepoints (no hinting, no layout tables: stb_truetype needs only glyf,
cmap, head, hhea, hmtx, maxp, loca), and writes the byte array the way the
existing header does.
"""
import io, re, sys
from fontTools import subset
from fontTools.ttLib import TTFont

ttf, icons_h, out_h = sys.argv[1:4]
cps = sorted({int(m.group(1), 16) for m in re.finditer(r"#define\s+ICON_\w+\s+0x([0-9A-Fa-f]+)", open(icons_h, encoding="utf-8").read())})
print("codepoints:", len(cps), " ".join("%04X" % c for c in cps))

opts = subset.Options()
opts.layout_features = []
opts.hinting = False
opts.notdef_outline = True
opts.name_IDs = [1, 2]
opts.drop_tables += ["GSUB", "GPOS", "GDEF", "DSIG", "FFTM", "post"]
opts.glyph_names = False
font = TTFont(ttf)
ss = subset.Subsetter(opts)
ss.populate(unicodes=cps)
ss.subset(font)
buf = io.BytesIO()
font.save(buf)
data = buf.getvalue()

# verify every wanted codepoint survived
chk = TTFont(io.BytesIO(data))
cmap = chk.getBestCmap()
missing = [c for c in cps if c not in cmap]
assert not missing, "missing: %s" % missing
print("tables:", sorted(chk.keys()), " bytes:", len(data), " glyphs:", chk["maxp"].numGlyphs)

lines = []
for i in range(0, len(data), 12):
    lines.append("  " + ",".join("0x%02x" % b for b in data[i:i + 12]) + ",")
with open(out_h, "w", encoding="utf-8", newline="\n") as f:
    f.write("// Subset of Tabler Icons v3.44 (MIT, https://tabler.io/icons) — %d glyphs.\n" % len(cps))
    f.write("// Regenerate: fontTools.subset on tabler-icons.ttf with the ICON_* unicodes\n")
    f.write("// from source/ui/icons.h, then bytes -> this array (tools/subset_icons.py).\n")
    f.write("unsigned char TablerIcons_ttf[] = {\n")
    f.write("\n".join(lines).rstrip(",") + "\n")
    f.write("};\n")
    f.write("unsigned int TablerIcons_ttf_len = %d;\n" % len(data))
print("wrote", out_h)
