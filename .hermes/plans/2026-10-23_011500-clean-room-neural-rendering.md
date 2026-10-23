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
- **DX12:** `kUavCount = 9` at `DX12Nova.h:92`, `kUavBase = 49`, `kDescriptorsPerHeap = kUavBase + kUavCount` (= 58). `rootParams[8]` is exactly full but `rootParams[10]` is declared — the constraint is descriptor-table entries, not root parameters (`docs/NOVA_BINDING_BUDGET.md`). Adding a UAV needs one more descriptor in `ranges[3]`, not a new root parameter.
- **DX11:** `std::array<Image,8> images` and a local `uavs[8]` rebuilt inside `Dispatch`, with `Unbind()` clearing afterwards. The 8 is a **construction convention, not a device limit** — and slot meaning is per-dispatch rebindable. The binding budget doc's conclusion stands.
- **OpenGL:** image unit 8 is free (0-7 bound in the Nova path, 8 belongs to an unrelated generator). `glBindImageTexture` and `glActiveTexture` are **separate namespaces** — a previous survey confused them.

**Task 4 must not begin until `tools/verify_nova_binding_budget.py` has been extended to assert the new registers and its `--self-test` proves it can fail.** That script exists precisely because a subagent once concluded the moments target "does not fit as a 9th UAV" from a report instead of from source.

### 1.3 Vulkan is the risk that is not a risk

VulkanNova is not invoked, so a new pass cannot be visually verified there. **Verify Vulkan by contract, not by screenshot**: `tools/verify_vulkan_nova_contract.py`, `tools/verify_vulkan_reservoir_allocation.py`, `tools/verify_vulkan_binding_budget.py` (add the new registers) and `tools/compile_nova_spirv.py --check`. Do not claim Vulkan renders.

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

### Task 1: Binding budget and pass-slot reservation (no shader change)

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

---

### Task 5: Trainer and weight exporter

**Objective:** Train the network on the task-2 dataset and emit a weight blob the engine can upload.

**Files:**
- Create: `tools/train_neural/train.py`
- Create: `tools/train_neural/model.py`
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

### Task 8: Documentation and evidence

**Files:**
- Create: `docs/NEURAL_RENDERING.md`
- Update: `docs/RTX_KIT_LICENSES.md` (add an explicit note that this stage is clean-room and contains no vendor code)
- Update: `docs/NOVA_FPS_DIAGNOSIS.md` §5.5/§5.9 references to vendor upscalers
- Update: `docs/NOVA_SHADER_PIPELINE.md` (the new pass table, and the "written but never dispatched" list — which now also covers 8/15/16 still)
- Update: `docs/NOVA_DENOISER_WIRING.md` (the "not verified" status of the new stage)
- Create: `docs/NEURAL_RENDERING_EVIDENCE/` with the screenshots from tasks 3 and 6

`docs/NEURAL_RENDERING.md` must state plainly, in its first paragraph: what this is, what it is **not** (not DLSS 5, not bit-compatible, not RTX-only), the training-data provenance, and the four-backend verification status with what is actually verified vs contract-verified.

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
