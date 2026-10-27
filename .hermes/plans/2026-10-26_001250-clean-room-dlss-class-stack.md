# Clean-Room DLSS-Class Rendering Stack (WLSR / WLAA / WLRR / WLFG) Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Give Wasteland four clean-room, non-vendor, cross-backend implementations of the DLSS-class
feature set — **WLSR** (super resolution), **WLAA** (anti-aliasing), **WLRR** (ray reconstruction) and
**WLFG** (AI frame generation, 2x–6x) — built on the engine's own already-working temporal
reprojection, trained on the engine's own frames, and gated so that a wrong model costs nothing.

**Architecture:** Every feature is a new **pass ID** in the existing single Nova compute program
(`u_PassID` switch in both twins), dispatched from the existing `RunNovaPassSchedule`. WLSR and WLAA
are analytic and reproject the *denoised accumulation* instead of writing a second history; WLRR is a
new A-Trous kernel family plus a residual-correcting trained head on the single-frame noisy target;
WLFG is a three-part system — a per-present reprojection warp, a game-time clock scaled by the
interpolation factor, and a neural frame-synthesis head trained on held-out interpolation factors.
All four share one new pass-index block, one new tier in `RenderCapabilities`, and one new verifier
per feature, each with negative controls.

**Tech Stack:** C++17 engine (existing), GLSL 4.30 compute twin (`Renderer3D_NovaRenderer.glsl`) and
HLSL `cs_5_0` twin (`.hlsl`, which also becomes SPIR-V via `tools/compile_nova_spirv.py`), DXC/glslang
verification path (existing), Python 3 stdlib + numpy + Pillow for training and dataset evaluation
(already in `tools/train_neural/`). No vendor SDK, no Tensor-Core requirement, no ONNX, no RTX gate.

---

## 0. Corrections to the premise — read before task 1

The request names four NVIDIA features with their NVIDIA names. Four parts of that premise do not hold
against this codebase, and the plan is built around the corrected version rather than the literal one.

### 0.1 "DLSS Super Resolution" already half-exists — and the half that exists is the wrong half

`RunComposite` (`Renderer3D_NovaRenderer.glsl:4615`) already carries a working **reconstruction
ladder** — Nearest / Bilinear / Box / Bicubic / Lanczos — selected by `u_ReconstructionFilter`, with
the kernels verified by transcription. It is spatial only.

What is missing is the half that makes a super-resolver worth having:

- **EASU** — an edge-adaptive gradient-reconstruction upscale with a *scale-aware* filter kernel, as
  opposed to a fixed 4/12/16-tap kernel applied at the destination resolution.
- **RCAS** — a sharpening pass that is driven by the *local contrast* of the reconstructed image and
  clamped to a maximum amount per pixel.
- **Reactive mask / autogen** — a per-pixel input telling the upscaler where to trust the current
  frame over the history. This engine has the exact inputs for it (`img_Velocity`,
  `img_TraceGBuffer` depth+normal, `img_AlbedoHit` hit mask), and no pass uses them for this.

Also relevant, and recorded in the codebase already: **pass 8 (`RunUpscale`) is declared in both twins
and dispatched nowhere** (`grep -rn 'Dispatch(8' Wasteland/src/` → nothing). The natural WLSR landing
is to give pass 8 a real body and dispatch it — but the plan deliberately does **not** do that, for a
reason in §1.4.

### 0.2 "DLAA" already exists here too — and it is not a separate feature

The engine's temporal accumulation (pass 2, `RunTemporalAccumulation`, `.glsl:4174`) is DLAA-shaped:
jittered sub-pixel sampling (`GetJitter`, `u_SamplePattern`), velocity reprojection, a 0.02 NDC depth
rejection, and YCoCg variance clipping (3x3 at Ultra, 5-tap cross below). That is the standard TAA
formulation. It is currently reachable only through the `ReconstructionFilter` enum's fall-through
bucket (`6-11` are documented as "separate passes" that fall through to bilinear).

So **WLAA is not new work, it is a new enum value that is actually consumed.** The plan implements the
missing half honestly: DLAA's distinguishing property over TAA is that it runs at **native
resolution** and spends its budget on the *filter*, not on the accumulation window. That means the
real deliverable is the switch plus a sharpening stage that counteracts the filter's blur, plus a
real measure of how much the history is helping.

### 0.3 "Ray Reconstruction" replaces a denoiser — this engine's denoiser is not a valid target yet

The honest statement, from `docs/NOVA_DENOISER_WIRING.md`: **the SVGF denoiser (passes 12/13) has
never been observed to run.** `SvfgEnabled = false` by default, the A-Trous ladder is a fixed
4-pass 1/2/4/8 schedule rather than variance-adaptive, and there is no GPU-verified screenshot of it.
`RestirEnabled = false` likewise.

Replacing a denoiser nobody has validated is the classic way to ship a feature that cannot be
compared against anything. So **WLRR's first task is to make the incumbent measurable**, not to
replace it:

1. A `tools/verify_wlrr_*` harness that measures the incumbent denoiser's PSNR against a converged
   reference, on the engine's own frames.
2. Only then, a **residual-correcting trained head** on top of the *single-frame noisy* target — the
   same target the existing neural stage found 24x more learnable headroom in (+2.042 dB vs +0.084 dB,
   `docs/NEURAL_RENDERING.md` §2).
3. WLRR is then *additive*: the incumbent stays, the trained head corrects its residual, and the
   gate decides per pixel how much of the correction to apply. If the head is wrong the image is the
   incumbent's image, not a worse one.

### 0.4 "AI Frame Generation up to 6x" — the naive reading does not exist, and the correct one is three systems

Frame generation has no shader in it. It is a **frame-pacing, simulation-clock and reprojection**
problem, and this engine is missing all three prerequisites:

| prerequisite | state today | consequence |
|---|---|---|
| a second history buffer to reproject *from* | `s_Accumulation` at `t48` is bound to **one** texture (`DX11Nova.h:512`, `DX12Nova.cpp:1475-1496`); `CommitHistory` overwrites it | there is no previous-present frame to warp from |
| a synthetic frame rate cap | **none** — `Application::Run()` loops unthrottled and `SetVSync(true)` is hardcoded (`WindowsWindow.cpp:312`) | an interpolated frame competes with a real one for the present |
| a game-time clock independent of the render rate | `Timestep = time - m_LastFrameTime` (`Application.cpp:424`) is the render delta | interpolation would have to either double the sim speed or run the sim twice |

So WLFG is three deliverables, not one: **(a)** a per-present reprojection warp (an optical-flow-free
motion-vector warp, using the velocity buffer the tracer already writes), **(b)** a `SimTimescale`
clock that divides game time by the interpolation factor, and **(c)** a neural frame-synthesis head
that fills the disocclusions the warp cannot. (c) is the only part that is "AI" in the marketing
sense, and it is the smallest part.

**And the 6x ceiling is the important design decision.** Wasteland is a **path tracer with
temporal accumulation** — its convergence *is* its quality. Re-projecting a converged frame at 6x
multiplies the effective sample count by 6; re-projecting a 1-sample frame multiplies noise by 6.
The plan therefore makes the interpolation factor a function of the accumulation state, and makes
6x reachable **only** on the path where the accumulation is converged:

| accumulation state | max WLFG factor | rationale |
|---|---|---|
| converged (alpha at its 0.02 floor, static camera) | **6x** | history is a real converged image; warping it is nearly free |
| partially converged | 2x | noise would otherwise multiply |
| camera moved / scene changed (`u_CameraMoved`) | **off** | reprojection is undefined; the incumbent is the only truth |
| any non-Nova backend | off | the raster path has no velocity buffer |

A 6x claim that does not state its precondition is not a 6x claim.

### 0.5 Two structural facts that shape every task

**The two shader twins are the ABI.** A new pass goes into *both* `Renderer3D_NovaRenderer.glsl` and
`.hlsl`, with identical hot-loop constants. `tools/verify_nova_glsl.py` is the only GLSL gate (MSBuild
has zero references to the shaders — a clean build proves nothing about a shader change, recorded in
`docs/NOVA_SHADER_PIPELINE.md:161`), `tools/verify_nova_shader_parity.py` compares the twins, and
`tools/compile_nova_spirv.py` is the *only* thing that compiles the HLSL (the `.spv` is built from the
HLSL, not the GLSL — `VulkanNova.cpp:387`). **A parity tool cannot see a scope error**; a real
incident in this repo had five green verifiers over an HLSL that did not compile.

**`GL_MAX_IMAGE_UNITS = 8` on the target GPU (RTX 3060 Ti, 4.5.0 NVIDIA 591.59)** — measured, logged
at `OpenGLContext.cpp:153`. Valid units are 0..7 and all eight are in use: `0` output, `1`
accumulation, `2` bloom, `3` material packed, `4` albedo hit, `5` velocity, `6` trace G-buffer, `7`
bloom temp. **A ninth image uniform does not degrade one pass — it fails the whole GL Nova program to
LINK, silently**, taking the ray-traced viewport black with no shader error (`OpenGLShader.cpp:150-160`).
This is the single hardest constraint in the plan and §1.2 is built around it.

### 0.6 The OpenGL-first rule, stated once

