"""Held-out evaluation for the clean-room neural enhancement network.

Run this, not the training loss, to decide whether the network is real:

    python tools/train_neural/test_model.py
    python tools/train_neural/test_model.py --weights out/weights.npz

WHAT IT REPORTS (plan task 5, step 3)
-------------------------------------
  * PSNR and SSIM of the enhanced output vs ground truth, on a held-out split;
  * PSNR of the RAW INPUT vs ground truth -- the number the model must beat;
  * mean gate, and the fraction of pixels with gate < 0.05 (the "degrades to no
    enhancement" case).

EXITS NON-ZERO IF THE MODEL DOES NOT BEAT THE RAW INPUT ON PSNR. A model that
cannot beat its own input is not an enhancement, and tuning the gate to hide
that is the failure mode this gate exists to catch.

Also runs two self-checks that need no dataset:
  * `verify_gradient` -- central finite differences against the analytic
    backward pass (float64, so float32 roundoff is not the limiting factor);
  * `verify_blob_roundtrip` -- pack -> bytes -> unpack, asserting the values the
    shader will read are bit-identical.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dataset as ds
import model as M

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = Path(__file__).resolve().parent / "out" / "weights.npz"

# A gate below this is, for practical purposes, "the network declined to act on
# this pixel". Chosen so that gate*residual < 0.05 * typical_residual.
GATE_FLOOR = 0.05


# --------------------------------------------------------------------------
# metrics
# --------------------------------------------------------------------------

def psnr(a: np.ndarray, b: np.ndarray, peak: float = 1.0) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    if mse == 0.0:
        return float("inf")
    return 10.0 * np.log10(peak * peak / mse)


def _gaussian_window(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    g = np.exp(-(x * x) / (2.0 * sigma * sigma))
    w = np.outer(g, g)
    return w / w.sum()


def ssim(a: np.ndarray, b: np.ndarray, window: int = 11) -> float:
    """Windowed SSIM on Rec.709 luma, valid convolution, mean over windows.

    Written out rather than imported: scikit-image is not installed, and the
    point of this file is that every number in it is reproducible from the repo.
    """
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch {a.shape} vs {b.shape}")
    wa = a @ np.array([0.2126, 0.7152, 0.0722])
    wb = b @ np.array([0.2126, 0.7152, 0.0722])
    w = _gaussian_window(window)

    def filt(x):
        # separable convolution, 'valid'
        pad = window // 2
        xp = np.pad(x, ((pad, pad), (0, 0)), mode="edge")
        tmp = np.apply_along_axis(lambda r: np.convolve(r, w[window // 2], mode="valid"), 0, xp)
        xp = np.pad(tmp, ((0, 0), (pad, pad)), mode="edge")
        out = np.apply_along_axis(lambda c: np.convolve(c, w[window // 2], mode="valid"), 1, xp)
        return out

    mu_a, mu_b = filt(wa), filt(wb)
    va = filt(wa * wa) - mu_a * mu_a
    vb = filt(wb * wb) - mu_b * mu_b
    cab = filt(wa * wb) - mu_a * mu_b
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    s = ((2 * mu_a * mu_b + c1) * (2 * cab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    return float(np.mean(s))


# --------------------------------------------------------------------------
# inference
# --------------------------------------------------------------------------

def predict(params: dict, anchor_rgb: np.ndarray, global_rgb=None, chunk: int = 65536):
    """anchor (N,3) in 0..1 -> (out (N,3), gate (N,)) in 0..1, chunked.

    `global_rgb` is the (4,) frame-mean conditioning vector, or None for the
    unconditioned v0 model.

    Chunked because a full 2332x1451 frame is 3.39 M pixels and the hidden
    activation alone would be ~430 MB.
    """
    x = M.features(anchor_rgb, global_rgb)
    n = x.shape[0]
    out = np.empty((n, 3), dtype=np.float32)
    gate = np.empty((n, 1), dtype=np.float32)
    for s in range(0, n, chunk):
        e = min(s + chunk, n)
        o, (_xc, _a, _z1, _h1, _z2, _h2, _z3, _h3, _z4, h4, _r, g) = M.forward(
            params, x[s:e], anchor_rgb[s:e])
        out[s:e] = o
        gate[s:e] = g
    return out, gate.ravel()


def predict_frame(params: dict, anchor_img: np.ndarray, global_rgb=None) -> tuple[np.ndarray, np.ndarray]:
    """HxWx3 anchor -> (HxWx3 enhanced, HxW gate).

    The conditioning vector is reduced from the ANCHOR frame itself, so the
    caller never has to supply it and there is no way to leak the ground truth
    through it. Pass `global_rgb` explicitly when the frame is being processed in
    bands -- reducing it per band silently substitutes each band's mean for the
    frame's, which is a different input and a different answer.
    """
    h, w = anchor_img.shape[:2]
    if global_rgb is None:
        global_rgb = M.frame_global(anchor_img)
    out, gate = predict(params, anchor_img.reshape(-1, 3), global_rgb)
    return out.reshape(h, w, 3), gate.reshape(h, w)


# --------------------------------------------------------------------------
# self-checks
# --------------------------------------------------------------------------

def verify_gradient(verbose: bool = True) -> bool:
    """Central finite differences vs the analytic backward pass, in float64.

    float64 matters: in float32 the loss is ~1.9 and eps=1e-3 leaves only ~4
    significant digits, which reads as a gradient error of the same order as the
    bug being looked for.
    """
    rng = np.random.default_rng(0)
    p32 = M.init_params(rng, scale=0.5)
    for k in ("Wh", "Wg", "W1", "W2", "W3", "W4"):
        p32[k] = rng.normal(0, 0.4, p32[k].shape).astype(np.float32)
    p = {k: v.astype(np.float64) for k, v in p32.items()}
    n = 9
    x = rng.normal(0, 1, (n, M.N_IN))          # signed: keeps ReLU paths live
    y = rng.random((n, 3))
    eps = 1e-6

    ok = True
    for lam in (0.05, 0.0, 0.5, 2.0):
        _, _, _, g = M.loss_and_grads(p, x, y, lam, anchor_rgb=x[:, :3])
        worst = (0.0, None)
        n_bad = 0
        for name, arr in p.items():
            it = np.nditer(arr, flags=["multi_index"])
            for _ in it:
                i = it.multi_index
                o = float(arr[i])
                arr[i] = o + eps
                lp, *_ = M.loss_and_grads(p, x, y, lam, anchor_rgb=x[:, :3])
                arr[i] = o - eps
                lm, *_ = M.loss_and_grads(p, x, y, lam, anchor_rgb=x[:, :3])
                arr[i] = o
                num = (lp - lm) / (2 * eps)
                ana = float(g[name][i])
                abs_err = abs(num - ana)
                # Both ~0 means a dead ReLU path with nothing to compare; the
                # meaningful test is the absolute floor plus the relative error.
                if abs(num) < 1e-9 and abs(ana) < 1e-9:
                    continue
                if abs_err > 1e-7:
                    n_bad += 1
                rel = abs_err / max(1e-12, abs(num) + abs(ana))
                if rel > worst[0] and abs_err > 1e-7:
                    worst = (rel, f"{name}{i}")
        status = "PASS" if n_bad == 0 else "FAIL"
        if n_bad:
            ok = False
        if verbose:
            print(f"  gradient lam={lam:<5} worst rel {worst[0]:.3e} at {str(worst[1]):<12} "
                  f"params over abs floor: {n_bad:>3}  {status}")
    return ok


def verify_anchor_passthrough(verbose: bool = True) -> bool:
    """A zero-head model must return the ANCHOR exactly, through the real
    standardised `features()` path.

    This is the check that catches the standardisation bug: when `features()`
    started returning standardised values, `forward` was still defaulting its
    anchor to `x[:, :3]`, so the model added its residual to
    `(r - 0.1498) / 0.1205` instead of `r`. `verify_anchor_at_init` passed
    anyway because it called `forward` with a hand-built `x` and no
    `features()`, so nothing exercised the real input path. This one does.
    """
    rng = np.random.default_rng(4)
    p = M.init_params(rng)
    p["Wh"][:] = 0.0
    p["bh"][:] = 0.0

    img = rng.random((96, 96, 3)).astype(np.float32)
    g = M.frame_global(img)
    flat = img.reshape(-1, 3)
    out, gate = predict(p, flat, g)
    err = float(np.max(np.abs(out - flat)))
    ok = err == 0.0
    if verbose:
        print(f"  anchor passthrough (standardised path): max|out-anchor| = {err:.3e}  "
              f"({'PASS' if ok else 'FAIL'})")
    return ok


def verify_blob_roundtrip(verbose: bool = True) -> bool:
    """pack -> raw bytes -> unpack must be bit-identical.

    This is the shader's read path: the engine uploads the bytes and indexes
    them in exactly the order `export_weights.py` wrote them.
    """
    rng = np.random.default_rng(1)
    p = M.init_params(rng)
    vec = M.pack(p)
    raw = vec.astype("<f4").tobytes()
    back = np.frombuffer(raw, dtype="<f4")
    ok = back.size == M.N_WEIGHTS and np.array_equal(back, vec)
    if verbose:
        print(f"  blob roundtrip: {vec.size} weights, {len(raw)} bytes, "
              f"bit-identical={ok}  {'PASS' if ok else 'FAIL'}")
    return ok


def verify_anchor_at_init(verbose: bool = True) -> bool:
    """With a zeroed head the network must be exactly the identity.

    This is the property that makes 'the engine frame is unyielding' mechanical:
    a fresh network cannot change a single pixel.
    """
    rng = np.random.default_rng(2)
    p = M.init_params(rng)
    x = rng.random((64, M.N_IN)).astype(np.float32)
    out, (_, _, _, _, _, _, _, _, _, _, _, gate) = M.forward(p, x, x[:, :3])
    err = float(np.max(np.abs(out - x[:, :3])))
    ok = err == 0.0
    if verbose:
        print(f"  identity at init: max|out-anchor| = {err:.3e}  {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------
# held-out evaluation
# --------------------------------------------------------------------------

def load_weights(path: Path) -> dict:
    path = Path(path)
    if path.suffix == ".npz":
        z = np.load(path)
        return {k: z[k].astype(np.float32) for k in z.files}
    if path.suffix == ".bin":
        raw = np.frombuffer(path.read_bytes(), dtype="<f4")
        return M.unpack(raw)
    raise ValueError(f"unknown weights format: {path}")


def fit_global_affine(pairs, n=40000, seed=11) -> np.ndarray:
    """Least-squares per-channel affine (gain+bias), fitted on `pairs`.

    This is the control the network has to beat. It is 12 parameters. If the
    1140-parameter network cannot beat it on held-out frames, the network is not
    earning its weights -- and that is a result worth reporting, not hiding.
    """
    rng = np.random.default_rng(seed)
    xs, ys = [], []
    per = max(1, n // max(len(pairs), 1))
    for p in pairs:
        a, t = ds.pair_arrays(p)
        h, w = a.shape[:2]
        yy = rng.integers(0, h, size=per)
        xx = rng.integers(0, w, size=per)
        xs.append(a[yy, xx]); ys.append(t[yy, xx])
    X = np.concatenate(xs).astype(np.float64)
    Y = np.concatenate(ys).astype(np.float64)
    A = np.concatenate([X, np.ones((len(X), 1))], axis=1)
    coef, *_ = np.linalg.lstsq(A, Y, rcond=None)
    return coef


def apply_affine(coef: np.ndarray, img: np.ndarray) -> np.ndarray:
    A = img.reshape(-1, 3)
    X = np.concatenate([A, np.ones((len(A), 1), np.float32)], axis=1).astype(np.float64)
    return (X @ coef).astype(np.float32).reshape(img.shape)


def evaluate_affine(coef, pairs) -> list[float]:
    return [psnr(apply_affine(coef, ds.pair_arrays(p)[0]), ds.pair_arrays(p)[1]) for p in pairs]


def evaluate(params: dict, pairs, chunk_rows: int = 256, verbose: bool = True) -> dict:
    """Full-frame metrics on every pair. Streams row bands to bound memory."""
    rows = []
    for pair in pairs:
        anchor_img, target_img = ds.pair_arrays(pair)
        h, w = anchor_img.shape[:2]
        # The frame mean is a property of the WHOLE frame. Reducing it per band
        # would feed each band its own mean, which is a different input and a
        # different answer -- measured at 38.183 dB instead of 38.763 dB.
        g = M.frame_global(anchor_img)
        out = np.empty_like(anchor_img)
        gate = np.empty((h, w), dtype=np.float32)
        for y0 in range(0, h, chunk_rows):
            y1 = min(y0 + chunk_rows, h)
            o, gg = predict_frame(params, anchor_img[y0:y1], g)
            out[y0:y1] = o
            gate[y0:y1] = gg
        rows.append({
            "key": pair.key,
            "psnr_in": psnr(anchor_img, target_img),
            "psnr_out": psnr(out, target_img),
            "ssim_out": ssim(out, target_img),
            "ssim_in": ssim(anchor_img, target_img),
            "gate_mean": float(gate.mean()),
            "gate_below": float(np.mean(gate < GATE_FLOOR)),
            "max_abs_delta": float(np.max(np.abs(out - anchor_img))),
        })
        if verbose:
            print(f"  {pair.key:34} in {rows[-1]['psnr_in']:6.3f} -> out {rows[-1]['psnr_out']:6.3f} dB "
                  f"({rows[-1]['psnr_out'] - rows[-1]['psnr_in']:+6.3f})  "
                  f"gate {rows[-1]['gate_mean']:.3f}  below@{GATE_FLOOR} {rows[-1]['gate_below']:.3f}")
    agg = {
        "n": len(rows),
        "psnr_in": float(np.mean([r["psnr_in"] for r in rows])),
        "psnr_out": float(np.mean([r["psnr_out"] for r in rows])),
        "ssim_in": float(np.mean([r["ssim_in"] for r in rows])),
        "ssim_out": float(np.mean([r["ssim_out"] for r in rows])),
        "gate_mean": float(np.mean([r["gate_mean"] for r in rows])),
        "gate_below": float(np.mean([r["gate_below"] for r in rows])),
        "max_abs_delta": float(np.max([r["max_abs_delta"] for r in rows])),
        "rows": rows,
    }
    agg["delta"] = agg["psnr_out"] - agg["psnr_in"]
    return agg


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default=str(ds.DEFAULT_DATASET))
    ap.add_argument("--weights", default=str(DEFAULT_OUT))
    ap.add_argument("--holdout", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261023)
    ap.add_argument("--self-test-only", action="store_true",
                    help="run the gradient / blob / identity checks and exit")
    args = ap.parse_args(argv)

    print("=== self-checks (no dataset needed) ===")
    ok_g = verify_gradient()
    ok_p = verify_anchor_passthrough()
    ok_b = verify_blob_roundtrip()
    ok_i = verify_anchor_at_init()
    if args.self_test_only:
        return 0 if (ok_g and ok_p and ok_b and ok_i) else 1

    print()
    print("=== held-out evaluation ===")
    pairs = ds.load_pairs(args.dataset)
    train, hold = ds.split_pairs(pairs, holdout=args.holdout, seed=args.seed)
    print(f"pairs: {len(pairs)} total, {len(train)} train, {len(hold)} held out")
    print(f"held out: {[p.key for p in hold]}")
    print()

    wpath = Path(args.weights)
    if not wpath.exists():
        print(f"no weights at {wpath} -- run `python tools/train_neural/train.py` first")
        return 2
    params = load_weights(wpath)

    hold_agg = evaluate(params, hold)
    print()
    print(f"  HELD OUT   PSNR raw input {hold_agg['psnr_in']:.3f} dB  ->  "
          f"enhanced {hold_agg['psnr_out']:.3f} dB   ({hold_agg['delta']:+.3f} dB)")
    print(f"  HELD OUT   SSIM raw input {hold_agg['ssim_in']:.4f}  ->  "
          f"enhanced {hold_agg['ssim_out']:.4f}")
    print(f"  HELD OUT   mean gate {hold_agg['gate_mean']:.4f}   "
          f"fraction with gate < {GATE_FLOOR}: {hold_agg['gate_below']:.4f}")
    print(f"  HELD OUT   max |out - anchor| = {hold_agg['max_abs_delta']:.4f} "
          f"(the engine frame is never replaced, only adjusted)")

    # ---- the control: a 12-parameter tone curve, fitted on TRAIN only ----
    print()
    print("  --- control: global per-channel affine, 12 parameters, fitted on TRAIN only ---")
    coef = fit_global_affine(train)
    aff_hold = evaluate_affine(coef, hold)
    aff_train = evaluate_affine(coef, train)
    aff_hold_mean = float(np.mean(aff_hold))
    print(f"  affine     train {np.mean(aff_train):.3f} dB   held out {aff_hold_mean:.3f} dB")
    print(f"  affine coefficients: gains {np.round(coef[:3, 0], 4).tolist()} "
          f"{np.round(coef[:3, 1], 4).tolist()} {np.round(coef[:3, 2], 4).tolist()}")
    beats_curve = hold_agg["psnr_out"] > aff_hold_mean

    # The gate: a model that cannot beat its own input is not an enhancement.
    beats = hold_agg["psnr_out"] > hold_agg["psnr_in"]
    print()
    if beats:
        print(f"PASS  enhanced beats raw input by {hold_agg['delta']:+.3f} dB on the held-out split")
    else:
        print(f"FAIL  enhanced LOSES to the raw input by {-hold_agg['delta']:.3f} dB -- "
              f"this is not an enhancement; do not tune the gate to hide it")
    if not beats_curve:
        print(f"NOTE  enhanced does NOT beat the 12-parameter affine "
              f"({hold_agg['psnr_out']:.3f} vs {aff_hold_mean:.3f} dB). The network is a "
              f"tone curve with extra weights -- real, but not what it is for.")
    else:
        print(f"PASS  enhanced beats the 12-parameter affine by "
              f"{hold_agg['psnr_out'] - aff_hold_mean:+.3f} dB -- the weights earn their keep")
    return 0 if (ok_g and ok_p and ok_b and ok_i and beats) else 1


if __name__ == "__main__":
    raise SystemExit(main())
