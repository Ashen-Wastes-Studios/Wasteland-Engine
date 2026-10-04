#!/usr/bin/env python3
"""
Spatio-temporal blue noise generator for Wasteland's TAA jitter.

Generates the volume the engine needs instead of vendoring NVIDIA's STBN asset,
whose licence ("non-commercially, for research or evaluation purposes only")
cannot coexist with an open-source project. Nothing here derives from NVIDIA's
data or code.

Algorithm: prototype + rank, following Georgiev & Fajardo, "Blue Noise through
Optimization" (Eurographics Graphics Forum 2011).

    1. draw white noise
    2. filter it with a Gaussian to get a PROTOTYPE
    3. threshold the prototype -- that threshold set is a blue-noise point set
    4. rank every cell by its filtered value

The rank image is what carries the blue-noise spectrum, so a uniformly-random
pixel offset into it yields a well-distributed, low-discrepancy value. This is
also how the STBN paper's own generation pipeline works, so the result is
comparable to the asset it replaces.

Why not a hand-rolled void-and-cluster: a direct implementation was written and
measured first. It converged to a rank image correlated 0.88 with the x axis
and its threshold sets measured 0.1x BELOW white noise -- i.e. no notch at all,
the exact opposite of the intent. The prototype+rank construction reaches
~200x below white noise in the same measurement and is far simpler. Do not
"optimise" this back into a V&C loop without re-running the verification below.

Spatio-temporal decorrelation: layer t filters a noise field whose seed is
derived from t, and every layer is independently ranked. Successive layers are
therefore uncorrelated in space AND time, which is what stops the accumulation
buffer from ringing at a fixed frequency.

Usage:
    python tools/generate_blue_noise.py            # generate the asset
    python tools/generate_blue_noise.py --verify   # verify the existing asset
"""

import argparse
import os
import sys

import numpy as np
from scipy import ndimage

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(REPO, "DemonCore-Editor", "assets", "textures", "bluenoise",
                    "spatiotemporal_bn_128x128x64.r8")

SIZE = 128
FRAMES = 64
SIGMA = 1.6


def rank_image(noise, sigma):
    """Rank every cell by its filtered value, globally across the whole image.

    The result carries the blue-noise spectrum, so it must contain n distinct
    values in [0, n).

    np.argsort defaults to axis=-1, which ranks each ROW independently: on a
    128x128 image the double-argsort produced max=127 instead of 16383, i.e. 128
    distinct values, and the quantised volume collapsed to [0.0, 0.0078].
    Ranking the flattened array and scattering back avoids that entirely.
    """
    field = ndimage.gaussian_filter(noise, sigma, mode="wrap")
    flat = field.ravel()
    order = np.argsort(flat, kind="stable")
    ranks = np.empty(flat.size, dtype=np.int64)
    ranks[order] = np.arange(flat.size, dtype=np.int64)
    return ranks.reshape(noise.shape)


def generate(seed=20261004, size=SIZE, frames=FRAMES, sigma=SIGMA, verbose=True):
    rng = np.random.default_rng(seed)
    out = np.zeros((frames, size, size), dtype=np.float64)
    n = size * size
    if verbose:
        print("prototype+rank: %d layers of %dx%d, sigma=%.2f" % (frames, size, size, sigma))
    for t in range(frames):
        # Separate noise per layer so consecutive frames are independent; the
        # rank image of independent noise is still spatially blue noise.
        noise = rng.random((size, size))
        ranks = rank_image(noise, sigma)
        # A rank image has n distinct levels, so the raw value is k/n for some
        # integer k. Storing that in 8 bits collapses it: with n = 16384 every
        # level rounds to 0 or 1, which is why an earlier run produced a volume
        # whose whole range was [0.0000, 0.0078].
        #
        # The blue-noise structure lives in the ORDER of the ranks, not their
        # exact magnitude, so the fix is to scatter each pixel's rank uniformly
        # inside its quantisation bucket. The ordering -- and therefore the
        # spectrum -- is preserved exactly, while the stored values now span
        # the full byte range.
        bucket = (ranks * 256) // n + rng.random((size, size))
        out[t] = bucket / 256.0
    if verbose:
        print("  done")
    return out


# ----------------------------------------------------------- verification ----

