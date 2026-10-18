#!/usr/bin/env python3
"""
Moment/variance maths for the SVGF denoiser, verified standalone.

WHY: plan section 5.2 names the risk -- "variance estimation fails silently. A
wrong variance produces a plausible-looking but wrong filter width." No
screenshot shows that: an estimator off by a constant factor, or blind to edges,
still renders a clean, plausible image. So it is checked here as arithmetic (no
engine link, no GPU, no numpy), matching what the twins already do at
`Renderer3D_NovaRenderer.hlsl:3789`, `:3812`, `:4056`.

NEGATIVE CONTROL: every check re-runs against a broken estimator that omits the
mean^2 subtraction, and the run fails outright if that broken one passes -- a test
that cannot fail is not a test. Same guard `generate_blue_noise.py` uses for its
spectral ratio, which once shipped inverted.

Run: python3 tools/verify_variance_math.py   (exit 0 = all green, 1 = failed)
"""

import math
import random
import sys

# History blend window: under 0.05 the history dominates and the frame ghosts;
# over 0.2 it stops being temporal. Renderer3D.cpp:3023 clamps the shipped alpha
# to [0.02, 1.0]; SVGF is tighter.
ALPHA_MIN, ALPHA_MAX = 0.05, 0.20

PHI_L, PHI_S = 2.0, 8.0                        # A-Trous edge-avoiding (SVGF 4.3)

# Guards both denominators: a converged pixel has variance exactly 0, and
# without this floor every tap weight becomes inf/NaN.
EPS = 1e-8

# Separable cubic B3 spline, the A-Trous interpolation wavelet. Sums to 1 in 1D
# and in 2D -- that is why a flat neighbourhood comes back untouched.
ATROUS_KERNEL = (1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0)


class Estimator:
    """Progressive luminance moments, and the variance read out of them.

    Carries three numbers so the incremental path can be checked against two
    independent others over the same samples: m1 (mean), m2s (mean of squares),
    wm2 (Welford sum of squared deviations). Variance is m2s - m1^2, algebraically
    wm2/n. Both exist because the shader carries only the moment pair -- the
    history blend stores m1 and m2s, so the subtraction is unavoidable there.

    The max(., 0) clamp is not cosmetic: for a constant signal m1^2 == m2s exactly,
    but in fp32 the subtraction cancels and lands a few ULPs either side of zero.
    sqrt of a negative is NaN, and one NaN weight poisons a whole 5x5 tap group --
    the denoiser emits black speckles that look like a raytracing bug and are not.
    """

    def __init__(self, name, broken=False):
        self.name, self.broken = name, broken
        self.n = self.m1 = self.m2s = self.wm2 = 0.0

    def push(self, x, w=1.0):
        """Add one observation. Exact for both moment forms, not an estimate:
        mean' = mean + w*(x-mean)/(n+w) is algebraically identical to the
        two-pass formula, so only the rounding path can differ.
        """
        if w <= 0.0:
            return
        if self.n == 0.0:
            self.n, self.m1, self.m2s, self.wm2 = w, x, x * x, 0.0
            return
        n2, delta = self.n + w, x - self.m1
        self.m1 += w * delta / n2
        self.m2s += w * (x * x - self.m2s) / n2
        # Welford: the residual must be measured AFTER the mean moves, or the
        # correction is biased by exactly one update.
        self.wm2 += w * delta * (x - self.m1)
        self.n = n2

    def stream(self, samples):
        """Accumulate into a FRESH instance: sharing `self` across checks let one
        check's samples bleed into the next, on the first run."""
        out = Estimator(self.name, self.broken)
        for s in samples:
            out.push(s)
        return out

    def variance_from(self, m1, m2s):
        raw = m2s if self.broken else m2s - m1 * m1
        return raw if raw > 0.0 else 0.0

    def variance(self):
        return self.variance_from(self.m1, self.m2s)

    def welford_variance(self):
        """None for the broken variant: it never consults the deviations."""
        return None if self.broken or self.n == 0.0 else max(self.wm2 / self.n, 0.0)

    def two_pass(self, samples):
        """Naive reference: one pass for the mean, one for the squares."""
        n = float(len(samples))
        return self.variance_from(math.fsum(samples) / n,
                                  math.fsum(s * s for s in samples) / n)


