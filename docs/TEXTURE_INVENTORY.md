# Measured texture inventory — Wasteland

MEASURED from the actual files in `DemonCore-Editor/assets/textures` (header parse, not estimated).
Regenerate with the snippet at the bottom if assets change.

| texture | size | mip levels | full chain | above level 4 |
|---|---|---|---|---|
| `Checkerboard.png` | 8x8 | 4 | 0.3 KB | 0.000 KB |
| `Frame.png` | 64x64 | 7 | 21.3 KB | 0.020 KB |
| `metal_floor.jpg` | 225x225 | 8 | 263.1 KB | 0.230 KB |
| `wood_floor.jpg` | 225x225 | 8 | 263.1 KB | 0.230 KB |
| `wood_wall.jpg` | 447x447 | 9 | 1038.4 KB | 0.840 KB |

**Total chain 1.55 MB. Unreachable above level 4: 1.3 KB (0.0013 MB).**

## Why this file exists

The options map (`§5.6 correction 3`) claimed the `clamp(mipLevel, 0, 4)` in the Nova shader
left "~33 MB of unreachable mip data" resident in VRAM. That number was an ESTIMATE that
assumed 2048x2048 assets. This repo has no such textures — the largest is 447x447. The real
figure is ~1 KB.

So the clamp fix is still correct (the cap made the top of every chain unreachable, and the
HLSL twin's comment justified it as "a deliberate quality/performance limit" when it was an
accident), but it is **not** a memory win and must not be described as one. The shader
comments in both twins now carry these measured numbers.

Do not re-derive this from assumptions — re-measure. A future 2K/4K asset drop would change it.