**Every feature in this plan is verified on OpenGL first and OpenGL only.** DX12 hangs during init on
this machine when forced; `VulkanNova` is compiled but never instantiated. The HLSL twin is still
written — it is what DX11, DX12 and Vulkan all consume — but its status is *contract-verified*, and
this plan says so in every doc it touches rather than implying the feature "works on all four
backends". Anyone reading a binding table and concluding otherwise would be wrong.

### 0.9 Baseline resolved — the Nova shader compiles again

Task 1 recorded this as a blocker. **It is resolved.** Re-measured after task 1:

```
GLSL braces: 336 / 336   balanced
HLSL braces: 356 / 355   depth 1 (inside a comment or string literal)
```

`tools/verify_nova_glsl.py` now reports **RESULT: PASS**, and the whole verifier
suite is green. The user's in-progress EASU work (26 `EASUTap` occurrences in both
twins, 853 uncommitted lines) landed in the meantime.

**Keep the attribution discipline anyway.** The suite went green *during* this
work, which is exactly the kind of coincidence that lets a later task take credit
for a fix it did not make. Every task still records the verifier's actual output.

**One consequence worth carrying forward:** there is already an `EASUTap`
implementation in both twins. Task 3 (WLSR EASU) must **read that code first** and
either reuse it or state why it does not meet the plan's requirement (a
scale-aware kernel plus a reactive-mask read). It must not write a second,
parallel EASU.

### 0.8 Findings from task 1's reconnaissance — two corrections to this plan

Written while reserving the registers, because both change what a later task has to do.

**(a) `img_Bloom_Temp`'s alpha is free, but not for the reason §1.2 gave.** Pass 7
(`RunBilateralBlur`) writes `vec4(finalFiltered, 1.0f)` — rgb *and* alpha = 1.0 —
at `.glsl:4504,5137`. The draft said "pass 7 writes rgb only". The alpha is
**written but never read**: a grep for `imageLoad(...).a` across the whole shader
returns nothing. So the channel is free for the reactive mask, and the correct
statement is *"nothing reads it"*, not *"nothing writes it"*. Both are true; only
the first matters, and the first is the one a future change could break.

**(b) Passes 15 and 16 are NOT empty — they are a complete, undispatched TSR
resolve, and reviving them is more than flipping a dispatch.** `RunTSRResolve`
(`.glsl:5690`) reads `img_Output`, reprojects with motion vectors, samples a
history buffer, does 3x3 YCoCg neighbourhood clamping, blends and sharpens, then
writes `img_Accumulation` and the history. `RunTSRCopy` (`.glsl:5694`) copies
`img_Accumulation` to `img_Output` to avoid a read-write race. The shader comment
at `.glsl:5695-5698` states the requirement explicitly:

> "The history buffer reuses `img_Bloom_Temp` (binding 7), which is free after the
> upscale pass. **The C++ side must bind the history texture to binding 7 for the
> TSR dispatch** and clear it on camera cuts and frame 0."

**That CPU-side rebind does not exist** — a grep for `binding = 7` /
`kHistorySlot` / `HistoryTexture` in `Renderer3D.cpp` returns nothing. So
dispatching pass 15 today would read a history buffer that nothing writes:
garbage, silently.

Two consequences:
- **WLAA is closer to existing than §0.2 claimed.** There is already a
  history-reproject-clamp-blend-sharpen chain here. The missing piece is the
  history *buffer*, not the algorithm. Task 5 must add the binding and verify
  the buffer is cleared on camera cuts, or WLAA will "work" while showing
  ghosts from frame 0.
- **The plan's "revive pass 15" is a misnomer.** It is *wire up*, not *revive*.
  Task 3 (WLSR RCAS) is the one that genuinely replaces a body; task 5 inherits
  a body that needs a host contract honoured.

### 0.7 What "clean room" means here

No vendor SDK, no vendor weights, no vendor architecture, no copied code. The published descriptions
of the *class* of method (edge-adaptive upscale, sharpening driven by local contrast, wavelet
denoising, motion-compensated frame synthesis) are used as a spec; the kernels, constants, weights
and training data are authored here from the engine's own frames. `docs/RTX_KIT_LICENSES.md` sets the
boundary: restricted RTX SDKs (NRD, RTXTF, RTXDI, OMM, RTXGI, RTXPT) cannot enter an open-source
Wasteland, and STBN is non-commercial only. This plan adds **none** of them.

**Consequence, stated plainly: the deliverable is a real, working, cross-backend-in-principle
clean-room rendering stack — not a DLSS replica, not bit-compatible with anything, and not
RTX-only.** Same problems, independently solved.

---

## 1. Measured ground truth

Built from source this session, not from the docs alone.

