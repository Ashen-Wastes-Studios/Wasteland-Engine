"""Held-out evaluation for the clean-room WLFG neural frame synthesis head.

Run this, not the training loss, to decide whether the head is real:

    python tools/train_wlfg/test_model.py
    python tools/train_wlfg/test_model.py --weights out/weights.npz

WHAT IT REPORTS (plan task 8, step 3)
-------------------------------------
  * PSNR and SSIM of the enhanced output vs ground truth, on a held-out split;
  * PSNR/SSIM of the RAW WARPED INPUT vs ground truth. This is not the separate
    no-WLFG control required by the plan's final acceptance;
  * mean gate, and the fraction of pixels with gate < GATE_FLOOR (the "degrades to
    no enhancement" case).

EXITS NON-ZERO IF THE HEAD DOES NOT BEAT THE RAW WARPED INPUT ON PSNR. A model
that cannot beat its own input is not an enhancement, and tuning the gate to
hide that is the failure mode this gate exists to catch.

Also runs two self-checks that need no dataset:
  * `verify_gradient` -- central finite differences against the analytic backward
    pass (float64, so float32 roundoff is not the limiting factor);
  * `verify_blob_roundtrip` -- pack -> bytes -> unpack, asserting the values the
    shader will read are bit-identical.

NOTE: the SHAPES here are the WLFG head's (7 inputs), not the existing neural
stage's (8 inputs). The existing pipeline is tools/train_neural; do not edit it
in the course of building the WLFG head.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dataset as ds  # noqa: E402
from model import Adam, FEATURE_STD, FEATURE_MEAN, init_params, loss_and_grads, predict  # noqa: E402
from model import LAYER_SHAPES  # noqa: E402

import model as M  # noqa: E402

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


def ssim(a: np.ndarray, b: np.ndarray, window: int = 11) -> float:
    """Spatial 2-D windowed SSIM on Rec.709 luma, averaged over the image."""
    if a.shape != b.shape or a.ndim != 3 or a.shape[2] != 3:
        raise ValueError("SSIM expects two aligned HxWx3 images")
    if window <= 0 or window % 2 == 0:
        raise ValueError("SSIM window must be a positive odd number")
    LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
    wa = a @ LUMA
    wb = b @ LUMA
    _w = _gaussian_window(window)
    mu_a, mu_b = _filt(wa, _w), _filt(wb, _w)
    va = _filt(wa * wa, _w) - mu_a * mu_a
    vb = _filt(wb * wb, _w) - mu_b * mu_b
    cab = _filt(wa * wb, _w) - mu_a * mu_b
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    s = ((2 * mu_a * mu_b + c1) * (2 * cab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    return float(np.mean(s))


def _gaussian_window(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    """1-D Gaussian window (the separable row/column profile)."""
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    g = np.exp(-(x * x) / (2.0 * sigma * sigma))
    return g / g.sum()


def _filt(x: np.ndarray, w: np.ndarray) -> np.ndarray:
    """Separable Gaussian filter over both image axes with edge padding."""
    pad = w.shape[0] // 2
    horizontal = np.pad(x, ((0, 0), (pad, pad)), mode="edge")
    horizontal = np.apply_along_axis(lambda row: np.convolve(row, w, mode="valid"), 1, horizontal)
    vertical = np.pad(horizontal, ((pad, pad), (0, 0)), mode="edge")
    return np.apply_along_axis(lambda col: np.convolve(col, w, mode="valid"), 0, vertical)


# --------------------------------------------------------------------------
# inference
# --------------------------------------------------------------------------

def predict(params: dict, anchor_rgb: np.ndarray, mask: np.ndarray,
            velocity: np.ndarray, depth: np.ndarray, global_rgb=None, chunk: int = 65536):
    """anchor (N,3) in 0..1 -> (out (N,3), gate (N,)) in 0..1, chunked.

    `global_rgb` is the (4,) frame-mean conditioning vector, or None for the
    unconditioned v0 model (the WLFG head is per-pixel, so this is accepted for
    API compatibility).
    """
    x = M.features(anchor_rgb, mask, velocity, depth)
    n = x.shape[0]
    out = np.empty((n, 3), dtype=np.float32)
    gate = np.empty((n, 1), dtype=np.float32)
    for s in range(0, n, chunk):
        e = min(s + chunk, n)
        o, (_xc, _a, _z1, _h1, _z2, _h2, _r, g) = M.forward(params, x[s:e], anchor_rgb[s:e])
        out[s:e] = o
        gate[s:e] = g
    return out, gate.ravel()


def predict_frame(params: dict, warped: np.ndarray, mask: np.ndarray,
                  velocity: np.ndarray, depth: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """HxWx3 warped -> (enhanced, HxW gate). The conditioning is per-pixel:
    warped, mask, velocity and depth are all sampled at the same pixel coords,
    exactly as the engine's shader would."""
    h, w = warped.shape[:2]
    if mask.shape != (h, w, 1) or velocity.shape != (h, w, 2) or depth.shape != (h, w, 1):
        raise ValueError("unaligned WLFG frame features")
    out, gate = predict(params, warped.reshape(-1, 3), mask.reshape(-1, 1),
                        velocity.reshape(-1, 2), depth.reshape(-1, 1))
    return out.reshape(h, w, 3), gate.reshape(h, w)


