# Clean-Room Neural Rendering (DLSS 5 analogue) Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Give Wasteland a neural rendering stage that enriches lighting and material detail from the engine's own frame — temporally stable, anchored to engine content — running on **every** backend (OpenGL / D3D11 / D3D12 / Vulkan) and **every** GPU, with no vendor SDK, no RTX requirement, and no per-vendor binary.

**Architecture:** A CPU-trained, GPU-executed MLP set (MLP-Mixer style) that runs inside the existing Nova compute pipeline as new passes at the **end** of the frame. Inputs are the engine's existing buffers: frame colour, velocity, depth, raster albedo, packed material, trace G-buffer. The engine frame is the anchor; the network proposes parameters and light, and a learned reliability gate keeps the model from inventing content. Runs as plain compute (`cs_5_0` / GLSL 4.30 compute) — no hardware RT, no tensor cores, no vendor runtime. Optional vendor SDK stays out of the tree per `docs/RTX_KIT_LICENSES.md`.

**Tech Stack:** C++17 engine (existing), GLSL + HLSL compute twins (existing), DXC/D3DCompile via the existing verifier path, Python 3 stdlib + `onnxruntime` + `numpy` for training and dataset evaluation (already installed on this machine), hand-written inference in the shaders (no vendor math lib).

---

## 0. Corrections to the premise — read before task 1

The user's phrasing is "DLSS 5 Neural Rendering". Two parts of that premise do not hold against this codebase, and the plan is built around the corrected version rather than the literal one.

### 0.1 "Neural Rendering" already exists here — and it is not a network

`wl_neural_tex_detail`, `wl_neural_micro`, `wl_neural_indirect`, `wl_neural_material` (`Renderer3D_NovaRenderer.glsl:756-900`, twin `.hlsl`) are **4-hidden-unit MLPs whose weights are shader literals**. They are hand-authored and hand-tuned, and their own comments say so:

- `.glsl:756` — "This is a hand-weighted network, not a trained one: the weights are the literals in the h0..h3 rows."
- `.glsl:829` — `wl_neural_indirect` "predicts one indirect bounce from a 4-unit MLP plus the two sky colours", and: *"this is a fixed analytic lobe with no occlusion from scene geometry, so it lights surfaces that should be in shadow."*

They are evaluated **per shading sample, inside the trace loop** (`.glsl:3993-4001`, `neuralK = strength * 0.65 * (1 - metal*0.8)`), i.e. at trace resolution, per pixel, before temporal accumulation. That is the opposite of DLSS 5's placement: NVIDIA's model runs **once per frame, at the end of the pipeline, on the whole frame**.

So the honest framing is: **the neural blocks exist; what is missing is the trained end-of-frame stage.** Task 3 removes the in-shading-sample block rather than leaving two competing "neural" systems.

### 0.2 "All hardware" rules out the actual DLSS 5 model

The RTX 50-only requirement is Tensor Cores plus an SDK that this project's license boundary forbids (`docs/RTX_KIT_LICENSES.md:7` — restricted RTX SDKs cannot be distributed in an open-source Wasteland; `RTX_KIT_LICENSES.md:3-11` sets the boundary and `docs/NOVA_FPS_DIAGNOSIS.md:597-598` records the project stance: "the project removed the NVIDIA RTX SDK and reimplements clean-room; do not reintroduce a vendor binary").

"Clean room" additionally means no copying of NVIDIA's published weights, architecture, or training procedure. This plan therefore targets a **from-scratch, openly-derivable** model: the published descriptions of the *class* of method (engine-buffer-conditioned, temporally-stable, artist-anchored enhancement) are used as a spec; the architecture, weights and training data are authored here from the engine's own frames.

**Consequence: the deliverable is a real, working, cross-hardware neural rendering stage — not a DLSS 5 replica, and not bit-compatible with anything.** Anyone who needs that distinction in writing, it is: same problem, independently solved.

### 0.3 The existing "neural" toggle is not a substitute and is currently load-bearing

`u_NeuralEnabled` defaults **true** (`Renderer3D.cpp:1475`) and is wired on all four backends (`Renderer3D.cpp:3509, 3752, 3981, 4127, 4521, 4936`). `Renderer3D_Basic.glsl:1759` states plainly: *"toggling u_NeuralEnabled changes the lighting response, not just [the detail effect]"*. So the "classic path" the new stage is meant to blend against **does not exist while the toggle is on** — the toggle is a mid-flight branch in the estimator, not a no-op.

Task 3 makes this explicit rather than silently leaving the toggle half-effective.

### 0.4 Two structural facts that shape every task

**The two shader twins are the ABI.** A new pass must be added to *both* `Renderer3D_NovaRenderer.glsl` and `.hlsl`, with identical hot-loop constants. `tools/verify_nova_shader_parity.py` is the gate; `tools/verify_nova_glsl.py` compiles the GLSL out-of-process because MSBuild has zero references to the shaders (`grep -ci "NovaRenderer\|\.hlsl\|\.glsl" Wasteland/Wasteland.vcxproj` → 0). **A clean build proves nothing about a shader change.**

**`BUILD_EXIT=0` is not evidence.** `docs/NOVA_SHADER_PIPELINE.md:85-89` records that both twins were once destroyed while the build stayed green. Run the shader verifiers after *every* shader edit.

The engine **does** link and run today (`bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe`, 2026-10-22) — the stale "engine does not link" premise in `tools/verify_nova_glsl.py:6-8` is out of date, but the script is still the only GLSL gate, so keep using it.

---

## 1. Measured ground truth (this plan is built on this, not on the docs alone)

| Fact | Source |
|---|---|
| Passes are selected by `u_PassID` in one compute shader; `main()` switches on it | `.glsl:5529-5566`, `.hlsl:3910-3943` |
| **Live and dispatched:** 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14 | `Renderer3D.cpp:3312-3340` (`RunNovaPassSchedule`) |
| **Declared but never dispatched:** 8 (`RunUpscale`), 15 (`RunTSRResolve`), 16 (`RunTSRCopy`) | `grep -rn 'pass(8)\|pass(15)\|pass(16)' Wasteland/src/` → nothing |
| Pass order is **not ascending and must not be "fixed"** | `Renderer3D.cpp:3306-3311` |
| A-rous iterations must be separate dispatches, driven from the CPU | `Renderer3D.cpp:3330-3340` (a shader-side loop recomputes iteration 1 four times) |
| Group size 8×8, one thread per output pixel | `.glsl:25`, `.hlsl:3909` |
| UAV slots in use: u0..u7, u9 (moments), u10 (reservoir write), u11 (history). **u8 is a deliberate hole** | `.hlsl:84-93,104-105` |
| Read SRVs: t48..t53 sampled + t64..t73 image reads | `.hlsl:109-114, 291-299` |
| Constant buffer `NovaConstants` = **1744 bytes**, 84 members, offsets asserted by a verifier | `VulkanNova.h:119-120`, `tools/compile_nova_spirv.py:245-249`, `VulkanNova.cpp:45-100` |
| Scene textures: 32 slots, bound as `u_SceneTextures[32]` / SRV range t15..t46 | `.glsl:472-503`, `Renderer3D.cpp:2863`, `DX12Nova.cpp:274` |
| Vulkan binding = `offset + slot` with shifts u=0, b=7, t=16, s=200; `u8` would collide with `NovaConstants` | `VulkanNova.h:35-44`, `compile_nova_spirv.py:55-58`, `docs/NOVA_BINDING_BUDGET.md` |
| A new UAV therefore needs `u12`+ on Vulkan; on DX11 the 8 is a local array rebuilt per dispatch, not a device limit | `docs/NOVA_BINDING_BUDGET.md` (DX11 section) |
| Backend caps: `RenderCapabilities` already has a `FeatureTier` ladder and a `TierFor()` resolver | `RenderCapabilities.h:16-60` |
| `denoiseComputeCapable` is `false` everywhere; the comment says it "only ever resolves for a future component" | `RenderCapabilities.h:20-25, 44-50` |
| CPU: no ML framework. `onnxruntime` **and** `numpy` are installed; `torch`, `onnx`, `tensorflow`, `sklearn` are not | measured this session |
| GPU: RTX 3060 Ti, 8 GB | `nvidia-smi` |
| No training infra, no dataset, no weight export path exists in the tree | `grep -rn 'torch\|onnx\|state_dict'` outside `vendor/` → nothing |
| VulkanNova is compiled but **never instantiated** — no `FlushVulkanNova` call site; Vulkan lands on the raster path | `Renderer3D.cpp:1931`, `docs/NOVA_DENOISER_WIRING.md:12` |
| Renderer runs software tracing in compute on all backends today | `RenderCapabilities.h:26-30` |

### 1.1 The binding budget is the real constraint — and it is tight