| Fact | Source |
|---|---|
| Passes selected by `u_PassID`; `main()` switches on it | `.glsl:5971-5993`, `.hlsl:4340-4360` |
| Live and dispatched: 0,1,2,3,4,5,6,7,9,10,11,12,13,14 (+17,18,19 gated) | `Renderer3D.cpp:3599-3641` |
| **Declared but never dispatched: 8 (`RunUpscale`), 15 (`RunTSRResolve`), 16 (`RunTSRCopy`)** | `grep -rn 'Dispatch(8\|Dispatch(15\|Dispatch(16' Wasteland/src/` → nothing |
| Pass order is **not ascending and must not be "fixed"** | `Renderer3D.cpp:3580-3583` |
| A-Trous iterations must be **separate dispatches** from the CPU | `Renderer3D.cpp:3614-3628` |
| Group size 8x8, one thread per output pixel | `.glsl:25`, `.hlsl:4312` |
| **GL image units in use: 0,1,2,3,4,5,6,7 — all of them. `GL_MAX_IMAGE_UNITS` = 8** | `Renderer3D.cpp:4814-4820`, `OpenGLContext.cpp:151-153` |
| HLSL UAV slots in use: `u0..u7`, `u9` (moments), `u10` (reservoir write), `u11` (history), `u12` (NeuralGlobals). **`u8` is a deliberate hole** (collides with `NovaConstants` b1 under Vulkan's b-shift 7) | `.hlsl:84-93,108` |
| Read SRVs: `t48..t53` sampled, `t64..t73` image reads, `t74` NeuralWeights | `.hlsl:108-136,306-318` |
| **`u_SceneTextures[32] : register(t15)` claims bindings 31..62 as a contiguous range** — a new `t`-register inside it is absorbed into the array | `tools/verify_nova_binding_budget.py:33-37` |
| Free HLSL `u`-slots start at **u13** (u12 = NeuralGlobals) | `tools/verify_nova_binding_budget.py:230` |
| Constant buffer `NovaConstants` = **1744 bytes**, 84 members, offsets asserted by a verifier | `VulkanNova.h:120`, `tools/compile_nova_spirv.py:245-249` |
| Vulkan binding = `offset + slot`, shifts u=0, b=7, t=16, s=200 | `tools/compile_nova_spirv.py:55-58` |
| DX12: `kUavCount = 13`, `kUavBase = 53`, `kDescriptorsPerHeap = 66`, `kImageSrvBase = 42`, `kSamplerBase = 36` | `DX12Nova.h:177-222` |
| DX11: `std::array<Image,8> images`, `uavs[11]`, `srvs[128]`; the 8 is a **construction convention**, not a device limit | `DX11Nova.h:71,427,540`; `docs/NOVA_BINDING_BUDGET.md:44-55` |
| Velocity buffer: written by pass 0 from `u_PrevViewProjection` reprojection, `RG16F`, `u5` | `.glsl:86,2522-2562` |
| Depth is **NDC** in the trace G-buffer's `.r` (`1.0` = sky); `.gba` is packed normal | `.glsl:3974-3982` |
| Albedo + hit mask in `img_AlbedoHit` (`u4`), written by pass 1 | `.glsl:3962` |
| `s_DepthBuffer` sampler is bound to unit **4**, which is **also** the `img_AlbedoHit` image unit — the same texture under two names | `.glsl:98,121`; `Renderer3D.cpp:4754-4760,4819` |
| `u_QualityLevel >= 1` gates the volumetric light loop; **Low is quality 0** | `Renderer3D.cpp` (skill `wasteland-nova-performance`) |
| `u_AccumulationAlpha = max(0.02, 1/(FrameIndex+1))`, or `1.0` when accumulation is off | `Renderer3D.cpp:3714,3964,4198,4880` |
| `s_Data.CurrentAccumulationIndex` ping-pongs the two GL accumulation textures | `Renderer3D.cpp:4739-4742,5436` |
| Editor camera is **not deterministic** across headless runs — pin with `WASTELAND_PIN_CAMERA` | `EditorLayer.cpp:538-546` |
| No frame-rate cap: `Application::Run()` loops unthrottled; `SetVSync(true)` hardcoded | `Application.cpp:424`; `WindowsWindow.cpp:312` |
| `Timestep` is the raw render delta; `ActiveCameraIsEditor` is tracked | `Application.cpp:424`; `Renderer3D.cpp:3376` |
| Training infra exists: numpy + Pillow, no torch/onnx | `tools/train_neural/` |
| GPU: RTX 3060 Ti, 8 GB | `nvidia-smi` |
| CPU: Python 3.12.10 / 3.14.7 (module targets 3.14) | session env |
| Shader verifier suite is 7/7 green; `verify_texture_sampler_indexing` pins the GL binding set to exactly `0..7` | `docs/NEURAL_RENDERING.md` task 8 postscript |

### 1.1 What the DLSS-class features must be re-expressed as

Because the constraint in §0.5 dominates, every new pass is budgeted **at zero or one new image
unit**, and the plan prefers zero.

| feature | new image unit? | why |
|---|---|---|
| **WLSR** (EASU + RCAS) | **0** | reads the *existing* `img_Bloom_Temp` (`u7`) as a sampler and writes `img_Output` — the exact same read/write pair `RunUpscale` (pass 8, currently dead) already uses. Two passes, one of which is a re-write of a dead one. |
| **WLAA** | **0** | a sharpen stage reading `img_Output` and writing it back per-pixel (the same pattern `RunNeuralEval` already uses) plus a re-tuned accumulation alpha |
| **WLRR** | **1** (`u13`) | needs a per-pixel **residual** target to be a *residual* corrector rather than a replacement. See §1.2. |
| **WLFG** | **1** (`u13`) on GL | needs a **second** history buffer to reproject from. See §1.2 — this is the one place the budget is genuinely tight. |
| **WLSR+WLFG together** | **2** | both want `u13`. **Resolve by one shared scratch image** (§1.2.3). |

### 1.2 The one real binding problem: one spare GL image unit

**Measured:** GL units 0-7 are all in use, and a ninth image uniform fails to LINK the whole Nova
program. So the plan cannot add two images to the GL path.

**What is actually free in the GL image namespace:** nothing.

**What is free elsewhere:**

1. **The HLSL/Vulkan/DX12 path has `u13` and above.** DX12's `kUavCount = 13` covers `u0..u12`, so a
   new UAV is a `kUavCount` bump plus a descriptor — cheap, and `DX12Nova.h:165-222` is the template
   for exactly that move (it records the last one, 52→53).
2. **GL sampler units are a different namespace and have room.** `glActiveTexture` units are bound
   from 3,4,8,10,13 in the Nova path (`Renderer3D.cpp:4754-4772`). A new *sampler* is free; a new
   *image* is not.
3. **`img_Bloom_Temp` (`u7`) is written by pass 7 and read by pass 6** — it is a scratch buffer whose
   content is transient within a frame. **Any new pass that runs after the composite can use it as
   scratch**, as long as it runs before pass 6 of the *next* frame writes it.

**The resolution, and it is the load-bearing design decision of the whole plan:**

> **The GL limit is a DECLARATION-COUNT limit, not a binding-index limit — and the engine already
> rebinds an occupied unit mid-frame. So the new passes declare NO new image uniforms and reach
> their scratch texture through a runtime rebind, exactly as the SVGF path already does.**

Two measured facts settle this, and the draft had the first one wrong:

1. **`verify_nova_glsl.py:213` asserts `len(image_uniforms) <= 8`** — a count of *declarations*. The
   driver failed the shader with **C7624** when a ninth declaration became active, and the script's
   own negative control injects a ninth uniform *at a legal binding index* (`binding = 7`) and
   expects rejection. So a rebind to an occupied unit is not merely legal, it is **the pattern the
   engine already uses**.
2. **`Renderer3D.cpp:5225,5238` rebinds image unit 6 from the trace G-buffer to the moments texture
   and back, inside one frame's SVGF dispatches.** The guard is only the comment "the two textures
   are not used together in these SVGF dispatches" — which is exactly the constraint the new passes
   must respect.

Consequences, which are stronger than the draft allowed:

- **The GL path needs no new image unit and no fallback.** The ninth-declaration problem cannot
  arise, because no ninth declaration is added. The draft's `shared`-memory-only WLRR head and
  analytic-only WLFG fallbacks are **withdrawn** — they were a response to a constraint that does
  not exist in this form.
- **The borrowable units are any unit whose texture is dead for the duration of the new dispatch.**
  Unit 7 (`img_Bloom_Temp`) is the natural choice for the post-composite passes; unit 6 for the
  pre-composite ones. The **reactive-mask verifier** (task 1) exists to prove the deadness claim
  statically, because a mask read after its writer has been unbound is a silent no-op.
- **The draft's "highest-risk assumption" (task 1 step 2) is downgraded to a regression check.**
  The mechanism is precedent-backed and live; what needs proving is only that the *specific* new
  rebinds do not disturb the bloom chain. That is a screenshot A/B, not a go/no-go probe.

**Task 1 must not begin until `tools/verify_nova_binding_budget.py` has been extended to assert the
new registers and its `--self-test` proves it can fail.**

### 1.3 Pass-index allocation

Free indices: **8, 15, 16** are declared-but-dead; **20+** are free.

| pass | name | status | notes |
|---|---|---|---|
| 8 | `RunWlsrEasu` | **revive + rewrite** | replaces the dead `RunUpscale`. Reads `img_Bloom_Temp`, writes `img_Output`. |
| 15 | `RunWlsrRcas` | **revive + rewrite** | replaces the dead `RunTSRResolve`. Reads `img_Output`, writes it back. |
| 16 | `RunWlaaSharpen` | **revive + rewrite** | replaces the dead `RunTSRCopy`. Reads/writes `img_Output`. |
| 20 | `RunWlrrDenoise` | new | the A-Trous kernel family + residual head. Writes the residual image. |
| 21 | `RunWlrrComposite` | new | applies the gated residual to `img_Accumulation`. |
| 22 | `RunWlfgWarp` | new | per-present reprojection warp. Writes `img_Output`. |
| 23 | `RunWlfgBlend` | new | neural frame-synthesis head + disocclusion fill. Writes `img_Output`. |

**Passes 8/15/16 are reused rather than left dead**, and that is a deliberate choice: leaving three
dead cases in the switch while adding four more new ones makes the switch unreadable, and the dead
passes' names (`RunUpscale`, `RunTSRResolve`, `RunTSRCopy`) are close enough to the new features'
purpose that a future reader would reasonably assume the wrong thing. **The doc must state which
`case` numbers were repurposed.**

### 1.4 Vulkan is a contract, not a renderer

`VulkanNova` is compiled but never instantiated, so **no Vulkan claim in this plan may be a rendering
claim.** Verify Vulkan by contract: `tools/verify_vulkan_nova_contract.py`,
`tools/verify_vulkan_reservoir_allocation.py`, `tools/compile_nova_spirv.py --check`, and the extended
binding-budget verifier. The `.spv` is regenerated by `compile_nova_spirv.py --deploy`, never
hand-edited.

### 1.5 The measurement harness already exists

`tools/bench_nova_phase.py` launches the editor with the camera pinned, the DRS pinned off and the
scale fixed, then reads `app`/`gpu`/`cpuSubmit`/`instances` from the engine's own `Frame time:` line
(`Renderer3D.cpp:5697`), marks a row **INVALID** when `gpu` reads 0.0, and prints GPU utilisation on
both sides of every run. It is the cost gate for every feature in this plan and it needs no
modification for tasks 2-5 — **but task 6 (WLFG) needs a second timing line** (§6.3).

---

## 2. Approach

### 2.1 Where each feature sits in the schedule

```
pass 1 (trace) → pass 0 (visibility+velocity) → [3,11,14 ReSTIR]
   → pass 2 (temporal accum) → [12,13 SVGF] → [7 bilateral] → pass 6 (composite)
   → [4,5 bloom] → [10 god rays]
   → PASS 20/21 (WLRR denoise + residual apply)      ← new, before the composite
   → PASS 8  (WLSR EASU)      → writes img_Output     ← replaces the dead RunUpscale
   → PASS 15 (WLSR RCAS)      → writes img_Output     ← replaces the dead RunTSRResolve
   → PASS 16 (WLAA sharpen)   → writes img_Output     ← replaces the dead RunTSRCopy
   → [17,19,18 neural stage]  (existing, unchanged)
   → FrameIndex++
   ... present ...
   → PASS 22/23 (WLFG warp + blend)                  ← new, on the interpolation present only
```

Order notes, each load-bearing:

- **WLRR runs before the composite** so it corrects the *denoised* image, not the finished one. The
  composite applies chromatic aberration, vignette, tonemap, colour grading and film grain —
  correcting after it would mean the network has to un-do a tone curve.
- **WLSR runs after the composite**, because it is an upscale of the finished frame (the same
  position the existing neural stage already occupies, and for the same reason: it sees bloom and
  volumetrics coherently).
- **WLFG runs on the interpolation present only**, and only when the factor > 1.
- **The neural stage (17/18/19) is unchanged.** It is a competing end-of-frame neural system and this
  plan does not stack a third one on top of it without saying so; task 12 records the interaction.

### 2.2 The reactive mask — the one genuinely new idea that is cheap

Both WLSR and WLAA need a per-pixel input saying *how much to trust the current frame*. This engine
already computes every component of it and throws the result away:

```
reactive = f( velocity magnitude          // img_Velocity
            , depth discontinuity         // s_DepthBuffer vs reprojected depth
            , normal discontinuity        // img_TraceGBuffer.gba vs reprojected normal
            , albedo/hit change           // img_AlbedoHit
            , u_CameraMoved )
```

This is a **per-pixel float in [0,1], computed in the existing passes and stored in a spare channel
of an existing image** — `img_Bloom_Temp.a` is free at the moment WLSR runs (pass 7 writes rgb only),
and `img_AlbedoHit.a` already carries a hit mask. **Zero new image units**, and it is the difference
between a super-resolver that looks like an upscale and one that looks like a reconstruction.

### 2.3 Hardware tiers

`RenderCapabilities::FeatureTier` already exists with a `TierFor()` resolver, and `denoiseComputeCapable`
is `false` everywhere with a comment saying it "only ever resolves for a future component"
(`RenderCapabilities.h:20-25,44-50`). This plan fills it — for the four features, not one:

| Tier | Backend | What runs |
|---|---|---|
| **ReconstructSpatial** | GL, DX11 | WLSR (EASU+RCAS), WLAA sharpen, reactive mask |
| **ReconstructTemporal** | DX11, DX12 | + WLRR full (kernel family + residual head) |
| **ReconstructFull** | DX12 | + WLFG with the neural synthesis head |
| **Unavailable** | any | analytic bilinear, no new passes |

Every tier **runs the same shader source**; they differ in pass count, kernel radius and model depth,
selected by a uniform. One authored shader, four backends, no `#ifdef` per vendor. **No tier is gated
on a GPU vendor or on `tensorCores`** — `RenderCapabilities::tensorCores` is declared and uploaded
but read by nothing, and it must stay unused; adding a vendor gate is the first thing that would make
this RTX-only.

### 2.4 Training data — the engine's own frames, and the target differs per feature

No external dataset exists and none is fetched. `tools/train_neural/export_dataset.py` already drives
the engine binary headlessly and produces matched pairs; it is reused and extended.

| feature | input | target | why this target |
|---|---|---|---|
| **WLRR head** | single-frame noisy, accumulation **off**, scale 0.5 | converged, accumulation **on**, scale 1.0 | the converged pass has **+0.084 dB** of affine headroom and the single-frame reduced-res pass has **+2.042 dB** — 24x more learnable signal. Measured, `docs/NEURAL_RENDERING.md` §2. |
| **WLSR RCAS** | trace-resolution image | full-resolution image | sharpening is a *per-pixel* correction with no temporal component; the target is the thing being approximated. |
| **WLFG head** | two warped frames at factor N | the real frame at that N | **N is held out** — see task 6. |

**The critical warning, already paid for once in this repo:** the first shipped neural model was
trained on a dataset captured *before* a shader change and had learned a 5.33-luma lift the renderer
no longer produced. On a fresh capture it scored **37.040 → 34.127 dB** — *worse than doing nothing*
(`docs/NEURAL_RENDERING.md` §3). **Every training task in this plan must re-export its dataset with
the current binary and record the dataset's sha256 in the commit message.**

---

## 3. Step-by-step

Each task is sized for a subagent. Every task that touches a shader ends with the verifier commands
and **their actual output pasted into the commit message** — not "it passed".

---

### Task 1: Binding budget, pass-index reservation, and the reactive-mask design — no shader change

**Objective:** Reserve the registers and pass IDs the four features will use, prove the GL image-unit
binding strategy in §1.2 actually works, and extend the binding-budget verifier so a collision cannot
slip through.

**Files:**
- Modify: `tools/verify_nova_binding_budget.py`
- Create: `docs/CLEANROOM_RECONSTRUCTION_BINDING_PLAN.md`
- Create: `tools/verify_reactive_mask_budget.py`
- Read-only: `Wasteland/src/Platform/NVRHI/VulkanNova.h`, `DX12Nova.h`, `DX11Nova.h`, `docs/NOVA_BINDING_BUDGET.md`

**Step 1 — decide the allocation from source, not from a report.**

```bash
grep -n 'register(' DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl
grep -n 'layout(binding' DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl
grep -n 'kImageCount\|kReadImageCount\|kSampledSlot\|kImageUavSlot' Wasteland/src/Platform/NVRHI/VulkanNova.h
grep -n 'kUavCount\|kUavBase\|kImageSrvBase\|kSamplerBase' Wasteland/src/Platform/NVRHI/DX12Nova.h
grep -n 'std::array<Image' Wasteland/src/Platform/NVRHI/DX11Nova.h
grep -n 'glBindImageTexture' Wasteland/src/Wasteland/Renderer/Renderer3D.cpp
```

Expected allocation (verify, do not assume):

| Resource | Register | GL unit | Notes |
|---|---|---|---|
| WLSR input (bloom temp) | — | **7** (sampler read) | `img_Bloom_Temp`, dead at this point in the frame |
| WLSR output | — | 0 (`img_Output`) | same pair as the current dead pass 8 |
| WLRR residual UAV | `u13` | **7** (image write, rebind) | declaration-count limit, so **no new declaration** — borrow unit 7 per the SVGF precedent |
| WLRR residual read SRV | `t75` | — | `t74` is NeuralWeights; `t75` is free |
| WLFG previous-present UAV | `u13` (shared) | **7** (image write, rebind) | same scratch image as WLRR; not used together |
| WLFG warp output | — | 0 | |
| Pass IDs | 8, 15, 16, 20, 21, 22, 23 | — | 8/15/16 repurposed, 20-23 new |

**Step 2 — regression-check the GL unit-7 rebind (downgraded from a go/no-go probe).**

The mechanism is precedent-backed and live: `Renderer3D.cpp:5225,5238` already rebinds unit 6 from the
trace G-buffer to the moments texture mid-frame. What needs proving is only that the *specific* new
rebinds do not disturb the bloom chain.

```bash
WL_SCREENSHOT=unit7_probe.png WL_SCREENSHOT_FRAME=120 ./DemonCore-Editor.exe
```

**Acceptance: the bloom chain is unchanged.** Compare mean luma and local-contrast p99 against a
control run with the probe disabled; both must agree within the scene's own noise floor (the
`--allow-blank` and contrast metrics in `export_dataset.py` are the existing tools for this).