def clamp_alpha(alpha):
    """History blend weight forced into [0.05, 0.2]. A non-finite alpha means
    the reprojection went NaN upstream; resetting to ALPHA_MAX (trust this frame
    completely) is the only safe answer, since NaN in the moments poisons the
    buffer for the rest of the run."""
    return ALPHA_MAX if not math.isfinite(alpha) else min(max(alpha, ALPHA_MIN), ALPHA_MAX)


def blend_variance(est, hist, cur, alpha):
    """Blend the MOMENTS, then derive variance -- never the variances directly.
    Variance is a quadratic form, not a mean, so a linear blend of variances is
    not the variance of the blend."""
    a = clamp_alpha(alpha)
    return est.variance_from(hist[0] * (1.0 - a) + cur[0] * a,
                             hist[1] * (1.0 - a) + cur[1] * a)


def atrous_taps(step=1):
    """Centre plus the four B3 taps per axis, carrying their kernel index."""
    taps = [(0, 0, 0)]
    for h in (1, 2):
        taps += [(step, 0, h), (-step, 0, h), (0, step, h), (0, -step, h)]
    return taps


def atrous_tap_weight(h_index, center_luma, variance, tap_luma, dx, dy):
    """One edge-avoiding wavelet tap (SVGF sec. 4.3):

        w = h_k * exp(-len^2 / (phi_l^2 * sigma^2 + eps))
              * max(0, 1 - (dL)^2 / (phi_s^2 * sigma^2 + eps))

    The spatial Gaussian is normalised BY the variance, so its width grows with
    local noise -- a converged pixel keeps a tight kernel, a noisy one opens up
    -- and EPS is what keeps that from being a divide by zero on exactly the
    converged pixels the filter should leave alone. The luminance stop is a hard
    max(0, ...): past phi_s sigmas a tap is rejected outright rather than
    downweighted, and that hard cut is what stops a step edge being blurred
    across. None for non-finite variance: the caller falls back to the centre tap
    rather than spread NaN across the group.
    """
    if not math.isfinite(variance):
        return None
    s2 = max(variance, 0.0)
    spatial = math.exp(-(dx * dx + dy * dy) / (PHI_L * PHI_L * s2 + EPS))
    stop = 1.0 - (tap_luma - center_luma) ** 2 / (PHI_S * PHI_S * s2 + EPS)
    return ATROUS_KERNEL[h_index] * spatial * (stop if stop > 0.0 else 0.0)


def atrous_weights(center_luma, variance, tap_luma):
    """Normalised tap weights, plus the raw sum they were normalised from.
    Normalisation is the partition-of-unity step: raw weights are a kernel
    TIMES two rejection factors, so they sum to well under 1 in general."""
    raw = [atrous_tap_weight(h, center_luma, variance, tl, dx, dy)
           for (dx, dy, h), tl in zip(atrous_taps(), tap_luma)]
    centre_only = [1.0] + [0.0] * (len(raw) - 1)
    if any(w is None for w in raw):
        return centre_only, float("nan")
    total = math.fsum(raw)
    if not math.isfinite(total) or total <= 0.0:
        return centre_only, total
    return [w / total for w in raw], total


# --------------------------------------------------------------- checks ----

def check_flat(est):
    v = est.stream([0.5] * 4096).variance()
    return abs(v) <= 1e-6, "variance=%.3e (tol 1e-06)" % v


def check_gaussian(est):
    sigma, n = 0.25, 200000
    rng = random.Random(20261004)
    v = est.stream(rng.gauss(0.5, sigma) for _ in range(n)).variance()
    err = abs(v - sigma ** 2) / sigma ** 2 * 100.0
    # Relative SE of the sample variance is sqrt(2/(n-1)) = 0.32%, so the 2%
    # window is ~6 sigma of sampling noise, not a tuned threshold.
    return err <= 2.0, "var=%.6f vs sigma^2=%.6f -> %.3f%% err (tol 2%%)" % (v, sigma ** 2, err)