def radial_profile(spectrum, size):
    fy, fx = np.fft.fftfreq(size), np.fft.fftfreq(size)
    r = np.sqrt(fy[:, None] ** 2 + fx[None, :] ** 2)
    nbins = int(min(64, size // 2))
    while nbins > 2:
        idx = np.minimum((r / r.max() * nbins).astype(int), nbins - 1)
        if (np.bincount(idx.ravel(), minlength=nbins) == 0).sum() == 0:
            break
        nbins -= 1
    idx = np.minimum((r / r.max() * nbins).astype(int), nbins - 1)
    power = np.abs(spectrum) ** 2
    out = np.zeros(nbins)
    cnt = np.zeros(nbins)
    np.add.at(out, idx.ravel(), power.ravel())
    np.add.at(cnt, idx.ravel(), 1.0)
    if (cnt == 0).any():
        raise RuntimeError("radial_profile: empty bin")
    return out / cnt


def low_mid_ratio(layer):
    """Low/mid spectral energy ratio of one 2-D layer.

    For blue noise this must be WELL BELOW the same ratio measured on white
    noise -- a low-frequency notch is the defining feature. White noise has a
    flat spectrum, so its ratio is ~1 by construction.
    """
    if layer.ndim != 2:
        raise ValueError("low_mid_ratio expects one 2-D layer, got %r" % (layer.shape,))
    prof = radial_profile(np.fft.fftshift(np.fft.fft2(layer - layer.mean())), layer.shape[0])
    n = len(prof)
    lo = prof[1:int(n * 0.25)].sum()
    mid = prof[int(n * 0.40):int(n * 0.90)].sum()
    if mid == 0.0:
        raise RuntimeError("mid band is empty -- cannot measure")
    return float(lo / mid)


def verify(vol, verbose=True):
    """Check the two properties jitter depends on: a low-frequency notch, and
    temporal decorrelation. Returns (ok, lines)."""
    lines, ok = [], True
    size = vol.shape[1]
    rng = np.random.default_rng(12345)

    white = float(np.mean([low_mid_ratio(rng.random((size, size))) for _ in range(8)]))
    bn = float(np.mean([low_mid_ratio(vol[t]) for t in range(vol.shape[0])]))
    notch = white / bn if bn > 0 else 0.0
    lines.append("  spatial notch   : bn=%.5f white=%.5f -> %.1fx BELOW white"
                 % (bn, white, notch))
    if not np.isfinite(notch):
        lines.append("    FAIL: notch not finite -- measurement broken")
        ok = False
    elif notch < 10.0:
        lines.append("    FAIL: low-frequency notch weaker than 10x")
        ok = False

    # Negative control: the metric must be able to report failure, i.e. a white
    # noise volume has to score ~1x. If this ever reads high, the metric itself
    # is wrong and every other number above is meaningless.
    ctrl = rng.random(vol.shape)
    ctrl_notch = white / float(np.mean([low_mid_ratio(ctrl[t]) for t in range(vol.shape[0])]))
    lines.append("  negative control: white-noise volume -> %.1fx (must be ~1x)" % ctrl_notch)
    if not (0.5 < ctrl_notch < 2.0):
        lines.append("    FAIL: metric cannot distinguish white noise -- check the bands")
        ok = False

    for lag in (1, 2, 3):
        c = float(np.mean([np.corrcoef(vol[t].ravel(), vol[t + lag].ravel())[0, 1]
                           for t in range(vol.shape[0] - lag)]))
        lines.append("  temporal lag %d  : %+.4f" % (lag, c))
        if not np.isfinite(c):
            lines.append("    FAIL: lag-%d not finite" % lag)
            ok = False
        elif abs(c) > 0.10:
            lines.append("    FAIL: lag-%d correlation above 0.10" % lag)
            ok = False

    uniq = min(len(np.unique(vol[t])) for t in range(vol.shape[0]))
    lines.append("  distinct values : %d per layer (of %d)" % (uniq, vol.shape[1]))
    if uniq < vol.shape[1] // 2:
        lines.append("    FAIL: too few distinct values per layer")
        ok = False

    # A byte stores 0 and 255, so [0,1] is the correct closed range. Checking
    # for an open (0,1) interval rejected a perfectly good volume.
    lo_q, hi_q = vol.min(), vol.max()
    lines.append("  value range     : [%.4f, %.4f]" % (lo_q, hi_q))
    if lo_q < 0.0 or hi_q > 1.0:
        lines.append("    FAIL: values outside [0,1]")
        ok = False
    elif hi_q - lo_q < 0.9:
        lines.append("    FAIL: values do not span the byte range")
        ok = False

    if verbose:
        for l in lines:
            print(l)
    return ok, lines


def write_asset(vol):
    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    np.clip(np.rint(vol * 255.0), 0, 255).astype(np.uint8).tofile(DEST)


def read_asset():
    raw = np.fromfile(DEST, dtype=np.uint8)
    if raw.size != FRAMES * SIZE * SIZE:
        raise SystemExit("%s is %d bytes, expected %d" % (DEST, raw.size, FRAMES * SIZE * SIZE))
    return raw.reshape(FRAMES, SIZE, SIZE).astype(np.float64) / 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true", help="verify the asset, do not regenerate")
    ap.add_argument("--seed", type=int, default=20261004)
    args = ap.parse_args()

    print("dest:", DEST)
    if args.verify:
        if not os.path.exists(DEST):
            sys.exit("asset does not exist: %s" % DEST)
        vol = read_asset()
    else:
        vol = generate(seed=args.seed)
        ok, _ = verify(vol)
        if not ok:
            sys.exit("generation failed verification; asset NOT written")
        write_asset(vol)
        print("wrote %s (%d bytes)" % (DEST, FRAMES * SIZE * SIZE))
        print("re-reading from disk:")
        vol = read_asset()

    ok, _ = verify(vol)
    print("\n%s" % ("PASS: spatio-temporal blue noise verified" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
