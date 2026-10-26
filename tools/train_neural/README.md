# Neural rendering: dataset exporter and trainer

Produces the matched `(input, ground-truth)` frame pairs that the clean-room
neural rendering stage trains on, and trains the network on them. See
`docs/NEURAL_RENDERING.md` for the stage itself and
`.hermes/plans/2026-10-23_011500-clean-room-neural-rendering.md` for the task
breakdown.

**Two toolchains, deliberately split:**

| | imports | why |
|---|---|---|
| `export_dataset.py` | stdlib only | it drives the engine binary and must run anywhere the engine does |
| `model.py`, `dataset.py`, `train.py`, `test_model.py`, `export_weights.py` | `numpy` + `Pillow` | training only; no framework, no ONNX, no vendor maths |

The exporter has no third-party dependency because it is the clean-room
boundary: it is what guarantees every target image came from *this* binary.

## Why this is the clean-room boundary

Every target image here is rendered by *this* engine, from *these* scenes, by
*this* binary. Nothing is downloaded, scraped or vendored, so no third-party
imagery enters the tree and no licence question arises downstream. The manifest
records the binary's SHA-256 so a dataset can always be traced to the exact
build that produced it.

## Usage

The default output root is **`dataset/` under the current directory**, i.e.
`<repo>/dataset/` when run from the repo root:

```
dataset/
  manifest.json
  gt/     <Scene>__<camera>__gt.png
  input/  <Scene>__<camera>__input.png
```

It is gitignored — a 72-frame run is ~700 MB of PNGs, and the manifest records
the engine binary's SHA-256, so the whole thing is reproducible with one command.
Regenerating is cheaper than versioning incompressible pixels.

```bash
# Plan only -- prints every capture it would run, touches nothing.
python tools/train_neural/export_dataset.py --dry-run

# Export the default set: gt + input for every scene, camera framed per scene.
python tools/train_neural/export_dataset.py

# Narrow it down while iterating.
python tools/train_neural/export_dataset.py \
    --scenes scenes/Example3D.wastescene --passes gt

# Use the hand-tuned camera list instead of per-scene framing.
python tools/train_neural/export_dataset.py --frame preset

# Check a dataset after the fact (defaults to dataset/).
python tools/train_neural/export_dataset.py --validate
```

## Camera framing

**`--frame auto` (default)** derives the camera from the scene's own content: the
per-axis **median** of the renderable entities' translations is the focal point,
and the distance is a multiple of the median entity distance from it, clamped to
8–150 m. Three yaws are tried at that distance.

Median, not mean and not bounding-box centre: Example3D's ground plane is 10000
units across, which put the bbox centre at y=27 — a point in mid-air above the
scene — and made the mean useless too. Eight entities are not moved by one of
them being huge.

Only entities with a `*RendererComponent` or `WaterComponent` count. Lights,
cameras, volumes and scripts have transforms but nothing to reconstruct, and
framing around a sun direction or a 10 km fog volume puts the camera a kilometre
from anything worth looking at.

**`--frame preset`** uses the hardcoded `CAMERAS` list. That list was tuned on
Example3D and is only valid for scenes of a similar extent — it rendered nothing
at all for DemoGame and WaterShowcase.

## The pair

Both passes use the **same scene, the same pinned camera and the same capture
frame index**. Jitter is seeded by the frame index, so a fixed index means a
fixed sub-pixel offset and the pair is genuinely aligned. The two passes differ
in exactly one thing:

| | `gt/` | `input/` |
|---|---|---|
| `RenderScale` | 1.0 | 0.5 |
| temporal accumulation | on | on |
| capture frame | 400 | 400 |
| quality preset | Ultra (3) | Ultra (3) |
| neural rendering | on | on |

Resolution is the degradation the stage exists to undo, and it is the *only*
difference. Keeping everything else identical means the network is never asked
to undo something the dataset introduced by accident.

