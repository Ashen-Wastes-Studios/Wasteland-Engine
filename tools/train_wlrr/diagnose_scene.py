"""Is a scene's WLRR arm an outlier, or is the head failing on it?

ScriptTest's incumbent scores 24.236 dB against gt while the next-worst of eleven
scenes is 35.986 -- 11.7 dB lower. Before calling that a model failure, check
whether the ARMS themselves are degenerate: a capture where the incumbent
destroyed the frame (or where the arms are near-constant) is a data defect, and
no head can be asked to correct it.

Reports per scene: the arms' own statistics (mean, std, and the fraction of
pixels that are exactly constant), the separation, and each arm's PSNR.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dataset as D


def psnr(a, b):
    d = a.astype(np.float64) - b.astype(np.float64)
    m = float(np.mean(d * d))
    return 99.0 if m <= 0 else 10.0 * np.log10(255.0 * 255.0 / m)


def main() -> int:
    ds = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("C:/tmp/dataset_wlrr_full")
    triples = D.load_triples(ds, allow_incomplete=True)
    scenes = {}
    for t in triples:
        inc, inp, gt = D.triple_arrays(t)
        scenes.setdefault(t.scene, []).append((inc, inp, gt))

    print(f"{'scene':<34s} {'incPSNR':>8s} {'inpPSNR':>8s} {'sep':>7s} "
          f"{'incStd':>7s} {'inpStd':>7s} {'gtStd':>7s} {'flat%':>6s}")
    print("-" * 92)
    for scene, items in sorted(scenes.items()):
        inc = np.concatenate([i for i, _, _ in items])
        inp = np.concatenate([j for _, j, _ in items])
        gt = np.concatenate([g for _, _, g in items])
        n = inc.shape[0]
        if n > 200_000:
            idx = np.random.default_rng(0).choice(n, 200_000, replace=False)
            inc, inp, gt = inc[idx], inp[idx], gt[idx]
        sep = float(np.mean(np.abs(inc.astype(np.float64) - inp.astype(np.float64))))
        # "flat" = pixels where all three arms agree to within 1 LSB: no signal
        flat = float(np.mean((np.abs(inc.astype(np.float64) - gt.astype(np.float64)) < 1.0)
                             & (np.abs(inp.astype(np.float64) - gt.astype(np.float64)) < 1.0)))
        print(f"{scene:<34s} {psnr(inc, gt):8.3f} {psnr(inp, gt):8.3f} {sep:7.3f} "
              f"{inc.std():7.2f} {inp.std():7.2f} {gt.std():7.2f} {flat * 100:6.2f}")
    print()
    print("sep    = mean |incumbent - input| over the scene")
    print("flat%  = fraction of pixels where BOTH arms are within 1 LSB of gt")
    print()
    print("A scene whose incumbent PSNR is far below the rest, or whose arms are")
    print("near-constant, is a CAPTURE DEFECT and should be excluded -- training a")
    print("head to 'correct' it teaches the head to invent detail.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
