"""Diagnose the WLRR negative result: is the global affine the cause?

The probe (one scene, Example3D) measured feature std 2.15-6.16. The full 11-scene
set measures 13.1-32.2. If the spread is BETWEEN scenes rather than within them,
then standardising by the global statistics leaves within-scene variation tiny,
and the head is being asked to correct a scene whose features all look the same.

Read-only. No training, no GPU.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, r"C:\dev\Wasteland\tools\train_wlrr")

D = Path(r"C:\tmp\dataset_wlrr_full")
man = json.loads((D / "manifest.json").read_text())

# group frames by (scene, camera)
by_key = {}
for f in man["frames"]:
    by_key.setdefault((f["scene"], f["camera"]["name"]), {})[f["pass_name"]] = f

rng = np.random.default_rng(0)
ARMS = ["wlrr_incumbent", "wlrr_input"]   # the model's 6 features, in order

rows = []
for (scene, cam), arms in sorted(by_key.items()):
    if not all(a in arms for a in ARMS):
        continue
    vals = []
    for arm in ARMS:
        p = D / arms[arm]["path"]
        with Image.open(p) as im:
            a = np.asarray(im.convert("RGB"), dtype=np.float32)
        h, w = a.shape[:2]
        ys = rng.integers(0, h, 4000)
        xs = rng.integers(0, w, 4000)
        vals.append(a[ys, xs])
    block = np.concatenate(vals, axis=1)          # (4000, 6)
    rows.append((scene.split("/")[-1].replace(".wastescene", ""), cam, block))

allb = np.concatenate([b for _, _, b in rows], axis=0)
gmean, gstd = allb.mean(0), allb.std(0)

print(f"{len(rows)} (scene,camera) pairs, {allb.shape[0]} pixels each")
print(f"\nGLOBAL mean {np.round(gmean, 2).tolist()}")
print(f"GLOBAL std  {np.round(gstd, 2).tolist()}")

# Per-scene statistics: how much does each scene's own distribution differ?
print(f"\n{'scene':22s} {'cam':9s} {'scene mean[0:3]':>26s} {'scene std[0:3]':>26s}")
per_scene_std = []
for scene, cam, b in rows:
    if cam != "auto_y0":
        continue
    smean, sstd = b.mean(0), b.std(0)
    per_scene_std.append(sstd)
    print(f"{scene:22s} {cam:9s} {str(np.round(smean[:3], 1)):>26s} {str(np.round(sstd[:3], 1)):>26s}")

per_scene_std = np.array(per_scene_std)
print(f"\nmean WITHIN-scene std (feature 0..2): {np.round(per_scene_std[:, :3].mean(0), 2).tolist()}")
print(f"mean BETWEEN-scene std of the scene means: "
      f"{np.round(np.array([b[b[:, 0].argsort()] for _, _, b in rows if True]).mean(0).std(0)[:3], 2).tolist()}")

# THE TEST. After the global affine, how much signal is left WITHIN a scene?
# If within-scene spread collapses relative to between-scene spread, the head is
# being handed near-constant features for any single scene.
within = per_scene_std[:, :3].mean(0)
between = np.array([b.mean(0)[:3] for _, _, b in rows]).std(0)
print(f"\nwithin-scene std (avg)  : {np.round(within, 2).tolist()}")
print(f"between-scene std (avg) : {np.round(between, 2).tolist()}")
print(f"ratio within/between    : {np.round(within / between, 3).tolist()}")

# How separated are the two arms, per scene, in the 0..255 domain?
print(f"\n{'scene':22s} {'cam':9s} {'mean|inc-input|':>16s}")
for scene, cam, b in rows:
    if cam != "auto_y0":
        continue
    d = np.abs(b[:, :3] - b[:, 3:]).mean()
    print(f"{scene:22s} {cam:9s} {d:16.4f}")
