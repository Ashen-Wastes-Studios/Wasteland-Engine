#!/usr/bin/env python3
"""Train the WLRR denoiser-corrector head.

    python tools/train_wlrr/train.py --dataset C:/tmp/dataset_wlrr2 --steps 2000

THE BASELINE IS THE POINT
-------------------------
The number that matters is not the model's PSNR. It is the model's PSNR MINUS the
incumbent's, measured on the same held-out triples. The incumbent already scores
well -- Task 9 measured it cutting max|d| 43 -> 30 -- so a head that merely matches
it has earned nothing, and a head trained as `out = incumbent + gate*residual`
starts EXACTLY at the incumbent's score by construction (the residual head is
zeroed). The only honest claim this tool can make is the delta, and it reports it
as the headline rather than burying it behind an absolute number.

NO LEAKAGE, IN THREE PLACES
---------------------------
1. The train/holdout split is by TRIPLE, never by patch. Adjacent patches of one
   frame are nearly identical, so a patch-level split would make the holdout a
   second copy of the training set.
2. The feature statistics are measured on the TRAIN split only. Computing them
   over the whole dataset would put holdout pixels into the affine the model is
   standardised by.
3. Evaluation uses the SAME feature affine the training used, and the holdout is
   never trained on.

WHAT THIS DOES NOT DO
---------------------
It does not write the shader blob. `export_weights.py` does that, from the .npz
this tool saves, and it re-parses what it wrote. Keeping the two apart means a
training run cannot half-publish a model.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dataset as D          # noqa: E402
import model as M            # noqa: E402


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((np.asarray(a, dtype=np.float64)
                         - np.asarray(b, dtype=np.float64)) ** 2))
    return float("inf") if mse == 0.0 else 10.0 * np.log10(255.0 ** 2 / mse)


def evaluate(p: dict, triples, n_patches: int, patch: int, seed: int) -> dict:
    """PSNR of the corrected output, the incumbent, the raw input AND the blend.

    All four, because the delta alone can be read wrongly. The head could raise
    its score over the incumbent simply by undoing it -- reverting towards the
    input -- and the incumbent is sometimes WORSE than the input (measured:
    -0.299 dB on auto_y0). So the input's own score is the control that
    distinguishes "the head learned the detail the filter smeared" from "the head
    learned to switch the filter off".

    THE BLEND IS THE FOURTH AND MOST IMPORTANT BASELINE. `inc + alpha*(inp-inc)`
    with a single global alpha beats BOTH arms on 11/11 scenes
    (`diagnose_oracle.py`), so "beats the incumbent" is a bar the head can clear
    by doing nothing but blending -- which is not a learned correction. The head
    is anchored at the blend, so it must beat the BLEND to have learned anything.
    """
    rng = np.random.default_rng(seed)
    out_all, inc_all, inp_all, tgt_all, blend_all = [], [], [], [], []
    for inc, inp, tgt in D.iter_random_crops(triples, n_patches, patch, rng):
        pred, _ = M.predict(p, inc, inp)
        out_all.append(pred)
        inc_all.append(inc)
        inp_all.append(inp)
        tgt_all.append(tgt)
        blend_all.append(M.blend_anchor(inc, inp))
    if not out_all:
        raise ValueError("evaluation produced no patches -- is `patch` larger than the frames?")
    pred = np.concatenate(out_all)
    base = np.concatenate(inc_all)
    raw = np.concatenate(inp_all)
    tgt = np.concatenate(tgt_all)
    blend = np.concatenate(blend_all)
    return dict(
        corrected=psnr(pred, tgt),
        incumbent=psnr(base, tgt),
        input=psnr(raw, tgt),
        blend=psnr(blend, tgt),
        mean_abs_correction=float(np.mean(np.abs(pred - base))),
        mean_abs_deviation=float(np.mean(np.abs(pred - blend))),
        # How much of the way towards the input the head moved, as a LEAST-SQUARES
        # projection of the correction onto the incumbent->input direction:
        #     alpha = <pred - base, raw - base> / <raw - base, raw - base>
        # alpha ~ 1 means "it just reverted to the unfiltered frame", ~0 means "it
        # did not move towards the input at all".
        #
        # The first version of this used mean(pred-base)/mean(raw-base) and
        # printed -2736.8%, which is the giveaway: the means of these signed
        # residuals are near zero and cancel, so the ratio is noise over noise.
        # A metric that can print a nonsense number is worse than no metric.
        reverted_fraction=float(
            np.sum((pred - base) * (raw - base)) / np.sum((raw - base) ** 2)
            if float(np.sum((raw - base) ** 2)) > 1e-9 else 0.0),
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default=str(D.DEFAULT_DATASET))
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "out"))
    ap.add_argument("--steps", type=int, default=2000)
    ap.add_argument("--batch", type=int, default=4096, help="pixels per training step")
    ap.add_argument("--patch", type=int, default=32, help="crop size, px per side")
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--lam", type=float, default=M.LAMBDA_ANCHOR,
                    help="anchor penalty; higher keeps the correction smaller")
    ap.add_argument("--holdout-camera", default=None,
                    help="hold out a whole camera (strongest split)")
    ap.add_argument("--holdout-scene", default=None)
    ap.add_argument("--allow-incomplete", action="store_true",
                    help="train on the usable triples when some are broken; every skip "
                         "is named on stderr and the count is reported with the result")
    ap.add_argument("--loss-weighting", choices=["none", "separation"], default="none",
                    help="per-sample loss weight. 'separation' weights each pixel by "
                         "1/(sep^2+1) where sep = mean|incumbent - input|. The target "
                         "signal spans 1.9..31.9 across scenes, so unweighted MSE is "
                         "dominated by the few high-separation scenes and the easy ones, "
                         "where the incumbent is already right, contribute almost nothing.")
    ap.add_argument("--eval-patches", type=int, default=24)
    ap.add_argument("--stats-pixels", type=int, default=400_000)
    ap.add_argument("--seed", type=int, default=20261105)
    ap.add_argument("--log-every", type=int, default=100)
    ap.add_argument("--clip-features", type=float, default=None,
                    help="clip standardised features to +/-N sigma; a no-op "
                         "in distribution, and it lets the head respond to "
                         "scenes whose separation is far outside training")
    ap.add_argument("--eval-every", type=int, default=0,
                    help="evaluate the holdout every N steps and print the curve; "
                         "the best point is post-hoc and is labelled as an upper bound")
    args = ap.parse_args(argv)

    t0 = time.time()
    triples = D.load_triples(args.dataset, allow_incomplete=args.allow_incomplete)
    train, hold = D.split_triples(triples, holdout_camera=args.holdout_camera,
                                  holdout_scene=args.holdout_scene, seed=args.seed)
    print(f"dataset   {args.dataset}")
    print(f"triples   {len(triples)} total -> {len(train)} train / {len(hold)} holdout")
    print(f"holdout   " + ", ".join(sorted({f"{t.scene}/{t.camera}" for t in hold})))

    # (2) Statistics from the TRAIN split only.
    mean, std = D.compute_feature_stats(train, samples=args.stats_pixels, seed=args.seed)
    D.apply_feature_stats(mean, std)
    # The anchor blend weight, also from the TRAIN split only. This is the
    # measured near-optimal starting point (11/11 scenes) that the head refines.
    alpha = D.compute_blend_alpha(train, samples=args.stats_pixels, seed=args.seed)
    D.apply_blend_alpha(alpha)
    if args.clip_features is not None:
        M.FEATURE_CLIP = float(args.clip_features)
        print(f"clip      +/-{M.FEATURE_CLIP} sigma")
    print(f"features  mean {np.round(mean, 2).tolist()}")
    print(f"          std  {np.round(std, 2).tolist()}")
    print(f"blend     alpha {np.round(alpha, 4).tolist()}  "
          f"(anchor = incumbent + alpha*(input - incumbent))")

    rng = np.random.default_rng(args.seed)
    p = M.init_params(rng)
    opt = M.Adam(p, lr=args.lr)

    # Baseline BEFORE any step. With the residual head zeroed this must equal the
    # BLEND's score exactly -- if it does not, the model is not the identity at
    # step 0 and the safety property is broken.
    before = evaluate(p, hold, args.eval_patches, args.patch, args.seed)
    print(f"\nstep 0    corrected {before['corrected']:.3f} dB   "
          f"blend {before['blend']:.3f} dB   "
          f"incumbent {before['incumbent']:.3f} dB   "
          f"input {before['input']:.3f} dB   "
          f"delta vs blend {before['corrected'] - before['blend']:+.4f} dB")
    if abs(before["corrected"] - before["blend"]) > 1e-9:
        print("FAIL: the untrained head is not the identity over the blend anchor. "
              "The residual head must be zeroed at init.", file=sys.stderr)
        return 1

    batch_patches = max(1, args.batch // (args.patch * args.patch))
    stream = D.iter_random_crops(train, batch_patches, args.patch, rng, repeat=10_000)
    first = None
    curve = []
    for step in range(1, args.steps + 1):
        try:
            inc, inp, tgt = next(stream)
        except StopIteration:
            stream = D.iter_random_crops(train, batch_patches, args.patch, rng, repeat=10_000)
            inc, inp, tgt = next(stream)
        feats = M.features(inc, inp)
        # NOTE anchor_rgb = the BLEND, not the incumbent. Passing `inp` here would
        # train the head to reproduce the raw input and silently discard the whole
        # premise of the feature; passing `inc` is the old behaviour, which the
        # oracle diagnostic showed is the WORST of the two arms on several scenes.
        #
        # The weights come from the ARMS, not the features: they must be in the
        # 0..255 domain, and `feats` is standardised.
        wts = (M.separation_weights(inc, inp)
               if args.loss_weighting == "separation" else None)
        loss, mse, anchor_pen, grads = M.loss_and_grads(
            p, feats, tgt, lam=args.lam,
            anchor_rgb=M.blend_anchor(inc, inp), weights=wts)
        if first is None:
            first = loss
        opt.lr = M.cosine_lr(args.lr, step, args.steps)
        opt.step(p, grads)
        if step % args.log_every == 0 or step == args.steps:
            print(f"step {step:5d}  loss {loss:9.3f}  mse {mse:9.3f}  "
                  f"anchor {anchor_pen:8.3f}  lr {opt.lr:.2e}")
        # The holdout curve, not just the endpoint.
        #
        # WHY: both earlier runs ended WORSE than their own best training loss
        # (unweighted 1.160 @2500 -> 3.143 @3000; weighted 1.696 -> 5.735), and v3
        # went 0.554 @2500 -> 90.350 @3000. A single endpoint therefore measures
        # whichever side of a cliff the schedule happened to stop on.
        #
        # The BEST point is POST-HOC SELECTED and must be reported as such -- it is
        # an upper bound on what the schedule produces, not a result. The FINAL
        # point is the result. Both are printed; neither is hidden.
        if args.eval_every and (step % args.eval_every == 0 or step == args.steps):
            ev = evaluate(p, hold, args.eval_patches, args.patch, args.seed)
            curve.append((step, ev["corrected"], ev["incumbent"], ev["input"], ev["blend"]))
            print(f"          [holdout @ {step}] corrected {ev['corrected']:.3f} dB   "
                  f"vs blend {ev['corrected'] - ev['blend']:+.4f}   "
                  f"vs incumbent {ev['corrected'] - ev['incumbent']:+.4f}")

    after = evaluate(p, hold, args.eval_patches, args.patch, args.seed)
    delta = after["corrected"] - after["incumbent"]
    print()
    if curve:
        best = max(curve, key=lambda r: r[1])
        print("holdout curve (post-hoc best is an UPPER BOUND, not a result):")
        for s, c, inc, inp, bl in curve:
            mark = "  <-- best" if s == best[0] else ""
            print(f"  step {s:5d}  corrected {c:.3f} dB   "
                  f"vs blend {c - bl:+.4f}   vs incumbent {c - inc:+.4f}{mark}")
        print(f"  POST-HOC BEST step {best[0]}: {best[1]:.3f} dB "
              f"(vs blend {best[1] - best[4]:+.4f}, vs incumbent {best[1] - best[2]:+.4f})")
        print(f"  FINAL step {curve[-1][0]}: {curve[-1][1]:.3f} dB "
              f"(vs blend {curve[-1][1] - curve[-1][4]:+.4f}, "
              f"vs incumbent {curve[-1][1] - curve[-1][2]:+.4f})")
        print()
    print(f"holdout   corrected {after['corrected']:.3f} dB   "
          f"blend {after['blend']:.3f} dB   "
          f"incumbent {after['incumbent']:.3f} dB   input {after['input']:.3f} dB")
    print(f"          delta vs blend {after['corrected'] - after['blend']:+.4f} dB   "
          f"vs incumbent {delta:+.4f} dB   "
          f"vs input {after['corrected'] - after['input']:+.4f} dB")
    print(f"          mean |deviation from blend| {after['mean_abs_deviation']:.4f} / 255   "
          f"mean |correction vs incumbent| {after['mean_abs_correction']:.4f} / 255   "
          f"reverted {after['reverted_fraction'] * 100:.1f}% of the way to the input")
    print(f"          loss {first:.3f} -> {loss:.3f}   ({time.time() - t0:.0f}s)")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    np.savez(out / "wlrr_weights.npz", **{k: v.astype(np.float32) for k, v in p.items()})
    (out / "dataset_stats.json").write_text(json.dumps(dict(
        feature_mean=[float(x) for x in mean],
        feature_std=[float(x) for x in std],
        blend_alpha=[float(x) for x in alpha],
        # Recorded so the ABI's affine is auditable: the shader must apply the
        # same one, and the WLFG head was burned by shipping the identity.
        n_in=int(M.N_IN),
        n_weights=int(M.N_WEIGHTS),
        n_payload=int(M.N_PAYLOAD),
        trained_on=dict(triples=len(train), holdout=len(hold),
                        holdout_keys=sorted(f"{t.scene}/{t.camera}" for t in hold)),
        steps=args.steps, batch=args.batch, patch=args.patch, lam=args.lam,
        loss_weighting=args.loss_weighting,
        holdout=dict(corrected=after["corrected"], incumbent=after["incumbent"],
                     input=after["input"], blend=after["blend"],
                     delta=delta,
                     delta_vs_blend=after["corrected"] - after["blend"],
                     delta_vs_input=after["corrected"] - after["input"],
                     mean_abs_deviation=after["mean_abs_deviation"],
                     mean_abs_correction=after["mean_abs_correction"],
                     reverted_fraction=after["reverted_fraction"]),
    ), indent=2) + "\n", encoding="utf-8")
    print(f"\nwrote {out / 'wlrr_weights.npz'} and {out / 'dataset_stats.json'}")

    # ---- Step 5: the plan's PSNR gate, evaluated and reported link by link ----
    #
    # The plan states: Enhanced PSNR > incumbent PSNR > raw-input PSNR, and "if the
    # chain does not hold, stop". Reporting which link failed matters, because the
    # MIDDLE link is a property of the incumbent, not of this head: Task 9 measured
    # the incumbent LOSING to the raw input on one camera (43.712 vs 44.460). A
    # head can be working perfectly and still not satisfy a chain whose middle term
    # is a view-dependent property of the filter it corrects.
    #
    # So the gate is reported in full, and the head's own claim is stated as the
    # strictly stronger condition: it must beat BOTH baselines. That is checkable
    # whatever the incumbent does.
    #
    # THE BLEND IS A FIFTH LINK AND IT IS THE ONE THAT MATTERS NOW. The head is
    # ANCHORED at the blend, so "corrected > blend" at step 0 is true by
    # construction; after training it measures whether the head learned anything
    # at all. Beating the incumbent alone is satisfiable by the anchor itself.
    links = [
        ("corrected > blend (the head learned something)", after["corrected"] > after["blend"]),
        ("corrected > incumbent", after["corrected"] > after["incumbent"]),
        ("incumbent > input", after["incumbent"] > after["input"]),
        ("corrected > input", after["corrected"] > after["input"]),
    ]
    print("\nStep 5 gate (plan: Enhanced > incumbent > raw-input, on a held-out split)")
    for name, ok in links:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
    print(f"  -> plan's chain {'HOLDS' if all(ok for _, ok in links) else 'DOES NOT HOLD'}")
    beats_both = after["corrected"] > max(after["incumbent"], after["input"])
    learned = after["corrected"] > after["blend"]

    if not links[0][1]:
        print(f"\nRESULT: the head did NOT beat its own anchor on the holdout "
              f"({after['corrected']:.3f} vs blend {after['blend']:.3f} dB). The blend is a "
              f"single global scalar measured on the train split and it beats both arms on "
              f"11/11 scenes, so this is the strongest of the baselines and the head has "
              f"learned nothing that generalises. Report that as the result.")
        return 6
    if not links[1][1]:
        print(f"\nRESULT: the head did NOT beat the incumbent on the holdout "
              f"({after['corrected']:.3f} vs {after['incumbent']:.3f} dB). Report that as "
              f"the result -- do not tune the gate to hide it.")
        return 2
    if not links[3][1]:
        print(f"\nRESULT: +{delta:.4f} dB over the incumbent, but it does NOT beat the raw "
              f"input ({after['input']:.3f} dB). The head may be undoing the filter rather "
              f"than recovering what it smeared -- treat as negative until ruled out.")
        return 3
    if not links[2][1]:
        print(f"\nRESULT: the head beats BOTH baselines "
              f"(+{delta:.4f} dB over the incumbent, "
              f"+{after['corrected'] - after['input']:.4f} dB over the input), but the plan's "
              f"middle link fails: the INCUMBENT loses to the raw input on this holdout "
              f"({after['incumbent']:.3f} vs {after['input']:.3f} dB). That is a finding about "
              f"the incumbent -- Task 9 measured the same thing on one camera -- not about the "
              f"head. The chain cannot hold here whatever the head does.")
        return 4
    print(f"\nRESULT: the plan's chain holds, and the head beats the blend "
          f"(+{after['corrected'] - after['blend']:.4f} dB), the incumbent "
          f"(+{delta:.4f} dB) and the raw input "
          f"(+{after['corrected'] - after['input']:.4f} dB) -- so it learned a per-pixel "
          f"correction that generalises, not a blend. Reverted "
          f"{after['reverted_fraction'] * 100:.1f}% of the way to the input.")
    return 0 if beats_both and learned else 5


if __name__ == "__main__":
    raise SystemExit(main())
