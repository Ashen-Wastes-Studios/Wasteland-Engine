"""What IS the optimal correction, and can (inc, inp) determine it?

The head's whole job is to predict `gt - inc` from the arms. Before changing the
architecture again, measure how predictable that actually is:

  1. corr(gt - inc, inp - inc) per scene -- if the correction is largely "move
     toward the input", the head's job is a blend and the reason it fails is
     magnitude/direction, not features.
  2. The best GLOBAL scalar alpha in  gt ~= inc + alpha*(inp - inc), per scene.
     A single number per scene tells us how much of the correction is a blend.
  3. The per-pixel optimal alpha = (gt-inc)/(inp-inc) -- its spread says whether
     ONE global blend can be right, or whether the correction is genuinely
     pixel-dependent.
  4. The PSNR of the global-alpha blend vs the incumbent and the input. If the
     blend beats both, a per-pixel head has real headroom; if it does not, the
     head is being asked to do something a blend cannot, and the ceiling is low.

Read-only: loads the dataset, prints, writes nothing.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dataset as D


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    if mse <= 0.0:
        return 99.0
    return 10.0 * np.log10((255.0 * 255.0) / mse)


def main() -> int:
    ds = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("C:/tmp/dataset_wlrr_full")
    triples = D.load_triples(ds, allow_incomplete=True)
    if not triples:
        print("no triples")
        return 1

    print(f"{'scene':<34s} {'inc':>7s} {'inp':>7s} {'blend':>7s} "
          f"{'alpha*':>7s} {'corr':>7s} {'a_med':>7s} {'a_iqr':>7s}")
    print("-" * 96)

    scenes = {}
    for t in triples:
        inc, inp, gt = D.triple_arrays(t)
        scene = t.scene
        scenes.setdefault(scene, []).append((inc, inp, gt))

    rows = []
    for scene, items in sorted(scenes.items()):
        inc = np.concatenate([i for i, _, _ in items])
        inp = np.concatenate([j for _, j, _ in items])
        gt = np.concatenate([g for _, _, g in items])

        # subsample for speed
        n = inc.shape[0]
        if n > 400_000:
            idx = np.random.default_rng(0).choice(n, 400_000, replace=False)
            inc, inp, gt = inc[idx], inp[idx], gt[idx]

        incf = inc.astype(np.float64)
        inpf = inp.astype(np.float64)
        gtf = gt.astype(np.float64)

        err = gtf - incf            # the correction the head must produce
        delta = inpf - incf         # "move toward the input"

        # global optimal alpha, least squares: err ~ alpha * delta
        denom = float(np.sum(delta * delta))
        alpha = float(np.sum(err * delta) / denom) if denom > 0 else 0.0
        blend = incf + alpha * delta

        # per-pixel optimal alpha, only where the denominator is meaningful
        mask = np.abs(delta) > 1.0
        if mask.any():
            a_pix = err[mask] / delta[mask]
            a_med = float(np.median(a_pix))
            a_iqr = float(np.percentile(a_pix, 75) - np.percentile(a_pix, 25))
        else:
            a_med, a_iqr = 0.0, 0.0

        corr = float(np.corrcoef(err.ravel(), delta.ravel())[0, 1])

        p_inc, p_inp, p_blend = psnr(inc, gt), psnr(inp, gt), psnr(blend, gt)
        rows.append((scene, p_inc, p_inp, p_blend, alpha, corr, a_med, a_iqr))
        print(f"{scene:<34s} {p_inc:7.3f} {p_inp:7.3f} {p_blend:7.3f} "
              f"{alpha:7.3f} {corr:7.3f} {a_med:7.3f} {a_iqr:7.3f}")

    print()
    print("alpha*  = the global blend weight that best maps incumbent -> gt")
    print("corr    = corr(gt-inc, inp-inc); near +/-1 means the correction is a blend")
    print("a_med   = median per-pixel optimal alpha")
    print("a_iqr   = its inter-quartile range; large means one global alpha cannot fit")
    print()
    beats = sum(1 for r in rows if r[3] > max(r[1], r[2]))
    print(f"the global blend beats BOTH arms on {beats}/{len(rows)} scenes")
    print("If that number is low, a per-pixel head has little headroom over a blend,")
    print("and the ABI -- not the training -- is the limit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
