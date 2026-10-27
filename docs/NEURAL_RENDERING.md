# Neural rendering (clean-room trained MLP)

This is a small neural network — 1140 weights, trained on this project's own captured
frames — that runs as three compute passes at the **end** of the frame, on the finished
image, and adjusts it slightly. It is **not** DLSS 5, **not** bit-compatible with any
vendor model, and **not** RTX-only: it runs on any GPU with compute shaders, including
plain OpenGL, and it contains no vendor code, no vendor SDK, and no Tensor-Core
requirement. The training data is exported from the engine itself by
`tools/train_neural/export_dataset.py` (stdlib only, drives the engine binary) into
`dataset_noisy/` — 72 frames, 12 scenes x 3 cameras, as matched `gt` (render scale 1.0,
temporal accumulation on) and `noisy` (scale 0.5, single frame, accumulation off)
pairs; 6 frames are held out from training. Backend status: **OpenGL is verified** (the
shader reproduces the Python model to `mean|d| 0.000026`), while **DX11, DX12 and
Vulkan are contract-verified only** — their shader twin compiles, carries the stage,
and is statement-parity-checked against the GLSL, but none of the three has ever been
seen to render on this machine. Details and the evidence for both claims are below.

---

## 1. What it actually does

The stage is a residual enhancer. For each pixel it computes eight features — the
pixel's `r,g,b` and its Rec. 709 luma, plus the frame's mean `r,g,b` and mean luma —
and pushes them through:

```
8 -> 32 -> 32 -> 32 -> 32 -> 3     (residual, added to the anchor)
8 -> 32 -> 32 -> 32 -> 32 -> 1     (gate, sigmoid, scales the residual)
```

`out = anchor + gate * residual`, clamped to `[0,1]`.

Three properties are load-bearing rather than stylistic:

- **The output is a residual, never a replacement.** The head is zeroed at init, so a
  freshly exported blob is *exactly* the identity. The worst case of a bad weight file
  is "no enhancement", not "wrong image".
- **The gate is per-pixel and per-frame.** It varies with the frame's mean colour, so
  the same network can leave a dark frame alone and lift a bright one. Measured mean
  gate on the held-out set: **0.4227**, ranging **0.383–0.461** per scene. It does not
  collapse to a constant — a collapsed gate was the failure mode of the first
  architecture, which reached only 35.978 dB and was discarded.
- **The frame mean is reduced from the anchor, never from the ground truth.** The
  network never sees the target image. Feeding it the ground truth would make the
  training loss a statement about the dataset rather than about the renderer.

### Why the frame mean matters

The frame mean is the whole result, not an optimisation. Measured on the training set:

| conditioning | held-out PSNR |
|---|---|
| no frame mean (constant vector, best fit) | 35.909 dB |
| best scalar constant | 35.653 dB |
| all-zeros | 32.829 dB |
| **real per-frame mean** | **38.763 dB** |

A model without it is ~2.9 dB worse. It is the difference between "a tone curve" and
"a tone curve that knows what it is looking at".

---

## 2. Training-data provenance

Everything the model knows comes from this renderer. There is no external dataset, no
synthetic data, and no vendor data.

```
tools/train_neural/export_dataset.py   stdlib only; drives the engine binary
tools/train_neural/dataset.py          streaming pairs, no full-frame residency
tools/train_neural/model.py            numpy only; forward pass and gradient by hand
tools/train_neural/train.py            Adam + cosine decay
tools/train_neural/test_model.py       PSNR/SSIM, gradient check, controls
tools/train_neural/export_weights.py   blob writer + CRC32
tools/train_neural/dataset_stats.py    regenerates the standardisation constants
```

Two toolchains, deliberately split: `export_dataset.py` is stdlib-only because it is the
clean-room boundary that drives a proprietary-looking binary; everything downstream is
numpy + Pillow. No framework, no ONNX, no vendor maths. The forward pass and its
gradient are written out by hand and checked against finite differences (worst relative
error ~1e-9).

### The `gt` / `noisy` split

The two passes are not a stylistic choice:

| pass | render scale | accumulation | frame | role |
|---|---|---|---|---|
| `gt` | 1.0 | on | settled | the target |
| `noisy` | 0.5 | off | single frame | the input |

A converged, temporally-accumulated frame is a *poor* training target: it has almost no
noise left for the network to learn to remove, and the measured headroom of a
12-parameter affine fit against it is **+0.084 dB**. The single-frame reduced-res pass
has **+2.042 dB** of headroom — 24x more, and that is where the learnable signal is.

The exporter refuses a run whose captures differ in size (a frame-index off-by-one
produced blank frames early on), records a cumulative manifest keyed by frame id, and
can rebuild that manifest from the PNGs on disk with `--rebuild-manifest`.

---

## 3. Result

Held-out, 6 frames, raw input vs enhanced:

```
raw input    38.612 dB
enhanced     41.547 dB      (+2.935 dB)
```

Per-pair gain: **+0.77 to +6.28 dB**. Mean gate 0.4227, fraction of pixels with gate
below 0.05: **0.0000** — no frame is passed through unenhanced.

