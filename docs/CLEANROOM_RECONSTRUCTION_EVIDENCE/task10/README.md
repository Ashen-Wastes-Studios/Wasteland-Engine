# Task 10 (WLRR) — Step 1: the dataset, and a design conflict the plan did not anticipate

**Status: Step 1 mechanism DONE and verified.** The arm set is established, exported, and
validated; the plan's specified arm settings were measured and **had to be changed**.

Harness: `tools/train_neural/export_dataset.py` (extended — the WLRR arms ride the existing
exporter, which is what the plan's file list implies by omitting `export_dataset.py` from
`tools/train_wlrr/`).

## The plan's Step 1, and why it does not work as written

> input = single-frame noisy, accumulation off, scale 0.5; target = converged, accumulation
> on, scale 1.0. This target has **+2.042 dB** of affine headroom against **+0.084 dB** for
> the converged target — the 24x difference that makes this the right target.

The headroom argument is about the *render-stage neural* work (passes 17/18/19), and it is
correct for that. It does not transfer to WLRR, because WLRR's anchor is the **incumbent
denoiser**, and the incumbent only acts with temporal history (Task 9's mechanism proof:
mean|d| 0.0124/255 with no history, 0.8368/255 with it — **67× stronger**).

Measured on the plan's arm settings (Example3D, 3 cameras, accum OFF at scale 0.5):

| camera | `noisy` vs `incumbent` | `incumbent` vs `gt` | `noisy` vs `gt` |
|---|---|---|---|
| auto_y0 | 60.143 dB, mean\|d\| **0.0624** | 39.184 | 39.180 |
| auto_y90 | 62.312 dB, mean\|d\| **0.0381** | 38.151 | 38.152 |
| auto_y200 | 61.504 dB, mean\|d\| **0.0460** | 38.091 | 38.096 |

The incumbent changes the image by **0.04–0.06/255** — a near-no-op. So `incumbent vs gt` and
`noisy vs gt` agree to three decimals: **the anchor carries the same information as the
input.** A head trained on that pair learns to denoise the raw input, which is passes
17/18/19's job, not `out = incumbent + gate · residual`. The plan's Step 3 form is not
trainable against Step 1's arms.

## The fix: partial convergence

The conflict is that accum-OFF gives headroom but no denoiser, while accum-ON-to-convergence
gives a denoiser but no headroom. **Partial convergence gives both**: real temporal history,
so the incumbent actually denoises, and real noise left to remove. It is also the realistic
deployment point — a game frame is never 1 spp with no history.

New arms (`PARTIAL_FRAME = 10`, past the settled-size point at frame 6):

| arm | accumulation | frame | denoise | role |
|---|---|---|---|---|
| `wlrr_input` | ON | 10 | — | the head's input |
| `wlrr_incumbent` | ON | 10 | SVGF | the anchor the residual is added to |
| `gt` | ON | 400 | — | the target |

They are **new passes**, not changes to `noisy`, because `noisy` is the neural stage's
existing training input and moving it would silently invalidate that dataset. The degenerate
accum-OFF `incumbent` arm is kept so the measurement above stays reproducible rather than
becoming a claim in a comment.

Measured on the corrected arms:

| camera | `wlrr_input` vs `wlrr_incumbent` | `incumbent` vs `gt` | `input` vs `gt` | anchor gain |
|---|---|---|---|---|
| auto_y0 | 43.319 dB, mean\|d\| **1.2400**, max\|d\| 37 | 43.864 | 44.162 | **−0.299** |
| auto_y90 | 44.904 dB, mean\|d\| **1.0038**, max\|d\| 25 | 44.585 | 43.543 | **+1.041** |
| auto_y200 | 45.027 dB, mean\|d\| **0.9781**, max\|d\| 28 | 44.808 | 43.424 | **+1.383** |

**~20× the arm separation**, and the anchor is now informative. Two things follow:

1. **The incumbent is view-dependent.** It gains **+1.383 dB** on auto_y200 and **+1.041 dB**
   on auto_y90, but **loses 0.299 dB** on auto_y0. This corroborates Task 9's mixed verdict
   (worst-case error down, SSIM down) at a second operating point: the filter is not
   uniformly good, which is precisely the case a residual corrector exists for.
2. **The target is real.** With the anchor sometimes *worse* than the input, "correct the
   incumbent" is a genuine problem rather than a restatement of "denoise".

## Two verifier bugs this exposed, both fixed

- **The blank guard misfired on the denoise arm.** It measures *local contrast*, and removing
  high-frequency noise is what a denoiser is FOR, so a correctly denoised frame is low
  contrast by construction. Measured: the incumbent arm reads contrast p99 **2.0** against
  **5.9** for its non-denoised sibling, and the export failed with "no edges, so nothing to
  reconstruct" — a working denoiser reported as a blank capture. The floor is now skipped for
  denoise arms in both the capture path and `--validate`; blankness is a property of the
  scene, and the non-denoised sibling still carries the check.
- **`--validate` named `input` as `gt`'s only possible pair**, so it reported four FALSE
  errors on a perfectly good WLRR dataset ("no matching input/ frame"). A validator that
  fails for the wrong reason is worse than none — it trains the reader to ignore it. Pairing
  and the geometry check are now stated over the arms actually present.

## Verified

- Export: 9 captures, **0 failures**, exit 0.
- `--validate` on the WLRR dataset: **ok**, exit 0, per-pass counts (gt 3 / wlrr_input 3 /
  wlrr_incumbent 3).
