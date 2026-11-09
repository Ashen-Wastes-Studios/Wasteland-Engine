# Clean-Room Reconstruction Stack — WLSR / WLAA / WLRR / WLFG

**Status:** implemented and verified at the contract level; runtime-verified on Vulkan.
**Source of truth:** `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.slang`, one Slang
module compiled to all four backends.

---

## 1. What each feature is — and what each is **not**

These are **clean-room re-expressions** of the *problems* DLSS Super Resolution, DLAA, Ray
Reconstruction and AI Frame Generation solve. They are **not** DLSS, FSR, XeSS or any other
vendor feature. They are **not bit-compatible** with any of them, they do **not** use any vendor
SDK, and they are **not RTX-only** — no tier is gated on a GPU vendor or on tensor cores.

| feature | what it is here | what it is **not** |
|---|---|---|
| **WLSR** | EASU-style edge-adaptive upscale (pass 15's kernel family) + an RCAS-style sharpen driven by local contrast, clamped per pixel. A *pair* — the editor toggle sets and clears both halves together. | Not FSR 1.x. No temporal component, no motion vectors, no vendor kernel. |
| **WLAA** | The engine's existing temporal accumulation, made reachable at **native resolution** with a sharpen stage that pays for the filter's blur. The deliverable is the switch plus the sharpen, not a new accumulator. | Not DLAA. Same TAA formulation this engine already had (jitter, velocity reprojection, YCoCg variance clipping). |
| **WLRR** | A **residual-correcting trained head** on top of the incumbent SVGF denoiser: pass 20 snapshots the pre-SVGF accumulation, pass 21 corrects the denoiser's output toward the converged frame, gated per pixel by a measured blend α. Additive — a wrong head degrades to the incumbent, not to a worse image. | Not a replacement for the denoiser. Not "Ray Reconstruction": the incumbent is not removed and no RTX pipeline is involved. |
| **WLFG** | Three systems: (a) a per-present reprojection warp using the velocity buffer the tracer already writes (pass 22), (b) a `SimTimescale` clock that divides game time by the interpolation factor, (c) a neural synthesis head that fills disocclusions (pass 23). Only (c) is "AI" in the marketing sense and it is the smallest part. | Not a shader trick and not a dropped-frame hack. The factor is a function of accumulation state, not a user slider with a fixed maximum. |

### The 6x claim, and its precondition

Wasteland is a path tracer with temporal accumulation — its convergence *is* its quality.
Re-projecting a converged frame at 6x multiplies the effective sample count by 6; re-projecting a
1-sample frame multiplies noise by 6. The interpolation factor is therefore a function of the
accumulation state:

| accumulation state | max WLFG factor | rationale |
|---|---|---|
| converged (α at its 0.02 floor, static camera) | **6x** | history is a real converged image; warping it is nearly free |
| partially converged | 2x | noise would otherwise multiply |
| camera moved / scene changed (`u_CameraMoved`) | **off** | reprojection is undefined |
| any non-Nova backend | off | the raster path has no velocity buffer |

**A 6x claim that does not state this precondition is not a 6x claim.** The editor exposes the
ceiling (`GetWlfgMaxFactor()`), not a fixed range; the slider greys out when the state does not
permit generation.

---

## 2. Training-data provenance

No external dataset exists and none is fetched. `tools/train_neural/export_dataset.py` drives the
engine binary headlessly and produces matched pairs.

| feature | input arm | target arm | why |
|---|---|---|---|
| **WLRR head** | single-frame noisy, accumulation **off** | converged, accumulation **on** | the converged pass has +0.084 dB of affine headroom and the single-frame pass +2.042 dB — 24x more learnable signal (measured, `docs/NEURAL_RENDERING.md` §2) |
| **WLSR RCAS** | trace-resolution image | full-resolution image | sharpening is a per-pixel correction with no temporal component |
| **WLFG head** | two warped frames at factor N | the real frame at that N | N is held out from training |

**The WLRR dataset is produced from ONE denoise-on run with the zero blob**: image 10 is the
pre-SVGF input (via the pass-20 snapshot) and image 1 is the post-SVGF incumbent. Two separate
runs would accumulate different temporal chains and the arms would not be a matched pair.

**Every training task re-exports its dataset with the current binary and records the dataset's
sha256.** The first shipped neural model in this repo was trained on a dataset captured before a
shader change and had learned a 5.33-luma lift the renderer no longer produced; on a fresh capture
it scored 37.040 → 34.127 dB — *worse than doing nothing* (`docs/NEURAL_RENDERING.md` §3).

### The feature-domain contract

The capture readback produces the training domain as `clamp(v,0,1) * 255`. The shader must compute
in that domain: it scales the arms by 255 at load and divides the store by 255.

This is not optional and it is **invisible to an identity control**: with zero weights the residual
is zero for *any* feature scale, and the blend α is domain-independent. A residual gate proves the
write path, never the feature domain. A training/inference domain mismatch is only detectable by
comparing the runtime correction against the model's.

---

## 3. Four-backend verification status

One authored shader, four backends, no `#ifdef` per vendor. Tiers are resolved by
`RenderCapabilities::TierFor(probe)`, which degrades a probe to the best level the backend runs.
The resolver keys on **the graphics API**, never on `tensorCores` (declared and uploaded, read by
nothing, and it must stay unused).

| tier | backend | what runs |
|---|---|---|
| **ReconstructSpatial** | OpenGL, DX11 | WLSR (EASU + RCAS), WLAA sharpen, reactive mask |
| **ReconstructTemporal** | DX11, DX12 | + WLRR full |
| **ReconstructFull** | DX12, Vulkan | + WLFG with the neural synthesis head |
| **Unavailable** | any | analytic bilinear, no new passes |

**OpenGL's entry still reports `computeRaytracing: false`** — a stale "not yet validated" note.
The resolver deliberately does not gate on that flag: Vulkan is the backend WLRR/WLFG actually run
on, and gating on it would exclude the only working path.

### Verified vs contract-verified

| backend | status | evidence |
|---|---|---|
| **Vulkan** | **runtime-verified** | the WLRR head beats the incumbent on **10 of 11** scenes (Example3D +1.919 dB, PhysicsPlayground +2.460, AtmosphereShowcase +1.855, ExampleGame +1.202, InventoryDemo +1.352, ScriptTest +0.792, TerrainShowcase +0.274, LightingShowcase +0.222, MaterialGallery +0.199, PrimitiveGallery +0.155; WaterShowcase −0.038, whose incumbent is already within 0.03 dB of converged). Runtime↔model parity within 2% (0.356 vs 0.362 LSB). Identity control 0.000026 against a noise floor of 0.000058. |
| **DX12** | **contract-verified** | the DXIL contains **zero** WLRR symbols — the WLRR resources (`t76`/`t77`/`u15`) and `case 20/21` are guarded `#if !defined(WASTELAND_DXIL) && !defined(WASTELAND_GLSL)`. `kUavCount` stays 15: bumping it without a `u15` in the DXIL causes `CreateComputePipelineState` to fail `E_INVALIDARG` (0x80070057). |
| **DX11** | **contract-verified** | loads the same DXIL as DX12; the no-op `CommitWlrrSnapshot()` stub is present. |
| **OpenGL** | **contract-verified** | the GL path dispatches the neural stage inline (passes 17/18/19) but does **not** dispatch WLRR or WLFG; those are Nova-schedule passes. |

### Pass inventory

| pass | feature | resolution | backend guard |
|---|---|---|---|
| 15 | WLSR RCAS sharpen | full | all |
| 16 | WLAA sharpen (native-res AA) | full | all |
| 17/19 | neural reduce + final | — | **GL inline only** |
| 18 | neural eval | — | **GL inline only** |
| 20 | WLRR snapshot (pre-SVGF) | trace | **not DXIL, not GLSL** |
| 21 | WLRR residual correct | trace | **not DXIL, not GLSL** |
| 22 | WLFG reprojection warp | full | all |
| 23 | WLFG neural synthesis head | full | all |
| 24 | reactive mask | trace | all |

`pass(p)` / `pass(p,false)` run at trace resolution; `pass(p,true)` at full resolution.

**A register added to the shared `.slang` must be covered by every target's root signature, or
compiled out of the targets that cannot provide it.** This trap has fired three times in this
codebase (`t75`, then the visibility block, then `kUavCount`), each time as
`CreateComputePipelineState` → `E_INVALIDARG` → raster fallback → the later Vulkan switch losing
the device.

---

## 4. Interaction with the existing neural stage

Three end-of-frame neural systems is two too many. The resolution in this codebase:

- **The neural stage (passes 17/18/19) is dispatched only in the OpenGL inline path**
  (`glDispatchCompute` in `Renderer3D::Flush()`), never through the Nova schedule.
- **WLRR (20/21) and WLFG (22/23) are Nova-schedule passes.** They cannot coexist with the neural
  stage on the same backend today, because the GL path does not run the Nova schedule.
- **When both WLRR and WLFG are enabled, WLRR runs first and WLFG consumes its output.** Pass 21
  corrects the denoiser's output; pass 22 then warps the *corrected* frame rather than the raw
  denoise. They compose — WLRR does not yield to WLFG.
- Each is gated on its own flag and each is **off by default**: every one is a change to the final
  image, so it is opt-in.

The editor's four checkboxes map to real schedule branches. The tier readout and the two head
availability readouts are read-only: without them a user cannot tell "no enhancement" from "the
head never loaded".

---

## 5. Verification chain

```
python tools/verify_nova_glsl.py                # GLSL target compiles, zero Vulkan shifts
python tools/verify_nova_slang_compile.py      # all four targets compile from one source
python tools/verify_nova_binding_budget.py     # per-target #if evaluation
python tools/verify_nova_constant_names.py     # cbuffer field contract
python tools/verify_vulkan_nova_contract.py    # Vulkan slot tables
python tools/verify_render_capabilities.py     # tiers: declared, named, resolved, pass-backed
python tools/verify_wlfg_warp.py               # pass 22 warp
python tools/verify_wlfg_frame_generation.py   # factor gating
python tools/verify_wlfg_clock.py              # sim clock
python tools/verify_wlrr_kernel.py             # pass 20/21 kernel
python tools/verify_wlrr_incumbent.py          # the incumbent is measurable
python tools/verify_rcas_sharpen.py            # RCAS stage
python tools/verify_wlaa_contract.py           # native-res AA contract
python tools/verify_reactive_mask_budget.py    # pass 24 mask budget
python tools/verify_neural_stage_parity.py     # neural stage GL parity
```

**Every gate must be green, with output in the commit message.**

### Measured status — 2026-11-08

```
verify_nova_glsl                PASS
verify_nova_slang_compile       PASS
verify_nova_binding_budget      PASS
verify_nova_constant_names      PASS  (repointed at the .slang; the .hlsl twin is dead)
verify_vulkan_nova_contract     PASS  (expectations updated to the refactored bodies)
verify_render_capabilities      PASS
verify_wlfg_warp                PASS
verify_wlfg_frame_generation    PASS
verify_wlfg_clock               PASS
verify_wlrr_kernel              PASS
verify_wlrr_incumbent           PASS  (runtime; intermittent Vulkan device-lost)
verify_rcas_sharpen             PASS
verify_wlaa_contract            PASS
verify_reactive_mask_budget     PASS
verify_neural_stage_parity      PASS
```

`verify_nova_constant_names` compares the CPU's `n.Set(...)` names against the declarations in
the shader. It originally read `Renderer3D_NovaRenderer.hlsl` — the **pre-Slang twin**, which is
in no `.vcxproj` and is not compiled by `tools/compile_nova_slang.py` (which compiles only the
`.slang`). The verifier has been repointed at the shipped source and now passes: all 83 pushed
constants are declared.

`verify_vulkan_nova_contract` had drifted from the code in four ways and now passes. Its
expectations were written before `FlushVulkanNova` was refactored to resolve the instance through
`Renderer3D::GetNovaVulkan()` and bind it to a reference (`auto& n = *nova;`), before
`ClearAccumulationBuffers` switched to `nvk->Clear();`, before the uniform names were qualified
(`NovaConstants_0.u_InstanceCount_0`), and before WLRR added `t77` (`read_img_WlrrInput`) — which
grew the read-image table from 8 to 9 entries
(`{64,65,66,68,69,70,71,73,77}` / `{0,1,2,4,5,6,7,8,10}`). One of its negative controls also
assumed `CreateImages` set `m_ImagesNeedClear = true;` exactly once; the WLFG history texture added
a second occurrence, so the control replaced only the first and reported a false failure. It now
replaces every occurrence, and all 13 negative controls pass.

### Structural rules the verifiers enforce

- **A tier must be backed by a dispatched pass.** `verify_render_capabilities.py` cross-checks
  `Renderer3D.cpp` for the pass IDs each tier implies (15/24, 20/21, 22/23). A tier that names a
  level nothing dispatches is a phantom.
- **No tier may be gated on `tensorCores`.** The verifier rejects it.
- **A feature given to a shared trunk is not conditioned on the gate.** The gate is a signed
  difference applied per pixel, never a branch that bypasses the trunk.
- **A zeroed-blob identity control is the only proof a wired pass is a no-op.** If the zeroed blob
  changes the frame, no PSNR number from it means anything.
- **Byte-identity is the wrong identity test for captures** — two captures of the same
  configuration differ by Monte Carlo sampling noise. Use a tolerance-based test with an A2
  baseline.
- **A stale deployed DXIL is a device-lost generator.** If the deployed
  `Renderer3D_NovaRenderer.cs.cso` is not the one the current `.slang` produces, DX12 init fails
  `E_INVALIDARG` → raster fallback → the later Vulkan switch loses the device. Re-deploy with
  `tools/compile_nova_slang.py --deploy` before blaming the driver.

---

## 6. Evidence index

| artifact | contents |
|---|---|
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task2/` | reactive mask design and budget |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task3/` | EASU kernel |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task4/` | RCAS sharpen |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task5/` | WLAA contract |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task6/` | frame pacing and clock |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task7/` | reprojection warp |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task8/` | neural synthesis head |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task9/` | incumbent measurability |
| `docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task10/` | residual-correcting head, runtime table |
| `docs/CLEANROOM_RECONSTRUCTION_BINDING_PLAN.md` | register and pass budget |

---

## 7. License posture

No NVIDIA RTX SDK is vendored. The only NVIDIA-derived code in this repository is MIT-licensed
(RTXMU, Donut). NRD, RTXTF, STBN, OMM and RTXDI were **removed on 2026-10-04** and are
re-implemented clean-room; they must never be re-vendored. See `docs/RTX_KIT_LICENSES.md`.