**The draft's fallback (`shared`-memory-only WLRR head, analytic-only WLFG) is WITHDRAWN** — it was
a response to a constraint that does not exist in this form. If the rebind *still* regresses the
bloom chain, the fix is to choose a different borrowable unit, not to weaken the features.

**Step 3 — extend the verifier.**

Add to `tools/verify_nova_binding_budget.py`:
- an `EXPECTED` table of the new registers with their intended purpose;
- an assertion that every `register()` in the HLSL maps to a known, non-colliding Vulkan binding,
  including `u13`, `t75`;
- an assertion that `u8` is still a hole and is not silently reused;
- a fourth self-test mode `--self-test-wlsr` that injects a colliding `register(u13)` twice and
  asserts FAIL.

**Step 4 — write `tools/verify_reactive_mask_budget.py`.**

The reactive mask is stored in spare channels of existing images. This verifier asserts the channel
is genuinely free at the moment the consuming pass runs — i.e. it checks the *writer* of each image
against the *reader*, by pass order. A mask stored in a channel that the writer overwrites after the
reader consumes it is a silent no-op, which is the failure mode this whole plan keeps hitting.

**Step 5 — run it.**

```bash
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_nova_binding_budget.py --self-test
python3 tools/verify_nova_binding_budget.py --self-test-dup
python3 tools/verify_nova_binding_budget.py --self-test-scenetex
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/verify_reactive_mask_budget.py
```

Expected: PASS on all six. If the first fails, the allocation in step 1 is wrong — fix the
allocation, not the verifier.

**Step 6 — commit.**

```
docs(verify): reserve registers u13/t75 and passes 8,15,16,20-23 for the clean-room reconstruction stack
```

---

### Task 2: Reactive mask pass (pass 24) — the input every other feature needs

> **STATUS: DONE (2026-10-26).** `RunReactiveMask` added to both twins, dispatched
> after pass 2, gated on `s_Data.ReconstructionEnabled` with a `WL_RECONSTRUCTION`
> env override. Acceptance passed: with `WL_FREEZE_TIME=10.0` and the camera
> pinned, `WL_RECONSTRUCTION=1` vs unset is **bit-identical (0 of 3,383,732
> pixels)**, and a determinism control confirms the harness can detect a
> difference. Evidence: `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task2/`.
>
> **Two lessons that apply to every later task:**
> 1. **`WL_FREEZE_TIME` is mandatory in every A/B.** The first attempt compared
>    two runs whose cameras differed (`(0.0,1.9,-10.4)` vs `(0.0,1.6,-8.9)`) and
>    reported 99.77% different. `WASTELAND_PIN_CAMERA` pins the *initial* pose,
>    not the simulation clock. Any comparison without `WL_FREEZE_TIME` measures
>    two different scenes.
> 2. **A guard must not fail on pending work.** The first version of
>    `verify_reactive_mask_budget.py` flagged any declared-but-undispatched
>    consumer — true of passes 8/15/16 today — and would have been red until
>    task 5. It now only flags a *dispatched* consumer.
>
> **Also fixed:** the G-buffer's packed normal is in `.yzw`, not `.gba` (the
> store at `.glsl:3978` is `vec4(tndc, normal*0.5+0.5)`). Reading `.gba` fetches
> `(ny, nz, nx)` — a rotated normal with a plausible length, so it would have
> produced a plausible-looking but wrong mask with no error.

**Objective:** Compute and store the per-pixel reactive/trust mask described in §2.2, writing it
into a spare channel of an existing image. **No new image unit.**

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl`
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/VulkanNova.h`, `VulkanNova.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/DX12Nova.h`, `DX12Nova.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/DX11Nova.h`