**But the converged `input` pass is no longer a useful target, and the shipped
dataset was built on it.** Measured on Example3D, camera `auto_y0`, against the
shipped `gt/`:

| input | luma | vs gt | 12-param affine headroom |
|---|---|---|---|
| converged, scale 0.5, accumulation on | 47.89 | 37.040 dB | **+0.084 dB** |
| single frame, scale 0.5, accumulation off | 50.21 | 28.391 dB | **+5.122 dB** |

By frame 400 the scale-0.5 render with accumulation on has converged to the
scale-1.0 one, so the pair differs by almost nothing and the 12-parameter affine
control — the bar the network must clear — buys 0.08 dB. There is nothing to
learn. **Use `--passes gt --passes noisy` for any new dataset.**

`gt/` is a **converged** reference, not a single sample. Past frame 4 the
accumulation alpha drops to its 0.02 floor, so by frame 400 the accumulated
image is a running average over roughly the last 50 frames and the sub-pixel
jitter has averaged out. Training against a single-sample target would teach the
network to reproduce noise.

The `noisy` pass (`RenderScale=0.5`, accumulation off) is the harder single-sample
variant, and it is where the headroom is:

```bash
python tools/train_neural/export_dataset.py --passes gt --passes noisy
```

**Measured on the 2026-10-24 re-export (21 complete pairs):** the 12-parameter
per-channel affine control buys a mean of **+2.042 dB** over the noisy input.
That is the same order as the gap the shipped network had to clear against the
affine control, and it is roughly **24x** the +0.084 dB the converged `input`
pass offers — which is the whole reason the target moved.

The headroom is not uniform, and the spread is worth knowing before training:

| scene | noisy vs gt | affine headroom |
|---|---|---|
| Example3D | 28.3–28.4 dB | **+4.4 to +4.8 dB** |
| DemoGame | 46.2–47.0 dB | +3.1 to +4.2 dB |
| InventoryDemo | 39.7–40.3 dB | +1.7 to +2.1 dB |
| AtmosphereShowcase | 36.6–41.4 dB | +1.2 to +1.9 dB |
| ExampleGame | 34.5–35.4 dB | +1.1 to +1.2 dB |
| MaterialGallery | 25.2–25.3 dB | +0.75 to +0.79 dB |
| ScriptTest | 36.1 dB | +0.75 dB |
| LightingShowcase | 28.9–29.7 dB | **+0.47 to +0.57 dB** |

Six of the 21 pairs sit under 1 dB. LightingShowcase in particular has almost
nothing for a network to learn — its noise is dominated by the light budget, not
by the spatial detail the stage is built for — so a model that improves the
average may still leave those frames flat. That is a property of the scene, not
a defect in the pass, and it is worth checking per-scene rather than trusting
the mean.

Its capture frame is deliberately the **same** frame 400 as the converged passes,
with accumulation off. The "noisy" property comes from the missing temporal
blend, not from the frame being early. This matters because the frame index is
load-bearing twice over — it sets both the accumulation state *and* whether the
capture has settled:

| frame | what Example3D `auto_y0` at scale 0.5 actually captured |
|---|---|
| 1 | blank — mean luma 0.0, contrast p99 0.0 |
| 2 | 1280×720 — a default, not the render target |
| 3 | 3840×2092 — the window, chrome included |
| 6 | 2332×1451 — settled |

An earlier `NOISY_FRAME = 1` therefore wrote **blank frames into the dataset and
recorded them as valid**. A wrong-size capture is not a weaker sample either:
the pairs are compared pixel-wise against `gt/`, so it is a different image
wearing a valid manifest entry, and `--validate` cannot catch it because it only
checks each PNG against the size the manifest already claims. The exporter now
refuses a run whose captures disagree in size.

