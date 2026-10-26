"""Regenerate `dataset_stats.json` -- the network's input standardisation constants.

    python tools/train_neural/dataset_stats.py

These constants are PART OF THE ABI. `model.features()` standardises its input
with them before the first matmul, so the shader must apply the same affine or
every weight is wrong.

Why they exist: without standardisation the layer-1 pre-activation has a std of
0.0118, half the ReLU units die at initialisation, the signal attenuates to
nothing across four layers, and training converges to `out = anchor + constant` --
a single number worth +2.4 dB, with plausible-looking per-pair PSNRs. The model
is a constant and nothing in an accuracy metric says so.

The script refuses to write constants that disagree with the ones compiled into
`model.py`, so a stale `dataset_stats.json` cannot silently desync from the code.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dataset as ds
import model as M

HERE = Path(__file__).resolve().parent
OUT = HERE / "dataset_stats.json"
SEED = 5
PIXELS_PER_PAIR = 20000
TOLERANCE = 5e-5


def measure(pairs, seed: int = SEED, per_pair: int = PIXELS_PER_PAIR):
    """Per-feature mean and std over the given pairs, in raw (unstandardised) units."""
    rng = np.random.default_rng(seed)
    acc = []
    for p in pairs:
        a, _ = ds.pair_arrays(p)
        h, w = a.shape[:2]
        ys = rng.integers(0, h, size=per_pair)
        xs = rng.integers(0, w, size=per_pair)
        rgb = a[ys, xs]
        g = M.frame_global(a)
        raw = np.concatenate([rgb, M.luma(rgb)[:, None],
                              np.broadcast_to(g, (len(rgb), M.N_GLOB))], axis=1)
        acc.append(raw.astype(np.float64))
    X = np.concatenate(acc)
    if X.shape[1] != M.N_IN:
        raise ValueError(f"measured {X.shape[1]} features, model expects {M.N_IN}")
    return X.mean(axis=0), X.std(axis=0), X.shape[0]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default=str(ds.DEFAULT_DATASET))
    ap.add_argument("--holdout", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261023)
    ap.add_argument("--check", action="store_true",
                    help="compare against model.py and exit without writing")
    args = ap.parse_args(argv)

    pairs = ds.load_pairs(args.dataset)
    train, hold = ds.split_pairs(pairs, holdout=args.holdout, seed=args.seed)
    mu, sd, n = measure(train)

    print(f"measured on {len(train)} training pairs, {n} pixels "
          f"({args.holdout} pairs held out)")
    for name, m, s in zip(["r", "g", "b", "luma", "mean_r", "mean_g", "mean_b", "mean_luma"], mu, sd):
        print(f"  {name:8} mean {m:.6f}  std {s:.6f}")

    ok_mean = bool(np.allclose(mu, M.FEATURE_MEAN, atol=TOLERANCE))
    ok_std = bool(np.allclose(sd, M.FEATURE_STD, atol=TOLERANCE))
    print()
    print(f"agrees with model.FEATURE_MEAN: {ok_mean}")
    print(f"agrees with model.FEATURE_STD : {ok_std}")

    if args.check:
        if not (ok_mean and ok_std):
            print("\nFAIL: dataset_stats.json would desync from model.py -- rerun without --check")
            return 1
        print("\nPASS: constants agree")
        return 0

    if not (ok_mean and ok_std):
        print("\nrefusing to write: the measured constants disagree with model.py.")
        print("Update FEATURE_MEAN/FEATURE_STD in model.py first, then rerun.")
        return 1

    doc = {
        "description": ("Per-feature standardisation constants for the neural rendering "
                        "network. Part of the ABI: the shader must apply the same affine "
                        "before the first matmul. Regenerate with "
                        "`python tools/train_neural/dataset_stats.py`."),
        "features": ["r", "g", "b", "luma", "mean_r", "mean_g", "mean_b", "mean_luma"],
        "mean": [round(float(v), 6) for v in mu],
        "std": [round(float(v), 6) for v in sd],
        "measured_on": {"train_pairs": len(train), "holdout_pairs": len(hold),
                        "holdout_keys": [p.key for p in hold],
                        "samples": int(n), "pixels_per_pair": PIXELS_PER_PAIR,
                        "seed": SEED},
        "why": ("Without standardisation the layer-1 pre-activation std is 0.0118, half "
                "the ReLU units die at init, and training converges to "
                "`out = anchor + constant` -- a single number worth +2.4 dB that is "
                "indistinguishable from a working model by any accuracy metric."),
    }
    OUT.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    print(f"\nwrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