For scale, the baselines on the same split: identity 33.578 dB, a 12-parameter global
per-channel affine **36.440 dB**, and a per-scene affine *oracle* (which sees the answer
at test time) 38.257 dB. The network beats the affine control by **+2.323 dB** and beats
the oracle. SSIM 0.9765 -> 0.9863. `max |out - anchor| = 0.0577` — the frame is
adjusted, never replaced.

### A warning about dataset staleness

The first shipped model was trained on a dataset captured **before** a later shader
change, and it had learned a 5.33-luma lift the current renderer no longer produces. On
a fresh capture of the same camera it scored **37.040 -> 34.127 dB** — *worse than doing
nothing*. The blob was not wrong; it was correct about a renderer that no longer
existed.

If the renderer's shading changes, the dataset must be re-exported. A neural stage is a
statement about the renderer that produced its training data, and that statement has an
expiry date.

---

## 4. The blob, and why it has a checksum

`assets/neural/neural_weights.bin` — 4592 bytes:

```
offset 0    magic "WLNR"
offset 4    version 1
offset 8    n_in, n_hidden, n_residual, n_gate, n_weights
offset 28   CRC32 of the payload
offset 32   1140 little-endian float32 weights, in LAYER_SHAPES order
```

The CRC32 is not decoration. A flipped bit in an uploaded constant buffer is a silent
visual corruption with no shader error, no link failure, and no log line — the same
failure class as a ninth image uniform (see §6). `export_weights.py --check` refuses a
blob whose payload does not match its checksum, and a negative control confirms the
check actually fires.

---

## 5. Backend status — what is verified and what is not

This is the part most likely to be misread, so it is stated as plainly as possible.

| backend | shader | stage present | gate readback | actually rendered |
|---|---|---|---|---|
| **OpenGL** | GLSL | yes | yes | **yes — verified** |
| DX11 | HLSL twin | yes (by contract) | no | no |
| DX12 | HLSL twin | yes (by contract) | no | no |
| Vulkan | HLSL -> SPIR-V | yes (`OpName` check) | no | no |

### OpenGL — verified

Measured, not asserted:

- **Shader vs Python model**: `mean|d| 0.000026`, `max|d| 0.000262`, **0.000%** of pixels
  off. On the 8-bit PNG the same comparison reads `mean|d| 0.00144` — the difference is
  entirely the PNG's quantisation, which is why the readback path dumps RGBA32F instead.
- **Zero-weight control**: with every weight zeroed the network is exactly the identity,
  and the stage reproduces the anchor to `mean|d| 0.000000`. This is the control that
  distinguishes "the stage reads the weights correctly" from "the stage happens to look
  plausible".
- **Reproducibility**: two identical stage-off captures compare bit-identical
  (`max|d| 0.00000000`). Reproducibility is not the problem; a contaminated capture is
  (see §7).
- **Gate readout**: the panel's `gate mean` is a real reduction from pass 19, measured
  at **0.312** on a live capture — matching the model's per-frame gate for that frame.

### DX11, DX12, Vulkan — contract-verified only

The HLSL twin carries the identical stage: `RWStructuredBuffer<uint> NeuralGlobals :
register(u12)`, `StructuredBuffer<float> NeuralWeights : register(t74)`, the same
three passes, the same constants. `tools/verify_neural_stage_parity.py` asserts
statement-level parity between the GLSL and the HLSL, and the SPIR-V built by
`tools/compile_nova_spirv.py` contains `NeuralWeights`, `NeuralGlobals` and the
`gs_neural_*` arrays as `OpName`s.

That is a **contract**, not a rendering. Specifically:

- **DX12 hangs during initialisation** on this machine when forced, so the DX12 path
  has never produced a frame.
- **VulkanNova is compiled but never instantiated** — the same standard the project
  already applies to Vulkan generally.
- **The gate readback exists only on GL.** DX12 would need a staging readback that has
  not been written, so the DX12/Vulkan gate number does not exist yet, not merely
  unverified.

Anyone reading "the stage runs on all four backends" from the binding table alone would
be wrong. It runs on one, and the other three are wired, compiled, and parity-checked.

---

## 6. Binding layout

**OpenGL** — the stage claims **no image unit**. `GL_MAX_IMAGE_UNITS` is **8** on the
target GPU (RTX 3060 Ti, 4.5.0 NVIDIA 591.59), the Nova program already uses all of
them, and a ninth image uniform does not merely disable that pass — it **fails to link
silently** and kills the whole GL Nova path. Measured: 9 image uniforms -> mean luma
`0.00`; 8 -> `56.49`.

So the anchor is read with `imageLoad` from `img_Output` (each thread reads its own
texel, which it also writes), and everything else arrives through SSBOs:

| binding | contents |
|---|---|
| 4 | `NeuralGlobals` — frame means, partials, gate mean |
| 5 | `NeuralWeights` — the 1140 floats |

The weights are an SSBO and not a uniform array on purpose: 1140 components on top of
the ~600 the Nova program already declares would exceed OpenGL's guaranteed 1024 per
stage, and going over it fails to LINK silently.