**The shipped `dataset/` predates a renderer change and must not be reused.** It
was captured 2026-10-23; Task 3's shader edits (removing the
`maxIndirectRays = 0` early-out and the `wl_neural_indirect` blend) landed
2026-10-24. Re-exporting the same cameras with the current binary gives inputs
that are **+5.2 luma brighter** than the shipped ones, and the shipped pairs'
5.33-luma input/gt gap collapses to 0.13. The model trained on the shipped pairs
learned to undo a brightness difference the renderer no longer has: it scores
+6.130 dB on its own data and **−2.913 dB** on a fresh capture of the same camera.
Treat `assets/neural/neural_weights.bin` as a correctness artefact, not a
shipping one.

## What `--validate` checks

- every `gt/` frame has exactly one `input/` frame with the same scene and camera
- pair dimensions agree
- every file is present, non-zero, readable, and matches its recorded SHA-256
- dimensions match the manifest
- **no black images** (mean luma floor) — error
- **no edgeless frames** (local contrast floor) — error

Both gates are errors, not warnings. A frame with no edges has nothing for the
network to reconstruct, so the pair is unusable however correct the render is —
and accepting it silently is exactly the failure mode the gate exists to prevent.
The exporter exits non-zero on a blank frame too; pass `--allow-blank` to accept
one deliberately.

## The local-contrast check, and why it exists

Mean luma cannot tell a dark scene from an empty one. Both are dark.

This bit us. A camera pinned 14 m out at pitch −8 renders a smooth near-constant
image (mean luma 13.8, range 12–17) because it is looking at open sky. That is a
*correct* render and a *useless* target, and it is indistinguishable from a
broken renderer if you only look at the mean. The editor's own viewport shows
the same image, which makes it look even more like an engine defect — two
independent read paths agreeing on a black picture.

Local contrast separates them: a scene with geometry produces edges, sky does
not.

| capture | mean luma | local-contrast p99 |
|---|---|---|
| sky only | 13.8 | 1.2 |
| sky only | 31.0 | 1.0 |
| floor + props | 30.7 | **15.9** |

So every capture is measured, the number goes in the manifest, and a frame below
`MIN_LOCAL_CONTRAST_P99` (4.0) fails. **If a scene starts failing this check,
move the camera — do not lower the floor.**

What the gate measures is **edges, not content**. A flat water plane or a
featureless ground also scores low, and that is a correct render of genuinely
low-detail content. Read a low score as "nothing here to reconstruct", whether
the cause is sky or a blank surface.

## Capture harness

The editor is driven entirely by environment variables; there is no UI
automation and no scripting round-trip. All of them are inert unless set, in
`EditorLayer::OnAttach` (after `OnProjectChanged`, which is what would otherwise
undo them):

| Variable | Effect |
|---|---|
| `WASTELAND_PIN_CAMERA` | `<fx,fy,fz>,<distance>,<pitch>,<yaw>` — holds the editor camera and skips `OnUpdate` camera control |
| `WL_CAPTURE_SCENE` | scene to load (absolute or project-relative) |
| `WL_CAPTURE_PRESET` | `0..3` (Low, Medium, High, Ultra) |
| `WL_CAPTURE_SCALE` | render scale, clamped 0.25–1.0 |
| `WL_CAPTURE_NEURAL` | in-sample neural rendering on/off |
| `WL_CAPTURE_ACCUM` | temporal accumulation on/off |
| `WL_DRS_OFF` | disables dynamic resolution, pinning the scale |
| `WL_SCREENSHOT` | output PNG path |
| `WL_SCREENSHOT_FRAME` | first frame of the capture window |
| `WL_SCREENSHOT_LINGER` | close N frames after the shot lands |

The process closes itself once the linger elapses, so the exporter waits for
exit rather than killing it — a killed process can be torn down mid-readback.

### Transient failures are retried

The editor sometimes dies mid-run: a device reset / TDR is a known failure mode
on Ultra, and when it happens the PNG simply never appears. A 72-frame run hit
this 3 times (2 DemoGame, 1 ExampleGame), and all 3 succeeded on an immediate
re-run — so the failure was transient, not a bad scene.