def _box_filtered_ramp(n=9, box=5):
    """Smooth ramp, box-filtered: a locally linear patch of the same mean. The
    box filter is what makes this the honest control -- a raw ramp already has
    low variance, but a box-filtered ramp is what a denoiser sees when flat."""
    ramp, half, out = [i / float(n - 1) for i in range(n)], box // 2, []
    for i in range(n):
        win = ramp[max(0, i - half):min(n, i + half + 1)]
        out.append(math.fsum(win) / len(win))
    return out


def check_edge_vs_gradient(est):
    edge_v = est.stream([0.0] * 4 + [1.0] * 5).variance()
    grad_v = est.stream(_box_filtered_ramp()).variance()
    ratio = edge_v / grad_v if grad_v > 0 else float("inf")
    # Both patches have mean 5/9. The step is 5/9*4/9 = 0.247 of variance; the
    # box-filtered ramp is near-uniform, 1/12 = 0.083. Near 1.0x means the
    # estimator cannot see an edge at all.
    return ratio >= 2.5, "edge=%.4f grad=%.4f -> %.2fx (want >=2.5x)" % (edge_v, grad_v, ratio)


def check_streaming_vs_two_pass(est):
    rng = random.Random(4242)
    sig = [1.0 + rng.gauss(0.0, 0.1) for _ in range(4096)]
    acc = est.stream(sig)                      # keep the instance: it holds wm2
    s, t = acc.variance(), est.two_pass(sig)
    err = abs(s - t)
    line = "stream=%.15f two_pass=%.15f -> %.3e (tol 1e-09)" % (s, t, err)
    if err > 1e-9:
        return False, line
    # wm2/n is a different arithmetic path over the same samples, so agreement
    # corroborates rather than restates. A SILENT skip is how a cross-check dies
    # unnoticed, so a missing run is a failure, not a pass.
    wv = acc.welford_variance()
    if wv is None:
        return False, line + " | FAIL: Welford cross-check did not run"
    werr = abs(s - wv)
    return werr <= 1e-9, line + " | Welford=%.15f -> %.3e (tol 1e-09)" % (wv, werr)


def check_blend(est):
    hist, cur = (0.30, 0.11), (0.70, 0.52)
    alphas = [-5.0, -1e-9, 0.0, 0.01, 0.05, 0.1, 0.2, 0.5, 1.0, 7.0, 1e9,
              float("inf"), float("-inf"), float("nan")]
    bad = []
    for a in alphas:
        v = blend_variance(est, hist, cur, a)
        if not math.isfinite(v) or v < 0.0:
            bad.append("alpha=%g -> %.3e" % (a, v))
    zero = blend_variance(est, (0.0, 0.0), (0.0, 0.0), 0.2)
    if zero != 0.0:
        bad.append("all-zero input -> %r" % (zero,))
    clamped = (clamp_alpha(-1.0), clamp_alpha(0.12), clamp_alpha(99.0))
    if clamped != (ALPHA_MIN, 0.12, ALPHA_MAX):
        bad.append("clamp wrong: %r" % (clamped,))
    return not bad, ("%d alphas incl. inf/NaN all finite and >=0; clamp=%r; all-zero -> %.1f; "
                     "blend(0.1)=%.4f%s" % (len(alphas), clamped, zero,
                                            blend_variance(est, hist, cur, 0.1),
                                            "; BAD: " + "; ".join(bad) if bad else ""))


def check_atrous_rejection(est):
    center, var = 0.5, 0.0625                  # sigma = 0.25; only the neighbour varies
    diffs = [0.0, 0.25, 1.0, 4.0, 1e6]
    ws = [atrous_tap_weight(1, center, var, center + d, 1, 0) for d in diffs]
    if any(w is None for w in ws):
        return False, "tap weight is None -- variance guard fired"
    flat, edge = ws[0], ws[-1]
    monotone = all(b <= a for a, b in zip(ws, ws[1:]))
    # Past phi_s sigmas the stop term hits its max(0, ...) floor and the tap is
    # rejected outright, not merely downweighted. That hard cut is what stops a
    # step edge being blurred across; a weight merely trending down would not.
    hard = edge == 0.0
    # Phi normalisation as a property: at the SAME relative difference (4 sigma,
    # inside the phi_s boundary) a converged and a noisy pixel must get very
    # different weights. Raw weight cannot be thresholded absolutely -- it caps
    # at the kernel weight (0.25) since both factors are <= 1.
    conv = atrous_tap_weight(1, center, var, center + 4.0 * 0.25, 1, 0)
    noisy = atrous_tap_weight(1, center, 1e6, center + 4.0 * 1000.0, 1, 0)
    phi_ok = conv > 0.0 and noisy > 20.0 * conv
    return (monotone and hard and edge < flat and phi_ok), \
        "sigma=0.25: " + " ".join("d=%.4g->%.3e" % (d, w) for d, w in zip(diffs, ws)) + \
        " | monotone=%s hard-reject=%s | at 4sigma: converged=%.3e noisy=%.3f (%.0fx wider)" \
        % (monotone, hard, conv, noisy, noisy / conv)