**Step 1 — write the GLSL half.**

Add to `.glsl`, after the god-ray pass and before `main()`:

```glsl
// Pass 24: reactive mask. One thread per output pixel. Reads the velocity buffer,
// the trace G-buffer (NDC depth + packed normal) and the albedo/hit mask, and
// writes a single float in [0,1] into the ALPHA channel of img_Bloom_Temp.
//
// The mask is the answer to "how much should the reconstruction trust the
// current frame at this pixel". It is deliberately computed from inputs the
// engine already has and already throws away: velocity magnitude, a depth
// discontinuity against the reprojected depth, a normal discontinuity, the
// albedo/hit change, and u_CameraMoved.
//
// Zero new image units: img_Bloom_Temp's alpha is free at this point in the
// frame (pass 7 writes rgb only, and pass 6 has already consumed the rgb).
void RunReactiveMask();
```

Body: reproject using the same velocity/`histUV`/depth-rejection pattern as
`RunTemporalAccumulation` (`.glsl:4219-4265`) — **the reprojection logic must be identical**, because
a mask computed with a different reprojection than the accumulation's is a mask that disagrees with
the thing it is masking. Guard every sample with a bounds check (the `floor()`-before-clamp bug class
documented at `.glsl:5083-5094`).

**Step 2 — add `case 24:` to both `main()` switches.**

**Step 3 — mirror in the HLSL twin.** Identical constants. The reprojection sequence is an ABI with
the GL twin; `tools/verify_nova_shader_parity.py` must continue to pass.

**Step 4 — dispatch it from `RunNovaPassSchedule`, after pass 2 and before the composite.**

`Renderer3D.cpp:3610` — add after `pass(2);n.CommitHistory();` and before the SVGF block. Order
matters: it must run after the velocity buffer and the G-buffer are written, and before pass 6
consumes the bloom temp.

**Step 5 — wire the new registers on each backend.**

- Vulkan: `kImageCount` 9 → 10, `kImageUavSlot[]` gains `u13`; `kReadImageSlot`/`kReadImageIndex`
  gain `t75`; `CreateBindingSet` and `VerifySpirvBindings` updated.
- DX12: `kUavCount` 13 → 14; `kDescriptorsPerHeap` follows; the readable-SRV block 11 → 12 to reach
  `t75`. **The block bases must move together** — `DX12Nova.h:165-222` is the template for the last
  such move and `kUavBase` must become a relative base.
- DX11: `uavs[11]` → `uavs[13]`; `srvs[128]` gains `t75`.
- OpenGL: **no new image unit** — the mask goes into `img_Bloom_Temp.a`, which is already bound.

**Step 6 — verify.**

```bash
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/verify_reactive_mask_budget.py
python3 tools/verify_vulkan_nova_contract.py
python3 tools/verify_vulkan_reservoir_allocation.py
python3 tools/compile_nova_spirv.py --check
python3 tools/verify_nova_constant_names.py
```

**Step 7 — accept: the image must not change.**

This pass writes only the alpha of `img_Bloom_Temp`, which nothing reads. Capture before/after
screenshots and confirm a **zero** pixel delta. Any delta means the pass wrote to a channel
something reads — stop and find it.

**Step 8 — commit.**

```
feat(nova): add reactive mask pass (24)
```

---

### Task 3: WLSR — EASU upscale (pass 8)  — **DONE 2026-10-26**

> **STATUS: DONE.** Evidence: `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task3/README.md`.
> Acceptance: masked vs unmasked EASU differs on **45.39%** of pixels (1,535,911 of
> 3,383,732; max delta 8, mean 1.0429). Verifier `--self-test`: 7/7 mutations
> detected. Full suite green.
>
> **The plan's premise was wrong and this is the important part.** The plan said
> to "revive the dead pass 8" with a new EASU. There is no dead pass 8 to revive:
> a complete, documented FSR1 EASU already existed in both twins (`RunFSR1EASU`),
> reachable via `ReconstructionFilter::FSR1` (8) inside `RunComposite`. Pass 8
> (`RunUpscale`) is the dead one, and it is *not* the EASU. Writing a second EASU
> would have duplicated working code.
>
> What was actually missing was narrow: **the EASU read no reactive mask.** Task 3
> wires the mask into the EASU branch, blending toward bilinear where the mask
> says the current frame cannot be trusted.
>
> Two real defects were found en route, both worth remembering:
> 1. **The OpenGL path tracer is a separate schedule** inline in
>    `Renderer3D::Flush()`. `RunNovaPassSchedule` serves only DX11/DX12/Vulkan, so
>    a pass added there does not exist on OpenGL — and the `WL_RECONSTRUCTION`
>    override, living inside it, was invisible on OpenGL too. Both now have
>    backend-independent homes.
> 2. **`-p:BuildProjectReferences=false` makes the build a silent no-op.** It skips
>    the referenced `Wasteland` project, so C++ changes never compile while
>    MSBuild still prints `MSBUILD_RC=0`. This produced a 50-minute-old exe and
>    made correct code look broken. Always check the exe's age after a build.
>
> **Methodology:** never prove a branch runs by comparing two different filters —
> that always differs. Use a forced constant inside the branch or read the value
> back. See the skill `wasteland-reconstruction-a-b`.