`capture()` therefore retries each frame up to `--attempts` times (default 2),
waiting 5 s between attempts for the driver to settle, and records the attempt
count per frame in the manifest. Without this, one GPU hiccup kills a 13-minute
export at frame 70.

A retry is not a substitute for a genuinely bad camera: if the editor dies on
the same frame twice, the frame is still recorded as a failure and the run still
exits non-zero.

### Paths must be real Windows paths

`$TMPDIR` on this host resolves to `/tmp`, which native Windows programs cannot
see. `std::ofstream` fails silently on it. Always pass a `C:/...` path to
`WL_SCREENSHOT` and `--out`.

## Requirements

- `bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe` must be
  built. The exporter refuses to run without it and prints the MSBuild command.
- Python 3.8+.

## Known limitations

- **One window size.** The capture is the render target at whatever size the
  headless window comes up. All frames in a run share it, which is what the
  validator's dimension check assumes; it is not currently configurable.
- **Auto-framing is a heuristic, not a composition.** It puts the median entity
  at the focal point and stands back a multiple of the median spread. It has no
  idea what the scene is *about*, so it can frame a cluster from the empty side
  or put the horizon through the middle. The three yaws are the mitigation, and
  the contrast gate is the backstop.
- **No depth or motion vectors.** The exporter writes colour only. Pass 17 stages
  `(luma, depth01, velocity.x, velocity.y)`, but the v1 network consumes only the
  luma: training on constant depth/velocity would teach the network to ignore
  them, so they are staged and unused. Adding them is a capture-path change, not
  a dataset change.
- **The `noisy` pass is not in the default set.** `gt`/`input` is the pair the
  stage trains on; `noisy` is opt-in.
- **A dataset is only as good as its cameras.** Run `--validate` and read the
  per-frame `contrast_p99` in the manifest before training on it.

---

# Training the network

## What the network is

```
pixel  = 4   (r, g, b, luma)                        -- this pixel
global = 4   (mean_r, mean_g, mean_b, mean_luma)    -- the whole anchor frame
in     = 8   pixel ++ global

stage1: 8 -> 32 -> 8
stage2: 8 -> 32 -> 8
head  : 8 -> 3          residual
gate  : 8 -> 1          sigmoid

out = anchor + gate * residual          anchor = the engine's own frame
```

1140 fp32 weights = 4560 bytes. `model.py` writes the forward pass and the
gradient out by hand — no autodiff, no framework. That is the clean-room
requirement made concrete: the arithmetic is in this repo, not imported.

**The output is a residual, never a replacement.** With the head zeroed at init
the network is *exactly* the identity (`test_model.py --self-test-only` asserts
`max|out - anchor| == 0`). That is what makes "the engine frame is unyielding"
mechanical rather than aspirational, and it is why the worst case is "no
enhancement" rather than "wrong image".

## Why the input is RGB + frame means, and not the plan's 18 features

The plan (§2.2) specifies 18 per-pixel features. Three measurements decide this:

1. **The binding budget.** The reservation gives the feature pass one `rgba32f`
   UAV — four floats. Pass 17 stages `(luma, depth01, velocity.x, velocity.y)`
   (`Renderer3D_NovaRenderer.hlsl:3970`). Depth and velocity are not in the
   dataset at all: the exporter writes colour only. Training on constant
   depth/velocity teaches the network to ignore them, so v1 consumes only luma,
   and pass 18 reads `r,g,b` straight from `s_Output` — the same texture pass 17
   reads, so it costs no extra register.

2. **A luma-only model is worse than doing nothing.** Measured on all 36 pairs
   before any model was written:

   | model | PSNR vs gt |
   |---|---|
   | raw input (do nothing) | **33.609 dB** |
   | global per-channel affine (12 params) | 36.014 dB |
   | 64-bin luma tone curve — best possible 1-input luma model | 27.833 dB |

   Luma is a lossy 1-D projection: unrelated colours that share a luma get mapped
   to the same output. That is a −5.8 dB regression, so the network takes RGB.

