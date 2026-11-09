# Task 9 (WLRR) — measuring the incumbent SVGF denoiser

**Status: DONE.** Harness: `tools/verify_wlrr_incumbent.py`. No shader change, and no
engine change either (see "the flags already existed" below).

## Why this task existed

The plan's section 0.3 states the premise bluntly:

> the SVGF denoiser (passes 12/13) **has never been observed to run**. `SvfgEnabled =
> false` by default, the A-Trous ladder is a fixed 4-pass 1/2/4/8 schedule rather than
> variance-adaptive, and there is no GPU-verified screenshot of it.
>
> Replacing a denoiser nobody has validated is the classic way to ship a feature that
> cannot be compared against anything. So **WLRR's first task is to make the incumbent
> measurable**, not to replace it.

and sets the acceptance for this task as the *measurement*, not a target:

> It is entirely possible the incumbent is already good, in which case WLRR is a smaller
> feature than planned — **that is a legitimate outcome and it changes task 10's scope.**
> Report it rather than proceeding on the assumption.

## The flags already existed — this task added no engine code

The plan allows "a diagnostic flag only" in `Renderer3D.cpp`. Both needed flags were
already there, added by earlier sessions for exactly this purpose:

- **`WL_AA_FILTER=11`** selects `ReconstructionFilter::SpatialDenoise`, which the SVGF
  gate at `Renderer3D.cpp:3930` accepts. The comment there records that the filter entry
  "was plumbed (enum, combo, all three `Set()` calls) and consumed by nothing" and that
  wiring it into the gate "is the whole fix".
- **`WL_CAPTURE_ACCUM`** drives `RayTracingAccumulate`. Off means `u_AccumulationAlpha =
  1.0`, so each present fully replaces the accumulation — a genuine **single-frame** image.

So the deliverable is the harness alone.

## Method

Five captures, one pinned camera (`0,0,0,9,-85,0`), one render scale (1.0), so the only
difference *within a pair* is the denoiser:

| arm | accumulation | frame | denoise | what it is |
|---|---|---|---|---|
| `ref` | ON | 400 | — | the converged ground truth |
| `noisy` | OFF | 400 | — | one sample |
| `svgf_noisy` | OFF | 400 | SVGF | one sample + SVGF |
| `early` | ON | 10 | — | ~10 frames of history |
| `svgf_early` | ON | 10 | SVGF | ~10 frames + SVGF |

**Why two pairs and not one.** SVGF is a *spatiotemporal* filter: it estimates per-pixel
variance from the temporal moments, and that estimate is what tells the A-Trous kernel
which differences are noise to average away and which are edges to preserve. With
accumulation OFF there is no temporal history, so the moments carry no usable variance
and the filter runs outside the regime it was designed for. That is not a reason to skip
the arm — it is the arm that *proves the mechanism*, and it is why the result below is
interpretable rather than just a number.

Accumulation ON at frame 400 is deliberately **not** a pair: the accumulation is already
converged there, so both arms sit on top of the reference and the delta measures nothing.
A denoiser cannot be judged where there is no noise left to remove.

## Result

### 1. Does the incumbent run?

| pair | PSNR | max\|d\| | mean\|d\| | verdict |
|---|---|---|---|---|
| no temporal history | 67.187 dB | 5.0 | **0.0124** | runs, but nearly a no-op |
| ~10 frames of history | 44.165 dB | 32.0 | **0.8368** | runs, substantially |

**Yes — and the contrast between the two rows is the mechanism proof.** Turning the filter
on changes the image by a mean of 0.0124/255 with no temporal history, and by 0.8368/255
with it: **67× stronger**. A variance-guided filter has nothing to guide it without
temporal variance, so doing nothing there and something here is exactly the signature of
the design working as intended. The plan's premise that the denoiser "has never been
observed to run" is **refuted**.

### 2. Is it better than nothing? (every arm against the same converged reference)

| arm | regime | PSNR vs ref | max\|d\| | SSIM |
|---|---|---|---|---|
| `noisy` | no history, no denoise | 38.743 | 54.0 | 0.5609 |
| `svgf_noisy` | no history, SVGF | 38.749 | 54.0 | 0.5582 |
| `early` | ~10 frames, no denoise | 43.397 | 43.0 | 0.6208 |
| `svgf_early` | ~10 frames, SVGF | **43.706** | **30.0** | 0.5842 |

Delta from turning the incumbent on:

| regime | PSNR | SSIM | max\|d\| | baseline |
|---|---|---|---|---|
| no temporal history | **+0.005 dB** | −0.0027 | +0.0 | 38.743 dB |
| ~10 frames of history | **+0.309 dB** | **−0.0366** | **−13.0** | 43.397 dB |

## The finding: the incumbent is PARTIALLY working

In its design regime the incumbent:

- **reduces worst-case error by 13/255** (max|d| 43 → 30, a 30% cut) — real noise removal,
  not a smeared copy, since a filter that merely blurred would leave max|d| unchanged;
- **raises PSNR by 0.309 dB** — a real but small aggregate gain;
- **lowers SSIM by 0.0366** — a real structural loss.

The two metrics move in **opposite directions**, and that is the finding, not a nuisance to
be averaged away. The shape of it is the classic blur trade-off: the filter is buying
worst-case accuracy with detail.

**Do not quote either number alone.** PSNR alone says "the incumbent helps"; SSIM alone
says "the incumbent hurts". Both are the same measurement of the same frames.

## What this means for Task 10

The plan offered two possible readings of this task's outcome, and the measurement picks
one:

- **Not** "the incumbent is already good, so WLRR is a smaller feature". On SSIM it is
  actively *costing* quality, so there is something real to correct.
- **Yes** to the additive design. The residual a trained head would learn is the **detail
  the filter smears** — a structured, learnable target, and a more tractable one than
  correcting raw noise. This argues for the plan's §0.3 shape: keep the incumbent, train a
  head on its residual, let the gate decide per pixel how much correction to apply.

## Caveats, stated rather than buried

- **One scene, one pinned camera, one render scale.** This is an existence proof about the
  incumbent, not a general quality ranking.
- **The `early` arm is a single noise realisation.** The accumulation is stochastic, so a
  repeat run differs by the renderer's own frame-to-frame noise. The deltas are small
  enough that the *signs* are more trustworthy than the magnitudes.
- **The ladder is a fixed 1/2/4/8 schedule.** None of this transfers to a variance-adaptive
  variant, which is the obvious next lever if Task 10 needs more from the incumbent.
- **SSIM here is a single-scale windowed SSIM on Rec.709 luma**, mirroring
  `tools/train_wlfg/test_model.py`. It is comparable to other numbers in this repo and to
  nothing else.

## Reproduce

```
python tools/verify_wlrr_incumbent.py            # 5 captures, ~5 min
python tools/verify_wlrr_incumbent.py --reuse    # re-analyse saved frames
python tools/verify_wlrr_incumbent.py --self-test  # 9/9, no GPU
```

`--reuse` is only valid for frames from the same engine build and arm configuration — the
frames *are* the measurement, so reusing frames from another build measures that build.