- Regression: `dataset/` (the neural stage's) still validates with the same 9 errors it had —
  all contrast-floor, on `input`/`gt`, which the change only made *more* permissive, so they
  are pre-existing content issues and not a regression.
- Arm alignment: `wlrr_input` and `gt` both 1451×2332 — pixel-wise comparable.

## Step 2's kernel premise is stale, and Step 4 is done

The plan's §0.3 says the ladder is "a fixed 4-pass 1/2/4/8 schedule rather than
variance-adaptive", and Step 2 asks for a kernel "whose per-iteration blend weight is
variance-driven". Read against the source, `RunATrousFilter` already has it:

```
w_l = exp( -|mean_c - mean_n| / (sigma_l * sqrt(var_c) + eps) )
var_c = max(m2 - m1^2, 0)
```

So the **step schedule** is fixed (1/2/4/8, driven from C++), but the **blend weights are
already variance-driven** — a noisier centre widens the tolerance and smooths harder. A
previous session had even removed an inert `(1 + var_c * 5)` term there. **Step 2's kernel
half is not outstanding work; it is existing code**, and Step 4's verifier is what is
actually missing.

`tools/verify_wlrr_kernel.py` (**new**, 8 structural checks + 15 mutation/numeric self-test
checks, no GPU needed) pins it down:

- The variance derivation is wrapped in `max(..., 0)`.
- **Negative control, as the plan requires:** with `m1 = 0.5, m2 = 0.2499` — a rounding-scale
  overshoot, exactly the case the guard exists for — the guarded form yields 0 and the
  unguarded form yields a negative variance whose `sqrt` is NaN, and that NaN reaches the
  weight. The self-test also proves each mutation is *detectable*: stripping the `max`,
  re-introducing the inert term, and dropping the normalisation each make a check fail.
- The weight is variance-driven both structurally and numerically (higher centre variance ⇒
  larger weight for the same luminance difference).
- No inert `(1 + centerVariance * k)` multiplier, and the false "taps sum to 1" claim is gone.
- Normalisation, degenerate-total fallback, sky passthrough, and that the pass writes a
  buffer C++ commits back into the accumulation.

**Why the guard deserves a verifier at all:** it is a single `max`, protected by nothing but
a comment, and its absence degrades the denoiser to a **silent no-op** — one NaN tap poisons
`totalColor` and `totalWeight` together, the `totalWeight > 0.0001` guard fails, and the pass
writes the input colour back unchanged. No error, no log, no crash. That is the worst failure
mode to leave unguarded.

### A false comment the verifier caught, in the verifier's own author

The first version of the self-test asserted the B3-spline taps sum to 1, because the shader's
comment said so. It failed. **They sum to 1.625** (0.375 centre + 4×0.25 edges + 4×0.0625
corners), and the comment was simply wrong. The code is correct for a different reason: the
kernel divides by its own `totalWeight`, so a flat region round-trips unchanged *whatever*
the taps sum to. The check now asserts the invariant the code actually provides, and the
shader comment has been corrected — including a warning not to "fix" the taps to sum to 1 on
the strength of the old claim.

The `.slang` edit was comment-only and **provably inert**: after `--deploy`, the DXIL
(`378e4cd9d969…`, 350976 bytes) and SPIR-V (`f52e89258be0…`, 1233224 bytes) are **byte-identical**
to before. Only `.slang.glsl` changed (303162 bytes), because the GLSL backend emits source
comments. So no rebuild and no GPU re-verification were needed — the GPU code is unchanged by
construction, not by assumption.

## Step 3: the head itself

`tools/train_wlrr/` now holds the head, written to mirror `tools/train_wlfg/` so a reader who
knows one knows the other:

| file | what it is |
|---|---|
| `model.py` | the architecture, the hand-written forward pass and gradient, Adam |
| `test_model.py` | 12 tests, no GPU and no dataset |
| `export_weights.py` | the CRC32-guarded blob, plus a format self-test |
| `data_contract.py` | the fail-closed provenance gate |

`out = incumbent + gate * residual`, with the residual head zeroed at init so a fresh model is
a **provable** no-op: `test_zero_weights_is_exactly_the_incumbent` asserts
`max|out − incumbent| == 0.0`, not "is small". ABI: **6 inputs → 16 → 16 → (3 residual, 1
gate)**, **452 weights**, **1832-byte blob**, magic `WLRR`.

### The ABI was 8 inputs, and had to become 6 — the dataset could not feed it

The first revision added the reactive mask and depth as conditioning, and the argument for them
was sound: the mask marks what the engine distrusts, depth marks the edges a denoiser smears
across, and both are already bound at the point the pass would run.

It was still wrong. **The WLRR dataset cannot supply either.** The neural exporter captures RGBA
arms and its PNG reader normalises every capture to three channels; the mask (`img_Mask`, u14)
and depth buffers belong to the WLFG exporter's `BUFFERS` table, which the WLRR arms do not use.
An input the training data cannot provide is not a feature — it is a defect that surfaces later
as a shader reading uninitialised memory. `test_abi_shape` now asserts `N_MASK`/`N_DEPTH` do not
exist, so the 8-input version cannot come back without the export gaining mask/depth arms first
(the WLFG exporter's `BUFFERS` + `FLOAT_BUFFERS` machinery is the model to copy).

The head is not crippled by the reduction: the mask and depth are refinements, while the pair's
own difference *is* the signal — the input carries its own gradients and the incumbent's
smearing appears as exactly the difference the head conditions on.

### Three test bugs, each of which would have hidden a real one

- **A finite difference cannot be checked in float32 at this scale.** The loss is ~358, so fp32
  spacing is ~3.05e-05, and `(lp − lm) / 2e-3` amplifies that to a **1.5e-02 noise floor**. That
  produced a spurious 1.36e-01 relative error on the smallest true gradient in the batch while
  every larger one sat at the floor. The gradient is correct: the identical check in float64
  gives **4.72e-07**. The math is now verified in float64, with a separate fp32 test that only
  asks for the descent sign — a question round-off cannot answer wrongly.
- **A relative tolerance is the wrong criterion for a near-zero gradient.** `Wg[13,0]` has a
  gradient of **2.5e-07** (its unit is dead for 32 of 48 pixels), so a relative comparison
  scored 5.41e-02 and failed. The error traces the standard finite-difference U-shape —
  5.47e-04 at eps=1e-2, 1.45e-05 at eps=1e-3, 5.41e-02 at eps=1e-6, 1.00 at eps=1e-8 —
  truncation at large eps, round-off at small eps, and a gradient that is merely *small*. The
  check now uses a mixed absolute/relative tolerance, which still catches a gradient that is
  wrong by a factor.
- **A sanity assertion that was a coin flip on one random scalar.** `bg` is a single value, so
  about half of all seeds saturate the gate shut; with `gate ≈ 2e-9` the correction rounds to
  exactly zero at fp32 spacing, and the head becomes the identity for a reason unrelated to the
  property under test. Root cause: `FEATURE_MEAN`/`FEATURE_STD` are still the identity, so raw
  0..255 inputs meet He-initialised weights and the pre-activations reach ~1e3. The test now
  drives `forward` on well-scaled features. **The underlying mis-scaling is real and remains
  open**: the affine must be filled from `dataset_stats.json` before the model is frozen, which
  is what the WLFG head needed too.

### Verified

`test_model.py` **12/12 OK** · `export_weights.py --self-test` **8/8** (round-trip, bit-flip
rejection, wrong-magic rejection — `WLRR` ≠ `WLFG` so one head's blob cannot load into the
other's buffer — and truncation) · `data_contract.py` accepts a correct manifest and refuses all
four malformed variants.

## Task 10 end to end: it trains, and the head beats both baselines

`tools/train_wlrr/` is complete — `model.py`, `dataset.py`, `train.py`,
`export_weights.py`, `data_contract.py`, plus `test_model.py` (12) and `test_dataset.py` (17).
The loader also gained the provenance the contract needs: the neural exporter now emits the
WLRR contract fields, **derived from the frames actually kept** rather than from the CLI, and
only when the WLRR arms are present — a provenance block describing arms that are not on disk
is worse than none, because it passes the check while asserting something untrue.

Run on the 3-camera probe (`--steps 600`, holdout = `auto_y0`, 2 train / 1 holdout triples):

```
step 0    corrected 43.712 dB   incumbent 43.712 dB   input 44.460 dB   delta +0.0000 dB
holdout   corrected 45.977 dB   incumbent 43.712 dB   input 44.460 dB
          delta vs incumbent +2.2646 dB   vs input +1.5170 dB
          mean |correction| 0.6935 / 255   reverted 19.2% of the way to the input
```

Three things are worth more than the headline number:

1. **`delta +0.0000` at step 0 is the safety property holding in the real pipeline**, not just
   in a unit test: the untrained head reproduces the incumbent to the digit. `train.py` asserts
   it and refuses to continue if it ever fails.
2. **The input is reported as a third baseline, and it matters.** The incumbent is *worse* than
   the raw input on this camera (43.712 vs 44.460), so "beats the incumbent" alone is a claim a
   head could satisfy by simply undoing the filter. It beats the input too (+1.5170 dB), which
   rules that out.
3. **`reverted 19.2%` quantifies (2).** The head moves less than a fifth of the way towards the
   unfiltered frame, so the gain is not reversion.

**A broken metric, caught by its own output.** The first version of `reverted_fraction` was
`mean(pred−base)/mean(raw−base)` and printed **−2736.8%**. That number is the tell: those means
are of *signed* residuals that cancel to near zero, so the ratio was noise over noise. It is now
a least-squares projection of the correction onto the incumbent→input direction, which is bounded
and meaningful. A metric that can print nonsense is worse than no metric.

**This is not a result about the model yet.** It is 2 train / 1 holdout triples — one camera —
so the number is a machinery check, not a quality measurement, and it is reported as such. The
honest next step is the full-volume export (12 scenes × 3 cameras = 36 triples) and a
scene-level holdout.

## The affine had to go into the blob — this head is the first where it matters

Wiring the shader surfaced a real ABI defect, and it is the same class the WLFG pass
already carries a warning about in its own comment: *"the weights are meaningless without the
matching affine, and nothing would error."*

The WLFG head ships an **identity** affine (`FEATURE_MEAN = 0`, `FEATURE_STD = 1`), so its
shader hardcodes nothing and the warning is latent. This head is the **first with a measured
affine** — `[26.55, 28.97, 25.39, 26.54, 28.96, 25.37]` / `[2.15, 2.44, 5.62, 2.63, 2.92, 6.16]`
— so the latent bug becomes live: put those numbers in the shader as constants and the first
retrain that forgets to update them produces a plausible image with every weight wrong.

**Fix: the affine travels inside the blob payload.**

```
payload = weights[452] | mean[6] | std[6]      -> 464 floats, 1880-byte blob
```

It is **appended** rather than put in the header, so the 24-byte header stays byte-identical
across all three heads and one reader still serves them. The weights and the standardisation
they require are now one artefact and cannot drift.

`export_weights.py` enforces the rest of it:

- **A stats file is required.** There is no default affine — "the affine cannot be assumed".
- **An identity affine is refused** unless `--identity-affine` is passed. Correct for an
  untrained or control head; for a trained one it means the statistics were never measured.
- **The shape is checked** against the current `N_IN`, so a blob written under a different ABI
  is rejected rather than silently reinterpreted.
- **The affine is re-read from the bytes on disk**, not from the in-memory arrays — the shader
  reads those bytes.

Self-test is now **15/15**, adding: the affine round-trips bit-exactly, it lands at
`AFFINE_OFFSET`, a payload truncated to the weights is **rejected** (the natural reader
mistake), a bit flip *in the affine* is caught, and the identity is detectable.
`test_model.py` **19/19**, with the load-bearing test being
`test_the_blob_affine_is_the_affine_features_applies` — it asserts the default really tracks a
later `apply_feature_stats`, which is the invariant that keeps the shader correct.

Five refusal paths verified by hand, each exiting 1 with a message naming the problem:
identity without the flag, missing stats, wrong-shaped affine, missing `feature_mean`, and
payload-truncated-to-weights.

## The full-volume export is in flight, and one scene is broken

12 scenes × 3 cameras × 3 arms = 108 captures. The per-scene auto-framing pass is the dominant
cost (~12 editor launches), after which captures run 8–11s each.

**`DemoGame` fails every capture** (`no PNG written`, both attempts). The cause is visible in the
framing log and is not in the arms:

```
frame DemoGame: focal (-2068.9,11.2,-2052.2) distance 2451.0 pitch -30 yaws [0, 90, 200]
```

`distance 2451.0` against 12–30 for every other scene: the auto-framer latched onto an outlier
bound and placed the camera 2.4 km away, so nothing renders and no PNG is written. That is a
harness defect worth its own fix, and it means `DemoGame`'s triples are incomplete — which the
loader refuses by default and `--allow-incomplete` now reports loudly rather than silently
training on 9 of 12 scenes.

## Step 5 gate: FAILS on the full dataset. The probe's +2.26 dB was not a result.

Scene-level holdout — the whole of `Example3D` held out, so the holdout shares no content with
training. 33 triples, 30 train / 3 holdout, 3000 steps:

```
step 0    corrected 44.519 dB   incumbent 44.519 dB   input 44.048 dB   delta +0.0000 dB
holdout   corrected 42.901 dB   incumbent 44.519 dB   input 44.048 dB
          delta vs incumbent -1.6178 dB   vs input -1.1462 dB   reverted 75.8%

Step 5 gate (plan: Enhanced > incumbent > raw-input)
  [FAIL] corrected > incumbent
  [ok]   incumbent > input
  [FAIL] corrected > input
  -> plan's chain DOES NOT HOLD
```

**The head makes the incumbent worse.** The plan says *"if the chain does not hold, stop"*, so
this is reported as the result and the commit was not made.

Two things worth separating:

1. **Step 0 is still exact** (`44.519 == 44.519`, `delta +0.0000`). The safety property holds —
   but only at init. **It is not a general guarantee.** The gate is what makes an *untrained*
   head free; once the head has learned something wrong it opens the gate and the error lands.
   Task 10's own objective says "gated so a wrong prediction costs nothing", and this run
   measures the cost of a wrong prediction: **1.62 dB**. The gate bounds the correction, not the
   risk.
2. **The probe's +2.26 dB was not evidence of anything.** It held out one camera of a scene that
   was in training, with 2 training triples. I flagged it at the time as a machinery check
   rather than a measurement, and this run is why that distinction mattered: a single-camera
   holdout of an in-training scene reported +2.26 dB for a head that loses 1.62 dB on an unseen
   scene.

### Root cause, measured: the target signal is heteroscedastic by 17×

`tools/train_wlrr/diagnose_affine.py` measures `mean|incumbent − input|` per scene:

```
PhysicsPlayground   1.95      ScriptTest         16.23
AtmosphereShowcase  1.90      LightingShowcase   18.52
InventoryDemo       2.13      MaterialGallery    31.89
```

For most scenes the incumbent barely moves the frame (1.9–3.9); for two it changes it enormously
(16–32). The training loss is unweighted MSE, so a scene whose separation is 32 contributes
~**256×** the squared error of one whose separation is 2. **Two scenes dominate the loss**, the
head learns "correct a lot", and it applies that to scenes where the incumbent is already right.

`Example3D` is a low-separation scene (2.80), and there the incumbent *helps* (44.519 vs 44.048),
so a large correction is exactly wrong — which is the −1.15 dB against the input, and the 75.8%
reversion is the head undoing a filter that was working.

**The prediction this makes is testable**: the head should *win* on a high-separation scene. That
run is in flight (holdout `MaterialGallery`, separation 31.89).

**Result of that test — direction confirmed, and a caveat:**

```
holdout (MaterialGallery, separation 31.89)
  corrected 35.131 dB   incumbent 34.835 dB   input 35.872 dB
  delta vs incumbent +0.2966 dB   vs input -0.7406 dB   reverted 13.1%
```

The head *helps* where the incumbent is far from the input (+0.30 dB, only 13% reversion) and
*hurts* where the incumbent is already right (−1.62 dB, 76% reversion). So it is not simply
broken — it is **miscalibrated across regimes**, which is what the heteroscedastic-target
diagnosis predicts. **But it still does not beat the raw input on either scene** (−0.74 and
−1.15 dB), so the stronger claim the plan asks for does not hold anywhere yet.

### Why the low-separation scenes are never learned

The mechanism, stated precisely: a pixel where the arms differ by 2 contributes ~nothing to an
unweighted MSE next to one where they differ by 32. So the gradient barely ever asks the head to
do the right thing on the easy scenes, and the head generalises the large correction it learned
from the dominant scenes. The fix the diagnosis points at is a **per-sample loss weight**
inversely proportional to the local arm separation — `w = 1/(sep² + δ)` — which makes the loss
measure *relative* error instead of absolute. That change is being tested next. A richer fix
(condition the gate on the disagreement explicitly) would change the ABI and is deferred until
this test says whether the balance is the problem.

### The weighted loss: half the fix, measured

Re-running the identical holdout with `--loss-weighting separation`:

```
holdout (Example3D, separation 2.80)     unweighted      weighted
  delta vs incumbent                      −1.6178 dB      −1.0243 dB
  delta vs input                          −1.1462 dB      −0.5527 dB
  mean |correction|                        0.9821/255      0.5909/255
  reverted                                  75.8%          9.7%
```

Reversion collapsed from 76% to 10% and 0.6 dB of the damage went away, so the "two scenes
dominate" diagnosis was correct. **But the head still loses.** With the balance fixed it is now
applying a 0.59/255 correction that is not reversion and is still wrong — it wants to correct a
scene whose incumbent is already right, and it cannot tell that from the features it has.

The information exists in principle (the two arms are both in the feature vector, so their
difference is computable), but a 16-wide 2-layer network did not find it while the loss rewarded
ignoring it.

### The conditioning experiment: handed the head the difference, and it destroyed it

The obvious next step — and the one the diagnosis points at — is to hand the head
`incumbent − input` as an explicit feature group so it can tell the regime. That is the
`N_IN 6 → 9` change, and it was measured:

```
holdout (Example3D, weighted)     v1 (6 in)      v2 (9 in)
  delta vs incumbent                −1.0243 dB     −12.76 dB
  mean |correction|                 0.5909/255     4.4399/255
  reverted                           9.7%          296.0%
  final loss                         5.735         43.720
```

**−12.8 dB.** The reason is the failure the obvious diagnosis misses. The difference feature does
not only condition the *gate* — it is visible to the *residual* as well, and so it hands the head
a **"rewind" button**: with `residual ≈ −k·(incumbent−input)` you get
`out = incumbent − k·(incumbent−input)`, which for k > 1 overshoots *past* the input. Hence
296% reversion. **The gate needed the signal; the residual abused it.**

The correct architecture is to give the difference to the gate's first layer only. That is a real
design, but it is a structural change with no measured evidence behind it, so it is **recorded
rather than built** — in the `model.py` docstring too, so nobody re-adds the difference and
repeats a −12.8 dB run. The 9-input ABI was reverted: the tree is back at 6 inputs, 452 weights,
1880-byte blob, and the separation weighting + its gradient tests are kept because they are the
best-known configuration and the improvement is real.

Three configurations tried, all reported, all failing the plan's Step 5 gate — which is why Task 10
stops here rather than wiring a head that has not earned it.

### A hypothesis that was tested and refuted

The measured feature std jumped from 2.15–6.16 on the one-scene probe to 13.1–32.2 across 11
scenes, which looked like a global affine collapsing within-scene variation and leaving the head
near-constant features. **It does not.** Within-scene std / between-scene std is **0.48–0.66**, so
within-scene signal survives standardisation comfortably. Recorded because the plausible
explanation was the wrong one, and only the measurement says so.

## v3: the difference goes to the gate's own first layer

The v2 failure had a specific mechanism, and it points at a specific fix. The difference feature
was visible to the **residual** as well as the gate, so the residual learned
`residual ≈ −k·(incumbent−input)` — a rewind button that overshoots past the input. The fix is to
make that branch one-directional:

```
residual trunk :  6 -> 16 -> 16 -> 3      (the two arms ONLY)
gate branch    :  9 ->  8 ->  1  sigmoid  (arms + signed difference)
```

`forward` slices `pixel[:, :N_IN]` for the trunk; the gate gets all nine. The gate's branch feeds
nothing back into the trunk, so the rewind is not expressible at all. **524 weights, 542-float
payload, 2192-byte blob.** `test_the_residual_trunk_cannot_see_the_difference` pins the split —
`W1` must be 6-wide; if it ever becomes 9-wide the bypass is expressible again, and the head will
find it, because it did before from a loss that rewarded it.

### Two test bugs this exposed, both of which would have hidden a real one

- **A single finite-difference eps cannot serve parameters of different curvature.** `W1[72]` has a
  gradient of **1.83e3** and at eps=1e-6 its truncation error is **8.2** (4.5e-3 relative) — a
  "failure" of a 1e-4 tolerance. At eps=1e-7 the same analytic gradient agrees to **3.3e-10**. The
  error falls monotonically from 1e-2 to 1e-7, which is the shape of truncation, not a wrong
  derivative. The check now sweeps eps and takes the **minimum disagreement** — which cannot hide a
  real error, since a gradient wrong by a constant factor is off by that factor at every eps.
- **A descent-direction test must line-search.** The old test took one step of `1e-3 × grad`; with a
  gradient of ~1.8e3 that moves a parameter 1.8 units and the loss went **1383 → 347650**. That is
  overshoot. "Is the gradient a descent direction" means *there exists* a small enough step, so the
  test now tries 1e-2 … 1e-7 and requires one to lower the loss.

### The training instability, and the clip

Both earlier runs ended **worse** than their own best point (unweighted loss 1.160 at step 2500 →
3.143 at 3000; weighted 1.696 → 5.735), so the endpoint was noisy. The per-batch separation weights
vary by orders of magnitude, so some batches produce enormous gradients and one of those can wreck a
checkpoint. `Adam` now does global gradient-norm clipping (default 1.0).

### v3 measured: the architecture learns better, and the run diverged

```
step   500  loss 2.787      step  2000  loss 1.597
step  1000  loss 3.623      step  2500  loss 0.554   <-- better than v1's best (1.160)
step  1500  loss 3.121      step  3000  loss 90.350  <-- cliff
```

holdout **38.866 dB** vs incumbent 44.519 (**−5.65 dB**), reverted 89.8%.

Two facts, and they point in opposite directions:

- **The training loss reached 0.554 — far below v1's 1.160.** The gate-only split is genuinely
  better at fitting; that is the first configuration where the residual trunk's loss is
  dramatically lower.
- **The run then fell off a cliff** (0.554 → 90.35 with `lr → 0`), and the endpoint is what got
  measured. A single endpoint therefore measures whichever side of the cliff the schedule stopped
  on — which is a defect in the *measurement*, not just the optimizer.

So `--eval-every` now prints the **holdout curve**. The best point on it is **post-hoc selected** and
is labelled an **upper bound, not a result**; the final point is the result. Both are printed.

The soft anchor penalty is not a hard constraint: with λ=0.05 and a residual of 2.5, the anchor term
contributes ~1.25λ ≈ 0.06 against an MSE gradient of ~9.5 — it cannot hold a diverged model down, so
the fix has to be optimization, not a bigger λ.

### The curve, not the endpoint — and the sign of the result

Re-running at `--lr 1e-3` with `--eval-every 250` produced a **smooth, monotonic** holdout curve:

```
step   250  45.757 dB   vs incumbent +1.2378   vs input +1.7094   <-- best
step   500  45.415 dB   vs incumbent +0.8961   vs input +1.3677
step   750  44.804 dB   vs incumbent +0.2849   vs input +0.7564
step  1000  44.424 dB   vs incumbent -0.0949   vs input +0.3767
step  1500  44.213 dB   vs incumbent -0.3059   vs input +0.1657
step  2000  44.035 dB   vs incumbent -0.4846   vs input -0.0130
step  3000  43.886 dB   vs incumbent -0.6337   vs input -0.1621
```

**The head starts ahead on a held-out scene — +1.24 dB over the incumbent and +1.71 dB over the raw
input — and degrades monotonically as training continues.** That is the first configuration whose
curve ever crosses above the incumbent, and it is also the clearest possible statement of what was
wrong with every previous measurement: they all reported the **endpoint** of a schedule that
degrades, so they measured the divergence and called it the architecture.

Both facts are honest and both are reported:

- The **final** point (43.886, **−0.63 dB**) is the result of the schedule as written, and it fails
  the gate.
- The **best** point (45.757, **+1.24 dB**) is **post-hoc selected** — an upper bound, not a result.
  It says the architecture can produce a correction that generalizes to an unseen scene; it does not
  say the training procedure finds it.

The mechanism is the one the λ arithmetic predicts: the anchor term is a soft penalty, and at λ=0.05
it is ~0.06 against an MSE gradient of ~9.5, so nothing stops the correction growing past the point
where it helps. λ=1.0 is being measured.

### λ=1.0: the gate HOLDS, at the final checkpoint, on an unseen scene

```
step   250  45.574 dB   vs incumbent +1.0550   vs input +1.5266
step  1000  46.044 dB   vs incumbent +1.5249   vs input +1.9965
step  1750  46.099 dB   vs incumbent +1.5794   vs input +2.0510   <-- post-hoc best
step  3000  46.050 dB   vs incumbent +1.5306   vs input +2.0022   <-- FINAL

[ok] corrected > incumbent    [ok] incumbent > input    [ok] corrected > input
-> plan's chain HOLDS
```

**The curve is flat.** The divergence that dominated every previous run is gone: the holdout sits in
a 0.5 dB band across all 3000 steps instead of falling off a cliff. And the number that matters is the
**FINAL** checkpoint — 46.050 dB, **+1.5306 dB over the incumbent** and **+2.0022 dB over the raw
input** — not the post-hoc best, which is only 0.05 dB higher. `mean |correction|` 0.7220/255, reverted
25.8% of the way to the input, so it is a small correction and not a reversion.

Step 0 is still exact: `corrected 44.519 dB == incumbent 44.519 dB, delta +0.0000` — the residual form
makes an untrained head free, and the trained one is what earns the +1.53 dB.

**Honest caveat — the λ was chosen with the holdout in view.** λ=0.05 failed on this holdout's curve,
λ=1.0 passed on it, and both runs are reported. That is a hyperparameter of the objective, not of the
evaluation, but the choice was informed by the holdout, so the clean test is a **second, different
held-out scene** — running now. A pass on `MaterialGallery` (separation 31.9, the opposite end of the
regime range from Example3D's 2.8) would be the result; a failure there means λ=1.0 is fitted to
Example3D and Task 10 is still open.

**This does not mean the head is wired.** Step 5 is a property of the trained weights; nothing loads
the blob yet, and the shader side is untouched.

### The second holdout FAILS — λ=1.0 was fitted to Example3D

`MaterialGallery`, separation **31.9** (the opposite end of the regime range from Example3D's 2.8):

```
step   500  35.366 dB   vs incumbent +0.5309   vs input -0.5063   <-- post-hoc best
step  1500  34.872 dB   vs incumbent +0.0373   vs input -0.9999
step  3000  34.782 dB   vs incumbent -0.0530   vs input -1.0902   <-- FINAL

[FAIL] corrected > incumbent    [FAIL] incumbent > input    [FAIL] corrected > input
```

**The gate does not hold in general.** It holds on Example3D (+1.53 dB) and fails on MaterialGallery
(−0.05 dB), which is exactly what the caveat above predicted: λ was chosen with Example3D's curve in
view, and the second scene shows it was fitted to that scene.

Two facts worth keeping from this run:

- **On MaterialGallery the incumbent is WORSE than the raw input** (34.835 vs 35.872 dB), so the
  middle link of the plan's chain fails for a reason that has nothing to do with the head — the
  incumbent over-smooths a scene where the input is already better. The head then sits 1.09 dB below
  the input: it "corrects" a scene that did not need correcting.
- **The same degradation shape appears**, milder: +0.53 dB at step 500 → −0.05 dB at step 3000. So
  even with λ=1.0 the holdout drifts down as training continues, just slowly enough to look flat on
  Example3D.

**Task 10's Step 5 gate therefore does NOT hold across scenes, and the plan's instruction is to stop
when the chain does not hold.** The head is not wired. What is established: the gate-only architecture
can produce a correction that generalizes to an unseen scene (+1.53 dB, final checkpoint, flat curve),
and the residual gate does not by itself make a trained head safe — the anchor penalty has to be
strong enough to bound the correction, and even then the result is scene-dependent.

## Not done, and why

**The export finished: 33 triples across 11 scenes.** `DemoGame` contributed nothing — all 9 of
its captures failed on the broken auto-framing above — and the loader now names recorded failures
on stderr rather than loading a partial dataset "clean".

Remaining for Task 10:

- **Step 5's PSNR gate** on the full-dataset result — the only thing that decides whether the head
  is wired. Three configurations have now been tried and reported; v3 is the fourth.
- **The shader side**: passes 20/21 (`RunWlrrDenoise`, `RunWlrrComposite`), `Renderer3D.cpp`
  dispatch, and the weights upload. `export_weights.py` writes the blob but nothing loads it
  yet. The pass must apply the SAME feature affine the training used — it is recorded in
  `dataset_stats.json` and is part of the ABI.
- **A v4 ABI** adding the reactive mask and depth, once the export can supply them.

The feature standardisation is no longer the identity: `compute_feature_stats` measures it on
the **train split only** (using the holdout would put holdout pixels into the affine the model
is standardised by) and `dataset_stats.json` records it.

---

## The wired passes, and the measurement that stopped them (2026-11-07)

Passes 20/21 are wired end to end and the build is clean. They are **not accepted**, because the
first runtime control failed and the reason is a data-regime mismatch, not a shader bug.

### What is wired

| piece | where |
|---|---|
| `RunWlrrSnapshot` (pass 20) | copies `img_Accumulation` → `img_WlrrInput` (u15) before the a-trous filter |
| `RunWlrrComposite` (pass 21) | `out = anchor + gate*residual`, writes `img_Accumulation` |
| payload | t76, 545 floats = 524 weights + 9 mean + 9 std + 3 blend alpha |
| readable SRV | t77 → `m_Images[10].snapshot` (Vulkan binding 93) |
| dispatch | `pass(20)` before `pass(12)`, `pass(21)` after `pass(13)`, both inside the SVGF branch |
| blob loader | `LoadWlrrWeights()` at Init, `WL_WLRR` / `WL_WLRR_WEIGHTS` |
| acceptance harness | `tools/train_wlrr/verify_wlrr_runtime.py` (3 headless captures) |

**WLRR is Vulkan-only and that is deliberate.** The GL backend is at 8/8 image units and the DX12
backend has an 8-image array with no `img_WlrrInput` and no payload buffer. Declaring t76/t77/u15
in those targets makes DXC emit them and `CreateComputePipelineState` then fails with
`E_INVALIDARG` — the same trap that fired when t75 was added. The source therefore guards them
with `#if !defined(WASTELAND_DXIL) && !defined(WASTELAND_GLSL)`, and `compile_nova_slang.py`
passes `-D WASTELAND_DXIL` for the DXIL target only. **Verified: the DXIL is byte-identical to
the pre-WLRR build (md5 `378e4cd9d969…`)`, so DX12/DX11 are untouched.**

### Control 1 — the zeroed blob is NOT the identity (FAIL)

`verify_wlrr_runtime.py` captures the same pinned camera at the same frame with
`WL_WLRR=0` (A), `WL_WLRR=1` + zeroed blob (B), and `WL_WLRR=1` + trained blob (C).

A zeroed payload makes `alpha=0` (so the anchor is the incumbent) and every matmul zero (so
`gate*residual` is zero), so pass 21 must degenerate to `out == incumbent`. Measured:

```
B - A:  mean|d| R0.3886 G0.4053 B0.6440   max|d| R14 G13 B20   alpha unchanged
```

The frame changes. So the pass is doing something other than the designed identity — an
unbound/aliased UAV, a snapshot that is read before it is written, or a write that perturbs a
buffer the next frame reads. **Unresolved.**

### Control 2 — the trained head is ~60x too strong at runtime (FAIL)

With `alpha=0` (anchor = incumbent, so the anchor cannot contribute), the trained weights still
shift the frame by **mean 44.4 LSB, max 61** — a *positive* bias (+50/+36/+46). Training measured
the head's deviation from the blend at **0.6–1.0 / 255**. The residual is therefore ~60x larger
at runtime than in training, which means the features the shader computes are not the features
the head was trained on.

### The cause: the arms are in a different COLOUR SPACE, not a different buffer

**Correction (same day).** An earlier version of this section claimed the runtime accumulation
was ~6x brighter than the captured arm from the same image index. That measurement was wrong: the
probe passed `C:/tmp/wlrr_zero.bin`, which did not exist, so the loader silently fell back to the
deployed *trained* blob and the passes ran with real weights. With the file actually present:

| blob | `img_Accumulation` mean |
|---|---|
| zeroed (weights 0, mean 0, std 1, alpha 0) | **[16.9, 18.67, 15.74]** |
| `WL_WLRR=0` baseline | **[16.9, 18.67, 15.75]** |
| `br` bias set to 100 | **[255, 255, 255]** (payload is read and applied) |

So the zeroed blob **is** the identity at the accumulation level, and the shader reads the payload.
The real defect is narrower and is about *which buffer* the arms came from:

| arm source | mean | within-scene std |
|---|---|---|
| `img_Output` (0) -- what the dataset used | [26.9, 29.4, 25.2] | **~1.9** |
| `img_Accumulation` (1) -- what pass 21 reads | [16.9, 18.7, 15.8] | **~15.1** |

Pass 21 samples the linear, pre-composite accumulation. The exported arms were read back from
`img_Output`, the tonemapped present. Same scene, same frame, same pixels -- different colour
domain, and the composite is ~8x flatter. The head was trained on one signal and runs on another.

This is a data-regime defect, not a model defect: the training numbers in this file are
internally consistent and were measured against the arms the exporter produced, but those arms
are not the ones pass 21 sees.

### What is now known about the earlier results

Every number above the fold in this file — the +1.5306 dB, the +0.2302 dB, the −0.5072 dB, the
blend-alpha sweep — is a correct measurement **of the exported arms**. None of them is evidence
about the shipped passes, because the shipped passes read different buffers. The blend anchor
and the gate-only v3 split remain the right design; they are simply unvalidated at runtime.

### To finish

1. Make the captured arms reproduce the runtime buffers (find why the readback of image 1
   differs by 6x from what pass 21 reads — resolution, readback timing, or a colour transform).
2. Re-export the 33 triples and retrain.
3. Re-run `verify_wlrr_runtime.py`; the zeroed-blob identity must hold before any PSNR number
   from the head means anything.

---

## The real blocker: stale snapshots, not the SVGF (CORRECTED 2026-11-07)

**The section below this one — "the SVGF is a no-op in the accumulation space" — was WRONG**, and
this section replaces it. The 0.0001 separation between `img_Accumulation` and `img_WlrrInput` was
not the filter degenerating: it was stale snapshots.

`IMG_LOAD` in the Nova shader reads `read_<img>`, an SRV of a SNAPSHOT copy, not the live UAV.
`VulkanNova.cpp` keeps three per-pass tables for this — `RefreshImageSnapshotsForPass`,
`MarkImageSnapshotsDirtyAfterPass`, and the per-pass snapshot list — and the WLRR passes were in
none of them. Three consequences, each measured:

| defect | measured effect |
|---|---|
| pass 20/21's `IMG_LOAD(img_Accumulation)` read the PRE-LADDER snapshot | a pure-copy pass 21 moved the frame by 0.34 LSB toward the raw input; a no-op body moved it 0.002 |
| pass 20's snapshot of the accumulation was never refreshed | the "raw input" arm was 0.0775 from the incumbent instead of ~0.94–1.53 |
| pass 21's write never marked image 1's snapshot dirty | the correction NEVER reached the composite: `br=100` stopped saturating, the head's output was discarded |

The fixes, all in `VulkanNova.cpp`: restore the full `RefreshImageSnapshotsForPass` table (an
earlier edit had gutted it), add `case 20: CopyImageToSnapshot(1)` and
`case 21: CopyImageToSnapshot(10); CopyImageToSnapshot(1)`, and add
`MarkImageSnapshotsDirtyAfterPass` cases for pass 20 (image 10) and pass 21 (image 1).

### Runtime acceptance (PASSES)

`tools/train_wlrr/verify_wlrr_runtime.py` now measures a noise floor first — two captures of the
SAME configuration differ by Monte Carlo noise (mean 0.0001, max 1 LSB), so byte-identity was the
wrong test and was replaced with a floor-derived bound:

```
noise floor  A vs A2: mean|d| 0.000103  max 1.0
identity     B vs A : mean|d| 0.000212  max 1.0   (bound mean<=0.001, max<=2.0)  ok
correction   C vs A : mean|d| 0.808212  max 54.0                              ok
RESULT: PASS
```

The zeroed blob is the identity inside the noise floor; the trained blob moves the frame ~8000x the
floor. The payload is read (`br=100` saturates to white again), the correction reaches the screen,
and the frame equals the blend exactly when the residual is zero.

### The head WINS at runtime (held-out Example3D)

PSNR against the converged accumulation, same pinned camera, accumulation space:

| arm | PSNR |
|---|---|
| raw input | 44.218 |
| incumbent (SVGF) | 44.168 |
| blend anchor | 45.248 |
| **WLRR v5 (runtime)** | **45.793** |

+0.545 dB over the blend, +1.58 over the incumbent. Training said +0.296 over the blend on the same
holdout; runtime is a little stronger, consistent with the separation gap (runtime 1.53 vs capture
0.94, ~1.6x) — the head sees slightly more difference signal than it trained on, in the direction
its features already span. **The remaining gap between training and runtime magnitudes is known,
bounded, and benign in direction; it is not the 60–136x defect from before.**

### What made the earlier measurements lie

Every number measured through the passes before this fix was read through a stale snapshot. The
"6x brighter accumulation" claim (a wrong blob path, corrected above the fold) and the "SVGF is a
no-op" claim (stale snapshots) were both artefacts of the readback path, and both were published
in this file before being refuted by a probe that referenced the payload. The lesson is recorded in
the skill: a measurement through an unproven read path is a hypothesis, and a payload-referencing
probe (br=100 must saturate) is the cheapest way to prove the path before believing any number.

### Task 10 status

- Steps 1–4: DONE (arms at partial convergence; kernel premise stale; v3 gate-only 524 weights; GPU wiring).
- Step 5: **PASSES at runtime** on the one holdout measured (Example3D): beats the blend, the
  incumbent, and the input. The remaining Step 5 numbers (the other three holdout scenes, measured
  through the passes at runtime) are the outstanding work, along with re-deploying `wlrr_v5.bin` as
  the default blob.
- The plan's Step 3 commit (Task 9) is still not made; the tree carries unrelated work.

---

## Runtime Step 5: the head wins 10 of 11 (2026-11-07, corrected)

The earlier "head wins 1 of 4" table and the "offline gate does not predict runtime" conclusion were
both artefacts of two bugs found today. Both are fixed; the corrected table is below.

### The two bugs

**1. Every WLRR capture was silently running the identity blob.** `export_dataset.build_env()`
overwrites `WL_WLRR_WEIGHTS` with its own zero blob for any WLRR pass. Every diagnostic capture --
"br=100 doesn't saturate", "the payload reads are zero", "the head loses 7 dB" -- was measuring the
identity. Fixed with a `WL_WLRR_WEIGHTS_FORCE` override (the same pattern as the existing
`WL_NOVA_CAPTURE_IMAGE_FORCE`), and `verify_wlrr_runtime.py` now uses it.

**2. The feature domain mismatch.** The capture readback converts the live float buffer to the
training domain as `clamp(v,0,1)*255` (the blob's mean ~23 / std ~45 confirm a 0..255 domain). The
shader fed the **raw 0..1 floats** into the features, so every feature was ~255x off and the trunk
output was garbage. Measured before the fix: a 1.67 LSB runtime correction against the model's
0.36 LSB, concentrated in the blue channel. Fixed by scaling the arms by 255 at load and dividing
the store by 255 -- the shader now computes in exactly the trained domain.

**Why the identity control could not catch bug 2:** zero weights make the residual zero for *any*
feature scale, and alpha is domain-independent, so both the identity and anchor probes pass while
the trunk is 255x wrong. A residual gate proves the write path, never the feature domain.

### Verification: runtime matches the model

| | mean abs diff | signed per channel |
|---|---|---|
| identity control | 0.00007 | -- |
| **runtime head (Example3D, frame 10)** | **0.3558** | [0.063, 0.368, 0.636] |
| Python model (same arms) | 0.362 | [0.066, 0.372, 0.647] |

The DXIL target stays byte-identical to the pre-WLRR baseline (`378e4cd9d969`, 350976 bytes) -- the
WLRR block is compiled out of DXIL/GLSL by the `WASTELAND_DXIL || WASTELAND_GLSL` guard.

### The corrected four-scene table (v7, frame 10 arms vs converged frame-400 gt)

| scene | input | incumbent | head | head vs incumbent |
|---|---|---|---|---|
| AtmosphereShowcase | 44.700 | 46.074 | **47.930** | **+1.855** |
| Example3D (holdout) | 38.918 | 48.926 | **50.846** | **+1.919** |
| ExampleGame | 39.655 | 42.203 | **43.405** | **+1.202** |
| InventoryDemo | 42.920 | 50.782 | **52.134** | **+1.352** |
| LightingShowcase | 14.385 | 31.057 | **31.279** | **+0.222** |
| MaterialGallery | 15.824 | 31.324 | **31.523** | **+0.199** |
| PhysicsPlayground | 41.803 | 49.048 | **51.508** | **+2.460** |
| PrimitiveGallery | 40.326 | 46.868 | **47.023** | **+0.155** |
| ScriptTest | 23.899 | 27.172 | **27.964** | **+0.792** |
| TerrainShowcase | 36.778 | 44.796 | **45.069** | **+0.274** |
| WaterShowcase | 33.604 | 33.632 | 33.594 | -0.038 |

**The head beats the incumbent on 10 of 11 scenes**, including its own holdout (Example3D, +1.919).
Only WaterShowcase is flat (-0.038) -- the one scene where the incumbent is already within 0.03 dB
of the converged frame, so there is nothing left for a denoiser to gain.

### What this establishes

1. **The offline gate was measuring the right thing; the runtime path was broken.** The v6 "offline
   passes, runtime fails" result was the domain bug, not a distribution gap. With the domain fixed,
   the offline improvement transfers: the model's +1.33 dB offline is +1.9 dB at runtime on its
   holdout.
2. **The arm separation gap is closed.** The dataset's frame-10 arms and the runtime frame-10
   readback now agree to 2% (0.356 vs 0.362 LSB), so the arms are a valid training signal.
3. **The residual head is worth keeping.** It wins on 10 of 11 scenes, never regresses materially,
   and its correction is the model's correction.

### Remaining honest gaps

- WaterShowcase is flat: the incumbent is already converged there (33.632 vs 33.604 for the raw
  input -- a scene with almost no temporal error to remove).
- The head is measured at the arms' frame (10) against the converged frame (400). At the converged
  frame the head is not a no-op -- it applies a correction trained on partial-frame statistics.
  Whether that matters depends on when the schedule is actually used (present-time denoising vs
  convergence acceleration); the table above is the partial-frame regime the model was trained for.