3. **A per-pixel model cannot be scene-dependent, and the correction is.** This
   is the one that changes the architecture. Fitting a per-channel affine *per
   scene* reaches **38.257 dB** against **36.014 dB** for one global affine —
   **+2.24 dB of scene conditioning**. The coefficients are nothing alike:
   DemoGame's are gains ~[0.885, 0.867, 0.897] (a contrast reduction);
   WaterShowcase's have *negative* gains and a 0.224 blue bias. A per-pixel map
   has no way to know which scene it is looking at, so it converges to the global
   compromise.

   The four `mean_*` inputs are the anchor frame's own mean colour and mean luma.
   They are reduced from the **anchor** — never the ground truth, which would be
   leakage — once per frame and broadcast to every pixel. Still a per-pixel map,
   but one that knows what frame it is in.

   Measured on the 6 held-out frames, the unconditioned v0 model reached 35.978 dB
   against the 12-parameter affine's 36.447 dB — i.e. **the 604-parameter network
   did not beat a tone curve.** Its learned gate was also a dead constant
   (0.4185 everywhere, std 0.0000). That is what motivated the global inputs.

## The network that shipped is the third one

The first two both produced *plausible* numbers and were both wrong. Worth
recording, because neither was caught by an accuracy metric.

**v0 collapsed to a constant.** With raw inputs in `[0,1]` the feature means are
~0.15–0.20 and the stds ~0.06–0.12, so the layer-1 pre-activation had a std of
**0.0118** — 85× too small. Half the ReLU units died at init, the signal
attenuated to nothing across four layers, and training converged to
`out = anchor + constant`: one number, worth +2.4 dB, with a dead-constant gate.
The per-pair PSNRs looked fine.

What gave it away was testing **sensitivity instead of accuracy**: the output was
byte-identical for global-vector values of 0.0, 0.2, 0.4 and 1.0, and stayed
identical after perturbing `W1` by +1.0. A model that cannot see its own input is
not a model.

The fix is input standardisation. `model.features()` now subtracts
`FEATURE_MEAN` and divides by `FEATURE_STD` (measured on the training pairs by
`dataset_stats.py`, recorded in `dataset_stats.json`). **These constants are part
of the ABI** — the shader must apply the same affine before the first matmul.

**Standardising then exposed a second bug, and this one diverged loudly.**
`forward()` defaulted its anchor to `x[:, :3]` — correct while `x` held raw
features, but after standardisation that is `(r - 0.1498) / 0.1205`, not `r`. The
model added its residual to the wrong quantity and the run went to **19 dB**, far
below doing nothing. `anchor_rgb` is now an explicit argument.

`verify_anchor_at_init` passed straight through it, because it calls `forward`
with a hand-built `x` and never touches `features()`. The replacement,
`verify_anchor_passthrough`, runs a zero-head model through the real standardised
path and asserts `max|out - anchor| == 0`. Non-vacuity checked: the bugged path
gives **10.73**, the correct path **0.0**.

**Held-out result after the fix — 200 epochs, 6 held-out frames:**

| model | held-out PSNR |
|---|---|
| identity (do nothing) | 33.578 dB |
| global per-channel affine, 12 params | 36.440 dB |
| **trained network, 1140 params** | **38.763 dB** |
| per-scene affine (oracle ceiling) | 38.257 dB |

`+5.185 dB` over the raw input, `+2.323 dB` over the 12-parameter tone curve,
and **above** the per-scene affine oracle. SSIM 0.9765 → 0.9863.
`max |out - anchor| = 0.0482` — the engine frame is adjusted, never replaced.

Per pair: InventoryDemo +9.986, AtmosphereShowcase +9.327, Example3D +5.630,
PhysicsPlayground +4.940, PrimitiveGallery +1.104, DemoGame +0.120.

---

## The 2026-10-24 retrain (current)

