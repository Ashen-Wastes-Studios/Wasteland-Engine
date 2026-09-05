"""Red/green probe for the Wood Wall Nova-POM warp + tile-seam artifact.

Cause evidence (analytic, no GPU needed):
  E1: march-span model mirrors ApplyPOM (Nova lines 899-909): total UV span of
      the height march in *texture tiles* for a wall pixel at incidence angle A.
      Span >> 1 tile  => the march samples random distant tiles, UV/swim warp.
  E2: wood_wall.jpg edge continuity: mean abs luminance gap across the
      top/bottom and left/right wrap borders. Large gap + GL_REPEAT =>
      hard full-width seams every tile (1 tile = 1 m on the wall).

Fix-state loop (must ALL hold for GREEN):
  F1: file-loading texture ctor uses GL_MIRRORED_REPEAT (seam-proof wrap).
  F2: Wood Wall DisplacementScale <= 0.05 in source AND deployed Release copy.
"""
import math
import re
import sys

from PIL import Image

# --- E1: march span (tiles) = tan(A) * uvScale * max(disp, bump); wall uvScale.x = 50
def march_span_tiles(disp, bump, angle_deg, uvscale=50.0):
    cos_a = math.cos(math.radians(angle_deg))
    view = math.sin(math.radians(angle_deg))
    off_per_h = view / max(abs(cos_a), 0.05)
    return off_per_h * uvscale * max(disp, bump)


print("E1 march span (tiles) at 20/45 deg incidence:")
for d, b in ((1.0, 1.0), (0.03, 1.0)):
    s20 = march_span_tiles(d, b, 20)
    s45 = march_span_tiles(d, b, 45)
    print(f"  disp={d:<4} bump={b}: 20deg={s20:6.2f} tiles  45deg={s45:6.2f} tiles")

# --- E2: wrap-border continuity of the photo texture
img = Image.open("DemonCore-Editor/assets/textures/wood_wall.jpg").convert("L")
w, h = img.size
px = img.load()
top = [px[x, 0] for x in range(w)]
bot = [px[x, h - 1] for x in range(w)]
lft = [px[0, y] for y in range(h)]
rgt = [px[w - 1, y] for y in range(h)]


def mad(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


print(f"E2 wood_wall.jpg {w}x{h}: top/bot border MAD={mad(top, bot):.1f} "
      f"left/right border MAD={mad(lft, rgt):.1f} (luminance 0-255)")

# --- F1/F2: fix-state loop
ok = True

src = open("Wasteland/src/Platform/OpenGL/OpenGLTexture.cpp").read()
ctor = src.split("const std::string &path)")[1].split(
    "OpenGLTexture2D::~")[0]
mirrored = "GL_MIRRORED_REPEAT" in ctor
print("F1 file-ctor wrap mirrored:", mirrored)
ok &= mirrored

for p in ("DemonCore-Editor/assets/scenes/Example3D.wastescene",
          "bin/Release-windows-x86_64/DemonCore-Editor/assets/scenes/"
          "Example3D.wastescene"):
    m = re.search(r"Tag: Wood Wall.*?DisplacementScale: ([0-9.]+)",
                  open(p).read(), re.S)
    v = float(m.group(1))
    print(f"F2 {p}: wall disp = {v}")
    ok &= (v <= 0.05)

print("LOOP:", "GREEN" if ok else "RED")
sys.exit(0 if ok else 1)
