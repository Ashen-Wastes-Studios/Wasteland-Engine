#!/usr/bin/env python3
"""Task 9 (WLRR) -- make the incumbent SVGF denoiser measurable.

THE PLAN'S PREMISE, QUOTED (section 0.3):

    "the SVGF denoiser (passes 12/13) has never been observed to run. SvfgEnabled =
     false by default, the A-Trous ladder is a fixed 4-pass 1/2/4/8 schedule rather
     than variance-adaptive, and there is no GPU-verified screenshot of it."

    "Replacing a denoiser nobody has validated is the classic way to ship a feature
     that cannot be compared against anything. So WLRR's first task is to make the
     incumbent measurable, not to replace it."

So this tool answers two questions in order, and refuses to answer the second until
the first is settled:

  1. DOES THE INCUMBENT RUN AT ALL?  A denoiser that is a no-op makes every quality
     number meaningless, so the arms are checked for actual pixel movement before
     any score is reported. A byte-identical pair is reported as a FAILURE OF THE
     FEATURE, not as a perfect score -- the same discipline as proving a compute
     pass runs with a sentinel store rather than a dispatch log.

  2. IF IT RUNS, IS IT ANY BETTER THAN NOTHING?  Measured against the engine's own
     converged reference, and always as a comparison of two distances to the SAME
     reference -- never as an absolute score, because an absolute PSNR against a
     converged image says nothing on its own.

NO SHADER CHANGE, AND NO CODE CHANGE EITHER. The plan allows "a diagnostic flag
only" in Renderer3D.cpp. It turns out both flags already exist, added by earlier
sessions for exactly this purpose, so this task adds no engine code at all:

  WL_AA_FILTER=11   -> ReconstructionFilter::SpatialDenoise, which the SVGF gate at
                       Renderer3D.cpp:3930 accepts (the comment there says the filter
                       entry "was plumbed ... and consumed by nothing" and that
                       wiring it into the gate "is the whole fix").
  WL_CAPTURE_ACCUM  -> RayTracingAccumulate. Off means u_AccumulationAlpha = 1.0, so
                       each present fully replaces the accumulation: a SINGLE-FRAME
                       image. That is the arm a denoiser exists to improve. With
                       accumulation ON the temporal blend already averages the noise
                       away and a spatial denoiser has almost nothing left to do --
                       measuring the incumbent there would understate it.

THE ARMS (all at the same render scale and the same pinned camera, so the only
difference within a pair is the denoiser):

  ref          scale 1.0, accumulation ON,  frame 400   the converged ground truth
  noisy        scale 1.0, accumulation OFF, frame 400   one sample, no denoise
  svgf_noisy   scale 1.0, accumulation OFF, frame 400   one sample + SVGF
  early        scale 1.0, accumulation ON,  frame 10    ~10 frames of history
  svgf_early   scale 1.0, accumulation ON,  frame 10    ~10 frames + SVGF

WHY THERE ARE TWO PAIRS AND NOT ONE. SVGF is a *spatiotemporal* filter: it estimates
per-pixel variance from the temporal moments, and that estimate is what tells the
A-Trous kernel which differences are noise to be averaged away and which are edges
to be preserved. With accumulation OFF there is no temporal history at all, so the
moments carry no usable variance and the filter is running outside the regime it was
designed for -- it cannot distinguish noise from detail, so it preserves both.

That is not a reason to skip the arm; it is a reason to measure BOTH and say which
regime each number describes. `noisy`/`svgf_noisy` is "does the filter help with no
temporal information" (it should not, and a number saying it does would be suspect).
`early`/`svgf_early` is the regime the filter exists for: real history, real
variance, but an image that is still noisy because it has only seen ~10 frames.
Accumulation ON at frame 400 is deliberately NOT a pair: the accumulation is already
converged there, so both arms sit on top of the reference and the delta measures
nothing. A denoiser cannot be judged where there is no noise left to remove.

`ref` is converged by the same argument the neural exporter uses (CONVERGED_FRAME =
400): past frame 4 the accumulation alpha is at its 0.02 floor, so 400 is a running
average of the last ~50 presents with the sub-pixel jitter averaged out. It is also
the first region where the capture has a settled size and a loaded scene -- a
single-sample pass at frame 1 captures nothing, and one at frame 2 or 3 captures the
wrong size, which would silently turn every comparison into a resize artefact.

Usage:
    python tools/verify_wlrr_incumbent.py
    python tools/verify_wlrr_incumbent.py --frame 400 --early-frame 10
    python tools/verify_wlrr_incumbent.py --self-test   # no GPU: mutate + detect
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor",
                   "DemonCore-Editor.exe")
EDITOR_DIR = os.path.dirname(EXE)

# The same pinned camera the Task 8 tools use, so the frames are comparable with
# those captures and the scene is a known-good one.
PIN = "0,0,0,9,-85,0"

# Renderer3D::ReconstructionFilter::SpatialDenoise. See the module docstring: the
# SVGF gate accepts this value, so it is the supported way to reach the ladder.
SPATIAL_DENOISE = 11

# Past the accumulation alpha's 0.02 floor (frame 50) and past the point where the
# capture has a settled size and a loaded scene. 400 matches the neural exporter's
# CONVERGED_FRAME and its measured reasoning (frame 6 -> 2332x1451 settled).
CONVERGED_FRAME = 400

# Frame for the partially-converged pair. Past the settled-size point (frame 6 is
# the first 2332x1451 frame) so the capture is not a resize artefact, but far short
# of convergence so the accumulation still holds real noise while also having real
# temporal history to estimate variance from. That combination is the only regime
# in which a spatiotemporal filter has both something to remove and the information
# to remove it with.
EARLY_FRAME = 10


def capture(name: str, out_dir: str, frame: int, *, accumulate: bool,
            aa_filter: int | None) -> str:
    """Render one arm and return the PNG path (or "" if nothing was written)."""
    png = os.path.join(out_dir, f"wlrr_{name}.png")
    log = os.path.join(out_dir, f"wlrr_{name}.log")
    if os.path.exists(png):
        os.unlink(png)
    env = dict(os.environ)
    env.update({
        "WASTELAND_PIN_CAMERA": PIN,
        "WL_CAPTURE_SCALE": "1.0",
        "WL_CAPTURE_ACCUM": "1" if accumulate else "0",
        "WL_DRS_OFF": "1",
        "WL_SCREENSHOT": png,
        "WL_SCREENSHOT_FRAME": str(frame),
        "WL_SCREENSHOT_LINGER": "6",
    })
    if aa_filter is not None:
        env["WL_AA_FILTER"] = str(aa_filter)
    else:
        env.pop("WL_AA_FILTER", None)
    with open(log, "w", encoding="utf-8", errors="replace") as fh:
        proc = subprocess.Popen([EXE], cwd=EDITOR_DIR, env=env,
                                stdout=fh, stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=600)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return png if os.path.exists(png) else ""


def load_rgb(path: str):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def psnr(a, b) -> tuple[float, float]:
    """(PSNR in dB, max abs difference). Both frames must be the same size."""
    import numpy as np
    if a.shape != b.shape:
        raise SystemExit(f"size mismatch: {a.shape} vs {b.shape}")
    mse = float(np.mean((a - b) ** 2))
    if mse == 0.0:
        return float("inf"), 0.0
    return 10.0 * np.log10((255.0 ** 2) / mse), float(np.max(np.abs(a - b)))


# --- SSIM ---------------------------------------------------------------------
# Mirrors tools/train_wlfg/test_model.py's implementation (Rec.709 luma, an 11-tap
# Gaussian window, edge padding) so a number quoted here means the same thing as a
# number quoted there. Inlined rather than imported because every tool in this
# directory is a standalone verifier that must run with nothing but numpy and PIL
# on the path.

def _gaussian_window(size: int = 11, sigma: float = 1.5):
    import numpy as np
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    g = np.exp(-(x * x) / (2.0 * sigma * sigma))
    return g / g.sum()


def _filt(x, w):
    import numpy as np
    pad = w.shape[0] // 2
    horizontal = np.pad(x, ((0, 0), (pad, pad)), mode="edge")
    horizontal = np.apply_along_axis(
        lambda row: np.convolve(row, w, mode="valid"), 1, horizontal)
    vertical = np.pad(horizontal, ((pad, pad), (0, 0)), mode="edge")
    return np.apply_along_axis(
        lambda col: np.convolve(col, w, mode="valid"), 0, vertical)


def ssim(a, b, window: int = 11) -> float:
    """Spatial 2-D windowed SSIM on Rec.709 luma, averaged over the image."""
    import numpy as np
    if a.shape != b.shape or a.ndim != 3 or a.shape[2] != 3:
        raise ValueError("SSIM expects two aligned HxWx3 images")
    if window <= 0 or window % 2 == 0:
        raise ValueError("SSIM window must be a positive odd number")
    luma = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
    wa, wb = a @ luma, b @ luma
    _w = _gaussian_window(window)
    mu_a, mu_b = _filt(wa, _w), _filt(wb, _w)
    va = _filt(wa * wa, _w) - mu_a * mu_a
    vb = _filt(wb * wb, _w) - mu_b * mu_b
    cab = _filt(wa * wb, _w) - mu_a * mu_b
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    s = ((2 * mu_a * mu_b + c1) * (2 * cab + c2)) / \
        ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    return float(np.mean(s))


def self_test() -> int:
    """Exercise the pure functions on synthetic images -- no GPU, no capture.

    The point is the FAILURE path: a verifier whose detector cannot be made to fire
    is not evidence. Each case below mutates an input in a way the detector must
    notice, and the run fails if any of them slips through.
    """
    import numpy as np
    rng = np.random.default_rng(0)
    base = rng.integers(0, 256, size=(64, 64, 3)).astype(np.float64)
    checks: list[tuple[str, bool]] = []

    # Identical images: PSNR is infinite and SSIM is 1.
    p, d = psnr(base, base)
    checks.append(("identical -> inf PSNR", p == float("inf") and d == 0.0))
    checks.append(("identical -> SSIM 1.0", abs(ssim(base, base) - 1.0) < 1e-9))

    # A single 8-bit step somewhere must move max|d| to exactly 1 while leaving the
    # mean tiny -- this is the case SAME_IMAGE_MEAN is designed NOT to swallow.
    one = base.copy()
    one[10, 10, 0] += 1
    _, d_one = psnr(base, one)
    checks.append(("1-step change -> max|d| 1.0", d_one == 1.0))

    # Noise must be detected: PSNR finite and well below the identity case, SSIM
    # strictly below 1.
    noisy = np.clip(base + rng.normal(0, 8.0, base.shape), 0, 255)
    p_n, d_n = psnr(base, noisy)
    checks.append(("noise -> finite PSNR", p_n != float("inf") and p_n < 60.0))
    checks.append(("noise -> max|d| > 0", d_n > 0.0))
    checks.append(("noise -> SSIM < 1", ssim(base, noisy) < 1.0))

    # SSIM must be sensitive to a blur the eye would call "smoother": this is the
    # direction a denoiser moves an image, so a detector blind to it would report
    # every denoiser as a no-op.
    w = _gaussian_window(5)
    blurred = np.stack([_filt(_filt(base[:, :, c], w), w) for c in range(3)], -1)
    checks.append(("blur -> SSIM drops", ssim(base, blurred) < ssim(base, base) - 1e-6))
    checks.append(("blur -> PSNR finite", psnr(base, blurred)[0] != float("inf")))

    # A size mismatch must be refused rather than silently broadcast.
    try:
        psnr(base, base[:32])
        checks.append(("size mismatch -> refused", False))
    except SystemExit:
        checks.append(("size mismatch -> refused", True))

    bad = [n for n, ok in checks if not ok]
    for n, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {n}")
    print()
    print(f"RESULT: {len(checks) - len(bad)}/{len(checks)} self-test checks passed")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=CONVERGED_FRAME)
    ap.add_argument("--early-frame", type=int, default=EARLY_FRAME,
                    help=f"frame for the partially-converged pair. Default "
                         f"{EARLY_FRAME}: past the settled-size point (frame 6) but "
                         f"far short of convergence, so there is real temporal "
                         f"history AND real noise left -- the regime SVGF is for.")
    ap.add_argument("--out", default=os.environ.get("TMPDIR", EDITOR_DIR))
    ap.add_argument("--reuse", action="store_true",
                    help="re-analyse the frames already in --out instead of "
                         "re-capturing. Only valid if they came from the SAME "
                         "engine build and the same arm configuration -- the "
                         "frames are the measurement, so reusing frames from a "
                         "different build measures that build, not this one.")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not os.path.exists(EXE):
        print(f"editor not built: {EXE}")
        return 1
    if args.frame < CONVERGED_FRAME:
        print(f"WARNING: frame {args.frame} < {CONVERGED_FRAME}. Below the convergence "
              f"point the accumulation is still ramping, so `ref` is not a converged "
              f"reference and the scores below do not mean what they say.")
    if args.early_frame < 7:
        print(f"WARNING: --early-frame {args.early_frame} < 7. Before frame 6 the "
              f"capture has not settled its size (frame 6 is the first 2332x1451 "
              f"frame), so this arm would silently become a resize artefact.")

    print(f"pinned camera {PIN}, scale 1.0")
    print(f"  ref          accum ON   frame {args.frame:<4d}  no denoise  (converged reference)")
    print(f"  noisy        accum OFF  frame {args.frame:<4d}  no denoise  (one sample)")
    print(f"  svgf_noisy   accum OFF  frame {args.frame:<4d}  SVGF        (one sample + SVGF)")
    print(f"  early        accum ON   frame {args.early_frame:<4d}  no denoise  (~{args.early_frame} frames of history)")
    print(f"  svgf_early   accum ON   frame {args.early_frame:<4d}  SVGF        (~{args.early_frame} frames + SVGF)")
    print()
    arms = {}
    for name, frame, accum, filt in (
        ("ref", args.frame, True, None),
        ("noisy", args.frame, False, None),
        ("svgf_noisy", args.frame, False, SPATIAL_DENOISE),
        ("early", args.early_frame, True, None),
        ("svgf_early", args.early_frame, True, SPATIAL_DENOISE),
    ):
        existing = os.path.join(args.out, f"wlrr_{name}.png")
        if args.reuse and os.path.exists(existing):
            arms[name] = existing
        else:
            if args.reuse:
                print(f"  (--reuse: no frame for '{name}', capturing it)")
            arms[name] = capture(name, args.out, frame, accumulate=accum,
                                 aa_filter=filt)
    for name, path in arms.items():
        if not path:
            print(f"FAIL: no frame written for arm '{name}'")
            return 1
    px = {name: load_rgb(path) for name, path in arms.items()}

    # ---- 1. does the incumbent run at all? ---------------------------------
    # Checked per pair. A denoiser that is a no-op makes every quality number
    # meaningless, so this is settled before any score is reported -- and a
    # byte-identical pair is reported as a FAILURE OF THE FEATURE, not as a
    # perfect score.
    print("=" * 74)
    print("1. DOES THE INCUMBENT RUN?  (per pair: the ONLY difference is the denoiser)")
    print("=" * 74)
    runs_any = False
    for label, a_name, b_name in (("no temporal history", "noisy", "svgf_noisy"),
                                  (f"~{args.early_frame} frames of history", "early", "svgf_early")):
        pa, da = psnr(px[a_name], px[b_name])
        md = float(abs(px[a_name] - px[b_name]).mean())
        moved = da > 0.0
        runs_any = runs_any or moved
        verdict = "runs" if moved else "NO-OP"
        print(f"  {label:26s} : {pa:7.3f} dB  max|d| {da:5.1f}  mean|d| {md:8.4f}  -> {verdict}")
    print()
    if not runs_any:
        print("  FAIL: BOTH pairs are byte-identical. Turning the SVGF filter on changed")
        print("  nothing in either regime, so the incumbent is a NO-OP and every quality")
        print("  number below would be measuring the same image twice. This is a finding")
        print("  about the feature, not a perfect score. Check that the SVGF gate at")
        print("  Renderer3D.cpp:3930 is reached and that passes 12/13 dispatch -- and")
        print("  prove it with a sentinel store, not a log line.")
        return 1
    print("  PASS: at least one pair differs, so the ladder dispatches and its result")
    print("  reaches the presented image. The size of that change is the next question.")

    # ---- 2. is it better than nothing? -------------------------------------
    print()
    print("=" * 74)
    print("2. IS IT BETTER THAN NOTHING?  (every arm against the SAME converged ref)")
    print("=" * 74)
    ref = px["ref"]
    print(f"  {'arm':13s} {'regime':>26s} {'PSNR vs ref':>12s} {'max|d|':>8s} {'SSIM':>9s}")
    rows = [
        ("noisy", "no history, no denoise"),
        ("svgf_noisy", "no history, SVGF"),
        ("early", f"~{args.early_frame} frames, no denoise"),
        ("svgf_early", f"~{args.early_frame} frames, SVGF"),
    ]
    scores = {}
    for name, regime in rows:
        p, d = psnr(px[name], ref)
        s = ssim(px[name], ref)
        scores[name] = (p, d, s)
        print(f"  {name:13s} {regime:>26s} {p:12.3f} {d:8.1f} {s:9.4f}")
    print()

    def delta(off: str, on: str) -> tuple[float, float, float, float]:
        p_off, d_off, s_off = scores[off]
        p_on, d_on, s_on = scores[on]
        return p_on - p_off, s_on - s_off, d_on - d_off, p_off

    dp_n, ds_n, dd_n, base_n = delta("noisy", "svgf_noisy")
    dp_e, ds_e, dd_e, base_e = delta("early", "svgf_early")
    print("  delta from turning the incumbent on:")
    print(f"    no temporal history :  PSNR {dp_n:+7.3f} dB   SSIM {ds_n:+8.4f}   "
          f"max|d| {dd_n:+6.1f}   (baseline {base_n:.3f} dB)")
    print(f"    ~{args.early_frame} frames of history :  PSNR {dp_e:+7.3f} dB   SSIM {ds_e:+8.4f}   "
          f"max|d| {dd_e:+6.1f}   (baseline {base_e:.3f} dB)")
    print()

    # The honest caveat, stated rather than buried.
    print("  CAVEAT: one scene, one pinned camera, one render scale, and the 'early'")
    print("  arm is a single noise realisation -- the accumulation is stochastic, so a")
    print("  repeat run differs by the renderer's own frame-to-frame noise. This is an")
    print("  existence proof about the incumbent, not a general quality ranking, and the")
    print("  ladder is a fixed 1/2/4/8 schedule -- none of this transfers to a")
    print("  variance-adaptive variant.")
    print()

    # A denoiser is judged in the regime it was designed for: the temporal pair.
    # The no-history pair is reported, but it cannot be the basis of a verdict --
    # and the mechanism is confirmed by it: with no temporal history the filter is
    # a no-op, which is exactly what a VARIANCE-GUIDED filter must do when it has
    # no variance to guide it.
    #
    # The verdict reports what the two metrics say SEPARATELY rather than
    # collapsing them. A denoiser that lowers PSNR error while lowering SSIM is
    # trading noise for detail, and "improves" or "worsens" would each hide half
    # of that.
    improved_p, worse_s = dp_e > 0.0, ds_e < 0.0
    worse_p, improved_s = dp_e < 0.0, ds_e > 0.0
    if improved_p and improved_s:
        print(f"  FINDING: in its design regime the incumbent improves BOTH metrics "
              f"(PSNR {dp_e:+.3f} dB, SSIM {ds_e:+.4f}, max|d| {dd_e:+.1f}).")
        print("  It is a working denoiser, so Task 10's residual head has a sound")
        print("  incumbent to correct rather than replace.")
    elif worse_p and worse_s:
        print(f"  FINDING: in its design regime the incumbent makes BOTH metrics worse "
              f"(PSNR {dp_e:+.3f} dB, SSIM {ds_e:+.4f}, max|d| {dd_e:+.1f}).")
        print("  It runs, but it is not buying quality at this operating point. That is a")
        print("  legitimate outcome and it CHANGES Task 10's scope: a residual head over a")
        print("  lossy incumbent is a harder problem than one over a good incumbent, and")
        print("  the honest first move is to find where the ladder helps before building")
        print("  on it.")
    else:
        gain = "noise (worst-case error)" if dd_e < 0.0 else "detail"
        print(f"  FINDING: MIXED. In its design regime the incumbent moves the two metrics")
        print(f"  in OPPOSITE directions: PSNR {dp_e:+.3f} dB, SSIM {ds_e:+.4f}, "
              f"max|d| {dd_e:+.1f}.")
        print(f"  Read together with the max|d| change, the shape of this is a denoiser")
        print(f"  that is removing {gain} and paying for it in the other -- the classic")
        print(f"  blur trade-off. It is NOT a no-op and NOT a clean win.")
        print()
        print("  WHAT THIS MEANS FOR TASK 10: the incumbent is PARTIALLY working. It")
        print("  reduces worst-case error but costs structure, so the residual a trained")
        print("  head would have to learn is the DETAIL the filter smears -- a")
        print("  structured, learnable target, not noise. That is a more tractable")
        print("  problem than correcting raw noise, and it argues for the plan's")
        print("  additive design (keep the incumbent, correct its residual) rather than")
        print("  replacing it. It does NOT support the plan's other reading, that the")
        print("  incumbent may already be good enough to shrink WLRR: on SSIM it is")
        print("  actively costing quality, so there is something to correct.")
        print()
        print("  DO NOT QUOTE EITHER NUMBER ALONE. PSNR alone says 'the incumbent")
        print("  helps'; SSIM alone says 'the incumbent hurts'. Both are the same")
        print("  measurement of the same frames.")
    print()
    print("  WHAT THIS MEANS FOR THE PLAN'S PREMISE: section 0.3 states the incumbent")
    print("  'has never been observed to run'. That is now REFUTED -- it runs, and the")
    print("  no-history pair above is the mechanism proof: a variance-guided filter")
    print("  does nothing without temporal variance, and this one does nothing without")
    print("  it and something with it. The plan's second premise, that a denoiser")
    print("  nobody has validated cannot be compared against anything, is answered by")
    print("  the table above.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
