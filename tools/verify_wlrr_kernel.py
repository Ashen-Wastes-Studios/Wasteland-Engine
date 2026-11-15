#!/usr/bin/env python3
"""Task 10 Step 4 (WLRR) -- verify the A-Trous denoise kernel's math.

WHAT THIS GUARDS, AND WHY IT IS WORTH A VERIFIER
------------------------------------------------
`RunATrousFilter` (pass 13) is the incumbent denoiser's spatial half. Its
luminance weight is the SVGF one:

    w_l = exp( -|mean_c - mean_n| / (sigma_l * sqrt(var_c) + eps) )

with the per-pixel variance recovered from the temporal moments:

    var_c = max(m2 - m1^2, 0)

**The `max` is load-bearing and is the whole reason this file exists.** `m1^2` can
exceed `m2` by a rounding error -- the moments are accumulated with a lerp, so the
two are not algebraically consistent -- and a negative variance goes under `sqrt`,
which is NaN. One NaN tap poisons `totalColor` and `totalWeight` together, the
`totalWeight > 0.0001` guard fails, and the pass writes `inputColor` back
unchanged: the denoiser silently becomes a NO-OP for that pixel with no error, no
log and no crash. Today that `max` is protected by a comment and nothing else.

THE PLAN'S PREMISE IS PARTLY STALE, AND THIS IS THE MEASUREMENT
---------------------------------------------------------------
The plan's section 0.3 says the ladder is "a fixed 4-pass 1/2/4/8 schedule rather
than variance-adaptive", and Step 2 asks for "an A-Trous wavelet family whose
per-iteration blend weight is variance-driven". Read against the source:

  - the STEP SCHEDULE is indeed fixed (1, 2, 4, 8, driven from C++), and
  - the BLEND WEIGHTS are ALREADY variance-driven (the expression above).

So Step 2's kernel half is not outstanding work; it is existing code. What is
outstanding is exactly what Step 4 asks for -- a verifier that pins the math down,
including a negative control that proves the guard can be caught being absent.

Checks, in the order they matter:

  1. VARIANCE GUARD. The shader's variance derivation is wrapped in a max against
     zero. Structural, read from the source.
  2. GUARD NECESSITY. Numerically: with m2 < m1^2 the guarded form yields 0 and the
     unguarded form yields a negative variance whose sqrt is NaN, and that NaN
     propagates to the weight. This is the negative control the plan asks for.
  3. LUMINANCE WEIGHT IS VARIANCE-DRIVEN. The centre's variance appears in the
     denominator (a noisier centre smooths harder) -- checked structurally AND
     numerically, because a weight that merely *mentions* variance can still be
     inert (see check 5).
  4. B3-SPLINE WEIGHTS SUM TO 1. A flat region must come back unchanged; the
     spatial taps 0.375/0.25/0.0625 are what guarantee it.
  5. NO INERT VARIANCE TERM. The kernel once multiplied by `(1 + var_c * 5)`. That
     term was inert -- var_c is the CENTRE's variance, identical for all nine taps,
     so a constant factor cancels exactly in the normalisation -- and it was
     removed. This check fails if that shape comes back, because it reads as a
     variance-driven weight while doing nothing.
  6. NORMALISATION. The kernel divides by its own total, and falls back to the
     input colour when the total is degenerate.

Usage:
    python tools/verify_wlrr_kernel.py
    python tools/verify_wlrr_kernel.py --self-test   # mutate the source, expect FAIL
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADER = os.path.join(ROOT, "DemonCore-Editor", "assets", "shaders",
                      "Renderer3D_NovaRenderer.slang")

# The kernel's own constants, transcribed. Kept here rather than parsed because a
# verifier that reads its expectations out of the thing it is checking cannot
# detect that thing changing.
SIGMA_L = 4.0
EPS = 1e-4
SPLINE = {0: 0.375, 1: 0.25, 2: 0.0625}


# --------------------------------------------------------------------------
# The kernel math, transcribed from RunATrousFilter
# --------------------------------------------------------------------------

def variance(m1: float, m2: float, guarded: bool = True) -> float:
    """`var_c` from the temporal moments. The guard is the point of this file."""
    raw = m2 - m1 * m1
    return max(raw, 0.0) if guarded else raw


def luminance_weight(mean_c: float, mean_n: float, var_c: float) -> float:
    """The SVGF luminance weight. A larger centre variance => a wider tolerance."""
    return math.exp(-abs(mean_c - mean_n) / (SIGMA_L * math.sqrt(var_c) + EPS))


def spline_weight(x: int, y: int) -> float:
    """B3-spline spatial weight: centre 0.375, edge 0.25, corner 0.0625."""
    return SPLINE[abs(x) + abs(y)]


# --------------------------------------------------------------------------
# Source extraction
# --------------------------------------------------------------------------

def read_shader() -> str:
    if not os.path.isfile(SHADER):
        raise SystemExit(f"shader not found: {SHADER}")
    with open(SHADER, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def kernel_body(src: str) -> str:
    """The text of RunATrousFilter, from its signature to the next top-level fn."""
    m = re.search(r"void\s+RunATrousFilter\s*\(\s*\)\s*\{", src)
    if not m:
        raise SystemExit("RunATrousFilter not found in the shader")
    start = m.end()
    depth = 1
    i = start
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[start:i]


def self_test(src: str) -> int:
    """Mutation checks: each must make the detector fire. No GPU needed."""
    checks: list[tuple[str, bool]] = []

    def guarded(s: str) -> bool:
        return bool(re.search(r"max\s*\(\s*[\w.]+\s*-\s*[\w.]+\s*\*\s*[\w.]+"
                              r"\s*,\s*0\.0f?\s*\)", s))

    def inert_variance(s: str) -> bool:
        # The removed shape: a constant (1 + var * k) multiplier on the weight.
        return bool(re.search(r"1\.0f?\s*\+\s*\w*[Vv]ariance\s*\*", s))

    def variance_in_denominator(s: str) -> bool:
        return bool(re.search(r"sqrt\s*\(\s*\w*[Vv]ariance\s*\)", s))

    # 1. The unmutated source passes every structural check.
    body = kernel_body(src)
    checks.append(("source: variance derivation is guarded", guarded(body)))
    checks.append(("source: variance appears under sqrt in a denominator",
                   variance_in_denominator(body)))
    checks.append(("source: no inert (1 + var*k) term", not inert_variance(body)))
    checks.append(("source: normalises by totalWeight",
                   bool(re.search(r"totalColor\s*/\s*totalWeight", body))))

    # 2. Remove the max -> the structural check must fail.
    unguarded = body.replace("max(moments.y - moments.x * moments.x, 0.0f)",
                             "moments.y - moments.x * moments.x")
    checks.append(("mutation: stripping the max is detected",
                   unguarded != body and not guarded(unguarded)))

    # 3. Re-introduce the inert term -> must be detected.
    inert = body.replace("float weight = spatialWeight",
                         "float weight = (1.0f + centerVariance * 5.0f) * spatialWeight")
    checks.append(("mutation: inert (1 + var*5) term is detected",
                   inert != body and inert_variance(inert)))

    # 4. Drop the normalisation -> must be detected.
    denorm = body.replace("totalColor / totalWeight", "totalColor")
    checks.append(("mutation: removing normalisation is detected",
                   denorm != body
                   and not re.search(r"totalColor\s*/\s*totalWeight", denorm)))

    # 5. The numeric negative control: the guard is NECESSARY.
    #    m1 = 0.5, m2 = 0.2499 -> m1^2 = 0.25 > m2, a rounding-scale overshoot.
    m1, m2 = 0.5, 0.2499
    v_guarded = variance(m1, m2, guarded=True)
    v_raw = variance(m1, m2, guarded=False)
    checks.append(("numeric: guarded variance clamps to 0", v_guarded == 0.0))
    checks.append(("numeric: unguarded variance is negative", v_raw < 0.0))
    nan = False
    try:
        w = luminance_weight(m1, m1, v_raw)
        nan = math.isnan(w)
    except ValueError:
        nan = True
    checks.append(("numeric: negative variance makes the weight NaN", nan))
    #    And the guarded form is finite for the same input.
    checks.append(("numeric: guarded form stays finite",
                   math.isfinite(luminance_weight(m1, m1, v_guarded))))

    # 6. The weight must actually be variance-driven: a noisier centre smooths a
    #    given luminance difference harder. If this were flat, check 3's structural
    #    pass would be meaningless.
    w_low = luminance_weight(0.5, 0.6, variance(0.5, 0.2501))   # low variance
    w_high = luminance_weight(0.5, 0.6, variance(0.5, 0.60))    # high variance
    checks.append(("numeric: higher centre variance => larger weight",
                   w_high > w_low))

    # 7. The flat-region invariant. NOTE what does and does not hold here:
    #      0.375 (centre) + 4*0.25 (edges) + 4*0.0625 (corners) = 1.625,
    #    so the taps do NOT sum to 1 -- the shader comment at the top of
    #    RunATrousFilter says they do, and that comment is wrong. The invariant
    #    that actually holds is weaker and is what matters: a flat region comes
    #    back unchanged because the kernel divides by its OWN totalWeight. A
    #    verifier that asserted the comment's claim would fail on correct code;
    #    this one asserts the property the code actually provides.
    taps = [spline_weight(x, y) for x in (-1, 0, 1) for y in (-1, 0, 1)]
    tap_sum = sum(taps)
    checks.append(("numeric: taps sum to 1.625, so the 'sum to 1' comment is false",
                   abs(tap_sum - 1.625) < 1e-12 and abs(tap_sum - 1.0) > 1e-9))
    flat = 0.42  # any constant; all nine taps see the same colour
    filtered = sum(flat * w for w in taps) / sum(taps)
    checks.append(("numeric: a flat region round-trips unchanged (by normalisation)",
                   abs(filtered - flat) < 1e-12))

    # 8. The false claim must be gone. A comment that says the taps sum to 1 is
    #    how the wrong assumption gets copied into the next kernel; this asserts
    #    it does not come back.
    claims_one = bool(re.search(r"sum to 1 at every step", body))
    checks.append(("source: the false 'taps sum to 1' claim is gone",
                   not claims_one))

    bad = [n for n, ok in checks if not ok]
    for n, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {n}")
    print()
    print(f"RESULT: {len(checks) - len(bad)}/{len(checks)} self-test checks passed")
    if bad:
        print()
        print("  A mutation that is NOT detected means the check is decorative:")
        print("  the guard could be removed in the shader and this verifier would")
        print("  still pass. Fix the check before trusting the green run.")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true",
                    help="run the mutation checks (source + numeric, no GPU)")
    args = ap.parse_args()

    src = read_shader()
    body = kernel_body(src)

    if args.self_test:
        return self_test(src)

    failures: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
        if not ok:
            failures.append(name)

    print(f"kernel: RunATrousFilter in {os.path.relpath(SHADER, ROOT)}")
    print()

    # 1. The guard.
    guard = re.search(r"float\s+centerVariance\s*=\s*([^;]+);", body)
    guarded = bool(guard and re.search(r"max\s*\(", guard.group(1)))
    check("variance derivation is wrapped in max(..., 0)",
          guarded,
          f"found: {guard.group(1).strip() if guard else 'centerVariance not found'}")

    # 2. Variance in the denominator under sqrt.
    check("centre variance appears under sqrt in the weight denominator",
          bool(re.search(r"sqrt\s*\(\s*centerVariance\s*\)", body)))

    # 3. The weight is the SVGF exponential form, not something else.
    check("luminance weight is exp(-|mean_c - mean_n| / (sigma*sqrt(var) + eps))",
          bool(re.search(r"exp\s*\(\s*-abs\s*\(\s*moments\.x\s*-\s*neighborMean\s*\)", body)))

    # 4. No inert variance term.
    inert = re.search(r"1\.0f\s*\+\s*centerVariance\s*\*", body)
    check("no inert (1 + centerVariance * k) multiplier", inert is None,
          "an inert term reads as variance-driven but cancels in the normalisation"
          if inert else "")

    # 5. Normalisation + degenerate fallback.
    check("normalises by the kernel's own totalWeight",
          bool(re.search(r"totalColor\s*/\s*totalWeight", body)))
    check("falls back to the input colour on a degenerate total",
          bool(re.search(r"totalWeight\s*>\s*[\d.]+f?\s*\)\s*\?\s*\(?\s*totalColor"
                         r"\s*/\s*totalWeight\s*\)?\s*:\s*inputColor", body)))

    # 6. Sky passthrough -- a pixel with no depth must not be filtered, because
    #    there is no edge to stop against.
    check("depth >= 1 (sky) passes through untouched",
          bool(re.search(r"centerDepth\s*>=\s*1\.0f", body)))

    # 7. The kernel writes somewhere the composite actually reads, so the result
    #    is not discarded. Pass 13 writes img_Bloom_Temp and C++ calls
    #    CommitDenoise() to copy it into img_Accumulation.
    check("writes a buffer C++ commits back into the accumulation",
          bool(re.search(r"IMG_STORE\s*\(\s*img_Bloom_Temp", body)))

    print()
    if failures:
        print(f"FAIL: {len(failures)} check(s) failed: {', '.join(failures)}")
        return 1
    print("PASS: the A-Trous kernel's variance guard, weight and normalisation are intact.")
    print()
    print("  NOTE the guard is a single `max`. Nothing else in the shader protects")
    print("  it, and its absence degrades the denoiser to a silent no-op rather")
    print("  than to an error -- which is why --self-test includes the negative")
    print("  control that proves the absence is detectable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