# --------------------------------------------------------------------------
# self-checks (no dataset needed)
# --------------------------------------------------------------------------

def _fd_loss_fixed(pp, x, anchor, target):
    """MSE + anchor penalty with fixed inputs for finite differences."""
    out, cache = M.forward(pp, x, anchor)
    gate, residual = cache[7], cache[6]
    mse = float(np.mean((out - target) ** 2))
    apen = float(np.mean((gate * residual) ** 2))
    return mse + 0.05 * apen


def verify_gradient(verbose: bool = True) -> bool:
    """Central finite differences vs the analytic backward pass, in float64."""
    rng = np.random.default_rng(0)
    params = {k: v.astype(np.float64) for k, v in M.init_params(rng, scale=0.5).items()}
    params["Wr"][:] = rng.normal(0, 0.1, params["Wr"].shape)
    x = rng.normal(0, 0.3, (32, M.N_IN))
    anchor = rng.random((32, 3))
    target = rng.random((32, 3))
    grads = M.loss_and_grads(params, x, target, 0.05, anchor_rgb=anchor)[3]
    ok = True
    for name, analytic in grads.items():
        for i in np.unique(np.linspace(0, analytic.size - 1, min(8, analytic.size), dtype=int)):
            idx = np.unravel_index(int(i), analytic.shape)
            original = params[name][idx]
            params[name][idx] = original - 1e-6
            l0 = _fd_loss_fixed(params, x, anchor, target)
            params[name][idx] = original + 1e-6
            l1 = _fd_loss_fixed(params, x, anchor, target)
            params[name][idx] = original
            fd = (l1 - l0) / 2e-6
            if not np.isclose(fd, analytic[idx], rtol=2e-4, atol=2e-7):
                ok = False
                if verbose:
                    print(f"  gradient mismatch {name}{idx} fd={fd:.8g} analytic={analytic[idx]:.8g}")
    return ok


def _make_warpr(): return np.random.rand(64, 3).astype(np.float32)
def _make_mask(): return np.random.rand(64, 1).astype(np.float32)
def _make_vel(): return np.random.rand(64, 2).astype(np.float32)
def _make_depth(): return np.random.rand(64, 1).astype(np.float32)
def _make_anchor(): return np.random.rand(64, 3).astype(np.float32)


def verify_blob_roundtrip(verbose: bool = True) -> bool:
    """pack -> bytes -> unpack must be bit-identical (the ABI the shader reads)."""
    rng = np.random.default_rng(42)
    p = M.init_params(rng, scale=0.1)
    raw = M.pack(p).tobytes()
    p2 = M.unpack(np.frombuffer(raw, dtype=np.float32))
    return all(np.array_equal(p[k], p2[k]) for k in p)


