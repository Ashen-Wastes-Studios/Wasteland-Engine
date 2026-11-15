"""Task 8 acceptance harness for the clean-room WLFG neural frame-synthesis head.

Plays the plan's Step 3/6 gates over a dataset without re-deriving any of them:
it validates provenance, checks pair alignment, and reports whether a model beats
the raw-warped control on the held-out factor. It does not train -- train.py is
the trainer; this only says whether to proceed.

Gates
-----
  1. provenance   every pair's manifest must carry verified temporal-training
                  fields (data_contract.require_training_manifest) -- fail-closed.
  2. alignment    warped / mask / target share pixels; velocity & depth are
                  lossless float32 at the anchor's exact shape.
  3. control      the identity model (raw warped input) PSNR vs the target is
                  printed as the baseline the head must beat.
  4. holdout      held-out factor PSNR/SSIM is reported, never used for tuning.
  5. model        a saved weights file (if given) is loaded and evaluated.

Exit 0 only when provenance, alignment and (with --weights) the model all pass.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dataset as ds
import test_model as T
import train as Tr
import model as M

HERE = Path(__file__).resolve().parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dataset", type=Path, help="dataset root with manifest.json")
    ap.add_argument("--weights", type=Path, default=None,
                    help="optional trained weights.npz to evaluate")
    ap.add_argument("--control-dataset", type=Path, default=None,
                    help="WL_WLFG=0 export of the same scene/camera/factors: the "
                         "engine's no-WLFG output the head must beat (plan Step 3)")
    ap.add_argument("--holdout", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261023)
    # evaluate() samples 64x64 patches, not whole images: the default 20000 budget is
    # 4 patches on a 4-pair holdout, which is far too few to separate two arms that
    # differ on ~10% of pixels. At that budget the raw warp and the no-WLFG control
    # both reported 22.2177 dB / 0.048217 SSIM -- identical to every printed digit --
    # which reads as "the control is the warp". A budget that converges on the full
    # image separates them (21.09 vs 21.00 dB). Acceptance numbers must be computed
    # at a budget that can actually distinguish the arms.
    ap.add_argument("--eval-samples", type=int, default=8_000_000,
                    help="pixel budget per evaluate() call; the default converges on "
                         "the full held-out image rather than a handful of patches")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    man = ds.load_manifest(args.dataset)
    ds.require_training_manifest(man)
    frames = man.get("frames", [])
    print(f"PROVENANCE ok  ({len(frames)} frames)")

    try:
        pairs = ds.load_pairs(args.dataset)
    except Exception as exc:  # noqa: BLE001
        print(f"ALIGNMENT fail: {exc}")
        return 1
    print(f"PAIRS ok       ({len(pairs)} complete pairs)")
    if args.holdout not in {p.factor for p in pairs}:
        print(f"holdout factor {args.holdout} not present; holding out {pairs[-1].factor}")
        args.holdout = pairs[-1].factor

    train_pairs, hold = ds.split_pairs(pairs, holdout=args.holdout, seed=args.seed)
    print(f"SPLIT  train={len(train_pairs)} hold={len(hold)}  held-out factor {hold[0].factor if hold else 'none'}")

    for p in hold:
        a, m, t, v, d = ds.pair_feature_arrays(p)
        h, w = a.shape[:2]
        if v.shape != (h, w, 2) or d.shape != (h, w, 1):
            print(f"ALIGNMENT fail {p.key}: state shapes {v.shape} / {d.shape} != RGB {h}x{w}")
            return 1
        if not (np.isfinite(v).all() and np.isfinite(d).all()):
            print(f"ALIGNMENT fail {p.key}: state is not finite")
            return 1
        # Velocity must be a real signed displacement, not a constant zero. A
        # static pin produces an all-zero field and an all-zero mask, and a
        # dataset gathered that way has no reprojection problem in it at all --
        # it would train a head that only ever learns the identity. This is a
        # FAILURE, not a warning: the whole point of the contract is that a
        # dataset which cannot teach the task is refused rather than scored.
        vx = v[..., 0]
        if float(np.std(vx)) < 1e-5 or float(np.abs(vx.mean())) < 1e-5:
            print(f"ALIGNMENT fail {p.key}: velocity has no observable displacement "
                  f"(std={float(np.std(vx)):.3g}, mean={float(np.abs(vx.mean())):.3g}); "
                  "the camera was static -- was WASTELAND_PIN_CAMERA_ORBIT set?")
            return 1
        # The reactive mask must carry signal, for the same reason.
        if float(np.std(m)) < 1e-5:
            print(f"ALIGNMENT fail {p.key}: reactive mask is constant "
                  f"(std={float(np.std(m)):.3g}); no disocclusion was ever detected")
            return 1
    print("ALIGNMENT ok   warped/mask/target share pixels; velocity & depth finite & aligned")

    # Identity baseline: a zero-initialised head reproduces the warped input,
    # so this is the engine's "no frame synthesis" reference line.
    _identity = {name: np.zeros(shape, dtype=np.float32) for name, shape in M.LAYER_SHAPES}
    identity_loss = Tr.quick_psnr(_identity, train_pairs)
    print(f"CONTROL        identity-model PSNR on train: {identity_loss:.3f} dB")

    if args.weights is None or not args.weights.is_file():
        # No head to score. Baseline recorded; the head still has to beat it.
        print("MODEL          no weights given; identity baseline recorded")
        return 0

    params = T.load_weights(args.weights)
    agg = T.evaluate(params, hold, n=args.eval_samples)
    print(f"HOLDOUT {hold[0].factor}  raw warped {agg['psnr_in']:.3f} dB" if hold else "HOLDOUT      no hold set")
    print(f"  enhanced  {agg['psnr_out']:.3f} dB   ({agg['delta']:+.3f} dB)")
    print(f"  SSIM     raw {agg['ssim_in']:.4f} / enhanced {agg['ssim_out']:.4f}")
    print(f"  gate     mean {agg['gate_mean']:.4f}  < floor {T.GATE_FLOOR}: {agg['gate_below']:.4f}")
    ok = agg["delta"] > 0
    print(f"MODEL {'PASS' if ok else 'FAIL'}  head beats the raw-warp control on held-out {hold[0].factor if hold else '?'}")

    # Plan Step 3: the head has to beat the NO-WLFG CONTROL, which is a different
    # and harder bar than the warped input it was handed. The warped input is the
    # head's own anchor, so beating it only shows the head is not a no-op. The
    # control is what the engine actually produced with pass 22 off -- with
    # WL_WLFG=0, img_Accumulation holds the plain temporal blend and no
    # reprojection happens at all. If the head cannot beat that, the plan says
    # report and stop rather than tune the gate to hide it.
    if args.control_dataset is not None:
        ctl_pairs = ds.load_pairs(args.control_dataset)
        ctl_hold = [p for p in ctl_pairs if p.factor == args.holdout]
        if not ctl_hold:
            print(f"CONTROL(no-WLFG) fail: control dataset has no factor {args.holdout}")
            return 1
        # The identity model reproduces its anchor, so `psnr_in` here is the
        # control's own temporal blend measured against the same target. Evaluated at
        # the same budget as the arm, or the two numbers are not comparable.
        ctl = T.evaluate(_identity, ctl_hold, n=args.eval_samples)
        print(f"CONTROL(no-WLFG)  engine temporal blend {ctl['psnr_in']:.3f} dB   "
              f"SSIM {ctl['ssim_in']:.4f}")
        print(f"  head {agg['psnr_out']:.3f} dB vs control {ctl['psnr_in']:.3f} dB   "
              f"({agg['psnr_out'] - ctl['psnr_in']:+.3f} dB)")
        if agg["psnr_out"] <= ctl["psnr_in"]:
            print("CONTROL FAIL  head does not beat the no-WLFG control; per the plan "
                  "this is not a feature -- report, do not tune the gate")
            return 1
        print("CONTROL PASS  head beats the no-WLFG control on the held-out factor")
    else:
        print("CONTROL(no-WLFG) not supplied -- plan Step 3 acceptance still open "
              "(re-run with --control-dataset <WL_WLFG=0 export>)")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
