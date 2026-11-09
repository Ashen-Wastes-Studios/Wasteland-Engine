"""Runtime Step 5 table: the trained WLRR head vs the dataset arms, per scene.

The dataset (dataset_wlrr_v3) supplies the INPUT and INCUMBENT arms plus the
converged GT, all from one zero-blob run per scene/camera. This script captures
the HEAD at the same camera and the CONVERGED frame with the trained blob and
reports each arm's PSNR against GT:

    input     dataset arm  (pre-SVGF snapshot, image 10)
    incumbent dataset arm  (post-SVGF accumulation, image 1)
    head      RUNTIME capture (WL_WLRR=1 + trained blob, image 1)

The head number is the only one that comes from the live engine. If the head
does not beat the incumbent on a scene, the correction the model learned offline
is not reaching the screen there -- which is what this table exists to show.

Usage:
    python tools/train_wlrr/runtime_table.py --dataset C:/tmp/dataset_wlrr_v3 \
        --trained C:/tmp/wlrr_v7.bin
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
# train_wlrr MUST be searched before train_neural: both trees carry a
# `dataset.py` and a `model.py`, and the neural ones silently shadow the WLRR
# ones (no N_PAYLOAD / no load_triples) -- the same trap verify_wlrr_runtime.py
# already documents. Import the WLRR modules while its directory is first.
sys.path.insert(0, str(HERE))
import dataset as D  # noqa: E402

sys.path.insert(0, str(HERE.parent / "train_neural"))  # export_dataset
import export_dataset as E  # noqa: E402


def psnr(a: np.ndarray, b: np.float64 | np.ndarray) -> float:
    a = a.astype(np.float64)
    b = np.asarray(b, dtype=np.float64)
    mse = float(np.mean((a - b) ** 2))
    if mse <= 0:
        return float("inf")
    return 10.0 * np.log10((255.0 ** 2) / mse)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default="C:/tmp/dataset_wlrr_v3")
    ap.add_argument("--trained", default="C:/tmp/wlrr_v7.bin")
    ap.add_argument("--out", default="C:/tmp/wlrr_runtime")
    args = ap.parse_args()

    ds = Path(args.dataset)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    triples = D.load_triples(ds, allow_incomplete=True)
    if not triples:
        print("error: no triples in the dataset", file=sys.stderr)
        return 2

    by_scene: dict[str, list] = {}
    for t in triples:
        by_scene.setdefault(t.scene, []).append(t)

    trained = Path(args.trained)
    if not trained.is_file():
        print(f"error: trained blob not found: {trained}", file=sys.stderr)
        return 2

    print(f"{'scene':<24s} {'input':>8s} {'incumbent':>9s} {'head':>8s} {'d(head-inc)':>11s}")
    print("-" * 64)

    rows = []
    for scene, items in sorted(by_scene.items()):
        # Dataset arms for the FIRST camera of this scene (the runtime capture
        # must match it exactly -- same camera, same converged frame).
        t = items[0]
        inc, inp, gt = D.triple_arrays(t)

        # Runtime head capture: same scene/camera, the ARMS' frame (PARTIAL_FRAME),
        # trained blob. The arms are frame-10 reads and gt is the converged frame
        # 400; capturing the head at 400 would compare a corrected converged frame
        # against a perfect incumbent (infinite PSNR) and report a loss that is
        # really "the head is not a no-op on an already-converged frame".
        cams = E.derive_cameras(scene)
        cam = next((c for c in cams if c[0] == t.camera), None)
        if cam is None:
            print(f"{scene:<24s}  (no camera {t.camera!r}; have {[c[0] for c in cams]})")
            continue
        png = out / f"{scene}__{t.camera}__head.png"
        os.environ["WL_WLRR"] = "1"
        os.environ["WL_WLRR_WEIGHTS_FORCE"] = str(trained)
        os.environ["WL_SCREENSHOT_FRAME_FORCE"] = str(E.PARTIAL_FRAME)
        os.environ["WL_NOVA_CAPTURE_IMAGE_FORCE"] = "1"
        E.capture(scene, cam, "wlrr_incumbent", png, timeout=240)
        if not png.exists():
            print(f"{scene:<24s}  (head capture failed)")
            continue
        from PIL import Image
        head = np.asarray(Image.open(png).convert("RGB"), dtype=np.float64)

        # The dataset arms are 8-bit PNGs of the same buffer; the runtime head
        # is the same readback path. Compare like for like.
        p_in = psnr(inp, gt)
        p_inc = psnr(inc, gt)
        p_head = psnr(head, gt)
        print(f"{scene:<24s} {p_in:8.3f} {p_inc:9.3f} {p_head:8.3f} {p_head - p_inc:+11.3f}")
        rows.append((scene, p_in, p_inc, p_head))

    print()
    if rows:
        wins = sum(1 for _, _, p_inc, p_head in rows if p_head > p_inc)
        print(f"head beats incumbent on {wins}/{len(rows)} scenes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