**Objective:** ~~Revive the dead pass 8 with a real edge-adaptive upscale that reads the reactive mask.~~
Wire the reactive mask into the existing EASU. (See STATUS above.)

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (replace `RunUpscale`)
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`
- Create: `tools/verify_easu_kernel.py`

**Step 1 — write the EASU kernel.**

The kernel is a gradient-reconstruction upscale: for each output pixel, sample the source at the
mapped position, compute the local gradient from the 4-neighbourhood, and weight the taps by
`exp(-|gradient| * k)` where `k` is **scale-aware** — the kernel's effective radius must grow as the
render scale drops, because a 0.5 scale means each source texel covers four output pixels.

Write the kernel from the published definition (edge-adaptive spatial upsampling: a directional
filter whose weights are the *inverse* of the local gradient magnitude, normalised). Do not copy any
vendor source.

**Step 2 — add the reactive-mask read.**

The mask scales the blend between the EASU result and the bilinear result: where the mask is low
(disocclusion, motion), the bilinear result is safer. This is what makes the difference between a
super-resolver and an upscale visible.

**Step 3 — keep the twins identical.**

Every constant is a named local with a comment. `0.65f`-style magic numbers are forbidden.

**Step 4 — dispatch it.**

`Renderer3D.cpp:3634` — add after `pass(6,true)` and the bloom block, before god rays.

**Step 5 — verify.**

```bash
python3 tools/verify_easu_kernel.py          # kernel maths, standalone, with negative controls
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_reactive_mask_budget.py
```

`tools/verify_easu_kernel.py` must: transcribe the shader's own kernel formula and evaluate it; check
that the weights sum to 1.0 at several fractional positions; check that the kernel's support grows
with `1/RenderScale`; and carry a negative control that mutates the normalisation and asserts the
verifier FAILs.

**Step 6 — accept: the image must change, and it must change in the right direction.**

A/B with `WL_AA_FILTER` pinned, camera pinned, DRS off:

| arm | expectation |
|---|---|
| control (bilinear) | baseline |
| WLSR EASU | **higher** local-contrast p99, **lower** or equal mean abs diff against a full-res reference |
| WLSR at scale 0.5 vs bilinear at 0.5 | p99 contrast higher, mean luma within noise |

**Do not report this task complete on a clean build.** The acceptance is the p99-contrast ordering
plus a screenshot.

**Step 7 — commit.**

```
feat(nova): add WLSR EASU upscale (pass 8)
```

---

### Task 4: WLSR — RCAS sharpening (pass 15)  — **DONE 2026-10-26**

> **STATUS: DONE.** Evidence: `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task4/README.md`.
> Acceptance: EASU+RCAS p99 local contrast **rises** over EASU alone
> (7.571 → 7.639); RCAS changes 1.40% of pixels (47,520 of 3,383,732; max delta 8,
> mean 1.04). Verifier `--self-test`: 6/6 mutations detected. Both budget
> verifiers PASS. Full suite green.
>
> **Design: an unsharp mask, not a weighted average.** The first form tried was the
> FSR1-style weighted mean `(ctr + gain*diag)/(1+4*gain)` — a convex combination
> that can only pull the centre *toward* its neighbours. Measured, it *lowered*
> p99 contrast: a blur, not a sharpen. The fix is the symmetric unsharp push
> `ctr + gain*(ctr - blur)`, which moves the centre *away* from its neighbourhood
> and is what raises contrast. The gain is the local range `mx - mn` (adaptive),
> × user sharpness × trust `(1 - mask)`, clamped to `RCAS_MAX_SHARPEN`; the result
> is clamped to the range. Two clamps, not one: the gain clamp stops the halo on a
> hard edge, the result clamp catches the residual push.
>
> **A real defect found en route (the reorder).** The reactive-mask budget
> verifier flagged that RCAS read `img_Bloom_Temp.a` — the mask — but the bloom
> blur (pass 4/5) *also* writes that image and was dispatched *between* the mask
> writer (pass 24) and the RCAS consumer. With bloom on, RCAS would read the
> bloom's alpha, not the mask, and the trust term would be a silent no-op. Fixed
> by dispatching RCAS **before** the bloom chain in both schedules — also the
> artistically correct order (sharpen scene detail, then add additive bloom on
> top, so it cannot halo around bright sources). The A/B captures were re-run
> after the reorder; identical here because bloom is off in this config, but
> correct when it is on.
>
> **Pass 15 was a complete but undispatched `RunTSRResolve`** — not dead code to
> delete, an orphaned stage. Repurposed it to `RunWlsrRcas` in both twins and
> removed 15 from the binding-budget `DEAD_PASSES`.

**Objective:** Add the sharpening stage that counteracts the upscale's blur, clamped per pixel.

**Files:**
- Modify: `.glsl` (replace `RunTSRResolve`), `.hlsl`, `Renderer3D.cpp`
- Create: `tools/verify_rcas_sharpen.py`

**Step 1 — write the sharpen.**

Per-pixel: compute local contrast from the 4-neighbourhood, derive a sharpen amount proportional to
the contrast **and** to the reactive mask, and clamp the amount to a maximum. The clamp is the part
that keeps it from ringing on high-contrast edges.

**Step 2 — dispatch after pass 8.** `Renderer3D.cpp` — after the EASU dispatch.

**Step 3 — verify + screenshot.**

Same harness as task 3. Acceptance: a scale-0.5 render with EASU+RCAS must have a **higher** p99
contrast than the same render with EASU alone, and **no** visible ringing on a high-contrast edge
(checked by the verifier's negative control: an unclamped sharpen must FAIL the clamp check).

**Step 4 — commit.**

```
feat(nova): add WLSR RCAS sharpening (pass 15)
```

---

### Task 5: WLAA — native-resolution AA with a real sharpen stage (pass 16)

> **DONE (2026-10-26).** `Wlaa = 12` in the `ReconstructionFilter` enum; a real
> `u_ReconstructionFilter == 12` branch in `RunComposite`'s full-resolution `else`
> arm sharpens the resolved `img_Accumulation` with the shared image-free
> `RunWlsrSharpenCore` (the same core pass 15 uses). Default `WLAA_SHARPNESS=0.5`,
> overridable via the WLSR sharpen knob, clamped either way. Gated on
> `WL_AA_FILTER=12`, default off. **Acceptance:** converged 800-frame A/B at
> RenderScale 1.0 — WLAA p99 local contrast **25.773** vs TAA (bilinear) **15.142**
> (**+10.631, PASS**); `mad = 1.727/255` over 3.38M px. The noise-free difference
> (both arms share the deterministic accumulation) is edge-concentrated — 91.67%
> of pixels untouched, 8.21% edge-enhancement, 0.12% at corners only — with **no
> parallel halo bands, i.e. no ringing**. `verify_wlaa_contract.py` static +
> self-test 4/4; RCAS verifier still green after the shared-core refactor (6/6);
> binding + mask budgets PASS. Evidence: `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task5/README.md`.
> Findings: (1) image-typed shader params do not compile under bindless GLSL → the
> image-free core; (2) stale pass-15 entry still passed the image → runtime
> compile error caught it; (3) the engine reads shaders from `bin/`, not the
> source tree → synced both bin trees.

**Objective:** Make the existing temporal accumulation reachable as a first-class mode, with the
sharpening DLAA requires and a real measurement of what the history buys.

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (replace `RunTSRCopy`)
- Modify: `.hlsl`, `Renderer3D.cpp`, `Wasteland/src/Wasteland/Renderer/Renderer3D.h`
- Modify: `DemonCore-Editor/src/EditorLayer.cpp`
- Create: `tools/verify_wlaa_contract.py`

**Step 1 — add the enum value.**

`Renderer3D.h` — add `Wlaa` to `ReconstructionFilter` with a documented value that does **not**
collide with the existing 0-11. The existing `6-11` range is documented as "separate passes" that
fall through to bilinear; the new value must be a real branch.

**Step 2 — make the branch real.**

`RunComposite` must branch on it: at `RenderScale == 1.0` the composite is a no-op upscale, so the
WLAA path is the sharpen stage plus the accumulation. The uniform must be **declared in both twins**
and **read** — the entire lesson of `u_ReconstructionFilter` being plumbed-but-dead for months.

**Step 3 — write `tools/verify_wlaa_contract.py`.**

Asserts, by source shape:
- the enum value exists on the CPU and is declared in both twins;
- `RunComposite` has a branch that reads it (not a fall-through);
- the sharpen stage is dispatched when it is selected;
- the sharpen's amount is clamped.

Each with a negative control.

**Step 4 — the measurement that makes the mode honest.**

A/B: TAA-style (jitter + accumulate + bilinear) vs WLAA (jitter + accumulate + sharpen) at
`RenderScale = 1.0`, camera pinned, 400 frames. Report p99 local contrast and the mean abs diff
against a converged reference for **both** arms.

**The honest outcome is acceptable.** If WLAA does not beat TAA on p99 contrast, that is a finding
and must be reported — the mode is then documented as "equivalent at this scene" rather than as a
win. **Do not tune the sharpen amount to manufacture a win.**

**Step 5 — commit.**

```
feat(nova): add WLAA native-resolution anti-aliasing (pass 16)
```

---

### Task 6: WLFG — the frame-pacing and clock prerequisites

**Objective:** Make the engine capable of presenting a synthetic frame at all. This task contains **no
AI** and is the prerequisite for task 8.

**Files:**
- Modify: `Wasteland/src/Wasteland/Core/Application.cpp`, `Application.h`
- Modify: `Wasteland/src/Wasteland/Core/Window.h`, `Wasteland/src/Platform/Windows/WindowsWindow.cpp`
- Modify: `DemonCore-Editor/src/EditorLayer.cpp`
- Modify: `Wasteland/src/Wasteland/Scripting/Bindings.cpp`
- Create: `tools/verify_wlfg_clock.py`

**Step 1 — a synthetic frame-rate cap.**

`Application::Run()` loops unthrottled. Add a cap that is *independent of vsync*: vsync is a display
synchronisation, not a pacing mechanism, and it cannot be disabled at runtime without a window
recreate. The cap is a sleep until the target interval.

**Step 2 — the `SimTimescale` clock.**

`Timestep` must be divisible. Add `Application::SetSimTimescale(float)` and make the timestep handed
to layers be `rawDelta * timescale`. **The render loop still runs at the real rate**; only the
simulation advances slower. This is the mechanism that makes interpolation correct: at a 6x factor
the game advances 1/6 of real time per *interpolated* frame, so the interpolated frames show the
game at the correct speed.

`tools/verify_wlfg_clock.py` checks this as arithmetic: over N presents, the sum of simulated deltas
must equal the real elapsed time regardless of the factor.

**Step 3 — a second timing line.**

`tools/bench_nova_phase.py` reads the `Frame time:` line. Add a second line reporting
`wlfgFactor`, `wlfgWarps`, `wlfgRealFrames` and `simDriftMs` (the accumulated difference between
simulated and real time — it must stay bounded). Without it, WLFG cannot be measured at all.

**Step 4 — the factor ceiling from the accumulation state.**

§0.4's table. `u_CameraMoved` must disable interpolation entirely; a partially-converged frame must
cap at 2x; a converged static frame may go to 6x. The cap is a *function*, not a constant, and the
function is what the verifier checks.

**Step 5 — verify.**

```bash
python3 tools/verify_wlfg_clock.py
```

**Step 6 — commit.**

```
feat(app): add a synthetic frame-rate cap and a divisible simulation clock for WLFG
```

---

### Task 7: WLFG — per-present reprojection warp (pass 22)

**Objective:** Warp the previous *present* frame to the current camera position using the velocity
buffer, and fill the disocclusions.

**Files:**
- Modify: `.glsl` (new `RunWlfgWarp`), `.hlsl`, `Renderer3D.cpp`
- Modify: `Wasteland/src/Platform/NVRHI/{VulkanNova,DX12Nova,DX11Nova}.{h,cpp}`
- Create: `tools/verify_wlfg_warp.py`

**Step 1 — the second history buffer.**

The plan's binding problem (§1.2) is that there is only one history texture. Add a second, and make
`CommitHistory` alternate between them. **This is a change to a live, working path** — the temporal
accumulation depends on `s_Accumulation` at `t48` — so it must be verified against a control run.

**Step 2 — the warp.**

For each output pixel: read the velocity, look up the previous present, and blend. The reprojection
must use **the same pattern as pass 2** (the mask's reprojection in task 2 is the template).

**Step 3 — disocclusion fill.**

A warp leaves holes where the previous frame had no data. Fill them by a 3x3 neighbourhood search
(preferring the tap with the closest depth), then fall back to the current frame's real render. The
reactive mask decides which.

**Step 4 — verify.**

`tools/verify_wlfg_warp.py`: transcribes the warp's address computation and checks, over a grid of
random velocities and depths, that (a) a zero velocity reproduces the identity exactly, (b) a
velocity that lands outside the frame is rejected, (c) the depth test rejects a genuine disocclusion,
(d) every output pixel is covered by exactly one source tap or the fill path.

Each with a negative control.

**Step 5 — accept: the warp must be exact at zero velocity.**

A static camera with `WLFG=1` (warp only) must produce an image **bit-identical** to the no-WLFG
control. Any delta is a bug in the address computation, not a quality issue.

**Step 6 — commit.**

```
feat(nova): add WLFG reprojection warp (pass 22)
```

---

### Task 8: WLFG — neural frame-synthesis head (pass 23)

**Objective:** Fill the disocclusions the warp cannot, with a trained head whose failure mode is "use
the real frame".

**Files:**
- Create: `tools/train_wlfg/model.py`, `dataset.py`, `train.py`, `export_weights.py`
- Modify: `.glsl` (new `RunWlfgBlend`), `.hlsl`, `Renderer3D.cpp`
- Modify: `Wasteland/src/Wasteland/Renderer/WlfgStageBlob.h` (new, mirroring `NeuralStageBlob.h`)

**Step 1 — the dataset.**

Export with the existing exporter, extended: capture the *real* frame at factor N for N in
{1, 2, 3, 4, 6}, with the camera on a deterministic motion path. **Hold out one factor entirely** —
train on {1,2,3,4}, evaluate on 6. A model trained on every factor and evaluated on one of them is
not a model, it is a lookup table.

**Step 2 — the architecture.**

Deliberately small. Inputs: the warped frame's RGB, the reactive mask, the velocity, the depth. Output:
a **residual** plus a **gate**, exactly the `out = anchor + gate * residual` form the existing neural
stage uses (`docs/NEURAL_RENDERING.md` §1). The residual form is what makes "the network is wrong"
degrade to "the real frame is shown" rather than to "a wrong frame is shown".

**Step 3 — train and gate.**

`tools/train_wlfg/test_model.py` must report PSNR/SSIM against the held-out factor **and** against
the no-WLFG control. **If the model does not beat the control, it is not a feature** — report and
stop, do not tune the gate to hide it.

**Step 4 — the blob.**

Reuse the `NeuralStageBlob.h` pattern: magic, version, shape fields, **CRC32**, payload. A
bit-flipped weight in an uploaded constant buffer is a silent visual corruption with no shader error,
no link failure and no log line — the same failure class as a ninth GL image uniform.

**Step 5 — verify + screenshot.**

```bash
python3 tools/verify_wlfg_warp.py
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/compile_nova_spirv.py --check
```

**Step 6 — the 6x claim, stated with its precondition.**

The acceptance is: at a **converged static camera**, `WLFG=6` produces a frame whose PSNR against the
real frame at that camera position is within X dB of the no-WLFG control, **and** the measured
present rate is ≥ 6x the render rate. **Both numbers go in the doc, with the accumulation-state
precondition attached.**

**Step 7 — commit.**

```
feat(nova): add WLFG neural frame synthesis (pass 23)
```

---

### Task 9: WLRR — make the incumbent denoiser measurable

**Objective:** Before replacing a denoiser, measure it. **This task produces no shader change.**

**Files:**
- Create: `tools/verify_wlrr_incumbent.py`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` (a diagnostic flag only)