The dataset above was captured **before** the Task 3 shader change, so the
shipped model had learned a 5.33-luma lift the current renderer no longer
produces — on a fresh capture of the same camera it scored **37.040 → 34.127 dB**,
i.e. *worse than doing nothing*. The blob was a correctness artefact, not a
shipping one.

Re-exported as `dataset_noisy/` (72 frames, 12 scenes x 3 cameras, `gt` +
`noisy`), retrained, and re-verified:

| | |
|---|---|
| export | 72 frames, 0 missing, 9 timeout gaps re-captured |
| affine headroom in the data | **+2.042 dB** mean (vs +0.084 dB for the old converged input) |
| held-out, raw input | 38.612 dB |
| held-out, enhanced | **41.547 dB** |
| **gain over the raw input** | **+2.935 dB** |
| per-pair gain | +0.77 to +6.28 dB |
| mean gate | 0.4227 (varies 0.383–0.461 per scene) |
| **shader vs model** | **mean\|d\| 0.000026, max\|d\| 0.000262, 0.000% of pixels off** |

The shader/model agreement was confirmed with a **zero-weight control**: with
every weight zeroed the network is exactly the identity (`out = anchor`), and the
stage then reproduces the anchor to **mean|d| 0.000000**. That control is what
distinguishes "the stage reads the weights correctly" from "the stage happens to
look plausible" — run it before trusting any shader-vs-model number.

**A capture can silently be the wrong run.** The first zero-weight attempt
returned a frame that was *not* the identity, which reads exactly like a broken
shader. The cause was a stale `DemonCore-Editor.exe` left over from a previous
detached capture: the new process never took over, and the old one's dump was
compared. Two identical stage-off captures compare **bit-identical**
(`max|d| 0.00000000`), so reproducibility is not the problem — a contaminated
capture is. Confirm the editor process is gone before starting one.

**The frame mean is not optional.** The four `mean_*` inputs are what the whole
result rests on. Measured on the same six held-out frames:

| conditioning | held-out |
|---|---|
| real per-frame mean | **38.763 dB** |
| best constant vector (dataset mean) | 35.909 dB |
| all 0.5 | 29.706 dB |
| all 1.0 | 31.023 dB |
| all zeros | 32.829 dB |

A constant is **worse than the 12-parameter affine**. The network is trained to
read the frame it is in; feed it a frame it was not told about and it actively
hurts. So the shader must reduce the frame mean per frame — that is a
reduction pass, not a constant.

**An evaluation bug hid this, and it also manufactured a regression.** The first
report said 38.183 dB with DemoGame *losing* 1.725 dB. `evaluate()` processed the
frame in 256-row bands and reduced the conditioning mean **per band**, so each
band was told its own mean instead of the frame's. That is a different input and
a different answer. With the mean reduced once per frame the number is 38.763 dB
and DemoGame improves by +0.120 — all six frames gain. `predict_frame` now takes
the conditioning vector explicitly so a banded caller cannot repeat this.

**The gate is alive.** It varies 0.319–0.422 across the six held-out scenes (it
was a dead constant 0.4185, std 0.0000, before the standardisation fix), and its
lowest values land on the scenes the model is least sure of.

## Train

```bash
# ~3 s/epoch on this machine; 200 epochs is about 10 minutes.
python tools/train_neural/train.py

# ~15 s end-to-end sanity run before committing to the long one.
python tools/train_neural/train.py --smoke

# Fewer held-out pairs, different seed, stronger anchor penalty.
python tools/train_neural/train.py --holdout 4 --lam 0.2 --seed 1
```

* **Adam**, lr 3e-3 with cosine decay, batch = `patches-per-pair × patch²` (64×256
  = 16384 pixels).
* **Random crops**, streamed one pair at a time. A full-res pair is ~80 MB as
  float32 and the whole set is ~2.9 GB, so nothing holds more than one pair.
  Patches are also the honest unit: the network has no spatial extent, so a
  patch is a bag of independent samples.