`NeuralGlobals` is **2,621,460 bytes** — `g_Mean[4]` + `g_Partial[131072*4]` +
`g_GateMean` + `g_GatePartial[131072]` — sized from one shared definition
(`Wasteland/src/Wasteland/Renderer/NeuralStageBlob.h`) so the GL buffer, the HLSL
`RWStructuredBuffer<uint>` and the DX12 UAV descriptor cannot disagree.

`NEURAL_MAX_BLOCKS = 131072` is the next power of two above the 129600 blocks a
3840x2160 frame produces at 8x8 workgroups. It was first set to 8192 — enough for
1024x512 and nothing else — and the result was that pass 19 summed 8192 of a 2332x1451
frame's 53144 blocks, under-counting by 6.5x and reporting a mean 6.5x too small. The
under-count is silent: no shader error, no link failure, just a wrong image.

**DX12** — a different namespace. 13 UAV slots (`u0..u7, u9..u12`, with the `u8` hole
costing a slot), UAV base moved 52 -> 53, and the readable-SRV block grown from 10 to 11
entries to reach `t74`.

---

## 7. Verification traps this stage has already fallen into

Worth recording, because each one produced a plausible-looking wrong answer:

- **A stale editor process silently supersedes a capture.** A detached capture can be
  replaced by an old `DemonCore-Editor.exe` that never exited; the new process never
  takes over, and the old one's dump is compared against weights it never used. The
  symptom is a mismatch indistinguishable from a broken shader. The fix is to confirm
  no `DemonCore-Editor` process exists before starting one — and the reproducibility
  control (two identical stage-off captures, bit-identical) is what proves the engine
  was fine and the *capture* was the problem.
- **A barrier that out-of-bounds threads can skip is undefined behaviour.** The eval
  pass originally returned early for ragged workgroups, so the last 3 rows came out
  79.9% wrong and the last 4 columns 56.5% wrong — matching the 40/64 and 32/64 of the
  weight array the missing threads would have filled. The bounds guard is now a branch,
  not a return, and the gate reduction depends on that.
- **`glGetNamedBufferSubData` vs `glNamedBufferSubData`.** One character apart,
  identical signatures, compiles either way, fails only at runtime, silently. The wrong
  one writes into the buffer and reports `0.000` forever while the stage runs perfectly.
- **The 8-bit PNG is not a valid comparison target.** It is floored at the anchor's
  quantisation and mimics a stage bug. Float dumps are required.
- **A converged `input` pass is not a training target.** +0.084 dB of headroom is not
  a problem to solve.

---

## 8. Editor surface

The world panel's **WLNR (Wasteland Neural Renderer)** section exposes:

- `Neural Textures + Material` — the in-estimator neural heuristics (texture / light /
  material strength), which run per shading sample inside the trace loop. Independent
  of the trained stage below.
- `Neural Stage (trained MLP)` — the stage documented here. Persisted per scene as
  `NeuralStage: {Enabled: true|false}` in the `.wastescene` YAML; a missing key means
  default-true, so scenes written before the key existed load unchanged.
- A tier readout (`backend OpenGL | tier TierC (compute) | weights loaded`) and the
  measured `gate mean`.

The gate mean is the "is this doing anything" signal, and it is a **measurement**, never
a constant. An earlier draft of the panel would have printed a hardcoded `0.42` — a
plausible-looking number that makes a disabled or broken stage look healthy. It
distinguishes the two failure modes that are otherwise identical on screen:

| reading | meaning |
|---|---|
| `gate ~0.3–0.5` | the network is choosing to enhance this frame |
| `gate 0.000` | the stage is not running (disabled, or no weights blob) |

Python: `Renderer3D_IsNeuralStageEnabled`, `Renderer3D_SetNeuralStageEnabled`,
`Renderer3D_IsNeuralStageAvailable`, `Renderer3D_GetNeuralGateMean`.

---

## 9. How to reproduce

```
# 1. export the dataset (engine binary is driven by the exporter)
python tools/train_neural/export_dataset.py --out dataset_noisy --frame auto

# 2. regenerate the standardisation constants and write them into model.py + BOTH shaders
python tools/train_neural/dataset_stats.py --dataset dataset_noisy

# 3. train
python tools/train_neural/train.py --dataset dataset_noisy --out tools/train_neural/out --epochs 200

# 4. export the blob (CRC32 checked)
python tools/train_neural/export_weights.py --check

# 5. copy to the run dir's assets/neural/, then verify the shader against the model
python tools/verify_neural_stage_parity.py
```

The standardisation constants in `model.py` and in both shaders are **part of the ABI**:
`FEATURE_MEAN` / `FEATURE_STD` are measured on the training split, and a model trained
with one set and a shader carrying another will produce a confident, plausible, wrong
image.

---

## 10. Evidence

`docs/NEURAL_RENDERING_EVIDENCE/` holds the capture pairs and logs behind the numbers
above. Every PNG there is exactly 10,153,485 bytes — the engine's hand-rolled
stored-deflate writer emits uncompressed deflate blocks, so file size carries no
information and must not be read as a signal.
