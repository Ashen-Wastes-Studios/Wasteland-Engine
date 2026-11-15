"""Train the clean-room WLFG neural frame synthesis head.

numpy only. Adam and the backward pass are written out in `model.py`; nothing here
imports a framework.

WHAT IS BEING LEARNED
----------------------
The dataset pairs the warped (reprojected previous present) frame's RGB + the
reactive mask with the converged target frame. The network sees the warped RGB +
mask (+ velocity + depth, fed per pixel) and predicts a residual, so:

    out = anchor + gate * residual       anchor = warped frame

`gate` in [0,1] is learned per pixel. The loss prices deviation from the warped
anchor with `lam`, the "engine frame is unyielding" term as a number.

MIRRORED FROM tools/train_neural: the existing neural stage (render-stage
Denoise) trains on (RGB + luma + global). The WLFG head is the sibling of the
warp, not its successor: its per-pixel conditioning is the warp's own state
(mask, velocity, depth) so it can decide, per pixel, whether to trust the
warped frame or fall back to the real frame.

THE BAR
-------
The plan's Step 3 gate: this must beat the no-WLFG control (the raw warped
frame, PSNR vs the converged target). If it does not, it is not a feature -- we
report that and stop, rather than tuning the gate to hide it.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dataset as ds
import model as M
import test_model as T

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = HERE / "out" / "weights.npz"


def train(params, train_pairs, epochs, patch, patches_per_pair, lr, lam, seed,
          log_every=10, verbose=True):
    rng = np.random.default_rng(seed)
    opt = M.Adam(params, lr=lr)
    history = []
    t0 = time.time()
    for epoch in range(1, epochs + 1):
        opt.lr = M.cosine_lr(lr, epoch - 1, epochs)
        losses, mses, pens, nb = [], [], [], 0
        for (a, t, m, velocity, dep) in ds.iter_random_crops(train_pairs, patches_per_pair,
                                                            patch, rng):
            x = M.features(a, m, velocity, dep)
            loss, mse, pen, grads = M.loss_and_grads(params, x, t, lam, anchor_rgb=a)
            opt.step(params, grads)
            losses.append(loss); mses.append(mse); pens.append(pen); nb += 1
        rec = {
            "epoch": epoch, "loss": float(np.mean(losses)), "mse": float(np.mean(mses)),
            "anchor_pen": float(np.mean(pens)), "lr": float(opt.lr), "batches": nb,
            "elapsed_s": time.time() - t0,
        }
        history.append(rec)
        if verbose and (epoch % log_every == 0 or epoch == 1 or epoch == epochs):
            print(f"  epoch {epoch:>4}/{epochs}  loss {rec['loss']:.6f}  mse {rec['mse']:.6f}  "
                  f"anchor_pen {rec['anchor_pen']:.6f}  lr {rec['lr']:.2e}  {rec['elapsed_s']:6.1f}s")
    return history


def quick_psnr(params, pairs, n=20000, seed=7) -> float:
    """PSNR of the enhanced output on a random sample of the given pairs against
    the target using the same velocity and depth as training."""
    if not pairs:
        raise ValueError("quick PSNR requires at least one pair")
    rng = np.random.default_rng(seed)
    a_all, t_all, m_all, v_all, d_all = [], [], [], [], []
    per = max(1, n // len(pairs))
    for p in pairs:
        a, m, t, v, d = ds.pair_feature_arrays(p)
        h, w = a.shape[:2]
        ys = rng.integers(0, h, size=per)
        xs = rng.integers(0, w, size=per)
        a_all.append(a[ys, xs]); t_all.append(t[ys, xs]); m_all.append(m[ys, xs])
        v_all.append(v[ys, xs]); d_all.append(d[ys, xs])
    out, _ = T.predict(params, np.concatenate(a_all), np.concatenate(m_all),
                       np.concatenate(v_all), np.concatenate(d_all))
    return T.psnr(out, np.concatenate(t_all))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default=str(ds.DEFAULT_DATASET))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--patch", type=int, default=16)
    ap.add_argument("--patches-per-pair", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--lam", type=float, default=M.LAMBDA_ANCHOR,
                    help="anchor penalty weight: how strongly deviation from the warped frame is priced")
    ap.add_argument("--holdout", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261023)
    ap.add_argument("--log-every", type=int, default=10)
    ap.add_argument("--smoke", action="store_true",
                    help="3 epochs, 2 pairs, small patches -- verifies the loop end to end")
    args = ap.parse_args(argv)

    pairs = ds.load_pairs(args.dataset)
    train_pairs, hold = ds.split_pairs(pairs, holdout=args.holdout, seed=args.seed)
    print(f"dataset: {len(pairs)} pairs -> {len(train_pairs)} train / {len(hold)} held out")
    print(f"config: epochs={args.epochs} patch={args.patch} patches/pair={args.patches_per_pair} "
          f"lr={args.lr} lam={args.lam}")
    print(f"weights: {M.N_WEIGHTS} ({M.N_WEIGHTS * 4} bytes as fp32)")

    if args.smoke:
        args.epochs, args.patch, args.patches_per_pair, args.log_every = 3, 8, 8, 1
        train_pairs = train_pairs[:2]
        print(f"SMOKE: {args.epochs} epochs, {len(train_pairs)} pairs, patch {args.patch}")

    rng = np.random.default_rng(args.seed)
    params = M.init_params(rng)
    base = quick_psnr(params, train_pairs)
    print(f"baseline PSNR (identity network, i.e. the raw warped input) on train: {base:.3f} dB")
    print()

    t0 = time.time()
    history = train(params, train_pairs, args.epochs, args.patch,
                    args.patches_per_pair, args.lr, args.lam, args.seed,
                    log_every=args.log_every)
    wall = time.time() - t0

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez(out, **params)
    print(f"wrote {out}  ({out.stat().st_size} bytes)")

    meta = {
        "architecture": {"n_in": M.N_IN, "n_hidden": M.N_HIDDEN,
                         "n_residual": M.N_RESIDUAL, "n_gate": M.N_GATE},
        "n_weights": M.N_WEIGHTS,
        "layer_shapes": {k: list(v) for k, v in M.LAYER_SHAPES},
        "features": ["r", "g", "b", "mask", "vel_x", "vel_y", "depth"],
        "train": {"pairs": len(train_pairs), "holdout": len(hold),
                  "holdout_keys": [p.key for p in hold],
                  "epochs": args.epochs, "patch": args.patch,
                  "patches_per_pair": args.patches_per_pair, "lr": args.lr,
                  "lam": args.lam, "seed": args.seed, "wall_s": round(wall, 3),
                  "baseline_psnr_train": round(base, 4), "final_loss": history[-1]["loss"]},
        "dataset": {"root": str(Path(args.dataset).resolve()), "pairs": len(pairs)},
    }
    mp = out.with_suffix(".json")
    mp.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"wrote {mp}")

    print()
    print("=== held-out check (vs the raw warped input) ===")
    agg = T.evaluate(params, hold)
    print()
    print(f"  raw warped input   {agg['psnr_in']:.3f} dB   (baseline on train: {base:.3f})")
    print(f"  enhanced           {agg['psnr_out']:.3f} dB   ({agg['delta']:+.3f} dB)")
    print(f"  mean gate   {agg['gate_mean']:.4f}   fraction < {T.GATE_FLOOR}: {agg['gate_below']:.4f}")
    if agg["delta"] <= 0:
        print("\nFAIL: the head does not beat the raw warped input. It is not an enhancement.")
        return 1
    print(f"\nPASS: enhanced beats the raw warped input by {agg['delta']:+.3f} dB on the held-out split")
    print("A separate no-WLFG control is still required for plan acceptance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