def check_atrous_sums_to_one(est):
    v = est.stream([0.5] * 64).variance()
    w, raw = atrous_weights(0.5, v, [0.5] * 9)
    total, is_max = math.fsum(w), w[0] >= max(w)
    return abs(total - 1.0) <= 1e-12 and is_max, \
        "flat 5x5: sum=%.15f (tol 1e-12), raw sum=%.4f, centre=%.4f is max=%s" \
        % (total, raw, w[0], is_max)


def check_single_sample(est):
    v, vw = est.stream([0.5]).variance(), est.welford_variance()
    return (v == 0.0 and vw in (None, 0.0)), \
        "n=1 -> %.1f (one sample has no deviation from itself)" % v


def check_identical_nonzero(est):
    v = est.stream([5.0] * 32).variance()
    # 25.0 is what a mean of squares WITHOUT the mean^2 subtraction returns. If
    # that is what shows up, the subtraction is missing.
    return abs(v) <= 1e-6 and abs(v - 25.0) > 1.0, \
        "all-5.0 -> %.3e (want ~0, and emphatically not 25.0)" % v


CHECKS = (
    ("flat signal has ~zero variance", check_flat),
    ("gaussian variance matches sigma^2", check_gaussian),
    ("step edge variance >> gradient variance", check_edge_vs_gradient),
    ("streaming == two-pass (and == Welford)", check_streaming_vs_two_pass),
    ("clamped moment blend stays finite, >= 0", check_blend),
    ("a-trous rejects a gradient tap", check_atrous_rejection),
    ("a-trous weights sum to 1 on a flat 5x5", check_atrous_sums_to_one),
    ("single-sample input", check_single_sample),
    ("identical non-zero input is not mean^2", check_identical_nonzero),
)


def run(est):
    out = []
    for name, fn in CHECKS:
        try:
            ok, line = fn(est)
        except Exception as exc:                  # a crash is a failure
            ok, line = False, "raised %s: %s" % (type(exc).__name__, exc)
        out.append((name, bool(ok), line))
    return out


def report(title, results):
    print("=" * 78 + "\n" + title + "\n" + "=" * 78)
    for name, ok, line in results:
        print("[%s] %-44s %s" % ("PASS" if ok else "FAIL", name, line))


def main():
    good = Estimator("moment estimator")
    bad = Estimator("BROKEN: mean of squares, no mean^2 subtraction", broken=True)

    res = run(good)
    report("estimator: %s" % good.name, res)
    failures = [n for n, ok, _ in res if not ok]

    bres = run(bad)
    report("NEGATIVE CONTROL: %s" % bad.name, bres)
    bfail = [n for n, ok, _ in bres if not ok]
    print("\nbroken variant failed %d of %d checks:" % (len(bfail), len(bres)))
    for f in bfail:
        print("    -", f)

    if not bfail:
        print("\n" + "!" * 78 +
              "\n!! NEGATIVE CONTROL DID NOT FAIL. The estimator above is not being"
              "\n!! measured by these checks -- they pass for a broken variance too,"
              "\n!! so every PASS printed above is worthless. Fix the harness."
              "\n" + "!" * 78)
        return 1

    print()
    if failures:
        print("RESULT: FAIL -- %d of %d checks failed:" % (len(failures), len(res)))
        for f in failures:
            print("  -", f)
        return 1
    print("RESULT: PASS -- %d checks green, and the negative control is provably"
          " failable (%d checks reject the broken estimator)." % (len(res), len(bfail)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