* **Split by pair, never by patch** (`dataset.split_pairs`). Splitting by patch
  leaks — adjacent patches of one frame are nearly identical, so the holdout
  would be a second copy of the training set and the reported PSNR fiction.

## Evaluate — this is the gate, not the training loss

```bash
python tools/train_neural/test_model.py
python tools/train_neural/test_model.py --self-test-only   # no dataset needed
```

`test_model.py` exits non-zero if the enhanced output does not beat the raw input
on PSNR. A model that cannot beat its own input is not an enhancement, and tuning
the gate to hide that is exactly the failure mode this gate exists to catch.

It also fits a **12-parameter global affine on the training pairs and scores it on
the same held-out frames**. That is the control that matters: the network is
1140 parameters, and if it cannot beat a tone curve it is a tone curve with extra
weights. The v0 4-input model failed exactly this test (35.978 vs 36.447 dB), which
is what motivated the frame-level conditioning.

It reports PSNR and SSIM of the enhanced output vs gt, **PSNR of the raw input vs
gt** (the number to beat), the mean gate, and the fraction of pixels with
`gate < 0.05` — the "degrades to no enhancement" case.

`--self-test-only` runs four checks that need no dataset:

| check | what it proves |
|---|---|
| `verify_gradient` | central finite differences vs the analytic backward pass, in **float64** |
| `verify_anchor_passthrough` | a zero-head model returns the anchor exactly **through the real standardised path** |
| `verify_blob_roundtrip` | pack → bytes → unpack is bit-identical |
| `verify_anchor_at_init` | a fresh network is exactly the identity |

The gradient check found two real bugs while this was being written, which is why
it exists rather than being decorative:

* `np.mean()` over an `(N,3)` array divides by `3N`, but the gradient used `2/N`
  — scaling the **entire** gradient by exactly 3.000×.
* `gate` is `(N,1)` and `residual` is `(N,3)`, so `dgate` broadcast to `(N,3)`
  instead of reducing over the channel axis.

The float64 detail matters: in float32 the loss is ~1.9 and `eps=1e-3` leaves
about 4 significant digits, which reads as a gradient error the same order as the
bug being looked for.

`verify_anchor_passthrough` exists because `verify_anchor_at_init` passed a bug
that made the model add its residual to the *standardised* pixel instead of the
pixel: the init check calls `forward` with a hand-built `x` and never touches
`features()`. The passthrough check runs the real input path and asserts
`max|out - anchor| == 0`; on the bugged path that error is **10.73**, on the
correct path **0.0**.

## Export

```bash
python tools/train_neural/export_weights.py            # writes assets/neural/
python tools/train_neural/export_weights.py --check    # round-trip + corruption controls
```

Writes `assets/neural/neural_weights.bin` (32-byte header + 1140 fp32 = 4592
bytes) and a `.json` sidecar with the layer shapes and the forward equations.

**The byte layout is the shader's read order and it is an ABI.** The header is
`magic "WLNR"`, version, `n_in`, `n_hidden`, `n_residual`, `n_gate`, `n_weights`,
**crc32 of the payload**. Every matrix is row-major in the shape given by
`model.LAYER_SHAPES`.

Three guards, all of which exist because the failure mode is a wrong image with
no error:

* **shape check on serialize** — a `.npz` from an older architecture is refused
  (`parameter 'W1' has shape (4, 32), the ABI requires (8, 32)`), because the
  shader reads by offset and would silently misread it;
* **shape/magic/CRC check on deserialize** — bad magic, truncation, a forged CRC
  and a wrong shape header are all refused;
* **`--check`** re-reads the file, asserts bit-identity, and asserts that a
  corrupted blob is refused.

The CRC32 is not decoration. The engine uploads this blob into a constant buffer
and one flipped bit would be a silent visual corruption — no shader error, no
link failure, no log line, the same failure mode as the ninth GL image uniform.