**Step 1 — the harness.**

Render a scene with SVGF **on** and with the reference (accumulation on, scale 1.0, converged), at a
pinned camera, and measure PSNR/SSIM of the incumbent against the reference. Use the exporter's
existing capture machinery.

**Step 2 — the finding.**

Write it up. It is entirely possible the incumbent is already good, in which case WLRR is a smaller
feature than planned — **that is a legitimate outcome and it changes task 10's scope.** Report it
rather than proceeding on the assumption.

**Step 3 — commit.**

```
docs(verify): measure the incumbent SVGF denoiser against a converged reference
```

---

### Task 10: WLRR — residual-correcting denoiser (passes 20/21)

**Objective:** Add a trained residual corrector on top of the incumbent, gated so a wrong prediction
costs nothing.

**Files:**
- Create: `tools/train_wlrr/{model,dataset,train,export_weights,test_model}.py`
- Modify: `.glsl` (new `RunWlrrDenoise`, `RunWlrrComposite`), `.hlsl`, `Renderer3D.cpp`
- Modify: `Wasteland/src/Wasteland/Renderer/WlrrStageBlob.h` (new)

**Step 1 — the dataset.**

§2.4: input = single-frame noisy, accumulation off, scale 0.5; target = converged, accumulation on,
scale 1.0. This target has **+2.042 dB** of affine headroom against **+0.084 dB** for the converged
target — the 24x difference that makes this the right target.

**Step 2 — the kernel family.**