New pass needs, at minimum: 1 new UAV (output), 1 new read SRV (previous frame's output for stability), N new SRVs for its own weights constant buffer, and M new constant-buffer members.

- **Vulkan:** `u12` → binding 12 (free), `u13` → 13 (free). The SRV shifts are what bite: weights as an SSBO need a `t` register ≥ t74, and `VerifySpirvBindings()` (`VulkanNova.cpp:36-43`) fails the pipeline if a declared binding is unused or a used one is missing. **Every new register must be reflected in `kImageCount`, `kReadImageCount`, `kSampledSlot[]`, `kImageUavSlot[]` and the binding-set builder.**

> **Measured correction — see `docs/NEURAL_RENDERING_BINDING_PLAN.md`.** The
> draft's allocation was wrong in two places, both found by running the greps
> rather than trusting the table:
> 1. `u8` is **not declared by the shader at all**. It is an *absence*, not a
>    reservation. It is unusable only because `cbuffer NovaConstants : register(b1)`
>    holds Vulkan binding 8 (b-shift 7) — so `u12` is the right answer for the
>    right reason.
> 2. The `t` range is **not** free above `t4`. `u_SceneTextures[32] : register(t15)`
>    claims bindings 31..62 as a contiguous 32-wide range; a new `t`-register
>    inside it is absorbed into the array and binds the wrong buffer. New `t`
>    registers must be `< 15` or `> 73`.
>
> **Weights go in `NovaConstants`, not an SSBO** — appending them at offset ≥ 1616
> avoids a descriptor-table change and the DX12 block-base shift
> (`DX12Nova.h:165-192`). `t74` is therefore spare.
- **DX12:** `kUavCount = 12` at `DX12Nova.h:163` (covering `u0`..`u11`), `kUavBase = 52`, `kDescriptorsPerHeap = kUavBase + kUavCount` (= 64). `rootParams[10]` is declared and `rootParams[8]` is used — the constraint is descriptor-table entries, not root parameters (`docs/NOVA_BINDING_BUDGET.md`). Adding a UAV needs one more descriptor in `ranges[3]` and a `kUavCount` bump, not a new root parameter. The `kSamplerBase` / `kImageSrvBase` / `kUavBase` block bases must move **together** — `DX12Nova.h:165-192` is the record of the last such shift and is the template.
- **DX11:** `std::array<Image,8> images` at `DX11Nova.h:69`, a local `uavs[11]` at `:525` (already covering `u0..u7`, `u9`, `u10`, with `u8` deliberately null), and `srvs[128]` at `:512`. The 8 is a **construction convention, not a device limit** — `Dispatch` rebuilds the table per pass and `Unbind()` clears it, so slot meaning is per-dispatch rebindable. The binding budget doc's conclusion stands, but note `uavs[11]` must widen to 13.
- **OpenGL:** image units **0,1,2,4,5,6,7 are Nova's** and **unit 8 is already the generator's** (`Renderer3D.cpp:6632`), so the new stage takes units **9 and 10**. `glBindImageTexture` and `glActiveTexture` are **separate namespaces** — a previous survey confused them.

**Task 4 must not begin until `tools/verify_nova_binding_budget.py` has been extended to assert the new registers and its `--self-test` proves it can fail.** That script exists precisely because a subagent once concluded the moments target "does not fit as a 9th UAV" from a report instead of from source.

### 1.3 Vulkan is the risk that is not a risk

VulkanNova is not invoked, so a new pass cannot be visually verified there. **Verify Vulkan by contract, not by screenshot**: `tools/verify_vulkan_nova_contract.py`, `tools/verify_vulkan_reservoir_allocation.py`, and `tools/compile_nova_spirv.py --check`. Do not claim Vulkan renders.

> The draft also named `tools/verify_vulkan_binding_budget.py`; **that file does
> not exist**. `verify_nova_binding_budget.py` is the binding verifier, and the
> Vulkan-specific contract lives in `verify_vulkan_nova_contract.py`.

---

## 2. Approach

### 2.1 Where the stage sits

```
pass 1 (trace) → pass 0 (visibility+velocity) → pass 2 (temporal accum)
   → [pass 6 composite] → bloom → god rays
   → PASS N (neural prep: features)      ← new
   → PASS N+1 (neural eval)              ← new
   → PASS N+2 (stability + gate + write) ← new, writes img_Output
```

Three passes, deliberately:

1. **Feature pass** — gathers the engine buffers into one packed feature texture. Doing this as its own pass keeps the eval pass a pure per-pixel MLP and keeps the sampling cost off the critical path.
2. **Eval pass** — the network. One dispatch per frame, one thread per pixel.
3. **Stability/gate pass** — temporal accumulation of the *output*, rejection of disocclusions from velocity, and the learned reliability gate. This is what makes the result stable rather than shimmering, and it is what implements "the engine frame defines what must remain".

Pass 6 (composite) still writes `img_Output`; the new stage reads it and rewrites it in place at full resolution, after god rays. That matches DLSS 5's described position exactly ("the final rendering stage of the graphics pipeline") and it means the neural stage sees the *finished* frame, including bloom and volumetrics — which is what makes it able to enhance them coherently.

### 2.2 The network

**Not a conv net, not a U-Net, and not per-sample.** The reason is the hardware floor: the worst case in the matrix is D3D11 + OpenGL on a GPU with 8 compute UAVs and no fp16. A U-Net needs multiple intermediate textures and multiple dispatches; an MLP-Mixer needs only the weights constant buffer and the pixel's own features.

**Architecture (v1, deliberately small):**

```
per-pixel, no spatial convolutions:
  inputs : frame colour (3) + velocity (2) + depth (1) + raster albedo (3)
         + packed normal (3) + roughness (1) + metal (1) + AO (1)
         + previous output colour (3)          = 18 features
  stage 1: channel-mixing MLP   18 -> 32 -> 18   (token-mixing skipped in v1)
  stage 2: channel-mixing MLP   18 -> 32 -> 18
  head   : 18 -> 3 (residual, added to the anchored frame colour)
```

~1.6k weights. Weights live in a **constant buffer**, not a texture — that keeps the new-pass cost to one SSBO read of ~6 KB and avoids fighting the UAV budget for a weight texture.

**The output is a residual, never a replacement.** `out = anchor + gate * residual`, where `anchor` is the engine frame and `gate ∈ [0,1]` is learned per pixel. That single line is what makes "the engine frame is an unyielding foundation" mechanically true rather than aspirational, and it is what makes the worst case (network wrong) degrade to "no enhancement" instead of "wrong image".

**fp32 everywhere in v1.** No fp16 in either twin today (`grep -c 'f16|min16float'` → 0 in both). fp16 is a v2 optimisation behind a tier check, not a v1 risk.

### 2.3 Training data — the engine's own frames

No external dataset exists and none is fetched. The dataset is generated **from Wasteland itself**:

1. Render N scenes with the engine at `RenderScale = 1.0`, all enhancement off, no temporal accumulation, no jitter → **ground truth** (slow but correct).
2. Render the same N scenes at the preset's `RenderScale` (0.5/0.66/0.8/1.0) with jitter + temporal accumulation on → **input**.
3. Pairs are matched by frame, camera and scene state. Camera motion is captured via the existing `u_CameraMoved` / movement-latch machinery (`Renderer3D.cpp:941-953`).

This is a **self-supervised regression on the engine's own output**, which is the clean-room requirement and also the only dataset that matches the engine's actual shading. It is slow to produce — see task 2 for the budget — but it needs no network access and no licence review.

### 2.4 Hardware tiers

`RenderCapabilities::FeatureTier` already exists with a `TierFor()` resolver, and `RenderCapabilities.h:20-25` documents `denoiseComputeCapable` as the slot a future component fills. This stage fills it.

| Tier | Backend | What runs | Model size | Gate |
|---|---|---|---|---|
| **NeuralCompute** | DX11, DX12 | Full 3-pass stage, fp32 | 18→32→18 ×2 | on |
| **NeuralComputeLite** | OpenGL, DX11 low-end | 2 passes, merged eval+stability | 18→24→18 ×1 | on |
| **Off** | any | Composite bilinear, no stage | — | — |

Every tier **runs the same shader source**; they differ in pass count, group size and model depth, selected by a uniform. That is the whole point of the design: one authored shader, four backends, no `#ifdef` per vendor.

No tier is gated on a GPU vendor or on `tensorCores`. `RenderCapabilities::tensorCores` is declared and uploaded but not read (`RenderCapabilities.h:31`) — it must stay unused; adding a vendor gate would be the first thing to make this RTX-only, which is the thing being avoided.

---

## 3. Step-by-step

Each task is sized for a subagent. Every task that touches a shader ends with the verifier commands and **their actual output pasted into the commit message** — not "it passed".

---

### Task 1: Binding budget and pass-slot reservation (no shader change) — **DONE**

**Status: complete, verified 2026-10-23.** Outputs:
`docs/NEURAL_RENDERING_BINDING_PLAN.md` (new) and an extended
`tools/verify_nova_binding_budget.py` with four modes, all passing:
`baseline`, `--self-test`, `--self-test-dup`, `--self-test-scenetex`.
The measured corrections to the draft allocation are in the doc's §0, and the
plan text above carries a summary.

**Objective:** Reserve the registers and pass IDs the stage will use, and extend the binding-budget verifier so a collision cannot slip through again.

**Files:**
- Modify: `tools/verify_nova_binding_budget.py`
- Create: `docs/NEURAL_RENDERING_BINDING_PLAN.md`
- Read-only: `Wasteland/src/Platform/NVRHI/VulkanNova.h`, `DX12Nova.h`, `DX11Nova.h`, `docs/NOVA_BINDING_BUDGET.md`

**Step 1 — decide the allocation from source, not from a report.**

Greps to run and record the answer to in the doc:
```bash
grep -n 'register(' DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl
grep -n 'layout(binding' DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl
grep -n 'kImageCount\|kReadImageCount\|kSampledSlot\|kImageUavSlot' Wasteland/src/Platform/NVRHI/VulkanNova.h
grep -n 'kUavCount\|kUavBase\|kImageSrvBase\|kSamplerBase' Wasteland/src/Platform/NVRHI/DX12Nova.h
grep -n 'std::array<Image' Wasteland/src/Platform/NVRHI/DX11Nova.h
grep -n 'pass([0-9]' Wasteland/src/Wasteland/Renderer/Renderer3D.cpp
```

Expected allocation (verify, do not assume):

| Resource | Register | Vulkan binding | Notes |
|---|---|---|---|
| Neural feature output UAV | `u12` | 12 | free (u8 is the hole, u9/u10/u11 taken) |
| Neural history UAV | `u13` | 13 | free |
| Neural weights SSBO | `t74` | 90 | ≥ t74; must be added to the SRV range table |
| Feature read SRV | `t75` | 91 | for the eval pass |
| History read SRV | `t76` | 92 | for the stability pass |
| Pass IDs | 17, 18, 19 | — | 8/15/16 stay reserved-but-dead; do not reuse them, a future upscale/TSR landing will want them |

**Step 2 — extend the verifier.**

Add to `tools/verify_nova_binding_budget.py`:
- an `EXPECTED` table of the above registers with their intended purpose;
- an assertion that every `register()` in the HLSL maps to a known, non-colliding Vulkan binding, including the new `t74..t76` and `u12..u13`;
- an assertion that `u8` is still a hole and is not silently reused.

Keep the existing `--self-test`. **Add a second self-test mode that injects a colliding `register(u12)` twice** (two resources claiming the same binding) and asserts FAIL. A verifier that only catches one collision shape is the failure mode the file's own docstring warns about.

**Step 3 — run it.**

```bash
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_nova_binding_budget.py --self-test
```

Expected: PASS, PASS. If the first fails, the allocation in step 1 is wrong — fix the allocation, not the verifier.

**Step 4 — commit.**

```
docs(verify): reserve registers u12/u13/t74-t76 for the neural rendering stage
```

---

### Task 2: Dataset generator (Python, no engine change)

> **STATUS: DONE 2026-10-23.** `tools/train_neural/export_dataset.py` and
> `tools/train_neural/README.md` exist, are stdlib-only, and have been run
> end-to-end. See "Task 2 results" at the end of this section. The exporter
> writes 72 frames across 12 scenes in ~13 minutes. **14 of those 72 frames are
> flagged edgeless and the exporter exits non-zero because of them** — see the
> results section before trusting a dataset from this tool.

**Objective:** Produce matched (input, ground-truth) frame pairs from the engine itself, with a manifest that records scene, camera, preset and render scale.

**Files:**
- Create: `tools/train_neural/export_dataset.py`
- Create: `tools/train_neural/README.md`
- Modify: `DemonCore-Editor/src/EditorLayer.cpp` (screenshot path only)
- Read-only: `EditorLayer.cpp:588-700` (`CaptureFramebufferPNG`, `WL_SCREENSHOT` / `WL_SCREENSHOT_FRAME` env vars)

**Step 1 — confirm the capture hook.**

`EditorLayer.cpp:706-735` already implements env-var-driven capture:
```bash
grep -n 'WL_SCREENSHOT' DemonCore-Editor/src/EditorLayer.cpp
```
It writes to `WL_SCREENSHOT` (default `wasteland_screenshot.png`) on frame `WL_SCREENSHOT_FRAME`. **Verify this still works before building anything on it** — run the editor with the env var set and confirm a non-black PNG appears. A capture path that returns true while writing a black image is the documented failure mode of the first version of this function (`EditorLayer.cpp:595-601`).

**Step 2 — write the exporter.**

`tools/train_neural/export_dataset.py`, stdlib only:
- reads a scene list (hardcoded for v1: `assets/scenes/*.wastescene`, pick ≥ 8 with varied content);
- for each scene: sets the preset, launches the editor with `WL_SCREENSHOT=<out>` and `WL_SCREENSHOT_FRAME=<n>`, waits for the file to appear, kills the process;
- emits `dataset/manifest.json` with, per frame: scene path, preset, `RenderScale`, camera position/rotation, frame index, and the SHA-256 of each output PNG.

**Step 3 — capture the two passes per scene.**

For each scene, twice:
1. `RenderScale=1.0`, neural toggle off, jitter off, no temporal accumulation → `gt/` (ground truth).
2. Preset scale, jitter on, temporal accumulation on → `input/`.

Determinism matters: the same scene, same camera, same frame index. If the engine cannot freeze the camera from the environment, **say so in the README and stop** — a dataset whose pairs do not correspond is worse than no dataset, and task 5 will silently train on misaligned pairs.

**Step 4 — validate the manifest.**

```bash
python3 tools/train_neural/export_dataset.py --validate
```
Asserts: every `gt/` file has exactly one `input/` file, dimensions match, SHA-256 present, no zero-byte files, no all-black images.

**Step 5 — commit.**

```
feat(train): add self-supervised dataset exporter for the neural rendering stage
```

**Note:** this task is where the "clean room" requirement is actually satisfied — the dataset is the engine's own output, so no third-party imagery enters the tree.

---

### Task 2 findings (2026-10-23) — the camera hook exists, but there is no image to capture

Executed the capture work. Three of the plan's assumptions were wrong, and the
fourth is a hard blocker. **Read this before writing `export_dataset.py`.**

**1. The camera-freeze blocker in this plan does not exist.** `WASTELAND_PIN_CAMERA`
is already implemented (`EditorLayer.cpp:534`, parsed as
`focalX,focalY,focalZ,distance,pitch,yaw`, sets `m_CameraPinned` so `OnUpdate`'s
camera control is skipped at `:804`). The log confirms it takes effect:
`Pinned editor camera: focal (0,1,6) distance 14 pitch -8 yaw -35`. Step 3's
"if the engine cannot freeze the camera from the environment, say so and stop"
does not apply.

**2. The capture path was broken in three ways, all now fixed and verified.**

| Defect | Symptom | Fix |
|---|---|---|
| Single-shot, no retry | Fired once at frame N, read the target before the Nova temporal history converged → a black image indistinguishable from a broken shader | Retry window: `WL_SCREENSHOT_FRAME <= frame < +WL_SCREENSHOT_RETRY_WINDOW` (40), armed once at the window's first frame |
| No linger | The window closed the instant the shot landed; the OS tore the device down mid-readback | `WL_SCREENSHOT_LINGER=<n>` closes after n frames via `Application::Close()` |
| Wrong output path | `$TMPDIR` resolves to `/tmp`, which native Windows programs cannot see — `std::ofstream` silently failed | The exporter must pass a real Windows path (`C:/...`), not an MSYS one |

**3. `ReadbackRayTraceTarget` was DX12-only, and the editor's guard made it
unreachable anyway.** Two independent defects:

- The function returned false unless `s_Nova12` was live. On OpenGL the traced
  image is `s_Data.RayTracingOutput`, so **there was no GL readback at all**.
  Added one: `glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, ...)` on that
  texture's `GetRendererID()`. `glGetTexImage`, never `glReadPixels` — the output
  is bound as a compute image, not to the default framebuffer.
- The editor guarded the branch on `m_FramebufferAPI == NVRHI_DX12`, but
  `m_FramebufferAPI` tracks the **window/editor** backend (OpenGL) while Nova
  runs on DX12. Those are different enumerators, so the guard could never be
  true while `s_Nova12` was live — the readback was dead code and every capture
  fell through to a `glReadPixels` of the editor window. Guard is now
  `Renderer3D::IsRayTracingEnabled()`.

**4. RESOLVED — there was never a blank render. The camera was pointed at sky.**

The previous revision of this section called the near-black capture a renderer
defect and made it a hard blocker. That diagnosis was wrong, and the error is
worth recording because the evidence looked conclusive.

What was measured, with `WASTELAND_PIN_CAMERA="0,1,6,14,-8,-35"`:

```
2332x1451, 307612 samples
mean=12.56  min=10  max=17   (of 255)
100% of samples in [0,31]; 0% exactly zero
```

and the editor's own viewport was black. Two independent read paths agreeing on
a black image is a strong signal, and it was read as "the tracer is broken".

It is not. Re-measured properly:

| Capture | mean luma | luma range | local-contrast p99 | verdict |
|---|---|---|---|---|
| `0,1,6,14,-8,-35` (the "blank" one) | 13.8 | 12–17 | **1.2** | sky only |
| `0,-20,10,30,15,120` | 31.0 | ~31 | **1.0** | sky only |
| `0,0,0,9,-85,0` (looking down) | 30.7 | 1–72 | **15.9** | **floor, box, cube, sphere, shadows** |

The discriminator is **local contrast**, not mean luma. A ray tracer with 7
instances in the BVH produces edges; a camera pointed at open sky produces a
smooth gradient. The first two captures have p99 local contrast of ~1 out of
255 — a perfectly correct render of an empty sky. The third has p99 15.9 and a
max of 41.6, and a visual read confirms floor, a rectangular block, a cube, a
sphere and cast shadows.

So the renderer was never broken, and the viewport was not either — both were
showing the same correct image of nothing. **Mean luma alone cannot distinguish
"dark scene" from "no scene"; local contrast can.** That is why
`export_dataset.py` measures local contrast on every capture and records it in
the manifest, and why `--validate` treats a low-contrast frame as a warning and
a black one as an error. A dataset of sky-only targets would have been the
real defect, and it would have surfaced as a flat training curve weeks later.

**Consequence for the plan.** Task 2 is unblocked. The blocker is withdrawn and
the exporter is written (Step 2), run (Step 3) and validated (Step 4).

**What is still true from the earlier revision:**

The DX12 path is not a workaround: setting `RendererAPI: 3` in `project.wasteland`
switches the window to DX12 and then **hangs during init** — the log stops after
`BlueNoise: loaded ...`, never reaching `DX12 Nova active`. (Project file restored.)

Also note `RendererAPI::GetAPI()` is **OpenGL** here, so
`Renderer3DNVRHIAvailable()` is false and the entire Nova init block
(`Renderer3D.cpp:2486`) is skipped. The `NVRHI Renderer API initialized: DirectX
12` line is the NVRHI *device*, a different object from `RendererAPI::s_API`.
Do not read that line as "Nova is on DX12". The Nova trace timings come from GL
queries (`glBeginQuery(GL_TIME_ELAPSED, ...)` at `Renderer3D.cpp:4672`), which
confirms the GL path is the one running.

**5. The renderer has no environment knobs for the capture state.** The plan's
Step 3 needs `RenderScale`, `QualityPreset`, the neural toggle, temporal
accumulation and the scene, and none of them were reachable without driving the
UI. Added to `EditorLayer::OnAttach`, immediately after `OnProjectChanged()`
(which is what loads the start scene and re-applies the project's preset, so an
override placed above it would be undone):

| Env var | Effect |
|---|---|
| `WL_CAPTURE_SCENE=<path>` | loads a scene (absolute or project-relative) |
| `WL_CAPTURE_PRESET=<0..3>` | `QualityPreset` (0 Low, 1 Medium, 2 High, 3 Ultra) |
| `WL_CAPTURE_SCALE=<f>` | `RenderScale`, clamped 0.25–1.0 |
| `WL_CAPTURE_NEURAL=<0\|1>` | `NeuralRenderingEnabled` |
| `WL_CAPTURE_ACCUM=<0\|1>` | `RayTracingAccumulate` (temporal blend) |

All are inert unless set. They live in `EditorLayer.cpp`, not in the render
path, which keeps the plan's "screenshot path only" constraint honest.

**Why the GT pass uses temporal accumulation rather than the plan's "no temporal
accumulation, jitter off".** The plan's Step 3 specifies a single-sample target.
That target is *noisy*, and a network trained to reproduce it learns to
reproduce noise. The GT pass instead runs at `RenderScale=1.0` with accumulation
on and captures at frame 400 — past the frame-4 point where the accumulation
alpha drops to its 0.02 floor, so the accumulated image is a running average over
roughly the last 50 frames and the sub-pixel jitter has averaged out. That is a
real converged reference, and it keeps the pair clean: **`gt` and `input` differ
only in render scale**, so the network is never asked to undo a difference the
dataset introduced by accident. A `noisy` pass (single sample, reduced res) is
also defined for anyone who wants the harder variant.

---

### Task 2 results (2026-10-23)

**Delivered.** `tools/train_neural/export_dataset.py` + `tools/train_neural/README.md`.
Stdlib only (own PNG reader, own unfilter, own luma stats). No new dependency.

**What the exporter does.** Per (scene, camera, pass) it launches the editor
headless with the `WL_CAPTURE_*` / `WASTELAND_PIN_CAMERA` / `WL_SCREENSHOT*`
environment, waits for the process to close itself via the linger, then measures
the PNG and records it in `manifest.json`. `--validate` re-reads the manifest and
asserts pairing, dimensions, SHA-256, non-black and edgeless.

**Run 1 — `--frame preset` (hand-tuned camera list), 72 frames:**

| scene | edgeless frames |
|---|---|
| Example3D | 0 / 6 |
| ExampleGame | 0 / 6 |
| PhysicsPlayground | 0 / 6 |
| PrimitiveGallery | 0 / 6 |
| TerrainShowcase | 0 / 6 |
| ScriptTest | 0 / 6 |
| LightingShowcase | 0 / 6 |
| MaterialGallery | 1 / 6 |
| InventoryDemo | 2 / 6 |
| AtmosphereShowcase | 4 / 6 |
| WaterShowcase | 6 / 6 |
| DemoGame | 6 / 6 |

**Run 2 — `--frame auto` (per-scene framing from the scene's own content):**
14 edgeless out of 72, down from 17. Example3D went from p99 7.6–29.2 to 9.6–19.4;
ExampleGame from 4.1–20.0 to 9.3–17.0.

**Run 3 — `--frame auto` with a per-scene distance ceiling** (`AUTO_DISTANCE_MAX`
was a fixed 150 m; it is now `max(150, p90)` per scene, because a fixed ceiling is
right for a compact scene and catastrophic for an open world):

| scene | run 2 | run 3 |
|---|---|---|
| DemoGame | 150 m, p99 1.5–2.5 | **1228.5 m, p99 26.3** |
| all 11 others | — | unchanged (distance still comes from the median spread alone) |

DemoGame confirmed by direct probe at three distances: 150 m → 2.5, 600 m → 27.1,
1228 m → 26.3. The 600–1228 m range is where the scene becomes usable; below it
the camera is inside a patch of near-black ground.

**A rejected intermediate.** Adding a p90 term to the *distance value* (not just
the ceiling) looked principled and is not: Example3D has a few entities ~56 m out,
which pushed its camera from 13 m (p99 9.6–19.4) to 28 m for no gain. Reverted —
p90 feeds the ceiling only.

**Run 3 also exposed a real robustness gap: transient capture failures.** 3 of 72
frames failed with "no PNG written" (2 DemoGame, 1 ExampleGame). All three
succeeded on an immediate re-run, so the cause was the editor dying mid-run — a
device reset / TDR, which is a known failure mode on Ultra — not a bad scene. The
DemoGame log showed the shape of it: `app 137.3ms (7.3 fps)`, `cpuSubmit 131.2ms`,
`GPU TIMER DEGRADED: 201 glBeginQuery failures`, and the process exiting before
frame 400.

`capture()` now retries each frame up to `--attempts` times (default 2, 5 s
between attempts) and records the attempt count per frame in the manifest. Unit
tested against a fake that fails N times then succeeds: 1 transient failure
recovers on attempt 2, 2+ gives up cleanly and still exits non-zero. Without this,
one GPU hiccup killed a 13-minute export at frame 70.

**What is still edgeless, and why it will not be fixed by a constant:**

| scene | frames | cause |
|---|---|---|
| WaterShowcase | 4 / 6 | a flat water plane is genuinely edgeless. Probed 8, 12, 16, 21, 30 m — p99 0.8–3.9 throughout |
| AtmosphereShowcase | 4 / 6 | 2 renderable entities 4 m apart; mostly atmosphere. 14 m gives p99 4.92, the derived 12 m gives 3.92 |

AtmosphereShowcase is a near-miss worth naming precisely, because the obvious fix
is the wrong one. `AUTO_DISTANCE_MIN = 12.0` is just barely too close for it.
Raising the minimum to 14 would fix that scene — and would move all 11 others,
since the minimum binds for several of them. That is the same error as lowering
the contrast floor, in the other direction. Both scenes need their own camera
(the `--frame preset` path), which is data curation rather than tooling.

**What this means for the plan.** Task 2's deliverable is done and its output is
usable for 10 of 12 scenes, with DemoGame recovered by the per-scene ceiling.
**Task 5 must not train on a dataset without running `--validate` first**; the
gate is the only thing standing between a clean run and a flat training curve.

---

### Task 3: Baseline the current neural path and make the toggle honest

**Objective:** Remove the competing in-shading-sample neural block from the trace, and make `u_NeuralEnabled` select between "classic estimator" and "neural stage" rather than being a mid-estimator blend.

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl`
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`
- Create: `tools/verify_neural_stage_parity.py`

**Step 1 — record the current behaviour first.**

Before changing anything, capture three screenshots of the same scene with the same camera:
- neural off, `RenderScale=1.0` → `baseline_classic.png`
- neural on, `RenderScale=1.0` → `baseline_neural_on.png`
- neural on, `RenderScale=0.5` → `baseline_neural_low.png`

These are the reference images tasks 6 and 7 compare against. Save them under `docs/NEURAL_RENDERING_EVIDENCE/`.

**Step 2 — remove the in-sample neural block.**

In `.glsl`, the block is at `:3927-3931` (indirect-ray skip), `:3993-4001` (radiance blend) and the `else if` at `:4001-4004`. In `.hlsl`, the twins at `:2614`, `:2654-2660`.

Remove the *blend*; keep the estimator. Concretely:
- delete the `wl_neural_indirect(...)` calls in the indirect path;
- delete the `maxIndirectRays = 0` skip at `:3927-3931` (it exists only to save the trace cost the neural cache was replacing);
- keep `wl_neural_material` and `wl_neural_tex_detail` — those are material/appearance, not GI, and they stay under `u_NeuralTexStrength` / `u_NeuralMatStrength`;
- update the `u_NeuralEnabled` comment at `.glsl:509-515` and `.hlsl:408` to state what it now selects.

**This is the highest-risk edit in the plan.** It changes lighting on every backend and preset. Follow `docs/NOVA_SHADER_PIPELINE.md:91-93`: **back up both files first** (sha256-verified, three locations), because the GLSL rewrite exists only in the working tree and a `git checkout` would destroy it silently.

**Step 3 — keep the twins identical.**

Hot-loop constants must match exactly. `wl_neural_indirect`'s `0.35f`, `0.45f`, `0.25f` and the `0.65f` neuralK factor are the numbers to check; the parity verifier compares executable tokens, and a silent semantic change passes with rc=0 (`docs/NOVA_SHADER_PIPELINE.md:37-40`).

**Step 4 — write the parity verifier.**

`tools/verify_neural_stage_parity.py`: extracts the pass bodies from both twins, strips comments, and asserts the executable token sequences are equal — the same technique as `tools/verify_nova_shader_parity.py:22-30`. Add a negative control: mutate one twin's constant and assert the verifier FAILs.

**Step 5 — verify.**

```bash
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_neural_stage_parity.py
python3 tools/verify_nova_glsl_negative_control.py
```
All four must PASS, with output in the commit message.

**Step 6 — build and confirm visually.**

```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSB" Wasteland/Wasteland.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```
Then capture the same three screenshots as step 1 and confirm: classic and neural-off are **identical** (byte-for-byte is too strong; compare with a pixel-diff threshold and record the max delta), and the preset's render scale still upscales.

**Do not report this task complete on a clean build.** The acceptance is the pixel comparison.

**Step 7 — commit.**

```
refactor(nova): move neural enhancement out of the shading estimator
```

**Results (2026-10-23).**

Baselines captured before any edit, into `docs/NEURAL_RENDERING_EVIDENCE/`
(three PNGs plus their run logs and a `BACKUP_SHA256.txt`). Both twins were
backed up to two locations and sha256-verified before touching them.

| capture | neural | scale | mean luma | contrast p99 |
|---|---|---|---|---|
| `baseline_classic` | off | 1.0 | 56.50 | 14.35 |
| `baseline_neural_on` | on | 1.0 | **48.85** | 17.35 |
| `baseline_neural_low` | on | 0.5 | **43.14** | 11.14 |
| `after_classic` | off | 1.0 | 54.35 | 19.63 |
| `after_neural_on` | on | 1.0 | **54.30** | 19.41 |
| `after_neural_low` | on | 0.5 | **54.22** | 11.84 |

**The toggle now does nothing, which is the point.** `after_classic` vs
`after_neural_on` agree to **0.05 luma**; before the change the same pair differed
by **7.65 luma** (56.50 vs 48.85). The removal also collapsed the reduced-scale
result onto the full-scale one (43.14 -> 54.22 vs 54.35): the estimator is
resolution-independent again, because the `maxIndirectRays = 0` skip that made
scale 0.5 take a different path is gone.

**Per-pixel diff is not usable as an acceptance test here, and the plan should
not claim otherwise.** `u_Time` (`Renderer3D_NovaRenderer.glsl:544`) drives
volumetric advection, cloud noise and Gerstner waves from wall-clock, so two
runs at different times produce different fog patterns. `baseline_classic` vs
`after_classic` -- the classic path, whose code is untouched by this task --
differ in **87.3% of pixels** and **2.15 luma** of mean. That 2.15 is the noise
floor, and the effect removed (7.65) is ~3.6x it while what remains (0.05) is
~43x below it. Mean luma is the only robust discriminator for this scene;
`--allow-blank` and the contrast metric are unaffected.

**The plan missed a consequence, now fixed.** `EditorLayer.cpp:1813` exposed a
checkbox labelled **"Neural Textures + Lighting"** and a **"Neural Light
Strength"** slider (`:1824`). With the in-estimator GI blend gone,
`u_NeuralLightStrength` is declared in both twins and never read, so the slider
wrote to a dead uniform -- the same dishonesty the task exists to remove, one
layer out. The checkbox is relabelled "Neural Textures + Material" (it still
gates the material and texture paths) and the dead slider is removed. The
getter/setter stay, since the Python bindings use them.

**`wl_neural_indirect` is now dead code and is left in place deliberately.** It is
defined at `.glsl:835` / `.hlsl:583` and called from nowhere. Removing it, and
removing the now-unread `u_NeuralLightStrength`, would shift the cbuffer layout
on both twins at once -- which is exactly the kind of layout change
`verify_vulkan_nova_contract.py` and `verify_nova_constant_names.py` exist to
guard, and it is out of scope for a task whose job is to stop a blend, not to
re-lay out a constant buffer. Flagged for a later cleanup rather than done here.

**`tools/verify_neural_stage_parity.py` (new).** Compares the indirect-lighting
estimator region -- from `int maxIndirectRays` to the mirror block -- between the
twins, normalized the same way as `verify_nova_shader_parity.py` plus a scalar
splat expansion (`vec3(a)` <-> `vec3(a, a, a)`). That expansion is required: the
twins spell the same `F0` base value differently by idiom, and without it the
verifier reports a difference on a line this task never touched. Also asserts
neither twin still references `u_NeuralLightStrength` or `wl_neural_indirect` in
the estimator, and carries negative controls for both twins (mutating the bounce
offset must FAIL). Both controls fire.

**Verification:** `verify_neural_stage_parity` PASS; `verify_nova_glsl` PASS;
`verify_nova_shader_parity` PASS; `verify_nova_constant_names` PASS (84 pushed,
all declared). Suite is 19 PASS / 2 FAIL, both pre-existing:
`verify_nova_glsl_negative_control` (two of its own negative controls are weak)
and `verify_texture_sampler_indexing` (expects 9 image declarations, the GLSL has
8 -- at HEAD, and this task only touched the HLSL's neighbourhood).

---

### Task 4: Feature pass (pass 17)

**Objective:** Add the feature-gathering pass to both twins and dispatch it, without changing the final image yet.

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl`
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/VulkanNova.h`, `VulkanNova.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/DX12Nova.h`, `DX12Nova.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/DX11Nova.h`

**Step 1 — write the GLSL half.**

Add to `.glsl`, after `RunTSRCopy` and before `main()`:

```glsl
// Pass 17: neural feature gather. One thread per output pixel. Reads the
// finished frame and the engine buffers, packs them into one rgba32f feature
// image, and writes nothing else. Deliberately separate from the eval pass so
// the eval pass is a pure per-pixel MLP with no gather cost on its critical
// path, and so the buffers it samples are not re-sampled per network layer.
layout(rgba32f, binding = 12) uniform image2D img_NeuralFeatures;
```

Body: sample `s_Output`, `s_DepthBuffer`, `img_Velocity`, `s_InputAlbedo`, `img_TraceGBuffer`, `s_PackedMaterialMap`; pack into a `vec4`; `imageStore` to `img_NeuralFeatures`. Guard every sample with a bounds check — `RunUpscale`'s comment at `.glsl:4955-4960` documents the negative-index bug class that comes from taking `floor()` before clamping.

**Step 2 — add `case 17:` to both `main()` switches** and to the `u_SceneTextures` sampler indexing if a new texture is needed.

**Step 3 — mirror in the HLSL twin.** Identical constants, identical packing order. The packing order is an ABI: the eval pass and the trainer must agree on it byte-for-byte.

**Step 4 — dispatch it from `RunNovaPassSchedule`.**

`Renderer3D.cpp:3306` — add after the god-rays block, before `CommitHistory`. Order matters and is commented there: the stage runs after the composite and after god rays so it sees the finished frame.

**Step 5 — wire the new registers on each backend.**

- Vulkan: `kImageCount` 9 → 11, `kImageUavSlot[]` gains 12, 13; `kReadImageSlot`/`kReadImageIndex` gain the new SRVs; `kSampledSlot` gains t74..t76; `CreateBindingSet` and `VerifySpirvBindings` updated.
- DX12: `kUavCount` 9 → 11; `kDescriptorsPerHeap` follows; the UAV descriptor loop at `DX12Nova.cpp:519-523` and the manual `kUavBase + i` writes updated; `ranges[3]` count updated.
- DX11: `std::array<Image,8> images` → the new count; `uavs[]` widened; the t74..t76 SRVs bound into the `srvs[128]` table.
- OpenGL: `glBindImageTexture` for the new images; image unit 8 is free but the new images need their own units — measure and record which.

**Step 6 — verify the budget still holds.**

```bash
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_vulkan_nova_contract.py
python3 tools/verify_vulkan_reservoir_allocation.py
python3 tools/compile_nova_spirv.py --check
```

**Step 7 — accept: the image must not change.**

This pass writes only `img_NeuralFeatures`, which nothing reads yet. Capture before/after screenshots and confirm a **zero** pixel delta. Any delta means the pass wrote to `img_Output` — stop and find it.

**Step 8 — commit.**

```
feat(nova): add neural feature gather pass (17)
```

**Task 4 findings (2026-10-23). Task 4 is NOT complete, and the reason is a
measured driver limit the plan did not know about.**

**1. The GLSL is OpenGL-only; the HLSL serves DX11, DX12 *and* Vulkan.**
This is the fact that reframes the whole task and the plan gets wrong.
`VulkanNova.cpp:387` loads `Renderer3D_NovaRenderer.spv`, not the `.glsl`, and
`tools/compile_nova_spirv.py:8-10` states it compiles the **HLSL** with DXC to
produce that SPIR-V. Only `Renderer3D.cpp:2604` loads the `.glsl`. So
`register(u12)` in the HLSL covers three backends and `binding = 12` in the GLSL
covers one -- and that one is capped.

**2. `GL_MAX_IMAGE_UNITS = 8` on the target GPU. Measured, not assumed.**
The plan's step 5 says "image unit 8 is free". It is not, and there is no unit
8 at all:

```
[21:40:44] WASTELAND:  Renderer: NVIDIA GeForce RTX 3060 Ti/PCIe/SSE2
[21:40:44] WASTELAND:  Version: 4.5.0 NVIDIA 591.59
[21:40:44] WASTELAND:  Max image units: 8 (GL 4.3 guarantees 8)
```

`OpenGLContext.cpp:145` now logs this permanently. Units are 0..7, of which the
Nova schedule binds 0,1,2,4,5,6,7. `img_NeuralFeatures` needs a ninth image
uniform, and **a compute shader declaring nine image uniforms fails to LINK on
such a driver** -- which would take down the entire GL Nova path, not merely the
neural stage. `verify_nova_glsl.py` already enforces the limit and carries a
negative control for the ninth uniform; it caught this on the first run.

So the GLSL addition was reverted and replaced with a comment recording why.
Pass 17-19 have no GLSL case, and the GL schedule must not dispatch them.

**The failure is SILENT and total, and it was measured.** The edit was built and
run before it was reverted, so both outcomes are on record from the same scene,
same camera, same frame:

| GLSL | image uniforms | mean luma | contrast p99 |
|---|---|---|---|
| with pass 17 | 9 | **0.00** | **0.00** |
| after revert | 8 | 56.49 | 14.44 |

A ninth image uniform does not merely make pass 17 unavailable -- it breaks the
whole GL Nova path, and it does so **without reporting a shader error**. This is
the exact failure mode `OpenGLShader.cpp:150-160` documents for the loader: a
program that fails to produce a usable stage still *links*, so
`glGetUniformLocation` returns -1 for everything, `glUniform` against -1 is a
documented no-op, `u_PassID` stays at its default 0, and "the ray-traced viewport
stayed black with no shader error reported". The verifier caught it in seconds;
a human would have chased a black viewport for much longer.

This is also why the revert is the right call rather than a workaround: there is
no ninth unit to bind, so the pass cannot be made to work on GL by rearranging
anything.

**3. The plan's dispatch line reference is stale.** Step 4 says "add after the
god-rays block, before `CommitHistory`" at `Renderer3D.cpp:3306`. In the current
file `CommitHistory` is at `:3331`, *before* the god-rays block at `:3359`. The
intent (after composite and god rays, so the pass sees the finished frame) is
still right; the line number is not.

**4. The plan's feature list cannot fit the reservation.** Step 1 says to sample
six sources and "pack into a `vec4`". The reservation gives the stage one rgba32f
UAV (`u12`) = four floats. Implemented as `(luma, depth01, velocity.x,
velocity.y)` -- Rec. 709 luma, not a flat average, because the network's notion
of brightness has to match the eye's. The other three sources are read directly
by pass 18, their only consumer. The packing order is an ABI and is stated as
one in the shader.

**5. Pre-existing bug this exposed: `glBindImageTexture(8, ...)` is a silent
no-op.** `Renderer3D.cpp:6685` binds `s_Data.GeneratedTextureID` to image unit 8.
Unit 8 does not exist on this driver, so the call sets `GL_INVALID_VALUE` and the
procedural material generator never writes its output. It is a *different phase*
from the Nova schedule, which is why nothing has noticed. Not fixed here -- it
needs its own change and its own verification -- but it is the same class of
defect as the `u_MaxLights` parity bug: a binding that looks correct and is not.

**State of Task 4:**
- HLSL pass 17: written, `register(u12)`, compiles clean under
  `dxc -T cs_5_0 -E main` (267,960-byte blob, only the pre-existing `:2857`
  vector-truncation warning).
- GLSL pass 17: deliberately omitted, documented in-file.
- Dispatch and the Vulkan/DX12/DX11 register wiring: **not done.**

**Why the wiring is not being shipped blind.** Task 4's own acceptance test is
"capture before/after and confirm a zero pixel delta". That test cannot be run:
the GL path cannot host the image at all, DX12 hangs during init when forced,
and VulkanNova is compiled but never instantiated. Editing `kImageCount`,
`kUavCount`, the DX11 `std::array<Image,8>` and the descriptor heaps across three
backends with no way to execute or observe any of them risks breaking them for
the users who can. The binding tables are exactly where a wrong constant is
invisible until it is not.

**To finish Task 4**, the remaining work is: dispatch `pass(17)` in
`RunNovaPassSchedule` (Renderer3D.cpp, after the god-rays block), create and
size `img_NeuralFeatures` on DX11/DX12/Vulkan, extend the Vulkan
`kImageUavSlot[]`/`kReadImageSlot` tables and re-run
`tools/compile_nova_spirv.py --deploy`, bump DX12 `kUavCount` and its heap
counts, widen the DX11 image array and its `uavs[]`, then verify with
`verify_nova_binding_budget.py`, `verify_vulkan_nova_contract.py`,
`verify_vulkan_reservoir_allocation.py` and `compile_nova_spirv.py --check` --
and the zero-delta screenshot, which needs a backend that can actually run.

---

### Task 5: Trainer and weight exporter — **DONE**

**Objective:** Train the network on the task-2 dataset and emit a weight blob the engine can upload.

**Files:**
- Create: `tools/train_neural/model.py`
- Create: `tools/train_neural/dataset.py`
- Create: `tools/train_neural/train.py`
- Create: `tools/train_neural/export_weights.py`
- Create: `tools/train_neural/test_model.py`

**Step 1 — write the model.**

`model.py`, numpy only (no torch — it is not installed, and installing a 2 GB framework for a 1.6k-weight model is not justified). Implement the forward pass and the gradient manually. For a 2-layer MLP this is ~60 lines and is the clean-room requirement made concrete: **the arithmetic is written here, not imported.**

Loss: `L = ||out - gt||² + λ * ||gate * residual||²`, where the second term penalises deviation from the anchor. The `λ` is the "engine frame is unyielding" term as a number.

**Step 2 — training loop.**

Adam, batch 256, lr 3e-3 with cosine decay, ~200 epochs. On CPU with a 1.6k-weight model this is minutes, not hours — confirm the wall-clock before committing to a long run, and record it in the README.

**Step 3 — test the model, not the training.**

`test_model.py` must report, on a held-out split:
- PSNR and SSIM of the enhanced output vs ground truth;
- **PSNR of the raw input vs ground truth** — the model must beat this, and by how much;
- the mean `gate` value, and the fraction of pixels with `gate < 0.05` (the "degrades to no enhancement" case).

If the model does not beat the raw input on PSNR, **it is not an enhancement** — stop and report, do not tune the gate to hide it.

**Step 4 — export.**

`export_weights.py` writes `assets/neural/neural_weights.bin` (raw fp32, little-endian, magic + version + count header) and a `.json` sidecar with the layer shapes. The byte layout must match the shader's read order exactly — `tools/train_neural/test_model.py` must round-trip it through the shader's packing order and assert the values are unchanged.

**Step 5 — commit.**

```
feat(train): train and export the neural rendering weights
```

#### Task 5 findings (2026-10-23)

**The plan's 18-feature architecture is not buildable, and the reason is
measured, not stylistic.** The reservation gives the feature pass one `rgba32f`
UAV = four floats; pass 17 stages `(luma, depth01, velocity.x, velocity.y)`
(`Renderer3D_NovaRenderer.hlsl:3970`). Depth and velocity are not in the dataset
at all — the exporter writes colour only — so training on them would teach the
network to ignore them. Pass 18 therefore reads `r,g,b` straight from `s_Output`
(the same texture pass 17 reads, so no extra register is consumed) and the model
is **4-input, not 18-input**.

**A luma-only model is worse than doing nothing.** Measured on all 36 pairs
before any model was written:

| model | PSNR vs gt |
|---|---|
| raw input (do nothing) | **33.605 dB** |
| global per-channel affine (12 parameters) | 36.010 dB |
| 64-bin luma tone curve — the best possible 1-input luma model | 27.833 dB |

Luma is a lossy 1-D projection: unrelated colours that share a luma get mapped to
the same output. That is a −5.8 dB regression, so RGB is the smallest input that
contains the signal that helps.

**Architecture actually trained:** `8 → 32 → 8`, `8 → 32 → 8`, head `8 → 3`,
gate `8 → 1`. **1140 fp32 weights = 4560 bytes.** The 8 inputs are 4 per-pixel
(`r,g,b,luma`) plus 4 frame-level (`mean_r, mean_g, mean_b, mean_luma`).

**The v0 4-input model was trained first, and it did not earn its parameters.**
On the 6 held-out frames it reached 35.978 dB against the 12-parameter affine's
36.447 dB — *worse than a tone curve*. Its learned gate was a dead constant
(0.4185 everywhere, std 0.0000), and a 200-epoch run scored the same as a
2-epoch run (+2.400 vs +2.582 dB), so more training was not going to fix it.

**Why: the correction is scene-dependent, and a per-pixel map cannot be.** The
manifest shows it directly — `mean_luma` for Example3D is gt 48.85 / input 43.14
(input *darker*) but for DemoGame gt 24.76 / input 26.49 (input *brighter*). Fitting
a per-channel affine *per scene* reaches **38.257 dB** against **36.014 dB** for one
global affine: **+2.24 dB of scene conditioning**. The coefficients are nothing
alike — DemoGame's are gains ~[0.885, 0.867, 0.897] (a contrast reduction),
WaterShowcase's have *negative* gains and a 0.224 blue bias. A per-pixel map has
no way to know which scene it is looking at, so it converges to the global
compromise.

The fix is frame-level conditioning, not more per-pixel features: the four
`mean_*` inputs are the anchor frame's own mean colour and mean luma, reduced
from the **anchor** (never the ground truth — that would be leakage) and broadcast
to every pixel. Still a per-pixel map, but one that knows what frame it is in.

**A 3-D RGB LUT overfits catastrophically.** A 16³ trilinear LUT fitted on the
training pairs scores 28.856 dB on the held-out frames — *worse than doing
nothing*. High-capacity lookup tables memorise the training scenes; this is why
the model stays small and smooth rather than wide.

**Two real bugs, both caught by the finite-difference gradient check:**

1. `np.mean()` over an `(N,3)` array divides by `3N`, but the gradient used
   `2/N` — scaling the *entire* gradient by exactly **3.000×**. Found only
   because the check ran in float64; in float32 the loss is ~1.9 and `eps=1e-3`
   leaves ~4 significant digits, which reads as a gradient error the same order
   as the bug being looked for.
2. `gate` is `(N,1)` and `residual` is `(N,3)`, so `dgate` broadcast to `(N,3)`
   instead of reducing over the channel axis. Raised immediately as a shape
   error, which is why it is written as an explicit `np.sum(..., axis=1)`.

**Then the network silently collapsed, and the run looked fine.** With raw inputs
in `[0,1]` the feature means are ~0.15–0.20 and the stds ~0.06–0.12, so the
layer-1 pre-activation had a std of **0.0118** — 85× too small. Half the ReLU
units died at init, the signal attenuated to nothing across four layers, and
training converged to `out = anchor + constant`: one number, worth +2.4 dB, with
a dead-constant gate (0.4185 everywhere, std 0.0000). It was indistinguishable
from a working model by any summary statistic — the per-pair PSNRs were
*plausible*.

What gave it away was testing sensitivity instead of accuracy: the output was
**byte-identical** for global-vector values of 0.0, 0.2, 0.4 and 1.0, and stayed
identical after perturbing `W1` by +1.0. A model that cannot see its own input is
not a model.

**The fix is input standardisation** (`FEATURE_MEAN`/`FEATURE_STD`, measured on
the training pairs, part of the ABI because the shader must apply the same
affine). Layer-1 pre-activation std goes 0.0118 → 0.302.

**Standardising then exposed a second bug, and this one diverged loudly.**
`forward()` defaulted its anchor to `x[:, :3]` — correct while `x` held raw
features, but after standardisation that is `(r - 0.1498) / 0.1205`, not `r`. The
model added its residual to the wrong quantity and the run went to **19 dB**, far
below doing nothing. `anchor_rgb` is now an explicit argument.

`verify_anchor_at_init` had passed straight through this because it calls
`forward` with a hand-built `x` and never touches `features()`. The replacement,
`verify_anchor_passthrough`, runs a zero-head model through the real standardised
path and asserts `max|out - anchor| == 0`. Non-vacuity checked: the bugged path
gives **10.73**, the correct path **0.0**.

**Result after the fix — 200 epochs, 6 held-out frames:**

| model | held-out PSNR |
|---|---|
| identity (do nothing) | 33.578 dB |
| global per-channel affine, 12 params | 36.440 dB |
| **trained network, 1140 params** | **38.763 dB** |
| per-scene affine (oracle ceiling) | 38.257 dB |

`+5.185 dB` over the raw input, `+2.323 dB` over the 12-parameter tone curve, and
above the per-scene affine oracle. SSIM 0.9765 → 0.9863.
`max |out - anchor| = 0.0482` — the engine frame is adjusted, never replaced.

Per pair: InventoryDemo +9.986, AtmosphereShowcase +9.327, Example3D +5.630,
PhysicsPlayground +4.940, PrimitiveGallery +1.104, DemoGame +0.120.

**The frame mean is not optional — measured, not assumed.** On the same six
held-out frames: real per-frame mean **38.763 dB**; best constant vector (the
dataset mean) **35.909 dB**; all 0.5 **29.706 dB**; all 1.0 **31.023 dB**; all
zeros **32.829 dB**. A constant is *worse than the 12-parameter affine*. So the
shader must reduce the frame mean per frame — that is a reduction pass, and it is
the reason pass 18 exists at all.

**A fifth bug, in the evaluation, and it manufactured a regression.** The first
report said 38.183 dB with DemoGame *losing* 1.725 dB. `evaluate()` processed the
frame in 256-row bands and reduced the conditioning mean **per band**, so each
band was told its own mean instead of the frame's — a different input and a
different answer. Corrected: **38.763 dB**, DemoGame **+0.120**, all six frames
gain. `predict_frame` now takes the conditioning vector explicitly so a banded
caller cannot repeat this. Training was never affected: `iter_random_crops`
supplies each crop its own pair's vector.

**The weight blob carries a CRC32, and that was not in the plan.** The first
`--check` run wrote a blob with no checksum, and its own negative control —
flip the last byte — was *accepted*. A bit-flipped weight uploaded into a
constant buffer is a silent visual corruption with no shader error, no link
failure and no log line: the same failure mode as the ninth GL image uniform. The
header is now `magic "WLNR"`, version, 4 shape fields, `n_weights`, **crc32**, and
`--check` proves that bad magic, truncation, a forged CRC and a wrong shape are
all refused.

**Wall clock (measured, per the plan's instruction):** ~3.0 s/epoch, so 200
epochs is ~10 minutes on this machine. A full-res pair is ~80 MB as float32 and
the whole set is ~2.9 GB, so `dataset.py` streams one pair at a time rather than
holding the set.

---

### Task 6: Eval + stability passes (passes 18, 19)

**Objective:** Run the network and produce a temporally stable, gated, anchored output.

**Files:**
- Modify: `Renderer3D_NovaRenderer.glsl`, `.hlsl`
- Modify: `Renderer3D.cpp`, `Renderer3D.h`
- Modify: `Wasteland/src/Wasteland/Renderer/RenderCapabilities.h`, `RenderCapabilities.cpp`

**Step 1 — weights upload path.**

Add `NeuralWeightsBuffer` to `Renderer3DData` (`Renderer3D.cpp:622`), created from `assets/neural/neural_weights.bin`. The constant-buffer offsets follow the existing pattern at `VulkanNova.cpp:45-100` — add the new members there and assert the block size still matches `kConstantBufferSize = 1744` after the addition (it will change; update the constant and the verifier).

**Step 2 — pass 18 (eval).**

```glsl
// Pass 18: neural eval. One thread per output pixel. Reads the packed feature
// image from pass 17 and the weights constant buffer, runs the MLP, and writes
// the RESIDUAL to img_NeuralResidual -- not to img_Output. The residual is
// gated and anchored in pass 19 so that a wrong prediction costs nothing.
```
Loop bounds identical in both twins. The `0.65f`-style magic numbers are forbidden; every constant is a named local with a comment saying what it is.

**Step 3 — pass 19 (stability + gate + write).**

Reads `img_Output` (anchor), `img_NeuralResidual`, `img_Velocity`, `s_Accumulation` (history) and `img_NeuralFeatures` (current). Implements:
1. reprojection using the velocity, with a disocclusion test that rejects history (the existing pattern in `RunTemporalAccumulation`, `.glsl:4077`);
2. `gate` — read from the residual's alpha, clamped;
3. `out = anchor + gate * residual`, stored with the history write for the next frame.

**This is where temporal stability lives.** A shimmering neural pass is the classic failure; the acceptance test is a static-camera sequence where the output must converge and stop moving.

**Step 4 — wire into `RunNovaPassSchedule`** after pass 17, gated on `s_Data.NeuralStageEnabled` and the tier from `RenderCapabilities::TierFor(FeatureTier::NeuralCompute)`.

**Step 5 — tier plumbing.**

`RenderCapabilities.h:20-25`: replace the `denoiseComputeCapable` comment's "only ever resolves for a future component" with the real resolver. Add `FeatureTier::NeuralCompute`. `TierFor()` resolves it to `ComputeEmulated` (the Nova path) on GL/DX11/DX12 and to `Unavailable` where Nova cannot run.

**Step 6 — verify, then build, then look.**

```bash
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_neural_stage_parity.py
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_vulkan_nova_contract.py
```
Then build (`BUILDING.md` recipe, `-m:1`, `/FS`, never start a build while the user may be building), then capture:
- static camera, 60 frames → confirm the image converges (measure frame-to-frame delta and show it decaying);
- moving camera → confirm no ghosting/smearing;
- a scene edge against sky → confirm the structure is preserved (the anchor requirement).

**Step 7 — commit.**

```
feat(nova): add neural eval and stability passes (18, 19)
```

---

### Task 6 findings (2026-10-24)

**The plan's pass structure does not survive contact with OpenGL, and it was
replaced.** The plan called for pass 17 = feature image, pass 18 = eval, pass 19
= composite, with `img_NeuralFeatures`, `img_NeuralResidual` and
`img_NeuralHistory`. Every one of those needs an image unit, and GL has eight,
all already in use (0 output, 1 accumulation, 2 bloom, 3 material, 4 albedo,
5 velocity, 6 trace G-buffer, 7 bloom temp). Task 4 had already recorded that a
ninth image uniform fails to LINK **silently**. So the stage would have been
GL-dead on arrival — and GL is the only backend that renders on this machine.

**What shipped instead: two passes, zero new image units.**

| pass | reads | writes |
|---|---|---|
| 17 | `img_Output` (sampler) | `NeuralGlobals` SSBO, binding 4 |
| 18 | `img_Output` (`imageLoad`), `NeuralGlobals` SSBO, `NeuralWeights` SSBO | `img_Output` (its own texel) |

Pass 18 reads the anchor through the **image binding**, not the `s_Output`
sampler. Reading a texture as a sampler while other threads write it as an image
is a hazard; each thread touching only its own texel of `img_Output` is not.

**The frame mean is the reason pass 17 exists.** It is not a nicety — measured on
the six held-out frames, the real per-frame mean scores **38.763 dB**, the best
constant vector **35.909 dB** (worse than the 12-parameter affine's 36.440), and
all-0.5 only **29.706 dB**. The shader must reduce it per frame.

**GLSL 4.30 has no `float atomicAdd`.** The reduction uses integer atomics on
fixed-point values: each pixel contributes `round(v * 256)` into a `uint`. The
sum of 3.4M pixels at 256× is at most 8.7e8, inside `uint32`, and the per-pixel
rounding averages out to 0.002 on the mean. Pass 18 turns the sums back into
means by dividing by `256 * pixelCount`. One dispatch, no second reduction pass.

**The weights are an SSBO, not a uniform array.** A 1140-component uniform array
on top of the ~600 components the Nova compute program already declares would
exceed OpenGL's guaranteed 1024 per stage — and, again, that fails to LINK
**silently**. `std430` packs a float array at four bytes, so element `i` is at
byte `4*i` here and in the HLSL's `StructuredBuffer<float>`.

**`shared` is global-scope only in GLSL.** Declaring the reduction's shared
arrays inside `RunNeuralReduce` is a compile error — `'shared' : not allowed in
nested scope` — caught by `verify_nova_glsl.py` under glslangValidator. They now
sit beside `gs_trav_stack`.

**Task 4's HLSL pass 17 (`img_NeuralFeatures`, `u12`, `t75`) is superseded.** The
shipped design computes luma inline and reads nothing but the anchor and the
weights, so a feature image has no reader. It was left in place rather than
removed: removing it would drop the `u12`/`t75` claims the binding-budget
verifier's reservation table documents, and that is a separate, larger edit.

**Verification override:** `WL_NEURAL_STAGE` is a **bitmask** — `0` off, `1`
reduce only, `2` eval only, `3` both — so a screenshot can prove the stage end to
end and attribute its cost to one pass. Off by default; it is a change to the
final image, so it is opt-in. (An earlier version of this comment read
"1 both, 2 reduce only, 3 eval only", which contradicted the code. Corrected.)

**Task 6 results (2026-10-24).** The stage is implemented and verified on GL.

| measurement | value |
|---|---|
| shader vs Python model, float anchor | mean\|d\| **0.000021**, max\|d\| **0.000235** |
| pixels off by more than one 8-bit step | **0.000%** |
| ragged workgroup edges (last 3 rows, last 4 cols) | **0.000%** off, uniform with the interior |
| stage's effect on the frame | 100% of pixels moved, max change 0.057 |
| determinism (two identical runs) | max\|d\| **0.00000** |

The residual 0.000021 is float32-vs-float64 accumulation in the matmuls — 1/50000
of a quantization step, i.e. the stage is exact.

**Two bugs found by that verification, both real:**

1. **A ragged-workgroup barrier.** `RunNeuralEval` staged the 1140 weights into
   `shared` memory inside `if (inBounds)`, with the `barrier()` inside the same
   guard. A 2332×1451 frame is 292×182 workgroups, so the last row has 24 valid
   threads and the last column 32. Those threads returned before the barrier, so
   they never wrote their share of the weight array — and a barrier with absent
   threads is undefined behaviour. Measured: **79.9%** of the last three rows and
   **56.5%** of the last four columns were wrong, with R and B driven negative and
   clamped to zero. Fixed by making the staging loop and the barrier
   unconditional. Before the fix max\|d\| was **0.396**; after, **0.000235**.

2. **The 8-bit PNG anchor made the verification look like a defect.** The readback
   reads `GL_RGBA/GL_FLOAT` and then quantizes to 8-bit for the PNG, so a
   shader-vs-model comparison made through the PNG is floored at the quantization
   of the *anchor* — the shader reads the float texture, the comparison reads the
   PNG. That produced a 0.0012 mean / 0.0089 max residual with 1.1% of pixels off
   by more than one step, which is exactly the shape of a stage bug. It is not
   one. `WL_CAPTURE_FLOAT=<path>` now dumps the unquantized RGBA32F texels
   beside the PNG, and on that path the residual collapses to 0.000021. The
   quantization hypothesis was checked and **killed** before being believed: a
   ±1/255 input perturbation moves the output by at most 0.0145, and fitting the
   shader's own frame mean recovered the exact mean to ±3.1e-5, so neither input
   quantization nor a biased reduction explained the residual.

---

### The dataset is stale, and the converged pass has nothing to learn (2026-10-24)

**This is the most important finding of Task 6, and it invalidates the shipped
model's reason to exist.**

The shipped dataset was captured **2026-10-23 20:49**. Task 3's shader edits —
removing the `maxIndirectRays = 0` early-out and the `wl_neural_indirect` blend —
landed **2026-10-24**. So the dataset was rendered by a renderer that no longer
exists. Re-exporting the same cameras with the current binary and comparing:

| | input luma | gt luma | gap | input vs gt | affine headroom |
|---|---|---|---|---|---|
| shipped dataset | 37.57 | 42.90 | **+5.33** | 32.257 dB | +6.311 dB |
| fresh export | 47.89 | 48.02 | **+0.13** | 37.040 dB | **+0.084 dB** |

Across all 36 shipped pairs the affine headroom averaged **+4.65 dB** and
tracked the brightness gap almost exactly: the three scenes whose gap was
*negative* (DemoGame, WaterShowcase, ScriptTest) had the least headroom
(+0.08 to +1.49 dB). The headroom was mostly **undoing a brightness difference
the renderer no longer has**.

The shipped model learned that lift. On its own training pairs it is good —
32.257 → 38.387 dB (+6.130) on Example3D. On a fresh capture of the same camera
it is **worse than doing nothing**: 37.040 → 34.127 dB (**−2.913**). A stage that
degrades the current renderer's output is not a stage.

**The converged `input` pass is the wrong target.** It is already within 0.1 luma
of the gt, because temporal accumulation at frame 400 converges the scale-0.5
render to the scale-1.0 one. The 12-parameter affine control — the bar the network
must clear — buys **+0.084 dB** there. There is nothing to learn.

**The `noisy` pass is the right target, and it was broken.** `NOISY_FRAME = 1`
captured a blank frame (mean luma 0.0, contrast p99 0.0), and the manifest
recorded it as a valid frame. The frame index is load-bearing for two reasons,
not one:

| frame | result on Example3D, camera auto_y0, scale 0.5 |
|---|---|
| 1 | blank — nothing rendered yet |
| 2 | 1280×720 — a default, not the render target |
| 3 | 3840×2092 — the window, chrome included |
| 6 | 2332×1451 — settled |

A wrong-size capture is not a weaker sample: the pairs are compared pixel-wise
against the gt, so it is a *different image* wearing a valid manifest entry.
`--validate` cannot catch it, because it only checks each PNG against the size the
manifest already claims.

**Fix:** `NOISY_FRAME = CONVERGED_FRAME`. The "noisy" property comes from the
missing temporal blend, not from the frame being early, and only a late frame has
a settled size and a loaded scene. With accumulation off at frame 400 the capture
is 2332×1451, **28.39 dB** against the gt, and the affine headroom is **+5.12 dB**
— a real job for the network. `export_dataset.py` now also refuses a run whose
captures disagree in size (`size_mismatch`, unit-tested against the four measured
sizes above).

**Consequence:** the dataset must be re-exported with `--passes gt --passes noisy`
and the model retrained on those pairs. Until then the shipped
`assets/neural/neural_weights.bin` should be treated as a **correctness
artefact**, not a shipping one: it is an exact implementation of a model that was
trained on the wrong target.

---

### Task 6 results (2026-10-24, part 2) — the other backends, and why they are compile-verified only

**The GL path is the only one that renders on this machine.** DX12 hangs during
init when forced, and VulkanNova is compiled but never instantiated. The plan's
Task 6 asked for every backend, so the other three got the stage too — verified
by compilation and by a statement-parity verifier, not by rendering. That is the
same standard the plan already applies to Vulkan ("never report Vulkan as
rendering; verify by contract only").

**The HLSL is shared by DX11, DX12 and Vulkan.** So the stage had to be written
into the HLSL to reach any of them. Three things were needed:

1. **The shader twin.** `RunNeuralReduce`, `RunNeuralReduceFinal` and
   `RunNeuralEval`, mirroring the GLSL statement-for-statement, plus `case 17/19/18`.
   `tools/verify_neural_stage_parity.py` now compares the two files'
   *computing* statements (not the comments) and passes.

2. **The buffers.** The GLSL's two SSBOs became an `RWStructuredBuffer<uint>`
   at `u12` (NeuralGlobals) and a `StructuredBuffer<float>` at `t74`
   (NeuralWeights). The element layout is byte-for-byte the SSBO's: both put
   element `i` at byte `4i`, so the GLSL's `g_Mean[4]` is elements 0..3 and
   `g_Partial[i]` is element `4+i`. `asfloat`/`asuint` are the only conversions.
   `NEURAL_PARTIAL_BASE = 4` in both files makes that auditable.

3. **The upload.** `u12` was an empty slot, but taking it forced the DX12
   descriptor tables wider: `kUavCount` 12 -> 13 (the UAV range is contiguous
   from `u0`, so `u12` cannot be addressed without sizing the range to cover
   it), and the readable-snapshot SRV block 10 -> 11 to reach `t74`. Because
   that block now ends at heap index 52, **the whole UAV block moved from heap
   index 52 to 53**. `kUavBase` is a named constant referenced everywhere as a
   relative base (`kUavBase + i`, `+9`, `+10`, `+11`), so the move is
   consistent by construction — and the header now says to grep for a bare 52
   or 63 before trusting the layout.

**The weight upload had to move.** The first version created the weights buffer
in `CreateImages()` and issued a `CopyResource` there. That is a null-pointer
call, not a subtle bug: `commandList` is a member that is only assigned inside
`BeginSubmit()`, and `Resize()` calls `CreateImages()` before any submission
exists. The upload now runs from `Clear()`, which is the first thing that opens
a command list, and which `Resize()` calls immediately after `CreateImages()`.
Idempotent via `neuralWeightsValid`.

**Pass 19 needs one block, and `Dispatch` cannot express that directly.** It
derives the group count as `((w+7)/8, (h+7)/8)`, so `Dispatch(19, 8, 8, {})`
gives exactly one block. Both dimensions have to be 8 — setting only `w` gives
`1 x (h+7)/8` blocks, which still computes the right answer (every block would
fold the same partials) but spends the frame's threads to do it.

**A shared blob loader, because two loaders is one drift.** `NeuralStageBlob.h`
holds the header ABI, the CRC32 and the load-and-validate, and both
`Renderer3D.cpp` (GL) and `DX12Nova.cpp` use it. The GL path's local copy was
removed in the same change, so there is exactly one definition of "valid blob".

**Superseded pass 17 removed.** Task 4's `RunNeuralFeatures` / `img_NeuralFeatures`
(u12) / `read_img_NeuralFeatures` (t75) are gone from the HLSL: the design that
ships has no feature image, because the eval pass computes luma inline and reads
the anchor plus the weights. `dxc -T cs_5_0` compiles the file clean (274,696
bytes, up 7.3 KB from the stage; the only warning is the pre-existing `:2871`
vector truncation).

**Editor UI (Task 7, part 1).** A "Neural Stage (trained MLP)" checkbox beside
the existing in-estimator toggle, off by default because it changes the final
image, with a tooltip that explains why the passes are skipped when
`assets/neural/neural_weights.bin` is missing or invalid. A checkbox that
appears to do nothing is worse than one that says why.

**Not verified: the Release link.** The Release build compiled clean but failed
at link with `LNK1104: cannot open file ...DemonCore-Editor.exe` — the dataset
exporter was running that exact binary. The link is re-run once the export
finishes; compilation (the part that would catch a mistake in the code above)
already passed in both Debug and Release.

### Task 7: Editor UI, presets, Python bindings

**Objective:** Make the stage controllable and inspectable.

**Files:**
- Modify: `DemonCore-Editor/src/EditorLayer.cpp` (the "Neural Rendering" block at `:1672-1696`)
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.h`, `Renderer3D.cpp`
- Modify: `Wasteland/src/Wasteland/Scripting/Bindings.cpp`
- Modify: `DemonCore-Editor/assets/scenes/*.wastescene` (preset field)

**Step 1 — replace the UI block.**

`EditorLayer.cpp:1672-1696` currently exposes "Neural Textures + Lighting" with three strength sliders. Replace with:
- enable checkbox (per tier);
- a tier readout (`RenderCapabilities::TierName`);
- the three existing strength sliders, retained and re-wired;
- a **gate readout** — mean gate over the last frame, from `Renderer3D::Statistics`. Without this the user cannot tell "no enhancement" from "broken network".

**Step 2 — persist to the scene YAML.**

Follow the existing `RenderScale` round-trip at `EditorLayer.cpp:2818-2838, 2871` — read at load, write at save. RenderScale 0 is the "never chosen" sentinel; use the same pattern for the stage flag.

**Step 3 — Python bindings.**

`Bindings.cpp:3640-3655` is the pattern. Add `Renderer3D_SetNeuralStageEnabled`, `Renderer3D_GetNeuralStageMeanGate`, and the tier getter. Test with the existing headless runner (`tests/headless/runner.py`) — the suite asserts source-level wiring for bindings (`tests/headless/test_runtime_types.py` is the model).

**Step 4 — verify + screenshot.**

```bash
python3 tests/runner.py
```
Capture the panel and one frame with the stage on and off.

**Step 5 — commit.**

```
feat(editor): expose the neural rendering stage in the editor and bindings
```

---

### Task 8: Documentation and evidence — **DONE**

**Files:**
- Create: `docs/NEURAL_RENDERING.md`
- Update: `docs/RTX_KIT_LICENSES.md` (add an explicit note that this stage is clean-room and contains no vendor code)
- Update: `docs/NOVA_FPS_DIAGNOSIS.md` §5.5/§5.9 references to vendor upscalers
- Update: `docs/NOVA_SHADER_PIPELINE.md` (the new pass table, and the "written but never dispatched" list — which now also covers 8/15/16 still)
- Update: `docs/NOVA_DENOISER_WIRING.md` (the "not verified" status of the new stage)
- Create: `docs/NEURAL_RENDERING_EVIDENCE/` with the screenshots from tasks 3 and 6

`docs/NEURAL_RENDERING.md` must state plainly, in its first paragraph: what this is, what it is **not** (not DLSS 5, not bit-compatible, not RTX-only), the training-data provenance, and the four-backend verification status with what is actually verified vs contract-verified.

**Result (2026-10-25).** All six items done.

`docs/NEURAL_RENDERING.md` (15,445 bytes) opens with the required four statements and
then covers: what the stage does and why the residual/gate/frame-mean design is
load-bearing; training-data provenance including the `gt`/`noisy` split and why the
converged pass is a bad target (+0.084 dB vs +2.042 dB of headroom); the result
(+2.935 dB held-out, beating the affine control and the per-scene affine oracle); the
blob ABI and why it carries a CRC32; the four-backend table; the binding layout; and a
section on the verification traps already fallen into.

`docs/RTX_KIT_LICENSES.md` gained an explicit clean-room section. The point of it is
not that the restrictions applied and were lifted — it is that they never applied,
because there was never any vendor code in it to restrict. That distinction is worth
stating, because "neural rendering" and "DLSS" are easy to conflate and the licence
boundary is the natural place to check.

`docs/NOVA_FPS_DIAGNOSIS.md` §5.5 now points at the trained stage as a working
non-vendor option, and the §5.5 vendor-upscaler rejection bullet now explicitly
exempts it — otherwise a reader applying the bullet mechanically would reject the
stage for the wrong reason.

`docs/NOVA_SHADER_PIPELINE.md` gained a pass inventory for both twins (19 passes,
which function each maps to, and which the GL renderer actually dispatches) plus two
findings that fell out of building it:

- **Passes 8, 15, 16 are written but never dispatched** — dead on every backend,
  including Vulkan. The risk is not the dead code but that a future reader can see
  `case 15` and assume TSR is live.
- **Pass 9 (`GenerateMaterialMaps`) is dispatched and broken**, in two independent
  ways: `u_PassID` is never set on its path (so the bake runs whatever pass the
  previous dispatch left behind), and it binds image unit 8, which does not exist
  (`GL_MAX_IMAGE_UNITS` is 8, valid units 0..7) while the GLSL declares
  `img_MaterialPacked` at `binding = 3`. Neither is fixed here; both are recorded so
  the pass table is not read as "pass 9 works".

`docs/NOVA_DENOISER_WIRING.md` now separates the denoiser's status from the trained
stage's: the stage **has** run and **is** verified against its own model, but only on
OpenGL, and the parity tool must not be read as a GPU result.

`docs/NEURAL_RENDERING_EVIDENCE/` gained `neural_stage_ab.png` (stage-off vs
stage-on, verified visually: identical geometry, edges and shadows, only a subtle
global lift), `neural_stage_verification.txt` (the measured comparisons, generated by
running them rather than transcribed), and a `README.md` that records why every PNG is
exactly 10,153,485 bytes and why the float dump — never the PNG — is the comparison
target.

**Cross-check that fell out of the evidence pass:** the Python model's gate mean on the
captured frame is **0.3119**, and the editor panel reported **0.312** for the same
frame. Two independent paths — a numpy forward pass and a GPU reduction read back over
SSBO — agree to three decimals.

---

## Task 8 postscript (2026-10-25) — pass 9, and three verifiers that were not verifying

Documentation work does not need a build, so it continued while the tree was blocked.
It produced one shader fix and four verifier fixes.

### Pass 9 (`GenerateMaterialMaps`) — three defects, all silent

Found by reading the dispatch function rather than the `case` labels. The GL path:

1. **never set `u_PassID`.** The DX11 and DX12 branches pass `9` explicitly to
   `Dispatch()`; the GL branch dispatched with whatever the previous dispatch left
   in it, so the bake ran an arbitrary pass. A stale pass id is a valid value for
   the switch, so nothing complained.
2. **bound image unit 8, which does not exist.** `GL_MAX_IMAGE_UNITS` is 8, so valid
   units are 0..7 and the bind was a silent no-op. This is the only writer of
   `GeneratedTextureID`, so every normal/roughness map baked from the editor or from
   `Renderer3D_GenerateMaterialMaps` was empty. Fixed to **unit 3** — not a guess:
   the GLSL declares `img_MaterialPacked` at `binding = 3` and the HLSL at
   `register(u3)`, so both twins already agreed and only the CPU was wrong.
3. **truncating division** `width / 8` instead of `(width + 7) / 8`, dropping up to
   7 rows or columns of the generated maps.

### Three verifiers that were passing vacuously

- **`verify_texture_sampler_indexing.py`** asserted `image_count != 9`. Correct when
  the shader carried a ninth declaration; wrong afterwards, since `GL_MAX_IMAGE_UNITS`
  is 8. Now asserts the binding set is exactly `0..7` — strictly stronger, and it
  rejects a ninth unit, a missing one, and a duplicate. Three negative controls.
- **`verify_nova_glsl.py` check 1** used `re.search` for `validHistory ... u_FrameIndex > 0`.
  `re.search` returns the first match anywhere, and each twin has **four**
  `bool validHistory = ...;` declarations — so mutating two of the four left the
  check green while shipping frame-0 history garbage in the other two. Now enumerates
  every declaration and asserts all are gated, reporting the count.
- **`verify_nova_glsl_negative_control.py`** decoded the shader with CRLF endings
  intact, so its only multi-line mutation (the Halton one) could never match and
  reported "MUTATION DID NOT APPLY". Now normalises to LF before mutating.

All six mutations are now caught, and the frame-gate control reports
`4 declarations, 2 ungated`.

### The HLSL twin did not compile, and five green verifiers missed it

`RunNeuralEval` declared `float g` **inside** the in-bounds branch, so the `else`
below assigned to an undeclared identifier and the gate partial after the branch read
one. dxc: `use of undeclared identifier 'g'`.

This is the Task 7 hoist applied to the GLSL but not to the HLSL. It survived every
check because **only `compile_nova_spirv.py` compiles the HLSL** —
`verify_neural_stage_parity.py` compares statements textually and cannot see a scope
error, and `verify_nova_glsl.py` compiles the GLSL, not the HLSL. The built SPIR-V was
stale as a result, so Vulkan would have run an older shader than the one on disk.

Fixed; SPIR-V rebuilt (765,492 bytes) and `NeuralWeights`, `NeuralGlobals`,
`gs_neural_gate`, `gs_neural_w` all present as `OpName`s.

**Final suite: 7/7 shader verifiers PASS** (was 5 PASS / 2 FAIL), plus
`test_model --self-test-only` and `export_weights --check`.

---

## 4. Files likely to change

**New**
```
tools/train_neural/export_dataset.py
tools/train_neural/train.py
tools/train_neural/model.py
tools/train_neural/export_weights.py
tools/train_neural/test_model.py
tools/train_neural/README.md
tools/verify_neural_stage_parity.py
docs/NEURAL_RENDERING.md
docs/NEURAL_RENDERING_BINDING_PLAN.md
docs/NEURAL_RENDERING_EVIDENCE/
assets/neural/neural_weights.bin
assets/neural/neural_weights.json
```

**Modified**
```
DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl
DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl
Wasteland/src/Wasteland/Renderer/Renderer3D.cpp
Wasteland/src/Wasteland/Renderer/Renderer3D.h
Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp
Wasteland/src/Wasteland/Renderer/RenderCapabilities.h
Wasteland/src/Wasteland/Scripting/Bindings.cpp
Wasteland/src/Platform/NVRHI/VulkanNova.h
Wasteland/src/Platform/NVRHI/VulkanNova.cpp
Wasteland/src/Platform/NVRHI/DX12Nova.h
Wasteland/src/Platform/NVRHI/DX12Nova.cpp
Wasteland/src/Platform/NVRHI/DX11Nova.h
DemonCore-Editor/src/EditorLayer.cpp
tools/verify_nova_binding_budget.py
docs/NOVA_DENOISER_WIRING.md
docs/NOVA_SHADER_PIPELINE.md
docs/NOVA_FPS_DIAGNOSIS.md
docs/RTX_KIT_LICENSES.md
```

**Do not touch**
- `Wasteland/vendor/**` — the licence boundary is `docs/RTX_KIT_LICENSES.md`; no vendor SDK enters the tree.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` is a build artefact of `tools/compile_nova_spirv.py`; regenerate it, never hand-edit it.

---

## 5. Tests / validation

**Per task, always:**
```bash
python3 tools/verify_nova_glsl.py                       # GLSL compiles the way the engine uses it
python3 tools/verify_nova_shader_parity.py              # twins agree
python3 tools/verify_neural_stage_parity.py             # new passes agree (task 3+)
python3 tools/verify_nova_binding_budget.py             # no register collision
python3 tools/verify_nova_binding_budget.py --self-test # verifier can still fail
python3 tools/verify_vulkan_nova_contract.py            # Vulkan by contract
python3 tools/verify_nova_glsl_negative_control.py      # parity guard fails when broken
```

**Build (never while the user may be building):**
```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSB" Wasteland/Wasteland.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false -m:1
"$MSB" DemonCore-Editor/DemonCore-Editor.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false -m:1
```
`-m:1` and `/FS` are mandatory (`BUILDING.md`): parallel builds fail on `C1041`, and a locked `Wasteland.pch` fails with a misleading `C1083 ... Invalid argument` that reads like a bad path.

**Visual (the real acceptance — this user does not accept a clean log):**
```bash
WL_SCREENSHOT=out.png WL_SCREENSHOT_FRAME=120 ./DemonCore-Editor.exe
```
Required captures: static-camera convergence sequence, moving-camera ghosting check, structure-preservation check, stage-on vs stage-off, each backend.

**Model quality gate (task 5):** enhanced PSNR > raw-input PSNR on a held-out split, reported in the commit message.

---

## 6. Risks, tradeoffs, open questions

**Risks**

1. **Task 3 changes lighting on every backend and preset.** It is the only task that can make the shipped image worse. Mitigation: baseline screenshots first, back up both shaders before editing, and treat the pixel comparison as the acceptance test rather than the build.
2. **The dataset may not be obtainable deterministically.** If the camera cannot be frozen from the environment, task 2 cannot produce matched pairs and task 5 cannot train. Mitigation: stop at task 2 and report; do not train on misaligned pairs, which produces a model that looks plausible and is wrong.
3. **Vulkan will not be visually verified.** It is compiled but never instantiated. Mitigation: contract verification only, and the docs must say so. Do not claim it renders.
4. **DX11's 8-UAV array is a convention, not a limit — but the doc's own conclusion is a promise about per-dispatch rebinding that nobody has measured on this GPU.** Task 4 must widen the array and confirm on hardware.
5. **A trained model that regresses the image will look fine in a small test.** Mitigation: the PSNR-vs-input gate in task 5, and a held-out split.
6. **The GLSL `#include` problem blocks any shared-source shortcut.** `docs/NOVA_SHADER_PIPELINE.md:25-45`: `OpenGLShader::PreProcess` expands no includes, so a shared header would compile only where the driver happens to implement `GL_GOOGLE_include_directive` — and `verify_nova_glsl.py` copies the include into its temp dir and **still passes**. That is a green verifier over a broken backend. Keep the twins separate.

**Tradeoffs**

- **MLP-Mixer over a conv net.** Chosen for the D3D11/OpenGL floor. Cost: weaker spatial coherence, which the stability pass has to carry. Accepted — a stage that runs everywhere beats a better stage that runs on two backends.
- **Residual + gate over direct output.** Costs one extra buffer and one multiply; buys "wrong network = no change" instead of "wrong network = wrong image". Accepted without reservation.
- **Training data from the engine, not from the internet.** Slower to produce and smaller in variety; but it is the clean-room requirement and it matches the engine's own shading. Accepted.
- **fp32 in v1.** ~2x the weight-upload bandwidth and the multiply cost; keeps the model source-identical across all four backends. Accepted; fp16 is a v2 tier.

**Open questions**

1. Can the camera and frame index be frozen from the environment for dataset capture? **Blocking for task 2.**
2. Does the existing `u_NeuralEnabled` toggle have any downstream consumer that assumes the in-sample neural block exists? Task 3 step 2 greps for reads before deleting.
3. Is `img_NeuralFeatures` at `rgba32f` affordable at 4K on the DX11 path, or does the feature pass need to drop to `rgba16f`? Measure in task 4; the register plan in task 1 should not assume.
4. Should pass 17 be merged into pass 6 (composite) to save a dispatch? Not in v1 — separate passes make the feature layout independently inspectable, which is what makes the trainer's packing order verifiable.
5. Is there a real `NeuralCompute` tier distinct from `ComputeEmulated` on OpenGL, or does the 2-pass Lite tier collapse into it? Decide from the task-6 measurement, not from the architecture diagram.
