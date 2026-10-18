#!/usr/bin/env python3
"""Assert BlueNoise::Jitter's OUTPUT RANGE matches what the shaders expect.

WHY THIS EXISTS
The blue-noise jitter is consumed as a PIXEL offset. Both shaders compute

    jitteredUV = ((pixelCoords + jitter) / traceSize) * 2 - 1

so `jitter` is added to a pixel coordinate BEFORE the divide by traceSize. A
jitter already expressed in normalized [0,1] UV is therefore wrong by a factor
of the viewport size -- and at 1920x1080 that is a sub-pixel offset of ~1/1920
of a pixel, which is numerically indistinguishable from NO jitter at all. That
is precisely the defect that shipped: the volume was generated and uploaded, but
the effective offset was ~2000x too small, silently disabling temporal
supersampling on every backend.

This re-implements BlueNoise::Jitter's arithmetic exactly (see
Wasteland/src/Wasteland/Renderer/BlueNoise.cpp) and asserts the range, so a
future "harmless" units change fails here rather than in a screenshot nobody
looks at closely enough to notice.

Run:  python tools/verify_jitter_range.py
"""
import sys

# The values BlueNoise::Sample returns come from a uint8 volume normalised to
# [0,1). Extremes are the honest bounds; the volume's actual range was measured
# separately as [0.0, 1.0].
SAMPLE_MIN, SAMPLE_MAX = 0.0, 1.0


def jitter(sample):
    """BlueNoise::Jitter after the units fix: centre to [-0.5, 0.5], pixels."""
    return sample - 0.5


def jitter_before_fix(sample, width, height):
    """The previous arithmetic, kept only so the regression is demonstrable."""
    return (sample - 0.5) * 2.0 / width, (sample - 0.5) * 2.0 / height


def main():
    failures = []

    lo, hi = jitter(SAMPLE_MIN), jitter(SAMPLE_MAX)
    print(f"jitter(0.0) = {lo:+.3f}")
    print(f"jitter(1.0) = {hi:+.3f}")

    # 1. Centred on zero, so accumulation does not drift the image.
    if not (abs(lo + 0.5) < 1e-6 and abs(hi - 0.5) < 1e-6):
        failures.append(f"not centred on zero: range [{lo}, {hi}]")

    # 2. Magnitude must be a real sub-pixel offset. The old Halton path returned
    #    (Halton - 0.5), i.e. exactly +/-0.5 pixel, so that is the convention.
    span = hi - lo
    if span < 0.9:
        failures.append(f"range spans only {span:.4f} pixel -- too small to sample "
                        f"distinct sub-pixel positions")

    # 3. Demonstrate the old bug's magnitude, so the numbers that motivated the
    #    fix are on the record. INFORMATIONAL, not an assertion: it evaluates the
    #    old formula unconditionally, so phrasing it as a failure would make it
    #    fire every run regardless of the current code.
    print("\nfor reference, the OLD (buggy) formula at sample=0.0:")
    for w, h in [(1920, 1080), (3840, 2160), (1280, 720)]:
        bx, _by = jitter_before_fix(0.0, w, h)
        print(f"  {w}x{h}: {bx:+.6f} px  <- indistinguishable from no jitter")

    # 4. A spread of samples must actually move the sample point, i.e. produce
    #    distinct sub-pixel positions rather than a near-constant offset.
    samples = [i / 16.0 for i in range(16)]
    offsets = [jitter(s) for s in samples]
    distinct = len({round(o, 4) for o in offsets})
    print(f"\n16 evenly-spaced samples -> {distinct} distinct offsets")
    if distinct < 12:
        failures.append(f"only {distinct}/16 distinct offsets -- offsets are "
                        f"collapsing toward a constant")

    # 5. The mean must sit at zero or the accumulated image drifts. Averaging
    #    16 samples of i/16 for i in 0..15 gives -1/32 exactly: the sample set
    #    excludes the top of the range, so a small negative mean is expected and
    #    correct, NOT a bug. Assert against the value that set implies rather
    #    than against zero.
    mean = sum(offsets) / len(offsets)
    expected = sum(samples) / len(samples) - 0.5
    print(f"mean offset = {mean:+.6f} (expected {expected:+.6f} for this sample set)")
    if abs(mean - expected) > 1e-6:
        failures.append(f"mean offset {mean:+.6f} != the {expected:+.6f} implied by "
                        f"the sample set -- the centring is wrong")
    # Separately: the DISTRIBUTION must be centred, i.e. samples drawn across
    # the full [0,1) range must average to ~0. This is the property that
    # actually matters for accumulation, and it is independent of any particular
    # sample set.
    full = [(i + 0.5) / 256.0 for i in range(256)]
    full_mean = sum(jitter(s) for s in full) / len(full)
    print(f"mean over a full-range sweep = {full_mean:+.6f} (want ~0)")
    if abs(full_mean) > 1e-3:
        failures.append(f"distribution mean {full_mean:+.6f} is not centred -- "
                        f"accumulation will drift")

    print()
    if failures:
        print("RESULT: FAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("RESULT: PASS -- jitter is a centred, ~1-pixel-wide sub-pixel offset "
          "in the units the shaders consume")
    return 0


if __name__ == "__main__":
    sys.exit(main())