An A-Trous wavelet family whose per-iteration blend weight is **variance-driven** (the existing
pass 13's ladder is a fixed 1/2/4/8 schedule, `docs/NOVA_DENOISER_WIRING.md`), plus the trained head
that corrects the residual.

**Step 3 — the gate.**

`out = incumbent + gate * residual`. Same form as task 8, same reason.

**Step 4 — verify + screenshot.**

`tools/verify_wlrr_kernel.py`: transcribe the wavelet kernel, check the weights, check the
variance derivation `max(m2/n - m1^2, 0)` (the `max` is load-bearing — a negative variance under
`sqrt` is NaN, which poisons every weight in the kernel at once). Negative control: omit the `max`
and assert FAIL.

**Step 5 — the PSNR gate.**

Enhanced PSNR > incumbent PSNR > raw-input PSNR, on a held-out split, **reported in the commit
message**. If the chain does not hold, stop.

**Step 6 — commit.**

```
feat(nova): add WLRR residual-correcting denoiser (passes 20, 21)
```

---

### Task 11: Tier plumbing and the editor surface

**Objective:** Make all four features controllable, inspectable, and tier-gated.

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/RenderCapabilities.h`, `.cpp`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.h`, `.cpp`
- Modify: `DemonCore-Editor/src/EditorLayer.cpp`
- Modify: `Wasteland/src/Wasteland/Scripting/Bindings.cpp`
- Modify: `DemonCore-Editor/assets/scenes/*.wastescene` (preset fields)
- Modify: `tools/verify_render_capabilities.py`

**Step 1 — the tiers.**

§2.3. Replace `denoiseComputeCapable`'s "only ever resolves for a future component" comment with the
real resolver. Add `ReconstructSpatial`, `ReconstructTemporal`, `ReconstructFull`.
`TierFor()` resolves each; `TierName()` names each.

**Step 2 — the editor surface.**

A "Clean-Room Reconstruction" section in the world panel: four enable checkboxes (WLSR / WLAA / WLRR /
WLFG), a factor slider (1x-6x) whose maximum is **greyed out** when the accumulation state does not
permit it, a tier readout, and a **gate readout** for each trained head. Without the readouts the user
cannot tell "no enhancement" from "broken network".

**Step 3 — persist to the scene YAML.**

Follow the existing `RenderScale` round-trip at `EditorLayer.cpp:2818-2838,2871` and the
`NeuralStage` round-trip at `:2933,3337`. A missing key means default-off.

**Step 4 — Python bindings.**

Follow the pattern at `Bindings.cpp:3640-3655` and `Renderer3D_IsNeuralStageEnabled` /
`SetNeuralStageEnabled`. Add getters/setters for all four, the factor, and the gate means. Test with
`tests/headless/runner.py`.

**Step 5 — update `tools/verify_render_capabilities.py`.**

It asserts `computeRaytracing` per API and that capabilities describe *wired* paths. The new tiers
must be asserted the same way — a tier that resolves to a pass nothing dispatches is a phantom.

**Step 6 — verify + screenshot.**

```bash
python3 tools/verify_render_capabilities.py
cd tests/headless && "C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe" runner.py
```

Capture the panel and one frame with each feature on and off.

**Step 7 — commit.**

```
feat(editor): expose the clean-room reconstruction stack in the editor and bindings
```

---

### Task 12: Documentation and evidence

**Files:**
- Create: `docs/CLEANROOM_RECONSTRUCTION.md`
- Create: `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/`
- Update: `docs/RTX_KIT_LICENSES.md`
- Update: `docs/NOVA_SHADER_PIPELINE.md` (the pass inventory, and which `case` numbers were repurposed)
- Update: `docs/NOVA_DENOISER_WIRING.md`
- Update: `docs/NOVA_FPS_DIAGNOSIS.md` §5.5 (the vendor-upscaler rejection bullet must explicitly exempt these)
- Update: `docs/NOVA_BINDING_BUDGET.md`

`docs/CLEANROOM_RECONSTRUCTION.md` must state plainly, in its first paragraph: what each feature is,
what each is **not** (not DLSS/FSR/XeSS, not bit-compatible, not RTX-only), the training-data
provenance, the four-backend verification status with what is actually verified vs
contract-verified, and **the precondition on the 6x claim**.

It must also state the interaction with the existing neural stage (passes 17/18/19): three
end-of-frame neural systems is two too many, and the doc must say which one wins when more than one
is enabled.

**Result:**

```bash
python3 tools/verify_nova_glsl.py
python3 tools/verify_nova_shader_parity.py
python3 tools/verify_neural_stage_parity.py
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/verify_reactive_mask_budget.py
python3 tools/verify_easu_kernel.py
python3 tools/verify_rcas_sharpen.py
python3 tools/verify_wlaa_contract.py
python3 tools/verify_wlfg_clock.py
python3 tools/verify_wlfg_warp.py
python3 tools/verify_wlrr_incumbent.py
python3 tools/verify_wlrr_kernel.py
python3 tools/verify_nova_constant_names.py
python3 tools/verify_vulkan_nova_contract.py
```

**Every one of these must be green, with output in the commit message.**

**Step — commit.**

```
docs: document the clean-room reconstruction stack and its evidence
```

---

## 4. Files likely to change

**New**
```
tools/train_wlrr/{model,dataset,train,export_weights,test_model}.py
tools/train_wlfg/{model,dataset,train,export_weights,test_model}.py
tools/verify_reactive_mask_budget.py
tools/verify_easu_kernel.py
tools/verify_rcas_sharpen.py
tools/verify_wlaa_contract.py
tools/verify_wlfg_clock.py
tools/verify_wlfg_warp.py
tools/verify_wlrr_incumbent.py
tools/verify_wlrr_kernel.py
docs/CLEANROOM_RECONSTRUCTION.md
docs/CLEANROOM_RECONSTRUCTION_BINDING_PLAN.md
docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/
Wasteland/src/Wasteland/Renderer/WlrrStageBlob.h
Wasteland/src/Wasteland/Renderer/WlfgStageBlob.h
```

**Modified**
```
DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl
DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl
DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv        (regenerate, never hand-edit)
Wasteland/src/Wasteland/Renderer/Renderer3D.cpp
Wasteland/src/Wasteland/Renderer/Renderer3D.h
Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp
Wasteland/src/Wasteland/Renderer/RenderCapabilities.h
Wasteland/src/Wasteland/Core/Application.cpp
Wasteland/src/Wasteland/Core/Application.h
Wasteland/src/Wasteland/Scripting/Bindings.cpp
Wasteland/src/Platform/NVRHI/VulkanNova.h
Wasteland/src/Platform/NVRHI/VulkanNova.cpp
Wasteland/src/Platform/NVRHI/DX12Nova.h
Wasteland/src/Platform/NVRHI/DX12Nova.cpp
Wasteland/src/Platform/NVRHI/DX11Nova.h
Wasteland/src/Platform/Windows/WindowsWindow.cpp
DemonCore-Editor/src/EditorLayer.cpp
tools/verify_nova_binding_budget.py
tools/verify_render_capabilities.py
tools/bench_nova_phase.py
docs/NOVA_DENOISER_WIRING.md
docs/NOVA_SHADER_PIPELINE.md
docs/NOVA_FPS_DIAGNOSIS.md
docs/NOVA_BINDING_BUDGET.md
docs/RTX_KIT_LICENSES.md
```

**Do not touch**
- `Wasteland/vendor/**` — the licence boundary is `docs/RTX_KIT_LICENSES.md`; no vendor SDK enters the tree.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` — a build artefact of
  `tools/compile_nova_spirv.py`; regenerate it, never hand-edit it.
- The existing neural stage (passes 17/18/19) — out of scope except for the interaction note in task 12.

---

## 5. Tests / validation

**Per task, always:**
```bash
python3 tools/verify_nova_glsl.py                        # GLSL compiles the way the engine uses it
python3 tools/verify_nova_shader_parity.py               # twins agree
python3 tools/verify_nova_binding_budget.py              # no register collision
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/verify_reactive_mask_budget.py             # the mask's channel is free when read
python3 tools/verify_nova_constant_names.py              # every pushed constant is declared
python3 tools/verify_vulkan_nova_contract.py             # Vulkan by contract
python3 tools/compile_nova_spirv.py --check              # the HLSL compiles and the bindings agree
```

**Feature-specific, each with negative controls:**
```bash
python3 tools/verify_easu_kernel.py
python3 tools/verify_rcas_sharpen.py
python3 tools/verify_wlaa_contract.py
python3 tools/verify_wlfg_clock.py
python3 tools/verify_wlfg_warp.py
python3 tools/verify_wlrr_incumbent.py
python3 tools/verify_wlrr_kernel.py
```

**Build (never while the user may be building; `BUILDING.md`):**
```bash
bash tools/msb.sh C:/dev/Wasteland/DemonCore-Editor/DemonCore-Editor.vcxproj -p:BuildProjectReferences=false
```
`-m:1` and `/FS` are mandatory. **A C++ change needs `DemonCore-Editor.vcxproj`** — `WastelandPython.vcxproj`
rebuilds only `Wasteland.pyd` and leaves the bench harness's `DemonCore-Editor.exe` stale, which has
already invalidated a render measurement in this repo.

**Cost gate (the only trustworthy perf measure is `app`, with `WASTELAND_NOVA_PROFILE` off):**
```bash
python3 tools/bench_nova_phase.py --counts 10000 --presets 3 --frames 300
```
A row whose `gpu` reads 0.0 is INVALID, not fast. Pin `WL_DRS_OFF=1` for every A/B, or the scaler
confounds the measurement.

**Visual (the real acceptance — this user does not accept a clean log):**
```bash
WL_SCREENSHOT=out.png WL_SCREENSHOT_FRAME=120 WASTELAND_PIN_CAMERA="0,0,0,9,-85,0" ./DemonCore-Editor.exe
```
Required captures per feature: static-camera convergence, moving-camera ghosting, structure
preservation, feature-on vs feature-off, at the target resolution and preset, on the backend that
actually renders here (OpenGL).

**Model quality gate (tasks 8, 10):** enhanced PSNR > control PSNR on a held-out split, reported in
the commit message, with the dataset's sha256.

---

## 6. Risks, tradeoffs, open questions

**Risks**

1. **The GL unit-7 rebind is a regression risk, not a feasibility risk.** The mechanism is
   precedent-backed (`Renderer3D.cpp:5225,5238` rebinds unit 6 mid-frame) and the limit is a
   declaration-count limit (`verify_nova_glsl.py:213`), so no new declaration is needed. What can
   still go wrong is borrowing a unit whose texture is *not* actually dead for the new dispatch's
   duration, which disturbs the bloom chain silently. Mitigation: the reactive-mask verifier proves
   the deadness claim statically, and the task-1 step-2 screenshot A/B is the runtime confirmation.
2. **Task 6 changes the frame loop.** A `SimTimescale` and a synthetic frame cap touch every layer's
   `OnUpdate` and every timing assumption in the editor. Mitigation: the cap and the timescale both
   default to their current behaviour (no cap, timescale 1.0) and are inert unless set.
3. **Task 7 changes a live, working path.** `CommitHistory` and the single `s_Accumulation` at `t48`
   are what the temporal accumulation depends on. A second history buffer is a real change to
   something that works. Mitigation: the control run must be bit-identical.
4. **Task 9 may find the incumbent denoiser is already good**, which shrinks task 10. That is a
   finding, not a failure — but it must be reported rather than worked around.
5. **Three end-of-frame neural systems is two too many.** The existing stage (17/18/19), the WLRR
   head and the WLFG head all sit at the end of the frame. Task 12 must state which wins, and the
   editor must make the interaction visible.
6. **The dataset-staleness trap has already cost this project once** (a model trained on a
   pre-shader-change dataset scored −2.9 dB on a fresh capture). Every training task re-exports and
   records the sha256.
7. **Vulkan will not be visually verified.** It is compiled but never instantiated. Contract
   verification only, and the docs must say so.
8. **The GLSL `#include` problem blocks any shared-source shortcut.** `OpenGLShader::PreProcess`
   expands no includes, so a shared header would compile only where the driver happens to implement
   `GL_GOOGLE_include_directive` — and `verify_nova_glsl.py` copies the include into its temp dir and
   **still passes**. Keep the twins separate.

**Tradeoffs**

- **Repurposing dead passes 8/15/16 over adding new indices.** Costs a reader the old meaning; buys a
  readable switch. Accepted.
- **Residual + gate for every trained head, everywhere.** Costs one extra buffer and one multiply;
  buys "wrong network = the incumbent's image" instead of "wrong network = a wrong image". Accepted
  without reservation.
- **Training data from the engine, not from the internet.** Slower and smaller in variety; but it is
  the clean-room requirement and it matches the engine's own shading. Accepted.
- **fp32 in v1.** ~2x the weight-upload bandwidth; keeps the model source-identical across all four
  backends. Accepted; fp16 is a v2 tier.
- **A 6x ceiling gated on the accumulation state, rather than an unconditional 6x.** Costs the
  headline number; buys a claim that is true. Accepted — a 6x that multiplies noise by 6 is not a 6x.

**Open questions**

1. Does the GL unit-7 rebind disturb the bloom chain? Task 1 step 2's screenshot A/B answers it.
   **Downgraded from blocking** — the mechanism is precedent-backed and the limit is a declaration
   count, so the question is regression, not feasibility.
2. Is the incumbent SVGF denoiser already good enough that WLRR is a smaller feature than planned?
   Task 9 answers it; task 10's scope depends on the answer.
3. Does WLAA actually beat TAA on p99 contrast at `RenderScale = 1.0`? Task 5's A/B answers it, and
   a negative result is a legitimate outcome.
4. Should WLFG's neural head be a separate network from the WLRR head, or one shared network with a
   mode input? Decide from the task-8 and task-10 measurements, not from the architecture diagram.
5. Is there a real `ReconstructSpatial` tier distinct from `ComputeEmulated` on OpenGL, or does the
   spatial tier collapse into it? Decide from measurement.
6. What happens when WLSR and WLFG are both enabled and both want the `u13` scratch image? §1.2.3
   says share it; the scheduling order in §2.1 is the proposal, and it needs a measurement.