# --------------------------------------------------------------------------
# held-out evaluation
# --------------------------------------------------------------------------

def evaluate(params: dict, pairs, n: int = 20000, seed: int = 7):
    """Evaluate deterministic spatial patches against target and raw warp.

    The raw warp is an input baseline, not the separate no-WLFG control.
    """
    if not pairs or n <= 0:
        raise ValueError("held-out evaluation requires pairs and a positive sample budget")
    rng = np.random.default_rng(seed)
    a_all, t_all, out_all, gates, out_ssim, in_ssim = [], [], [], [], [], []
    for p in pairs:
        a, m, t, velocity, depth = ds.pair_feature_arrays(p)
        h, w = a.shape[:2]
        side = min(64, h, w)
        count = max(1, n // (len(pairs) * side * side))
        for _ in range(count):
            y = int(rng.integers(0, h - side + 1))
            x = int(rng.integers(0, w - side + 1))
            sl = np.s_[y:y + side, x:x + side]
            ac, mc, tc, vc, dc = a[sl], m[sl], t[sl], velocity[sl], depth[sl]
            out, gate = predict(params, ac.reshape(-1, 3), mc.reshape(-1, 1),
                                vc.reshape(-1, 2), dc.reshape(-1, 1))
            out_img = out.reshape(side, side, 3)
            a_all.append(ac.reshape(-1, 3)); t_all.append(tc.reshape(-1, 3))
            out_all.append(out); gates.append(gate)
            out_ssim.append(ssim(out_img, tc)); in_ssim.append(ssim(ac, tc))
    out, raw_out, t = np.concatenate(out_all), np.concatenate(a_all), np.concatenate(t_all)
    gate = np.concatenate(gates)
    psnr_out, psnr_in = psnr(out, t), psnr(raw_out, t)
    delta = 0.0 if np.isinf(psnr_out) and np.isinf(psnr_in) else psnr_out - psnr_in
    return {
        "psnr_out": psnr_out,
        "psnr_in": psnr_in,
        "ssim_out": float(np.mean(out_ssim)),
        "ssim_in": float(np.mean(in_ssim)),
        "delta": delta,
        "gate_mean": float(np.mean(gate)),
        "gate_below": float(np.mean(gate < GATE_FLOOR)),
    }


def _synthetic_pairs() -> list[ds.Pair]:
    """Build two tiny in-memory pairs so `test_model.py --smoke` runs while the
    engine readbacks (mask, vel/depth) are still being wired. Pair objects carry
    dummy paths; the module-level `pair_arrays` is monkeypatched to return small
    canned arrays instead of reading PNGs."""
    paths = [(Path("syn") / f"{n}_{i}.png") for n in
             ("warped", "mask", "target", "velocity", "depth") for i in range(2)]
    return [ds.Pair(
    key=f"syn_{i}", scene="syn", camera="cam", factor=1,
    warped_path=paths[0 * 2 + i], mask_path=paths[1 * 2 + i],
    target_path=paths[2 * 2 + i],
    velocity_path=paths[3 * 2 + i], depth_path=paths[4 * 2 + i],
    meta={},
    ) for i in range(2)]


def _synthetic_pair_feature_arrays(pair: ds.Pair):
    """Canned features only for the explicitly synthetic smoke check."""
    key = int(pair.key.split("_")[-1])
    rng = np.random.default_rng(1000 + key)
    h, w = 4, 6
    return (rng.random((h, w, 3)).astype(np.float32),
            rng.random((h, w, 1)).astype(np.float32),
            rng.random((h, w, 3)).astype(np.float32),
            rng.uniform(-0.01, 0.01, (h, w, 2)).astype(np.float32),
            rng.random((h, w, 1)).astype(np.float32))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", default=str(DEFAULT_OUT))
    ap.add_argument("--dataset", default=str(ds.DEFAULT_DATASET))
    ap.add_argument("--holdout", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261023)
    ap.add_argument("--smoke", action="store_true",
                    help="3 pairs, tiny sample: verifies the evaluation path end to end")
    args = ap.parse_args(argv)

    if args.smoke:
        pairs = _synthetic_pairs()
        _orig_pair_feature_arrays = ds.pair_feature_arrays
        ds.pair_feature_arrays = _synthetic_pair_feature_arrays
    else:
        pairs = ds.load_pairs(args.dataset)
    _, hold = ds.split_pairs(pairs, holdout=args.holdout, seed=args.seed)
    if args.smoke:
        # Smoke test: the synthetic pairs are all factor 1, so holdout 1 gives
        # the tiny hold set the smoke test needs. With the real dataset,
        # holdout 6 is used (the held-out factor).
        hold = hold[:3] if hold else pairs[:3]
        args.holdout = 1
    print(f"held-out metrics on {len(hold)} pairs from {args.dataset}")

    params = M.init_params(np.random.default_rng(args.seed))
    if args.weights and os.path.exists(args.weights):
        params = np.load(args.weights)
    agg = evaluate(params, hold)
    print(f"  raw warped input  {agg['psnr_in']:.3f} dB")
    print(f"  enhanced          {agg['psnr_out']:.3f} dB   ({agg['delta']:+.3f} dB)")
    print(f"  spatial patch SSIM  raw {agg['ssim_in']:.4f} / enhanced {agg['ssim_out']:.4f}")
    print(f"  mean gate         {agg['gate_mean']:.4f}   < {GATE_FLOOR}: {agg['gate_below']:.4f}")

    if args.smoke:
        ds.pair_feature_arrays = _orig_pair_feature_arrays

    # the two self-checks
    g_ok = verify_gradient()
    b_ok = verify_blob_roundtrip()
    print(f"  gradient check: {'OK' if g_ok else 'FAIL'}")
    print(f"  blob roundtrip: {'OK' if b_ok else 'FAIL'}")

    if args.smoke:
        print("\nSMOKE: synthetic evaluation only; no quality or acceptance claim.")
        return 0 if g_ok and b_ok else 1
    if not g_ok or not b_ok:
        print("\nFAIL: a model self-check failed.")
        return 1
    if agg["delta"] <= 0:
        print("\nFAIL: the head does not beat the raw warped input. It is not an enhancement.")
        return 1
    print(f"\nPASS: enhanced beats the raw warped input by {agg['delta']:+.3f} dB")
    print("A separate no-WLFG control is still required for plan acceptance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
def load_weights(path: Path) -> dict:
    """Load a weights.npz into a parameter dict. Refuses wrong shapes/magic.

    train.py writes the NAMED archive (`np.savez(out, **params)`), so the file is
    a set of named arrays rather than one flat vector. Reading it with a flat
    `raw[off:off+k]` slice -- which is what this used to do -- raises
    `'NpzFile' object has no attribute 'size'` on every real training run. Read
    by name and let LAYER_SHAPES do the checking; the flat form is still accepted
    so an older single-array file does not silently become unreadable.
    """
    import numpy as np
    from model import LAYER_SHAPES, N_WEIGHTS
    known = dict(LAYER_SHAPES)
    raw = None
    with np.load(path) as z:
        names = list(z.files)
        if names and all(n in known for n in names):
            return {name: z[name].reshape(shape).astype(np.float32)
                    for name, shape in LAYER_SHAPES}
        if len(names) == 1:
            raw = z[names[0]]
    if raw is None:
        raise ValueError(f"weights {path.name}: unrecognised archive (keys {names})")
    if raw.size != N_WEIGHTS:
        raise ValueError(f"weights {path.name}: expected {N_WEIGHTS} floats, got {raw.size}")
    p, off = {}, 0
    for name, shape in LAYER_SHAPES:
        k = int(np.prod(shape))
        p[name] = raw[off:off + k].reshape(shape).astype(np.float32)
        off += k
    if off != N_WEIGHTS:
        raise ValueError(f"weights {path.name}: trailing {N_WEIGHTS - off} floats")
    return p
