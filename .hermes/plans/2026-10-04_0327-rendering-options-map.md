# Implementation Options Map — Wasteland cross-backend rendering

**Companion to** `.hermes/plans/2026-10-03_213029-rtx-kit-cross-backend.md`. That plan says *what* to build. This document maps *how* — 2–4 concrete options per component, each grounded in verified repo evidence, with what each option forecloses.

**How this was produced.** Six parallel agents, one per component group, each instructed to verify the plan's claims against the code rather than restate them, and to file a "Corrections to the plan" section wherever the plan was wrong. **Every correction in this document was independently re-verified by the parent** — the load-bearing ones by direct inspection, quoted below with `path:line`.

**Evidence convention.** Claims are labelled **MEASURED** (reproduced on this machine, with the method stated) or **ESTIMATE** (engineering judgement). Nothing is asserted outside those two categories.

**Reading order.** §5.2 → §5.10 → §5.3 → §5.4 → §5.4b → §5.5 → §5.6 → §5.7 → §5.9 → §5.8. Not numerical order — it is dependency order, and it follows what each investigation found.

---

## Corrections to the plan that change decisions

These are not cosmetic. Each one alters scope, sequencing, or effort.

### 1. The blue-noise fix is not actually live — §5.1 is marked DONE but the defect still ships

**The single most important finding in this document.** The plan's §7 progress table lists blue noise as `DONE` and §5.1 describes replacing `Halton(index, 2) % 16` with generated blue noise. The C++ half is done. **The shader never reads it.**

Verified:

```
Renderer3D_NovaRenderer.hlsl:176:    float2  u_Jitter;      <- declared
Renderer3D_NovaRenderer.glsl:231:  uniform vec2 u_Jitter;   <- declared
```

Two declarations. **Zero reads.** Meanwhile both shaders compute the jitter themselves, on the GPU:

```
Renderer3D_NovaRenderer.hlsl:1896:    float x = Halton(effectiveIndex % 16 + 1, 2);
Renderer3D_NovaRenderer.hlsl:1897:    float y = Halton(effectiveIndex % 16 + 1, 3);
Renderer3D_NovaRenderer.glsl:1825:    float x = Halton(effectiveIndex % 16 + 1, 2);
Renderer3D_NovaRenderer.glsl:1826:    float y = Halton(effectiveIndex % 16 + 1, 3);
```

So the exact defect §5.1 claims to have removed — *16 offsets then an exact repeat* — is still running on all four backends, and the 11000×-suppression asset is uploaded but unused.

**Fix:** read `u_Jitter` in `GetJitter()` instead of computing Halton. That is a few lines, not a task — but until it is done, **§5.1 must be marked PARTIAL, not DONE**, and the ~33 MB blue-noise volume is dead weight.

### 2. DXC is present on this machine, but neither local copy compiles ray tracing — §5.3's premise is wrong in both directions

Plan §5.3 asserts DXC is absent and vendors it as a prerequisite. **MEASURED: four copies exist.** But neither usable one supports RT:

- `TraceRay` undeclared under `lib_6_3`…`lib_6_8`
- `RayQuery` reports a pre-1.7 class-template shape
- `RayQueryInitialize` absent from the DLL symbol table
- a `WaveActiveSum` `cs_6_0` control **compiles fine** (so the failure is RT-specific, not a broken toolchain)

The two copies also disagree on SPIR-V: the VulkanSDK 1.8.0.4739 build has it, the Windows Kits 1.8.2502.11 build does not.

**Net effect: harder, not easier.** Vendoring a current DXC is a hard prerequisite that the plan's 10–20 day estimate excludes. **Mitigating:** `premake5.lua:103` already has `C:/VulkanSDK/1.3.296.0/Lib` in `libdirs`, so a drop-in needs only `"dxcompiler.lib"` added to the Windows `links` block — two of the plan's three premake steps are unnecessary.

### 3. "BLAS per mesh" is not implementable as written — there are no per-mesh vertex arrays

Plan §5.3 step 3 says "BLAS per mesh, one TLAS." **There is no per-mesh vertex array in the engine** — only two dynamic batches (`Renderer3D.cpp:931`, `:954`). Vertices are written **world-space** (`:2519`), so a TLAS refit cannot express per-instance motion.

The ProceduralMeshes situation is the opposite of what the plan's open question asks. They *do* produce real triangles and *are* uploaded — but the tracer never uses them. `DrawProceduralPrimitive` rasterises them, then separately pushes a **bounds-only** `RayTracingInstance` (`Renderer3D.cpp:2542-2556`), and the shader branches to an **analytic** routine on `MaterialParams.z`. They are excluded **by construction, not by limitation**. Feeding them to a BLAS is a trace-model change, not a plumbing fix.

Also: `sizeof(Vertex3D) == 76` (**MEASURED** — glm's unaligned `vec3`, not 96). That is an inefficient DXR stride.

### 4. The ReSTIR schedule risk does not exist — the material system is a flat POD, not a shader graph

Plan §4 names the emissive-triangle light buffer as **the** structural risk: *"our material system is shader-graph-style, so emissive contribution often cannot be statically analysed… it decides whether §5.4 is 15 days or 40."*

**The premise is false.** Verified:

```
Wasteland/src/Wasteland/Scene/Components.h:668:  glm::vec4 EmissionColor = {1.0f,1.0f,1.0f,1.0f};
Wasteland/src/Wasteland/Scene/Components.h:677:  float EmissionIntensity = 0.0f;
```

`grep -rl 'ShaderGraph|shadergraph|MaterialGraph|NodeMaterial'` over `Wasteland/src` + `DemonCore-Editor` → **0 files**. The shader consumes it in one line (`.glsl:2586`): `info.emission = inst.Emission.xyz * inst.Emission.w;` — the only emission assignment in 2,682 lines.

**And the light list is already built and uploaded on all three backends:**

```
Renderer3D.cpp:2288:  if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
Renderer3D.cpp:2494:  if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
```

So §5.4b is roughly a day of work, not a 40-day investigation. **Revised §5.4 estimate: 12–18 person-days flat (ESTIMATE).**

Note the path is `Scene/Components.h`, not `Wasteland/Components.h` as the plan implies.

### 5. Open question #2 is closed, and open question #3 has a live bug attached

**Open question #2** — is `vendor/nvrhi/build/%{cfg.buildcfg}` populated? **Yes.** NVRHI RT is genuinely built: `d3d12-raytracing.cpp` and `vulkan-raytracing.cpp` are in the vcxproj source lists, `.obj` present in both Debug and Release, with 327 / 211 `AccelStruct` symbols in the libs respectively. No NVRHI rebuild is needed.

**Open question #3** — what happens past `MaxAnalyticLights = 8` (plan says `:209`; actual is **`:298`**)? There is a **live silent-drop bug**: `PushAnalyticLight` (`Renderer3D.cpp:2849-2854`) is order-dependent, so **8 point lights starve every spot and area light** in the scene. That is a 2-day cull fix, unrelated to reservoirs — and it should be fixed regardless of whether ReSTIR ever lands.

### 6. §5.6's stated risk is inverted, and its premise is false

Plan §5.6 says backend divergence in footprint semantics is the risk. But **`ComputeTextureFootprint` does not exist in NVRHI at all** (zero grep hits). The divergence risk is therefore *lower* than stated — a shared shader function removes it.

Worse, the premise: **no BC or compressed format exists anywhere in the engine** (only inside vendored Box3D/sokol). §3's tier-table row "Texture compression | TierC (BC via NVRHI)" is false, and footprint selection's entire justification — a compressed texel block larger than a pixel — has no current subject.

§5.6 does surface a real live defect though: `clamp(mipLevel, 0, 4)` at `.glsl:1744` makes mips 5–11 **unreachable** while `ComputeMipLevels` still allocates them (~33 MB dead VRAM, ESTIMATE).

### 7. §5.7 should not be built at all

**MEASURED: essentially zero alpha-tested geometry.** 0 `discard`/`clip` across all six 3D shaders (only 2D UI sprites), 0 alpha-cutout keys in `MaterialComponent` or any of 5 scenes, 38 opaque analytic primitives — and **no mesh assets at all**: `DemonCore-Editor/assets/` contains `ai graphs scenes scripts shaders textures trees`, no `meshes/`.

An opacity micromap accelerates ray traversal of alpha-tested triangles. There are none, and there are no triangles. **Recommend moving §5.7's 10–20 days to §5.3.**

The plan's NVRHI claims *are* correct: `OpacityMicromapFormat` (`nvrhi.h:1383`), `OpacityMicromapUsageCount` (`:1399`), `ResourceStates::OpacityMicromapWrite`/`BuildInput` (`:375`/`:376`) all exist. And the RTXMU/OMM conflict is real — **broader than the plan says**, since `d3d12-device.cpp:388-396` has it too, so it is both backends, not Vulkan-only. Currently moot: both flags are `OFF` in `CMakeCache.txt`.

### 8. §5.5's tier table row is wrong — there is no TierC probe system

Plan §3 claims OpenGL/DX11 already have "TierC (raymarch)" diffuse GI probes. **No probe grid, SH, or octahedral map exists anywhere.** The only thing resembling it is `wl_neural_ambient()` (`Renderer3D_Basic.glsl:616-634`), which self-describes as an occlusion-free stand-in.

Also: **no declared world extent exists**, so probe grids must be camera-anchored cascades (2 × 904–6 × 656 probes, vs ~125 M for a world grid at 2 u). And **`docs/ClusterLOD.md` does not exist** — plan §5.12 cites it as a source of LOD algorithms. `docs/` contains only `AGE_ASSURANCE.md`, `BUILD_ISOLATION.md`, `NVRHI_SETUP.md`, `RTX_KIT_LICENSES.md`.

### 9. Binding limits are tighter than the plan assumes

Consolidated from three sections. The plan treats register space as free:

| Backend | Constraint | Evidence |
|---|---|---|
| **DX11** | All **8** UAV slots bound (`u0`–`u7`); guaranteed limit is 8 | `DX11Nova.h:477`, `Renderer3D_NovaRenderer.hlsl` |
| **DX12** | `rootParams[8]` **exactly full** | `DX12Nova.cpp:128` |
| **OpenGL** | Only **image unit 3** free | `Renderer3D.cpp:1123-1126` |
| Vulkan | Free `t` slots: t47, t53–t63, t72+ | `compile_nova_spirv.py` shifts |

And a performance trap: `DX12Nova::Upload()` calls `WaitForGpuIdle()` (`:474`), which would stall **every frame** if a reservoir is uploaded through it.

### 10. §5.8's "1 person-day" is 1–3 days, and requires a network fetch

`CMakeCache.txt` says `NVRHI_WITH_RTXMU:BOOL=OFF`. Zero `rtxmu` strings in `nvrhi_vk.lib` / `nvrhi_d3d12.lib`, nothing in `_deps/`, no RTXMU tree in the repo. Enabling it means **git clone + 4-lib rebuild**, and it would introduce a build-time network dependency into an open-source engine — a supply-chain and reproducibility problem the plan does not mention.

Minor correction: `d3d12-raytracing.cpp:444+` is a **destructor**, not the build routing. The build routing is `:2039-2065`.

### 11. §5.9 is ~70% already built — but gated on a bug fix

Present today: velocity, a depth+normal G-buffer, a velocity-reprojecting accumulation pass with YCoCg variance clamping, a full-res composite with inline bilinear upscale, a working scaler, and a 1 MiB jitter volume. Missing: dilation, sharpening — and, per correction #1, the blue noise never arrives.

**A ~0.5–1 day fix is a prerequisite for every upscaler option**, because the jitter sequence they all build on is the broken one.

---

## Recommended order, revised

Re-sequenced against what the investigations actually found. Two of these are corrections to the plan's own ordering.

```
Step 0  FIX THE BLUE NOISE PLUMBING        0.5 d   <- §5.1 is PARTIAL until done
Step 1  Fix analytic-light silent drop     2 d    <- live bug, independent of RT
Step 2  Shader-pipeline probe (§5.10 D)     1 d    <- decide duplication policy
Step 3  SVGF option B (§5.2)               5-8 d   <- all four backends, no RT
Step 4  Fix mip clamp (§5.6)               0.5 d   <- 33 MB dead VRAM
Step 5  DXC + RT harness (§5.3)            10-20 d <- the gate
Step 6  Vulkan Nova wiring                        <- finishes TierC parity
Step 7  ReSTIR (§5.4 A+B, §5.4b A)      12-18 d   <- risk RETIRED, see #4
Step 8  DDGI (§5.5 C, or B after Step 5)
Step 9  Upscaler (§5.9 C)                 3-6 d
Step 10 OMM (§5.7) — NOT RECOMMENDED            <- see correction #7
         RTXMU (§5.8) — defer
```

**Why steps 0–4 come before hardware RT.** The investigations found that the three cheapest items in the whole programme are all bug fixes to code that already exists, and two of them (the jitter plumbing and the light silent-drop) are **live defects shipping right now**. None require RT, none require a DXC that works, and all three are prerequisites for work further down.

**Why §5.3's gate still holds.** Everything in §5.4–5.7 depends on it. But note that §5.7 is now recommended out of the programme entirely, which shrinks what sits behind the gate.

**Honest cost, revised down.** The plan's 55–100 person-days was estimated with a 40-day ReSTIR risk that **does not exist** (correction #4), and included a 10–20 day OMM that **should not be built** (correction #7). Removing those: roughly **30–60 person-days (ESTIMATE)** to reach the feature set that actually matters for this engine — TierC denoising on all four backends, hardware RT, ReSTIR, DDGI, and upscaling.

---

## What is in each section

| § | Component | Lines | Recommendation |
|---|---|---|---|
| 5.2 | SVGF temporal denoiser | 330 | **Option B** (fixed-iteration A-Trous + moments), with D as a 30-min probe |
| 5.10 | Shader pipeline | (above) | Accept status quo; probe Option D on one pass |
| 5.3 | Hardware ray tracing | 326 | **Option C** (RayQuery in existing SM6.0 compute), gated on a 1-day harness |
| 5.4 | ReSTIR DI | 944 | **Option A** then B; defer C until §5.3 |
| 5.4b | Emissive light buffer | (above) | **Option A now** — 0.5–1 day, already built |
| 5.5 | DDGI probes | 245 | **Option B**, or C if presets need GI before §5.3 |
| 5.6 | Filtered texturing | 490 | **Option A only** (ray-differential, 2–3 d) |
| 5.7 | Opacity micromap | (above) | **Do not build** |
| 5.9 | Temporal supersampling | 922 | **Option C**, gated on a 0.5–1 d fix |
| 5.8 | RTXMU | (above) | **Defer** |

Each section contains, per option: Approach / Files touched / Data layout with byte sizes / Backend notes / Effort and risks / What would prove it — then a Recommendation and its own Corrections subsection.

---

## Verification standard — unchanged

Per project convention: **rendering claims require 4K screenshots, not clean logs.** Both Release and Debug must pass. `docs/BUILD_ISOLATION.md` rules apply.

**Currently unmet, and not fixable by this work:** `Scene.cpp` fails to compile on `Navigation::` (in-flight refactor), and 41 `Renderer3D` symbols declared in `Renderer3D.h` have no implementation. The engine does not link, so **nothing in this document is verified running** — it is verified against the source, and in the cases marked MEASURED, verified by execution.

---

## §5.2 SVGF temporal denoiser

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A — Full SVGF** | Variance estimation + variance-driven A-Trous iteration count, new moment targets | All four | 10–15 d | Nothing; this is the plan's stated target |
| **B — Fixed-iteration A-Trous + temporal moments** | Moment targets and variance-driven step size, but a fixed 4-iteration ladder (the existing `{1,2,4,8}`) instead of per-pixel iteration count | All four | 5–8 d | Per-pixel adaptive iteration count — cannot recover the "stop early in converged regions" behaviour |
| **C — SVGF on DX12/Vulkan only, bilateral stays on GL/DX11** | Correct implementation where resources are plentiful; TierC keeps the existing blur | DX12, Vulkan | 6–9 d | Making the denoiser a *cross-backend* win — OpenGL keeps its worst-case quality |
| **D — Motion-vector-only temporal filter (no A-Trous)** | Reprojection + luminance-driven exponential blend; no wavelet stage | All four | 3–5 d | Multi-pass denoising; gives up edge-aware spatial filtering entirely |

### Option A — Full SVGF

#### Approach

Implement Schied et al. 2017 as written: reproject history with the previous view-projection, reject disocclusions by depth comparison, accumulate first and second luminance moments, derive per-pixel variance, then drive an A-Trous wavelet filter whose iteration count and step size come from that variance. High variance filters wider and iterates more; converged regions filter once and stop.

Critically, **this is less new code than the plan implies.** The engine already has the two hardest prerequisites:

- `u_PrevViewProjection` exists in **both** shader twins — `Renderer3D_NovaRenderer.hlsl:91` and `Renderer3D_NovaRenderer.glsl:66`.
- `u_InverseViewProjection` exists in both — `.hlsl:90`, used at `:1388` to reconstruct world position from clip space.
- `RunTemporalAccumulation()` already reprojects history with velocity and blends by a velocity-weighted alpha.

The new work is the **moment targets**, the variance derivation, and the variance-driven iteration. That is a substantially smaller delta than "8–15 person-days from scratch."

#### Files touched

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — new `RunTemporalVariance()` + `RunSVGFFilter()`
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — the twin
- `Wasteland/src/Platform/NVRHI/DX12Nova.cpp` / `DX11Nova.h` / `VulkanNova.cpp` — 2 new UAV targets, ping-pong
- `Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp` — replace the `nrdCapable` gate

#### Data layout

Two `RG32F` targets (mean, mean-of-squares) at render resolution — 8 bytes/px each, 16 bytes/px total. At 1920×1080 that is 33.2 MB for both, doubled for ping-pong = 66.3 MB.

**Binding collision check — MEASURED.** Simulating the existing register map (`Texture_UAV` base 0, `Texture_SRV` base 16, `ConstantBuffer` base 7, `Sampler` base 200) with the two new UAVs appended:

```
baseline                : 24 resources, 24 distinct, collisions=0
+ u8 img_Moments1       : 25 resources, 25 distinct, collisions=0
+ u9 img_Moments2       : 25 resources, 25 distinct, collisions=0
+ u10 rd_Moments        : 25 resources, 25 distinct, collisions=0
```

No collision. Register space is **not** the constraint for this feature.

#### Backend notes

Identical shader logic on all four. Differences are resource-creation only:
- **DX12**: two `ID3D12Resource` UAVs in a `DescriptorHeap`, ping-pong by copying or by two allocated sets.
- **DX11**: `ID3D11UnorderedAccessView` + `RWTexture2D<float2>`.
- **Vulkan**: two images + two `VkDescriptorSet` layouts; `VulkanNova.cpp:138` already asserts a 16-byte stride under both D3D and Vulkan for NovaConstants arrays — the same class of layout hazard applies here.
- **OpenGL**: `GL_RG32F` requires `GL_ARB_texture_float`; check availability at init, fall back to `GL_RG16F` (lower precision, acceptable for moments) if absent.

#### Effort and risks

**Risk: the UAV slot ceiling is the real constraint, and it binds on DX11.** The HLSL already occupies `register(u0)` through `register(u7)` — **all eight**, verified. DX11 compute guarantees only 8 UAVs, and while the device requests `D3D_FEATURE_LEVEL_11_1` first (`NVRHIContext.cpp:350-351`), which raises the guaranteed limit to 64, that is a *guarantee*, not a fact about the machine. On a DX11 GPU that only reaches 11_0, or on an SM5.0 path, **two more UAVs do not fit.**

Mitigation: pack mean and variance into **one** `RG32F` UAV rather than two (8 bytes/px, one slot), or fall back to an SRV-read/RT-written ping-pong pair that does not consume UAV slots. Option B is materially safer here.

Second risk, unchanged from the plan: **variance estimation fails silently.** A wrong variance produces a plausible-looking but wrong filter width. This is why the negative-control style test below matters more than a screenshot.

#### What would prove it

Standalone harness, no engine link required (precedent: `tools/generate_blue_noise.py` self-verifies). Feed a synthetic 1D signal, run variance estimation, assert three properties: (1) variance of a flat region → ~0; (2) variance of a true step edge → high; (3) estimated variance tracks analytic variance of the underlying distribution within 5%. Then the **negative control**: assert that a deliberately mis-seeded variance estimator *fails* this test — otherwise the test is not measuring anything, which is exactly the bug class that made the blue-noise ratio get inverted earlier.

### Option B — Fixed-iteration A-Trous + temporal moments

#### Approach

Same moment accumulation and variance derivation, but keep the wavelet iteration count fixed at 4 and let variance drive only the per-iteration step size and blend weight. Reuse the existing `{1,2,4,8}` ladder from `RunBilateralBlur()`.

#### Files touched

Identical to Option A. The difference is confined to the filter loop bound.

#### Data layout

As Option A — and **prefer the single-target packing** (one `RG32F`, 8 bytes/px, one UAV slot) to stay inside the DX11 ceiling described above.

#### Backend notes

Same as A. Lower register pressure, which is the entire point.

#### Effort and risks

Roughly half of Option A. Risk is the quality ceiling: without per-pixel iteration count, converged regions still pay full filtering and thin geometry gets over-blurred. That is a **known, bounded** failure mode rather than an unknown one, which makes this the right first cut.

#### What would prove it

Same variance harness, then a screenshot comparison at fixed camera/frame index: Option B vs the existing bilateral blur at 1 spp. The bar is Option B at 1 spp ≈ current bilateral at 4 spp, with no visible edge rounding at geometry silhouettes.

### Option C — DX12/Vulkan first, TierC keeps the bilateral blur

#### Approach

Implement SVGF on the two backends with resources to spare and leave GL/DX11 on the existing blur. Rationale: DX12 and Vulkan are where the tier model says quality should be highest anyway, and the DX11 UAV ceiling above makes the full implementation genuinely awkward there.

#### Files touched

As A, minus the DX11/Vulkan binding changes; `RenderCapabilities.cpp` keeps `denoiseComputeCapable` as a real gate for the first time in this engine's history.

#### Data layout

As A. No savings — the constraint is scheduling, not resources.

#### Backend notes

This is the *only* option that keeps a per-backend capability branch, which is the thing the tier architecture exists to avoid. It is defensible during bring-up and should be collapsed into A once the DX11 path is proven.

#### Effort and risks

Lowest risk per day of work. Highest long-term cost: it forfeits the plan's single biggest selling point, which is that hand-written SVGF reaches **OpenGL**, a backend NRD never supported.

#### What would prove it

Same as A. The extra proof obligation is a screenshot-diff showing DX12 SVGF does not regress versus the bilateral blur — a new denoiser that looks worse on the backend you *can* test is a common outcome.

### Option D — Motion-vector-only temporal filter

#### Approach

Reprojection, disocclusion rejection, and a luminance-difference-driven exponential blend. No A-Trous stage, no moments, no new UAVs.

#### Files touched

Nova shaders only. No `RenderCapabilities.cpp` change needed — it slots in beside `RunTemporalAccumulation()`.

#### Data layout

**Zero new resources.** This is the whole appeal: it reuses the existing history buffer and needs no UAV slot, which sidesteps the DX11 ceiling entirely.

#### Backend notes

Trivially portable — all four, with no binding-map changes at all.

#### Effort and risks

Lowest effort of the four, and the highest risk of *looking worse* than the bilateral blur, because it has no spatial filtering stage to clean up what temporal reuse cannot. Neon ringing around moving specular highlights is the expected failure.

#### What would prove it

A 30-minute experiment: run it at 1 spp, screenshot. If it does not visibly beat the bilateral blur at equal spp, abandon it and go to Option B, which is only marginally more work and has a strictly better failure mode.

### Recommendation

**Option B**, with Option D as a 30-minute probe first.

The reason is the UAV ceiling, which changes the arithmetic in the plan. All eight `u0`–`u7` slots are occupied, DX11's guaranteed limit is 8, and the 11_1 guarantee that raises it to 64 is a promise about the device rather than a measured property of this one. Option B's single-`RG32F` packing sidesteps that entirely at half the cost of Option A, and its failure mode (bounded over-blurring) is preferable to Option A's (silent variance error).

Run Option D first purely as a probe. If temporal-only beats the bilateral blur at 1 spp, the reprojection plumbing is sound and Option B is low-risk. If it does not, that is evidence the history path needs work before any A-Trous stage is layered on, which is worth knowing for 30 minutes.

Take Option A only if B's fixed iteration count proves visibly insufficient on high-contrast moving speculars.

### Corrections to the plan

Three, and the first two reduce the estimate rather than increase it.

**1. The plan says unjittered matrices "must be added." They already exist.** Plan §5.2 states reprojection needs unjittered view/projection matrices and that these must be carried in the denoiser input. Verified: `u_PrevViewProjection` is declared and used in **both** shader twins — `Renderer3D_NovaRenderer.hlsl:91` and `Renderer3D_NovaRenderer.glsl:66` — and `u_InverseViewProjection` sits alongside at `.hlsl:90`, consumed at `:1388`. The reprojection infrastructure SVGF needs is present in all four backends already. The plan's warning remains valid as a *correctness rule* (do not feed jittered matrices), but it is not a missing-plumbing task.

**2. The plan's 8–15 day estimate is high, and the reason is that it double-counts prerequisites.** The plan treats reprojection, history and variance as new work. Reprojection and history exist. Variance estimation is the genuinely new part. 5–8 days (Option B) is the more defensible number for a first cut.

**3. The plan does not mention the DX11 UAV ceiling, which is a real constraint on this specific feature.** All eight UAV slots are in use, and DX11 guarantees only 8. This is absent from the plan's risk column and should be added to §5.2's risk row. The full-SVGF two-target layout (Option A) may not fit on a DX11 path that does not reach feature level 11_1.

---

## §5.10 Shader pipeline

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A — Author HLSL once, keep GLSL** | Current direction; `tools/compile_nova_spirv.py` derives SPIR-V from the `.hlsl` | All four | 0 d (status quo) | Growing linearly with every new feature |
| **B — Author GLSL once, derive HLSL via SPIR-V round-trip** | The plan's preferred "compile twice" | All four | **Blocked — see findings** | Would foreclose nothing *if it worked*; it does not |
| **C — Author GLSL once, derive SPIR-V only** | GLSL feeds Vulkan; `.hlsl` stays hand-maintained for DX | Vulkan benefits, DX unchanged | 4–7 d | Nothing immediately; does not fix duplication |
| **D — Single shared source + thin per-language shim** | One algorithm file, two short preprocessor-selected wrappers | All four | 8–14 d | Nothing; the only option that actually ends duplication |

### The measured facts that decide this

These are not estimates. Each was reproduced on this machine.

**Fact 1 — the GLSL cannot be compiled to SPIR-V as it stands.** `Renderer3D_NovaRenderer.glsl` declares **85 loose uniforms** and **zero** `std140` blocks (verified: `grep -cE "^uniform "` = 85, `grep -cE "layout\s*\(\s*std140"` = 0). Vulkan GLSL rejects loose uniforms:

```
ERROR: Renderer3D_NovaRenderer.glsl:64: 'u_CameraPosition' :
  'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan
```

The `-G` (OpenGL SPIR-V) path fails too, differently: non-opaque uniforms need explicit locations, and neither `--auto-map-bindings` nor manual injection suffices because **arrays consume multiple locations** (first attempt collided at location 41).

**Fact 2 — a mechanical edit pass does make it compile.** Stripping the `#version` pragma and injecting `layout(location=)` across 85 uniforms (respecting array sizes, consuming 182 locations total) produces a working 234,796-byte `.spv`. So the GLSL→SPIR-V direction is *viable but non-trivial* — it needs a real preprocessor, not a one-liner.

**Fact 3 — SPIR-V→GLSL is blocked outright.** spirv-cross fails on the SSBO blocks:

```
SPIRV-Cross threw an exception: Buffer block cannot be expressed as any of
std430, std140, scalar, even with enhanced layouts.
```

The documented workarounds all fail: `--flatten-ubo` → *"All basic types in a flattened block must be the same."* `--flatten-ubo --remove-unused-variables` → same. `--flatten-ubo --version 430` → same.

**Fact 4 — SPIR-V→HLSL works, but breaks the engine's binding contract.** spirv-cross does emit valid HLSL from the SPIR-V (139,281 bytes), and the output is *plausible* — but it destroys everything the engine depends on:

- **Register numbers are rewritten.** `t15` becomes `t0`, samplers become `s0..s52`. The engine's register-space scheme (shifts u=0, b=7, t=16, s=200) exists specifically to keep these distinct under NVRHI's `offset + slot` Vulkan derivation. spirv-cross discards it.
- **The cbuffer disappears.** Output has no `NovaConstants` block. This is fatal, not cosmetic: `DX11Nova.h:213` does `reflection->GetConstantBufferByName("NovaConstants")` and `DX12Nova.cpp:151-154` does `strcmp(desc.Name, "NovaConstants")` then throws `"NovaConstants cbuffer not found"`. The source comment at `DX11Nova.h:208` spells out that renaming the cbuffer breaks the lookup.
- **SSBOs become `RWByteAddressBuffer`** rather than structured buffers, which changes both the HLSL access syntax and the CPU-side layout expectations.

**Fact 5 — the duplication is real and large.** 2,681 lines of GLSL against 2,902 lines of HLSL — **5,583 lines to maintain in parallel**, for one feature set.

### Option A — Author HLSL once, keep GLSL (status quo)

#### Approach

Unchanged. `.hlsl` is authored by hand for DX11/DX12; `tools/compile_nova_spirv.py` compiles it to SPIR-V for Vulkan; the `.glsl` is maintained by hand for OpenGL.

#### Files touched

None.

#### Data layout

Unchanged: 27 register() declarations, `u0`–`u7` UAVs, textures up to `t71`, samplers from `s200`.

#### Backend notes

This is the status quo that already works and already passes SPIR-V compilation.

#### Effort and risks

Zero new cost, but 5,583 parallel lines. The plan's claim that duplication is a "3-file-per-feature tax" understates it: every feature is a hand-edited pair *plus* a SPIR-V build, and drift between them is silent until a screenshot shows it.

#### What would prove it

Nothing to prove — it is the current state.

### Option B — Author GLSL once, derive HLSL via SPIR-V round-trip

#### Approach

The plan's first-preference option: compile `.glsl` → SPIR-V → HLSL via spirv-cross, and delete the hand-written `.hlsl`.

#### Files touched

Would rewrite `Renderer3D_NovaRenderer.hlsl` (2,902 lines, replaced by generated output), `DX11Nova.h`, `DX12Nova.cpp`, and the register-space scheme in `compile_nova_spirv.py`.

#### Data layout

**Unworkable as-is**, per Fact 4. Would require either post-processing spirv-cross output to restore registers and the cbuffer, or abandoning the named-cbuffer reflection path entirely — and the engine *cannot* abandon it, because `DX11Nova.h:213` and `DX12Nova.cpp:151` both depend on the literal name.

#### Backend notes

Would break DX11 and DX12 outright, not degrade them.

#### Effort and risks

Not "4–7 days." It is a research project with a known-broken intermediate. **Recommend rejecting this option.**

#### What would prove it

Already effectively proved *against*: Fact 4 was measured, and the cbuffer-name dependency at `DX11Nova.h:213` is a hard architectural constraint that spirv-cross output violates.

### Option C — Author GLSL once, derive SPIR-V only

#### Approach

Keep the mechanical preprocessor from Fact 2 (strip `#version`, inject `layout(location=)` respecting array sizes) and route Vulkan through it. The `.hlsl` stays hand-maintained for DX11/DX12.

#### Files touched

New tool alongside `compile_nova_spirv.py`; no shader changes.

#### Data layout

Requires generating 182 explicit locations and keeping them stable across edits — a maintenance burden of its own, and any added uniform shifts every later location.

#### Backend notes

Vulkan only. DX11/DX12 unchanged, so duplication survives at 5,583 lines.

#### Effort and risks

4–7 days for a benefit limited to one backend that **already has a working SPIR-V path**. Low value.

#### What would prove it

Not recommended. It solves the half of the problem that was not broken.

### Option D — Single shared source + thin per-language shim

#### Approach

One algorithm file expressing the passes in a common subset; two short wrappers that `#include` it behind preprocessor conditionals for `layout(binding=)` vs `register()` vs `cbuffer` syntax. The generated-location problem from Fact 2 disappears because the OpenGL backend keeps loose uniforms natively — only the Vulkan path needs the location injection, and it already gets that from `compile_nova_spirv.py`.

#### Files touched

New shared include; `.glsl` and `.hlsl` shrink to wrappers; the 85-uniform divergence point collapses to a small, explicit compatibility header.

#### Data layout

Preserves the existing register-space scheme exactly, because the shims — not a code generator — own the binding syntax.

#### Backend notes

All four, unchanged from today. This is the only option that leaves the working Vulkan SPIR-V path and the working DX11 cbuffer reflection both intact.

#### Effort and risks

8–14 days. The risk is a shared-subset tax: if GLSL and HLSL genuinely diverge in capability anywhere, the shim grows ugly. Fact 4 shows the divergence is real (SSBO representation differs), so this is not free.

#### What would prove it

Convert **one** pass — the bilateral blur, which is self-contained — to the shared form and build all four backends. If the wrapper approach survives one real pass, it scales; if it turns ugly on the first pass, stop and accept Option A. This is a one-day probe with a clear pass/fail.

### Recommendation

**Accept Option A as the status quo and run Option D's one-day probe on the bilateral blur.**

The plan lists Option B ("author once, compile twice") as first preference. That is now measurably wrong: spirv-cross rewrites registers and drops the `NovaConstants` cbuffer that `DX11Nova.h:213` and `DX12Nova.cpp:151` look up by name. Option B does not degrade — it breaks DX11 and DX12 outright.

What survives from the plan's intent is the *goal*, not the mechanism: stop maintaining 5,583 parallel lines. Option D reaches it without violating the two contracts the engine has already committed to (named cbuffer, explicit register spaces).

The one-day bilateral-blur probe decides it. If the shim stays clean through one pass, adopt D for all new features and migrate opportunistically. If not, keep A and accept that each new feature costs a hand-edited pair — which is a real cost, but a known one.

### Corrections to the plan

**1. The plan's preferred shader-pipeline option (Option B in the table above) does not work, and this was measurable.** Plan §5.10 ranks "author once, compile twice" first and glslang/SPIR-V cross-compile second, with no ranking of *feasibility* between them. Measured: the GLSL does not compile to SPIR-V unmodified (85 loose uniforms rejected by Vulkan GLSL); it does compile after a mechanical location-injection pass (182 locations); and spirv-cross output cannot be used because it destroys the register map and eliminates the `NovaConstants` cbuffer that both DX backends resolve by name. **The plan should state that cross-compilation is blocked by an engine-side reflection dependency, not merely "not preferred."**

**2. The plan does not identify the actual constraint.** It says each feature becomes "a 3-file-per-feature tax." The real numbers: 2,681 + 2,902 = **5,583 lines maintained in parallel today**, and the binding contract that constrains any codegen approach is the *named cbuffer* at `DX11Nova.h:213` / `DX12Nova.cpp:151`. That name is load-bearing in three separate backends and is the thing any generated-shader approach must preserve.

**3. The plan's existing-work framing understates what is already proven.** §5.10 presents `tools/compile_nova_spirv.py` as merely "a pattern already proven." It is more than that — it is the reason Option B fails. Its register-space shifts (u=0, b=7, t=16, s=200) are exactly what spirv-cross cannot reproduce, which makes the existing tool an asset to extend rather than a precedent to follow.

---

## §5.3 Hardware ray tracing

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A** | Keep the analytic tracer's geometry model; add `D3D12_RAYTRACING_GEOMETRY_AABBS`/`SPHERES` procedural NVRHI geometry so BLAS wraps the *existing* `RayTracingInstance` bounds, not triangles | DX12, Vulkan | 8–14 d | Real per-triangle fidelity: no displacement, no relief/POM, no mesh geometry. Locks out the OMM baker (§5.7) since a micromap needs triangles. Locks out ReSTIR's emissive-triangle light buffer (§5.4 step 1) as designed |
| **B** | First-world-class DXC vendoring + SM 6.6 `TraceRay` path over the **already-uploaded dynamic batch** (Cube/Sphere VB/IB, world-space), one BLAS per draw range | DX12, Vulkan | 12–22 d | The hand-maintained `.glsl`/`.hlsl` twin discipline (§5.10 option 3) — a third shader language generation enters. Forbids the SM5 Nova shader from ever being the DX12 path |
| **C** | DXC vendoring + `RayQuery` (inline RT) inside the *existing* SM 6.0 compute pipeline, keeping Nova's pass structure, buffers and accumulator byte-for-byte | DX12, Vulkan | 10–18 d | Any use of RT state that needs hit-shader payloads (callable shaders, SBT-indexed material selection, DXR 1.1 `CandidateForAnyHit`). No any-hit → no opacity micromaps. No miss shader → environment lighting must be restructured |
| **D** | Vendor DXC **and** rebuild NVRHI/DXC from pinned upstream tags into a single in-tree toolchain, then B's raygen path | DX12, Vulkan | 18–30 d | Nothing technical; it costs the whole DX12+Vulkan parity matrix as schedule. Forecloses shipping anything else in the plan's Phase 1–3 window |

### Option A — Analytic-bounds acceleration (no triangles)

#### Approach

The decisive fact is that **this engine's path tracer has no triangle geometry at all today.** `HitPrimitive` in the Nova shader takes a `RayTracingInstance` and switches on `MaterialParams.z` (a `PrimitiveShape` ordinal) to run an analytic routine. The instance is 272 bytes of transform, AABB and material — no vertex or index buffer reference.

So a triangle BLAS has nothing to point at. Option A instead builds the acceleration structure out of the bounds that *do* exist, using NVRHI's non-triangle geometry types, which are present in this header and already compiled into the shipped libs.

`nvrhi::rt::GeometryAABBs` / `GeometrySpheres` are declared in `Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h` and selected by `GeometryDesc::setAABBs(...)` / `setSpheres(...)` (`nvrhi.h:1644-1645`). The `InstanceDesc` union already carries a `bottomLevelAS` pointer (`nvrhi.h:1671`), so the existing per-instance record maps almost directly onto a TLAS instance — `WorldTransform` is the instance transform, `Min`/`Max` become the bounds, and `instanceID` maps to `setInstanceID` (`nvrhi.h:1685`) so the closest-hit shader can recover the same array index the compute path uses.

This is the smallest change that makes `GetRenderCapabilities().hardwareRT` mean something, and it is honest about what it is: it accelerates *traversal*, not *geometry*.

#### Files touched

Verified to exist:

- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — `RayTracingInstance` at `:43-59`; `BuildAndFlattenBVH` at `:248`; `SceneInstancesChanged()` at `:891-898`; the four instance-push sites at `:2542`, `:3164`, `:3288`, `:3475`
- `Wasteland/src/Wasteland/Renderer/Renderer3D.h` — `GetRenderCapabilities` consumers; `MaxAnalyticLights = 8` at `:298`
- `Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp` — `hardwareRT` resolution at `:52-68`
- `Wasteland/src/Platform/NVRHI/NVRHIContext.h` — `GetDevice()` at `:56`, `GetCommandList()` at `:60`
- `Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h`

To create:

- `Wasteland/src/Platform/NVRHI/RTBackend.h` / `.cpp` — sibling class, per §5.3 item 5's "prefer a sibling class" rule
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRT.hlsl`

`RTBackend.cpp` must `#include "wlpch.h"` and be followed by `premake5.exe vs2022`; the `files` glob at `premake5.lua:57-69` is `%{prj.name}/src/**.cpp`, so a new file under `Wasteland/src/` is picked up **only** after regeneration.

#### Data layout

One BLAS per `PrimitiveShape` value, static, built once at init — there are exactly 8 ordinals (`ProceduralMeshes.h:136-153`, `Cube=0` … `Grid=7`).

```c
// Procedural BLAS descriptors. GeometrySpheres uses a unit-radius sphere
// (position vec4 = (cx,cy,cz,r)); GeometryAABBs uses a unit AABB whose
// centre/half-extent are packed into two vec4s.
nvrhi::rt::GeometryDesc sphereGeo;
sphereGeo.geometryType = nvrhi::rt::GeometryType::Spheres;
sphereGeo.setSpheres(nvrhi::rt::GeometrySpheres{}
    .setVertexBuffer(s_BoundsBuffer)      // IBuffer*, unit sphere record
    .setVertexFormat(nvrhi::Format::R32G32B32A32_FLOAT)
    .setVertexCount(8)                    // one per PrimitiveShape ordinal
    .setVertexStride(16));                // 1x vec4 = 16 B

nvrhi::rt::AccelStructDesc asDesc;
asDesc.setTopLevelMaxInstances(1024);              // TLAS
// BLAS: asDesc.addBottomLevelGeometry(sphereGeo);  // isTopLevel -> false
```

TLAS build, per frame when `SceneInstancesChanged()` is true — the gate already exists at `Renderer3D.cpp:891`:

```c
m_CommandList->buildTopLevelAccelStruct(m_Tlas, instances.data(), instanceCount,
    nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
    nvrhi::rt::AccelStructBuildFlags::PerformUpdate);
```

Instance records are 64 B, asserted by NVRHI itself: `static_assert(sizeof(InstanceDesc) == 64, ...)` (`nvrhi.h:1693`). Field mapping from the existing 272 B `RayTracingInstance`: `WorldTransform` → `transform`, a stable per-frame slot → `instanceID`, `MaterialParams.z` → `instanceContributionToHitGroupIndex` so hit groups are per-shape.

NVRHI resource states: `ResourceStates::AccelStructRead`, transitioned via `ICommandList::setAccelStructState(rt::IAccelStruct* as, ResourceStates stateBits)` (`nvrhi.h:3622`), and bound with `BindingSetItem::RayTracingAccelStruct(uint32_t slot, rt::IAccelStruct* as)` (`nvrhi.h:2244`).

**MEASURED** (compiled with MSVC 19.51 against `Wasteland/vendor/glm`, `cl /std:c++17`, struct definitions copied verbatim from the repo):

```
sizeof(Vertex3D)            = 76      // glm vec3 is NOT 16-byte aligned in this build
offsetof(Vertex3D,Position) = 0
offsetof(Vertex3D,Normal)   = 28
sizeof(RayTracingInstance)  = 272     // alignas(16)
offsetof(RTI, MaxDistance)  = 208
offsetof(RTI, TextureScale) = 224
sizeof(BVHNodeGPU)          = 32      // std430 SSBO, two vec4
```

The 76 is worth flagging: a triangle BLAS over `CubeVertexBuffer` would need `vertexStride = 76`, which is **not** one of DXR's efficient strides (8/12/16/24/32). A vertex-position-only extraction buffer would be required — that is Option B's cost, and Option A avoids it entirely.

#### Backend notes

**DX12** — NVRHI maps `buildTopLevelAccelStruct` to `BuildRaytracingAccelerationStructure` (`nvrhi.h:3504`) and `dispatchRays` to `DispatchRays` (`nvrhi.h:3449`). Shader creation needs `nvrhi::ShaderType::RayGeneration` / `Miss` / `ClosestHit` / `AnyHit` (`nvrhi.h:787+`) — note these are **four more `ShaderType` values beyond the three the repo uses today** (`Vertex`, `Pixel`, `Compute`, at `NVRHIShader.cpp:360/418/497`).

**Vulkan** — same NVRHI calls, mapped to `vkCmdTraceRaysKHR` (`nvrhi.h:3450`). The descriptor-shift contract in `VulkanNova.cpp:225-232` (`shaderResourceOffset(16)`, `constantBufferOffset(7)`, `samplerOffset(200)`) must be **replicated exactly** for the RT binding layout, or every binding collides. The RT shader needs its own `-fvk-*-shift` values matched in `tools/compile_nova_spirv.py`'s `SHIFTS`/`BINDING_OFFSETS` tables (`compile_nova_spirv.py:55-66`).

**DX11 / OpenGL** — hard fallback, no code path. `setRayTracingState` is documented "Not supported on DX11" (`nvrhi.h:3438`), and `caps.hardwareRT = false` for both (`RenderCapabilities.cpp:41`, `:48`). Nova's compute tracer remains their only path.

#### Effort and risks

**ESTIMATE 8–14 person-days**, of which roughly 2 are DXC plumbing (see the Corrections section — this machine's DXC story changes the shape of that) and the rest is BLAS/TLAS wiring, an SBT, and a raygen/closesthit pair.

Risks, in order:
1. **The any-hit/alpha story is absent.** Closest-hit only means alpha-tested foliage becomes opaque. Since `GeometryFlags::Opaque` and `ForceOpaque`/`ForceNonOpaque` per-instance flags exist (`nvrhi.h:1468`, `:1650-1658`), the mitigation is marking geometry non-opaque and letting it degrade visibly — a *regression* versus the analytic tracer, which does no alpha test at all today. Neutral in practice.
2. **Sphere/AABB bounds vs. analytic shapes.** The analytic tracer intersects exact spheres, cylinders and tori. An AABB hull for a torus has a much looser bound, so traversal is correct but less selective. Correctness is not at risk; speed is.
3. **Vulkan descriptor collisions** are silent visual corruption, not crashes — exactly as §5.3 already warns.

#### What would prove it

Cheapest experiment that decides it: **a standalone NVRHI program, no engine** — one BLAS of 8 unit spheres, one TLAS with 1 instance at the origin, a raygen that traces one ray down +Z and writes `(hit ? green : red)` into `img_Output`, dispatch 8×8. If that frame is green with DX12 and blue on Vulkan, A/B/C are all still open and only the shader layer remains. **This does not need DXC** if compiled as a minimal SM 6.6 lib with a *modern* DXC — see Corrections. If it is red on both, the problem is NVRHI device creation or feature query, not this design.

### Option B — First-world-class DXC + `TraceRay` over the dynamic batch

#### Approach

Add a real SM 6.6 raygen/miss/closesthit set compiled by a vendored DXC, and feed it actual triangles from the batch buffers the engine *already* uploads every frame.

The enabling discovery: `DrawProceduralPrimitive` transforms every vertex to **world space on the CPU** and writes it into `SphereVertexBufferPtr` (`Renderer3D.cpp:2519`: `Position = glm::vec3(transform * glm::vec4(p, 1.0f))`), then rebases indices by `startIndexOffset` (`:2532-2533`). Both buffers are uploaded in `Flush` via `SetData` (`:1652-1655`).

Two consequences:
- A **single BLAS over the whole batch, in world space, is valid** and needs no TLAS at all — one `TraceRay`, one hit group, all geometry already in place.
- Because vertices are world-space, moving an object means re-uploading vertices, so a TLAS refit is *not* sufficient for the batch. The TLAS is only needed if geometry is kept in local space per object.

That makes this option genuinely distinct from A: it traces real triangles and gets displacement, per-triangle material and a path to OMM. Its cost is the whole shader-generation change.

#### Files touched

Verified to exist (modified, not created):

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — currently compiled at `cs_5_0` via `D3DCompileFromFile` (`DX12Nova.cpp:62-64`), and at `cs_6_0` → SPIR-V for Vulkan (`compile_nova_spirv.py:82`)
- `Wasteland/src/Platform/NVRHI/DX12Nova.cpp` — `:57-67` shader load + reflection; `:95-98` `CreateConstantBuffer`/`CreateRootSignature`/`CreatePSO`
- `Wasteland/src/Platform/NVRHI/VulkanNova.cpp` — `:176-222` `Init()` reads `.spv` and calls `createShader`; `:225-273` builds binding layout + compute pipeline
- `tools/compile_nova_spirv.py` — would need a second output target for the RT library
- `premake5.lua` — `libdirs` at `:99-104`, `links` at `:106-118`, Windows `links` filter at `:149-159`

To create:

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRT.hlsl` — raygen/miss/closesthit/anyhit in one lib_6_6 unit
- `Wasteland/src/Platform/NVRHI/DX12NovaRT.{h,cpp}` — sibling class, not an extension of `s_Nova12`

#### Data layout

```c
// One BLAS over the whole world-space batch. Triangles geometry.
nvrhi::rt::GeometryTriangles tris;
tris.setVertexBuffer(nvrhiVB)          // CubeVertexBuffer / SphereVertexBuffer
    .setIndexBuffer(nvrhiIB)
    .setVertexFormat(nvrhi::Format::R32G32B32_FLOAT)   // reads Position only
    .setIndexFormat(nvrhi::Format::R32_UINT)
    .setVertexStride(76)               // MEASURED sizeof(Vertex3D)
    .setVertexOffset(0)
    .setIndexOffset(0)
    .setIndexCount(s_Data.SphereIndexCount);

nvrhi::rt::AccelStructDesc batchBlas;
batchBlas.addBottomLevelGeometry(nvrhi::rt::GeometryDesc().setTriangles(tris)
    .setFlags(nvrhi::rt::GeometryFlags::Opaque));
```

`vertexStride = 76` is the problem, and it is the reason this option is 12–22 days rather than 8. DXR's efficient position strides are 8/12/16/24/32; 76 forces the driver into a byte-stride fetch path. Two escapes:
- **Position-only extraction buffer** — repack just `Position` into a `Format::R32G32B32_FLOAT` stream with `vertexStride = 12`, built on the CPU alongside the existing write loop (≈ `:2515-2531`). Cheap: one extra `std::vector<glm::vec3>` per batch, uploaded once per frame. **This is what to do first.**
- `vertexPositionStride` on the NVRHI 5.x AS-descriptor path — not applicable to the pinned header; `GeometryTriangles` here has only `vertexStride` (`nvrhi.h:1504`).

Pipeline state, from the real NVRHI signatures:

```c
nvrhi::rt::PipelineDesc pd;
pd.addShader(nvrhi::rt::PipelineShaderDesc().setExportName("RayGen")
        .setShader(m_RayGenShader).setBindingLayout(m_RtLayout));
pd.addHitGroup(nvrhi::rt::PipelineHitGroupDesc().setExportName("HitGroup0")
        .setClosestHitShader(m_ClosestHitShader).setAnyHitShader(m_AnyHitShader)
        .setBindingLayout(m_HitLayout));
pd.setMaxRecursionDepth(1);
pd.setMaxPayloadSize(32);
pd.setMaxAttributeSize(8);          // default is float2 = 8 B (nvrhi.h:2816)

m_ShaderTable = m_Pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc());
m_ShaderTable->setRayGenerationShader("RayGen", m_Bindings);
m_ShaderTable->addMissShader("Miss", m_MissBindings);
m_ShaderTable->addHitGroup("HitGroup0", m_HitBindings);

nvrhi::rt::State rtState;
rtState.setShaderTable(m_ShaderTable);
rtState.addBindingSet(m_RtBindings);
m_CommandList->setRayTracingState(rtState);                       // nvrhi.h:3441
m_CommandList->dispatchRays(nvrhi::rt::DispatchRaysArguments()
    .setDimensions(w, h, 1));                                     // nvrhi.h:3451
```

A 32 B payload (two `float4`s: hit position + barycentrics/instanceID) is **ESTIMATE**. Every recursive bounce is another payload; at `maxRecursionDepth = 1` this is one hit, so 32 B is sufficient for a first pass and is a genuine constraint to revisit at §5.4.

Cost model, **ESTIMATE**: at 1920×1080 with 8-byte positions, a batch of 10⁵ triangles is a ~1–3 MB BLAS on the compact side; the same batch as 12 B positions is 1.2 MB vertex data before BVH expansion, so expect roughly 2–4× that in the final AS. TLAS for 1024 instances is 1024 × 64 B = **64 KB** (MEASURED: `sizeof(InstanceDesc) == 64` is asserted at `nvrhi.h:1693`).

#### Backend notes

**DX12** — the whole `lib_6_6` set compiles to DXIL via one `D3DCompileFromFile`-equivalent DXC call, then four `device->createShader` calls with the four new `ShaderType` values. Note `DX12Nova` today talks **raw D3D12**, not NVRHI: `Init(ID3D12Device* d, ID3D12CommandQueue* q)` (`DX12Nova.h:147`) is handed `ctx->GetD3D12Device()` (`Renderer3D.cpp:991`). Mixing a raw-D3D12 Nova with an NVRHI BLAS means cross-API resource sharing, which is a real cost. **Prefer routing the RT pass through NVRHI's device**, as `VulkanNova` already does via `m_Device` (`VulkanNova.cpp:198`) — that keeps one device, one command list, one barrier model.

**Vulkan** — same lib_6_6 source to SPIR-V, with the register-shift contract extended. `VulkanNova`'s `kReadImageSlot = {64,65,66,69,71}` and `Texture_SRV(15)` array (`VulkanNova.cpp:22-23`, `:250-252`) are Nova-pass-specific; the RT shader needs its own consistent shift set, verified with the same `check_bindings` machinery.

**DX11 / OpenGL** — nothing. SM 5.1 `fxc` cannot emit RT, DX11 has no DXR, OpenGL has neither DXR nor `VK_KHR_ray_tracing`. Both stay on Nova.

#### Effort and risks

**ESTIMATE 12–22 person-days.** Roughly: 2 DXC vendoring + premake, 3 shader authoring, 4 AS build/refit plumbing, 3 barriers and resource states, 4–10 debugging Vulkan descriptor and layout bugs.

Risks:
1. **Shader-generation discontinuity.** Two DX12 compute paths at different shader models in one file, plus a third language generation. §5.10 must be decided first — the plan already says so (`§6` Phase 1 gates Phase 2), and this option is what makes that gate binding.
2. **Vertex stride 76** → must ship the position-only extraction buffer or accept a slow BLAS build.
3. **World-space batching defeats TLAS refit.** Any moving object currently rewrites vertices (`:2519`); a refit-based TLAS is pointless until geometry is per-object and local-space. Decide before writing the refit code.
4. **Cross-API hazard** if the RT pass is built on raw D3D12 while the BLAS lives in NVRHI.

#### What would prove it

Compile one `lib_6_6` unit with the chosen DXC that emits DXIL, and separately to SPIR-V. Then, before touching the engine: build one BLAS over `SphereVertexBuffer` in a standalone NVRHI harness and `dispatchRays` a ray straight down from above the scene. **A non-black frame that matches the raster image is the entire proof.** Both compile checks cost minutes and decide the option before any engine code exists.

### Option C — `RayQuery` inside the existing compute pipeline

#### Approach

Do not add a ray-tracing pipeline at all. Compile Nova at SM 6.0 with `RayQuery` intrinsics and keep the pass structure, the `RayTracingInstance` SSBO, the CPU BVH and the accumulator exactly as they are. Replace only the traversal routine: analytic `HitPrimitive` loop → `RayQueryInitialize` / `Proceed` / `CommittedTriangleIndex`.

The appeal is unusually high for this repo specifically: **it is the only option that does not disturb `img_TraceGBuffer` (u6), the temporal accumulation ping-pong, or the `pass(1);pass(0);pass(2);` ordering** at `Renderer3D.cpp:1381`, which the plan flags as deliberate and must not be "sorted" (`§7`). The G-buffer already carries packed world normal and NDC depth (`Renderer3D_NovaRenderer.hlsl:2160`), so denoising and reprojection keep working unchanged.

It also keeps one shader file and one pipeline, and `RayQuery` needs no shader binding table, no payload, no `ShaderType` additions — the entire RT pipeline-state surface in `nvrhi.h:2782-2906` goes unused.

#### Files touched

Verified to exist (modified):

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — `HitPrimitive` at `:884`, instance fetch at `:908`, `CalculateUV` at `:1274`, `ApplyPOM` at `:1539`, second fetch at `:1689`, light loop at `:1961`. `Instances` is `StructuredBuffer<RayTracingInstance> Instances : register(t1)` (`:65`)
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — the twin, same regions shifted (`:707`, `:731`, `:1097`, `:1400`, `:1626`, `:1907`)
- `Wasteland/src/Platform/NVRHI/DX12Nova.cpp:62-64` — `cs_5_0` → `cs_6_0`
- `Wasteland/src/Platform/NVRHI/VulkanNova.cpp` — already at `cs_6_0`; only the shift table may need review
- `premake5.lua:149-159` — add `dxcompiler.lib`

To create: `Wasteland/src/Platform/NVRHI/RTQuery.h` — a thin traversal abstraction with a `#define`d RayQuery body and a fallback to the current analytic loop.

**Note the GLSL twin becomes a real liability here.** The `.glsl` file cannot express `RayQuery`, so it must keep the analytic path. That is defensible — OpenGL needs it anyway — but it means OpenGL and DX12 now trace *different geometry models*, and every visual difference between them stops being a backend bug.

#### Data layout

No new structures. The BLAS/TLAS from Option A is reused verbatim (a `RayQuery` against the same AABB/sphere BLAS works identically), and `RayTracingInstance` stays 272 B (MEASURED) on `t1`.

```hlsl
// In the shader, replacing the analytic HitPrimitive loop:
RayQuery rq;
RayQueryInitialize(rq, AccelBuffer, RayFlags::None, 0xFF, 0 /*sbt*/, 0 /*offset*/,
                   origin, tmin, /*cullMask*/ 0xFF);
while (rq.Proceed()) {
    if (rq.TriangleFrontFace) break;
    uint tri = rq.CommittedTriangleIndex;      // global into the batch
    uint v0 = Indices[tri*3+0], v1 = Indices[tri*3+1], v2 = Indices[tri*3+2];
    // Reconstruct the same barycentrics/normal the analytic path produced.
    Barycentrics bary; bary.bary[0]=rq.BarycentricX; bary.bary[1]=rq.BarycentricY;
    bary.bary[2]=1.0f-bary.bary[0]-bary.bary[1];
}
```

The barycentric → normal/UV → material reconstruction must reproduce `CalculateUV` (`:1274`) and `ApplyPOM` (`:1539`) exactly, since both branch on `PrimitiveShape` from `MaterialParams.z`. That is the main correctness risk and the bulk of the work.

Descriptor cost: **+1 binding** for the AS SRV. Under Nova's existing Vulkan shifts that is `StructuredBuffer_SRV` in the t-space, so it must not collide with the current `t1..t3` / `t15` / `t48..t52` / `t64..t71` assignments (`VulkanNova.cpp:239-260`) — the `check_bindings` audit in `compile_nova_spirv.py:127-166` will catch it.

#### Backend notes

**DX12** — `cs_6_0` with `RayQuery` compiles to DXIL through DXC; the pipeline stays a `ComputePipelineDesc`, so `NVRHIShader.cpp`'s compute path is untouched. NVRHI reports `Feature::RayQuery` in its feature enum (`nvrhi.h:3046`) — queryable via `IDevice::queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0)` (`nvrhi.h:3770`) rather than assumed.

**Vulkan** — `RayQuery` lowers to `rayQueryEXT` in SPIR-V; NVRHI already wraps the capability and the plan's `caps.rayQuery = true` (`RenderCapabilities.cpp:55`, `:64`) is accurate.

**DX11 / OpenGL** — the `.glsl` twin keeps the analytic path unchanged. This option degrades most gracefully of the three: the fallback is *already the shipped code*, not a new one.

#### Effort and risks

**ESTIMATE 10–18 person-days.** Lower than B because no pipeline state, no payload, no SBT, and no barrier rework; higher than the naive reading because barycentric/material reconstruction must match the analytic path exactly.

Risks:
1. **No any-hit, no miss.** Alpha-tested geometry becomes opaque; there is no miss shader, so environment lighting must stay outside the trace. Both are acceptable *today* (the analytic path does neither) and become blockers at §5.7.
2. **`RayQuery` recursion cost.** Inline traversal in a compute shader cannot use the full RT pipeline, and per-ray divergence is worse than in a raygen shader. **ESTIMATE**: acceptable at low spp, a real cost at the spp levels SVGF will want.
3. **The GLSL twin divergence** described above.
4. **This option forecloses the hardware path.** Once traversal lives inside a compute shader, §5.7's micromaps and §5.4's per-material hit groups have nowhere to attach. Treat C as *sufficient for §5.2/§5.4/§5.5* and insufficient for §5.7.

#### What would prove it

One shader: `cs_6_0`, `numthreads(8,8,1)`, `RayQuery` against a BLAS over a hardcoded two-triangle quad, output `hit ? green : red`. Compile it **both** to DXIL and to SPIR-V with the chosen DXC. Green on DX12 *and* on Vulkan means the whole approach is viable; if DXIL works and SPIR-V does not, the option collapses to DX12-only and the plan's tier table needs a column.

### Recommendation

**Start with Option A's standalone harness as a gate, then build C, and hold B in reserve.**

The reasoning is that the plan's §5.3 is described as "the gate for §5.4–5.7", but on the evidence here it is **not** the gate it thinks it is. The scene's ray-traced geometry is **analytic primitives, not triangles** — `HitPrimitive` branches on `MaterialParams.z`, and `RayTracingInstance` (`Renderer3D.cpp:43-59`) holds bounds and material, with no buffer reference. Every downstream feature that needs real triangles (§5.4's emissive-triangle light buffer, §5.7's opacity micromaps) is blocked on a **geometry-model** gap that no amount of DXC or BLAS plumbing closes. §5.4's light buffer should be prototyped *before* Phase 2, not after — that is the single fact that should reorder the plan.

Given that: **C** is the best value. It makes `hardwareRT` mean something on DX12 and Vulkan, it needs the least new machinery, it leaves the denoiser path and pass ordering untouched, and its fallback is code that already ships. **A** is the honest first milestone inside it — the AABB/sphere BLAS is what a RayQuery traverses, and proving it standalone costs a day. **B** is the correct destination but should wait until §5.7 justifies it, because it is the only option that forecloses nothing and therefore costs the most.

The DXC finding below changes the effort table materially: this machine already has DXC in two places, one of which can emit SPIR-V and one of which cannot, and **neither compiles ray-tracing intrinsics at all** as measured.

### Corrections to the plan

**1. "No DXC" (`§5.3` item 2, `§2` gap 2) — wrong, and the replacement is not simple.** DXC is **present on this machine in four locations** (MEASURED):

| Path | Version (MEASURED) | SPIR-V? | RT intrinsics? |
|---|---|---|---|
| `C:\VulkanSDK\1.3.296.0\Bin\dxc.exe` | `1.8 - 1.8.0.4739` | **YES** | **NO** |
| `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe` | `1.8 - 1.8.2502.11` | **NO** | **NO** |
| `...\Redist\D3D\x64\dxc.exe` | `1.8 - 1.8.2502.11` | — | — |
| `...\arm64\`, `...\x86\` | same SDK | — | — |

`dxcompiler.lib` import libraries exist at `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64\` and `C:\VulkanSDK\1.3.296.0\Lib\`. And `premake5.lua:103` **already has `C:/VulkanSDK/1.3.296.0/Lib` in `libdirs`** — so `dxcompiler.lib` needs only a `links` entry, not a libdir change. `tools/compile_nova_spirv.py:46-47` already hardcodes the VulkanSDK `dxc.exe`.

But the load-bearing MEASURED result is this: **neither available DXC compiles ray-tracing intrinsics.** `TraceRay` is "use of undeclared identifier" under `lib_6_3`…`lib_6_8`; `RayQuery` reports "use of class template 'RayQuery' requires template arguments" (a pre-1.7 API shape) and has no `Init` member; `RayQueryInitialize` is absent from the SDK DLL's symbol table entirely; `RayFlags`, `RayType` and `BuiltInTriangleIndices` are all undeclared. Yet the SDK DLL *does* contain the string `TraceRay` (7 occurrences), `-help` advertises `lib_6_1`…`lib_6_8` and SM 6.6 raytracing-payload qualifiers, and a control shader using `WaveActiveSum` at `cs_6_0` compiles fine — so the harness is sound and the compilers are not compute-only. **Consequence: vendoring a current DXC release is not optional and not a 1-day task; it is a hard prerequisite, and §5.3's stated 10–20 days excludes it.** Any option below depends on it.

The plan's verification command (§5.3 item 1) is also misleading: `./Wasteland/vendor/dxc/bin/x64/dxc.exe` does not exist, and the `-T cs_6_6` in it would not exercise RT anyway — RT needs a `lib_6_x` library target.

**2. "NVRHI was built with ray tracing" (`§2` resolved #1, open question #2) — correct, and open question #2 is closed.** VERIFIED per backend, per configuration: `d3d12-raytracing.cpp` is in `nvrhi_d3d12.vcxproj:337` and `vulkan-raytracing.cpp` in `nvrhi_vk.vcxproj:327`; `nvrhi_d3d11.vcxproj` contains **zero** raytracing references. Object files exist for both backends in **both** `Debug/` and `Release/`, and all four libs are present. `dumpbin /SYMBOLS` finds 327 `AccelStruct` symbols in `nvrhi_d3d12.lib` and 211 in `nvrhi_vk.lib`, with 8 and 7 `dispatchRays` symbols. `build/CMakeCache.txt:352` confirms `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP:BOOL=OFF` (micromap only, as the plan says) and `:401` `NVRHI_WITH_RTXMU:BOOL=OFF`. **No NVRHI rebuild is needed.** Open question #2 in `§7` asks whether `build/%{cfg.buildcfg}` is populated for both configurations — it is, and `premake5.lua:102` already points at it.

**3. Open question #1 — answered, and the answer reframes the whole section.** `ProceduralMeshes` **does** produce real triangles: `ProceduralMesh` holds `Positions`, `Normals`, `TexCoords` and `std::vector<uint32_t> Indices` (`ProceduralMeshes.h:24-59`) with `TriangleCount()` = `Indices.size()/3`. They are rasterised into the dynamic batch (`Renderer3D.cpp:2515-2536`).

But **they are never used as ray-traced geometry.** `DrawProceduralPrimitive` rasterises the triangles and then, separately, pushes a `RayTracingInstance` whose only geometry is `Min`/`Max` AABBs plus a `PrimitiveShape` ordinal in `MaterialParams.z` (`:2542-2556`). The shader picks an **analytic** intersection routine from that ordinal (`HitPrimitive`, `Renderer3D_NovaRenderer.hlsl:884`). So the answer to "are they real triangles available for a BLAS, or must they be excluded?" is: **the triangles exist and are uploaded, but the ray tracer never sees them — they are excluded by construction, not by limitation.** Turning them into BLAS geometry means *changing the trace model*, not *feeding existing data to a BLAS*. That is a larger change than §5.3 item 3 implies.

Two supporting details that constrain the work: vertices are written **world-space** (`:2519`), so a TLAS refit cannot express object motion — the batch must be restructured to local space first; and `sizeof(Vertex3D) == 76` (MEASURED), an unefficient DXR stride.

**4. "BLAS per mesh" (§5.3 item 3) — there are no per-mesh vertex arrays.** `VertexArray` is a thin interface over `std::vector<Ref<VertexBuffer>>` plus one index buffer (`VertexArray.h:55-72`); the engine has exactly **two** dynamic batches, `CubeVertexBuffer` and `SphereVertexBuffer` (`Renderer3D.cpp:931`, `:954`), each capped at `MaxVertices = 100000` / `MaxIndices = 300000` (`:305-306`). There is no per-mesh VA, no indexed scene mesh submitted to the tracer, and `Renderer3D::Submit(shader, vertexArray, transform)` (`:83`) is a raster-only path. `MeshLoader.h` defines `MeshData`/`MeshVertex` and is never referenced from the ray tracer. **"BLAS per mesh" is not implementable as written**; the real granularity is per-batch or per-draw-range.

**5. TLAS rebuild frequency (`§5.3` item 3) — answerable, and it is per-frame-under-motion.** The scene re-submits every primitive each frame from `Scene::SubmitMeshPrimitives()` (`Scene.cpp:1647`, called from `OnUpdateRuntime` at `:736`), reading `TransformComponent` (`Components.h:105-116`) fresh from entt. The gate is `SceneInstancesChanged()` (`Renderer3D.cpp:891-898`): a size check plus a `memcmp` over the whole 272 B instance array against `m_PrevSceneInstances`. Both DX11 (`:1391`) and DX12 (`:1496`) rebuild the BVH and re-upload instances only when that returns true. **So the TLAS rebuild cost is identical to the CPU BVH cost the engine already pays** — a `memcmp` of ~272 KB at 10⁴ instances (MEASURED size × ESTIMATE count), plus a `buildTopLevelAccelStruct`. Every primitive pushes `m_SceneDirty = true` (`:2574`), and the analytic path applies no motion threshold, so **in practice any moving object rebuilds every frame.** A TLAS refit will hit the same frequency.

**6. `cs_5_0` is the only current target, and the Vulkan path already uses `cs_6_0`.** `DX12Nova.cpp:63` targets `cs_5_0`; `compile_nova_spirv.py:82` targets `cs_6_0`. The plan's "SM 6.6 for RT passes only, keep SM5 Nova" is therefore a *second* DX12 compute generation alongside an existing SPIR-V-only `cs_6_0` — slightly more shader churn than §5.3 item 2 implies.

**7. Small but load-bearing: RT needs four more `ShaderType` values, and the repo uses three.** `nvrhi.h:787+` adds `RayGeneration`, `AnyHit` and (just past line 788) `ClosestHit`/`Miss`; `NVRHIShader.cpp` currently creates only `Vertex` (`:360`), `Pixel` (`:418`) and `Compute` (`:497`). Any option using an RT pipeline must extend the shader factory, which is a `Shader.h`/`NVRHIShader.cpp` change, not a new-file-only change.

**8. premake5.lua structure, as requested.** For the `Wasteland` project (`premake5.lua:43`): `files` glob at `:57-69` is `%{prj.name}/src/**.h` + `**.cpp` — a new engine `.cpp` under `Wasteland/src/` is *not* compiled until `premake5.exe vs2022` regenerates, which is exactly the documented silent-success trap. `includedirs` at `:77-97`, `libdirs` at `:99-104` (already containing both the NVRHI build output and `C:/VulkanSDK/1.3.296.0/Lib`), base `links` at `:106-118`, and a `filter "system:windows"` `links` block at `:149-159` listing `d3d12.lib`, `dxgi.lib`, `d3dcompiler.lib`, `vulkan-1.lib`, `volk.lib`. **A DXC drop-in needs exactly two edits:** `"dxcompiler.lib"` into the Windows `links` block at `:155`, and nothing else — the libdir is already there. Plan item 1's "add include dir, lib dir and `dxcompiler.lib`" is over-specified by two of three. Also note `WastelandPython` (`:215`) and `Sandbox` (`:328`) have their own `libdirs`/`links`; if the Python bindings must see RT symbols, they need matching entries.

**Not verified / out of scope:** I did not run MSBuild (per instructions, the tree does not link), so no claim here is compile-verified against the engine. I did not fetch NVIDIA RTX SDK sources, per §1. Papers and public API documentation were the only external references consulted.

---

# §5.4 ReSTIR DI — implementation options for Wasteland

Scope: `direct-light sampling` on top of the Nova compute path tracer. Read with the plan of
record `.hermes/plans/2026-10-03_213029-rtx-kit-cross-backend.md` §2 (context), §3 (tier model),
§4 (effort + the named schedule risk), §5.3 (the gate), §7 (known constraints).

Everything below is grounded in this tree as of `HEAD = 6fdf820`. Numeric claims are labelled
**MEASURED** (I ran it, and say how) or **ESTIMATE** (engineering judgement). Nothing is asserted
from NVIDIA source — the reservoir math is from Bitterli et al., ACM TOG 2020, and ReGIR (Yariv
et al., HPG 2023) is cited only as prior art.

---

## §5.4 ReSTIR DI

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A** — Reservoir over the existing per-instance light list | Reuse `LightIndices` + `BVHNodes` as they already are. A candidate is an emissive *instance*, not a triangle. Reservoir is a new 32-byte SSBO + 2 new passes. | All four, TierC. No hardware RT. | 6–10 person-days | No per-triangle area weighting. No textured emission. Imported-OBJ emitters are invisible to it. Locks in the proxy light model (a point at the instance origin — which is what the shader already does today). |
| **B** — Area-correct analytic-shape sampling + light CDF | Same reservoir as A, but a candidate is a *point on the analytic surface* of an emissive instance, drawn by area through a precomputed CDF/alias table over emitters. Needs `SampleShapeSurface()` per shape in both shader twins. | All four, TierC. No hardware RT. | 12–18 person-days | No per-triangle. No textured emission. Only the 8 shapes `HitPrimitive` already knows. Requires a CPU-side surface-area formula per shape (exact for quad/disc/cylinder, closed-form for sphere, approximate for torus). |
| **C** — Triangle light buffer + dedicated light BVH | Tessellate `ProceduralMeshes::*` on the CPU into world-space emissive triangles; new SSBO + its own BVH SSBO; hardware `dispatchRays` for visibility on DX12/Vulkan, brute-force traversal on GL/DX11. | DX12/Vulkan TierA; GL/DX11 brute-force over the same buffer. **Gated on §5.3.** | 18–28 person-days, *plus* §5.3's 10–20 | Nothing algorithmically, but it cannot start before §5.3 lands and it inherits §5.3's risk. Also inherits a real inconsistency: Nova has no triangle intersector, so shadow rays still use the analytic form while the sample point comes from a triangle. |

A and B share the reservoir, the passes, the combine math and the acceptance test. They differ only
in what a "candidate" is. **B is the one worth building.** A exists because it is the smallest thing
that could possibly work and it de-risks B; C is deferred.

### Option A — Reservoir over the existing per-instance light list

#### Approach

This is the option that requires believing nothing new.

The critical fact about this engine's current direct-lighting path is that **an emissive instance is
already treated as a point light at its own origin**. `Renderer3D_NovaRenderer.glsl:1947`:

```glsl
vec3 toLight = light.WorldTransform[3].xyz - h.worldPos;
```

There is no area, no triangle, no surface — the entity's translation column is the light position.
And the loop that consumes `LightIndices` (`:1943-1946`) is already a *stochastic subset with an
unbiasedness correction*: `:1934-1941` picks 2 of N by stride and scales by
`lightScale = lightsToSample / evalCount`. So the engine already ships an unbiased-by-construction
stochastic light selector.

ReSTIR DI replaces that selector with a reservoir. Per pixel, per frame:

1. **Generate** `M` candidates. Each candidate is a uniform draw from `LightIndices` (all emitters
   are already listed there — see §5.4b), resolved to a point at the instance origin. Weight
   `w = p̂(x)/p(x)` where `p̂` is the unshadowed contribution and `p` is the uniform-over-emitters
   proposal. Feed each through `ReservoirUpdate`.
2. **Temporal combine.** Reproject and combine with last frame's reservoir, using the MIS-weighted
   combine from Bitterli et al. Alg. 4.
3. **Spatial combine.** 5 neighbour taps, gated on normal similarity (available: `img_TraceGBuffer`
   binding 6 carries packed world-space normal in `.gba` and NDC depth in `.r`, `glsl:15`).
4. **Shade.** One shadow ray for the single surviving sample, then `W · contribution`.

The whole thing is `RunNovaPassSchedule` plus two new pass IDs, and it reuses the velocity
reprojection that `RunTemporalAccumulation` already implements at `glsl:2246-2268` (`prevUV = uv -
velocity`, `histUV = prevUV * u_RenderScale`, depth rejection at `abs(currentDepth - prevDepth) >
0.02f`). That is a screen-space UV lookup — exactly what a temporal reservoir combine needs, so the
reprojection code is copied, not invented.

Note the estimator is unchanged from today's, so this option is **variance reduction on an already
unbiased estimator**. The image does not change; the noise does. That is exactly what makes it the
right first move.

#### Files touched

Real paths, verified to exist unless marked NEW.

| Path | Status | Change |
|---|---|---|
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` | exists (2682 lines) | `Reservoir` struct, `ReservoirUpdate`, `CombineReservoirs`, `RunRestirTemporal` (pass 12), `RunRestirSpatial` (pass 13), dispatch cases in `main()` |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` | exists (2724 lines) | Same, hand-maintained twin (§5.10 tax) |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` | exists, **binary** | Regenerated by `tools/compile_nova_spirv.py`; never hand-edited |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.h` | exists | Declare reservoir SSBO handle accessors; `MaxAnalyticLights` context at `:298` |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | exists | Wire the new SSBO into all **three** flush paths and the GL inline dispatch |
| `Wasteland/src/Platform/NVRHI/DX12Nova.h` / `.cpp` | exist | 4th structured buffer + 9th root parameter |
| `Wasteland/src/Platform/NVRHI/DX11Nova.h` | exists | 4th `Upload()` scene buffer |
| `Wasteland/src/Platform/NVRHI/VulkanNova.h` / `.cpp` | exist | 4th binding slot |
| `Wasteland/src/Wasteland/Renderer/RenderCapabilities.h` | exists | New `restirDirect` flag, or reuse `TierFor(FeatureTier::ComputeEmulated)` |
| `tools/compile_nova_spirv.py` | exists | Re-run after any register change; it verifies the binding map |
| `tests/test_restir_reservoir.cpp` | **NEW** | Standalone unbiasedness harness (§5.4b "prove it") |

**No new engine `.cpp` is required for Option A**, which sidesteps the `premake5.exe vs2022`
regeneration trap entirely. That is a real, non-obvious scheduling benefit.

#### Data layout

**Reservoir — 32 bytes, verified against what the math actually requires.**

```c
struct alignas(16) LightReservoir   // 32 B, 2x RGBA16F-equivalent slots
{
    uint32_t lightIndex;   // +0   index into LightIndices[] / emitter table
    uint32_t M;            // +4   candidates seen this frame (clamped)
    float    W;            // +8   unbiased weight = 1 / (pHat * Mhat)
    float    wSum;         // +12  sum of candidate weights (RIS accumulator)
    glm::vec2 bary;        // +16  sample point on the emitter surface
    uint32_t normalOct;    // +24  octahedral normal, 2x uint16 (spatial-reuse rejection)
    uint32_t flagsValid;   // +28  bit0 = reservoir non-empty
};                           // = 32 B exactly
```

**Why 32 bytes is the right target and not a rounding convenience.** The ReSTIR reservoir
mathematically needs four scalars: the selected sample `y`, `wSum`, `M`, and `W`. The paper's Alg. 4
MIS combine recomputes `p̂_q(y)` at the *receiving* pixel rather than storing it — which works here
only because emission is not textured (§5.4b: one `float` per instance), so `p̂` is a cheap
recompute rather than a texture fetch. That is the engine-specific fact that buys the 4 bytes back.
If emission ever becomes textured, `p̂` must be stored and the struct becomes 48 B — **so the 32 B
budget is load-bearing on a property of the material system, not an arbitrary target.**

`bary` is stored rather than a world-space `vec3` so that a sample survives reprojection and stays
re-evaluatable against the emitter; a world-space point would be 12 B and would push the struct to
36 B, breaking the 16-byte alignment of the second slot.

**Light buffer — Option A reuses what exists, adds nothing.**

| Buffer | Location | Stride | Notes |
|---|---|---|---|
| `SceneInstances` | SSBO, GLSL binding 1 / HLSL `t1` | `sizeof(RayTracingInstance)` = **272 B** | `Renderer3D.cpp:42-58`, `alignas(16)` |
| `BVHBuffer` | SSBO, GLSL binding 2 / HLSL `t2` | 32 B | `BVHNodeGPU`, `Renderer3D.cpp:61-65`; `w = leftChild` or `-(firstInstance+1)` |
| `LightListBuffer` | SSBO, GLSL binding 3 / HLSL `t3` | 4 B (`int`) | Built at `Renderer3D.cpp:1397` and `:1502` |
| **reservoir (NEW)** | SSBO, GLSL binding 13 / HLSL `t10` | 32 B | ping-pong pair |

`RayTracingInstance` is 272 B: two `mat4` (128 B) + six `vec4` (96 B) + four scalars (16 B) + three
`vec4` (48 B) = 288 B… recomputed from the declaration: 64+64 = 128, then Albedo/MaterialParams/
Min/Max/Emission = 5×16 = 80 → 208, then MaxDistance/LODLevel/TextureID/PackedMaterialMapID = 16 →
224, then TextureScale/DisplacementParams/WaveParams = 48 → **272 B**. `alignas(16)` with 272
already a multiple of 16, so no tail padding. **MEASURED** by summing the field widths in
`Renderer3D.cpp:42-58`.

**BVH — reused unchanged.** `BVHNodeGPU` is 32 B (`Renderer3D.cpp:61-65`), matching the shader's
`BVHNode` (`glsl:50-53`). Build is `BuildAndFlattenBVH` → `BuildBVHRecursive` (`:296-395`),
`BVH_LEAF_SIZE = 4` (`:259`), median split on the longest centroid axis (`:342-375`). Option A needs
no new BVH: shadow rays already traverse this one, and `IsOccluded` (`:793-830`) already supports
the `skipIdx` self-shadow exclusion (`:822`).

#### Backend notes

| Backend | Tier | What is the same | What differs |
|---|---|---|---|
| **OpenGL** | TierC | Same GLSL, same SSBOs, `glDispatchCompute` | **The GL path does not use `RunNovaPassSchedule`.** It inlines `glDispatchCompute` for passes 0,1,2,7\*,6,4,5,10 at `Renderer3D.cpp:2416-2490`. Two new passes means editing *both* that inline block and `RunNovaPassSchedule` (`:1378-1386`). New SSBO needs a `glNamedBufferStorage`/`SubData` beside `LightListBufferID` (`:2609-2614`) plus `GL_SHADER_STORAGE_BARRIER_BIT` (`:2619`). GLSL bindings 0–12 are all taken; **13 is free.** |
| **DX11** | TierC | Same reservoir, SM5-compatible | Shader is `cs_5_0` via `D3DCompileFromFile` (`DX11Nova.h:191`). t-space is partitioned t1–t3 (Upload buffers), t15–t46 (32 caller textures), t48+ (image reads) — documented at `DX11Nova.h:424-427`. **t10 is free**, inside the gap between t3 and t15. The `srvs[128]` array at `DX11Nova.h:438` needs a 4th `Upload()` entry; the `for(int i=0;i<3;++i)` loop is hardcoded. |
| **DX12** | TierC **or** TierA | Same reservoir | Root signature is a **fixed** `CD3DX12_ROOT_PARAMETER1 rootParams[8]` (`DX12Nova.cpp:128`) and is completely full: params 0–3 are CBV + three individual SRV root views for the `Upload()` buffers (`:131-134`), params 4–7 are four descriptor tables. **Adding a 4th structured buffer means growing the array to 9 and bumping `rsDesc.Init_1_1`.** `Upload()` hard-returns at `if (slot >= 3)` (`DX12Nova.cpp:470`) and `structuredBuffers` is `std::array<..., 3>` (`DX12Nova.h:86`). Descriptor heap has documented free space: indices 1–3 are unused padding (`DX12Nova.h:102`), and t-space t4–t14 is unclaimed. |
| **Vulkan** | TierC **or** TierA | Same reservoir, same HLSL→SPIR-V path | Binding is `offset + slot` with shifts u=0, b=7, t=16, s=200 (`compile_nova_spirv.py:55-58`), so HLSL `t10` lands on SPIR-V binding 26 — disjoint from every existing binding. `VulkanNova.h:60` documents the current slot contract as "0 = Instances, 1 = BVHNodes, 2 = LightIndices"; extend to 3. **`VulkanNova` compiles but is never invoked** — `Renderer3D.cpp:1931` notes Vulkan always lands on the raster path. So the Vulkan reservoir path is compile-verified only until Phase 4. |

**What TierC does without hardware RT — this is where the plan's claim needs care.** The claim
*"reservoir arithmetic is pure arithmetic — it compiles unchanged in GLSL"* is **true**, and I
verified the preconditions rather than assuming them:

- The GLSL twin already contains every feature the reservoir needs: an SSBO read
  (`glsl:45-63`), a BVH traversal with an explicit 32-deep stack and a `stackPtr < 30` guard
  (`:1885-1913`), a shadow-ray visibility test with self-exclusion (`:793-830`), and a screen-space
  velocity reprojection (`:2246-2268`).
- The shader uses **zero** wave/subgroup intrinsics today — `grep -c` for `WaveActive|WaveGetLaneIndex|
  WaveReadLane` in the `.hlsl` returns 0, and `subgroup|GL_KHR_shader_subgroup` in the `.glsl`
  returns 0. **MEASURED.** ReSTIR *DI* needs none (only *GI* compaction does), so this is not a
  barrier. It does mean there is no precedent to copy.
- The shader uses **zero** atomics today (`atomicAdd|atomicCompSwap` → 0). Also fine for DI.

What the plan overstates is not the *compilability* but the *cost model*. ReSTIR's entire payoff is
amortising one visibility test across M candidates. That holds on TierC — but a TierC visibility test
is `IsOccluded`, a full walk of the instance BVH (`:802-830`), not a hardware `TraceRay`. Per pixel
per frame Option A costs **1 initial shadow ray + up to 2 more for the 5-tap spatial pass = ~3 BVH
walks**, against today's capped `lightsToSample` of 1/2/4/4 by quality preset
(`Renderer3D.cpp:2612-2645`). **ESTIMATE.** So TierC Option A is roughly cost-neutral at Low/Medium
and a net win at High/Ultra, where today it is capped at 4 emitters regardless of scene size.

#### Effort and risks

**6–10 person-days. ESTIMATE**, decomposed:

| Item | Days | Driver |
|---|---|---|
| Reservoir struct + update + MIS combine, both shader twins | 2–3 | The math is published; the GLSL/HLSL duplication is the tax (§5.10) |
| Temporal pass + reprojection | 1 | Copied from `RunTemporalAccumulation`, low novelty |
| Spatial pass, 5 taps + normal gating | 1–2 | Neighbour selection and the octahedral pack |
| SSBO plumbing across 3 NVRHI backends + GL | 1.5–2.5 | DX12 root-signature growth is the fiddly part |
| `compile_nova_spirv.py` re-run + binding audit | 0.5 | Automated, but must be re-run |
| Acceptance test (§ below) | 1 | |

Risks, ranked:

1. **The DX12 root signature is full.** `rootParams[8]` is exactly consumed. Growing it touches the
   barrier/descriptor model that `DX12Nova.cpp:2902-2912` ordering depends on. Mitigation: add the
   reservoir as a **descriptor-table range appended to `ranges[]`** rather than a 9th root parameter,
   which avoids touching `rootParams`. **This is the recommended approach.**
2. **Three flush paths, not one.** `FlushDX11Nova` (`:1387`), `FlushDX12Nova` (`:1492`) and the GL
   inline block (`:2416-2490`) all hand-upload the light list separately. Missing one produces a
   backend that silently renders with no lights. There is precedent for this class of bug in the tree.
3. **`Upload()` destroys and recreates the buffer on every scene change** (`DX12Nova.cpp:476`) and
   calls `WaitForGpuIdle()` first (`:474`). For a reservoir that changes *every frame* this is a
   per-frame GPU stall. **The reservoir must NOT use `Upload()`** — it needs a persistent
   double-buffered resource written with `CopyBufferRegion` or written by the shader itself as a
   storage image. This is the single most important implementation note in this section.
4. **Bias from the existing light-model choice.** The `lightScale` correction at `glsl:1941` assumes
   uniform-over-emitters selection. A reservoir whose candidates are uniform-over-emitters preserves
   that assumption exactly, so Option A is safe. Option B does not — see its own risks.

#### What would prove it

The standalone harness in §5.4b, run against a CPU mirror of the exact reservoir code. It decides
one thing: **is the estimator unbiased?** If the mean of `W · contribution` over 10⁷ iterations does
not match the closed-form irradiance to within 3σ, the `wSum` normalisation is wrong and nothing
visual should be touched until it is right. That test needs no GPU, no window, and does not care
that the engine does not link.

The cheapest *GPU-side* proof, once the harness passes, is a single frame at Ultra with the bilateral
blur disabled and `u_AccumulationAlpha = 1.0`, comparing Option A's output against today's — the
image should be identical in expectation and visibly cleaner per sample.

### Option B — Area-correct analytic-shape sampling + light CDF

#### Approach

Option A's flaw is inherited from the engine, not introduced by ReSTIR: a large emissive quad is a
point light at its centre. Under any camera motion this reads as a flickering pool rather than a
soft surface, and no amount of variance reduction fixes a wrong model. Option B fixes the model
without introducing triangles.

The insight is that **the engine already knows the exact surface of every emitter analytically.**
`HitPrimitive` (`glsl:746-760`) dispatches on `MaterialParams.z` to `HitCube`, `HitSphere`,
`HitPlaneXZ`, `HitDisc`, `HitCylinder`, and further shapes below. The surface parameterisation needed
to sample a point on those same surfaces is the same parameterisation. So:

1. **CPU side, per emitter, once per scene change.** Compute the emitter's world-space surface area
   from its shape and transform, and emit an `EmissiveSurface { centroid, normal, area, shape,
   shapeParam, transform }` record. Build a CDF over areas (or a 64-entry alias table) so that
   sampling by area is O(1).
2. **Candidate generation.** Draw an emitter index from the alias table, then draw a point on that
   emitter's analytic surface with a shape-specific routine. `p̂(x)` is then the *real* area-solid-
   angle term `cos θ_l · cos θ_s / d²`, and `p(x)` is the emitter-selection probability times the
   surface-uniform density.
3. **Everything after that is Option A verbatim** — same reservoir, same 32-byte struct, same
   combine, same passes. The `bary` field becomes the surface parameter `(u, v)` in the emitter's
   local frame, which has the same meaning for a cube face (bilinear on a quad) as for a sphere
   (direction from a golden-ratio spiral) as for a cylinder (`(θ, y)`).

The BVH does not change. Shadow rays still traverse `BVHNodes` and hit the analytic primitive.

#### Files touched

Everything in Option A, plus:

| Path | Status | Change |
|---|---|---|
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | exists | Build `EmissiveSurface` array + alias table alongside the existing `m_LightIndices` build at `:1394-1398` / `:1499-1503`. Area math per shape, gated on `MaterialParams.z`. |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.h` | exists | `EmissiveSurface` struct; accessor |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` | exists | `SampleShapeSurface(shape, params, u, v) → (point, normal)`; alias-table read; replace the `glsl:1947` point-light line |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` | exists | Same |
| `Wasteland/src/Platform/NVRHI/{DX12Nova,DX11Nova,VulkanNova}.*` | exist | 5th structured buffer (alias table), or fold the surface array into the existing `Instances` upload to avoid growing the root signature further |

**Folding the surface record into the existing `RayTracingInstance` is strongly preferred.** It
carries `Emission`, `Min`, `Max`, `MaterialParams` (which holds the shape discriminator and radius)
and `WorldTransform` already. Adding `float SurfaceArea` and `vec3 SurfaceCentroid` to that struct
costs 16 B on a 272 B record, and avoids a 5th binding on a DX12 root signature that is already
full. **ESTIMATE:** this is the difference between a 1-day and a 4-day plumbing change.

#### Data layout

**Reservoir — identical to Option A, 32 B.** Unchanged. This is the payoff for structuring the work
as A-then-B.

**Emissive surface — 32 B per emitter**, either a new SSBO or appended to `RayTracingInstance`:

```c
struct alignas(16) EmissiveSurface   // 32 B
{
    glm::vec3 centroid;    // +0   world-space, for the CDF and for rejection
    float     area;        // +12  world-space surface area (m^2)
    glm::vec3 normal;      // +16  conservative world normal (one-sided emitters)
    uint32_t  shapeAndTwo; // +28  shape:8 | twoSided:1 | pad:23
};                           // = 32 B
```

`twoSided` matters for unbiasedness: a one-sided emitter must contribute zero when sampled from
behind, and folding that into the flag lets the shader reject before computing `p̂` rather than
weighting by zero.

**Light CDF / alias table — 8 B per emitter**, or 4 B with a CDF + binary search:

```c
struct LightAliasEntry { uint32_t alias; float prob; };  // 8 B
```

For N emitters this is 8N bytes. With a shared CDF instead it is 4N plus a binary search in the
shader. **ESTIMATE:** alias table preferred — a binary search in a divergent shader is a real cost
and alias tables are branch-free.

A shape's own parameters (`Min`, `Max`, `MaterialParams.w`, `Min.w`) are read from
`Instances[emitterIndex]`, so the surface buffer does not duplicate them.

**BVH — unchanged.** 32 B `BVHNodeGPU`, `BVH_LEAF_SIZE = 4`, median split. No new BVH is the
specific architectural win of Option B over Option C, and it is why B is 6–10 days cheaper.

#### Backend notes

The backend matrix is **identical to Option A** — same passes, same bindings, same TierC
availability. Three differences:

- **GLSL/HLSL twin growth.** `SampleShapeSurface` is the largest new function in either option:
  a cube-face bilinear, a sphere Fibonacci point, a disc polar sample, a cylinder `(θ, y)` sample,
  a torus `(θ, φ)`, a cone disc, a plane/grid bilinear. **ESTIMATE: 120–180 lines of GLSL**, doubled
  for the HLSL twin. This is the dominant cost of Option B and the reason §5.10's "author once,
  compile twice" decision gates it. **Do not start Option B before §5.10 is settled.**
- **OpenGL** additionally needs the area computed on the CPU where the transform is known; there is
  no GPU-side shape instantiation to piggyback on, because `BuildAndFlattenBVH` works from
  `RayTracingInstance` AABBs (`Renderer3D.cpp:262-263`) which do not encode surface area.
- **TierC without hardware RT.** Unchanged from A and **genuinely viable**: sampling an analytic
  surface needs no intersection at all, which is *cheaper* than Option C's triangle fetch. The
  visibility test that follows is the same `IsOccluded` walk. So Option B is the option where the
  plan's "TierC ReSTIR is genuinely available and is the highest-value TierC work" claim holds
  closest to true.

#### Effort and risks

**12–18 person-days. ESTIMATE** = Option A's 6–10, plus:

| Item | Days | Driver |
|---|---|---|
| `SampleShapeSurface` for 8 shapes, both twins | 3–5 | Volume is the issue, not difficulty |
| CPU surface-area formula per shape + transform | 1–2 | Exact for quad/disc/cylinder/plane; closed-form for sphere; **torus and cone need care** |
| Alias table build | 0.5 | Well-trodden |
| Rebuilding `p̂`/`p` and re-deriving the RIS weights | 2–3 | The actual MIS work; **this is where the bias risk lives** |
| Acceptance test extension (area sampling, not point) | 1–2 | New ground truth |

Risks:

1. **Area formulas are wrong in a way that is hard to see.** A torus's true area is
   `4π²Rr`; getting the `R` wrong yields a plausible-looking light. Mitigation: assert each shape's
   computed area against a hand-computed value in the standalone harness — **the same harness that
   tests the reservoir now also tests the geometry**, which is free.
2. **Transform with non-uniform scale breaks the area.** `area_world = area_local · |det J|`, and the
   engine already computes `GetWorldScale(transform)` (`Renderer3D.cpp:2558`, `3178`) as an
   *average* of the three axes — which is wrong for area. **This is a real trap**; the surface-area
   code must use the full `abs(det(...))`, not `GetWorldScale`.
3. **The `p̂` recompute is now a function of the receiver.** `p̂_q(x) = cos θ_s · cos θ_l / d²`. Both
   cosines depend on the *receiving pixel's* normal, which is why temporal and spatial combine can
   reuse a sample at all. Getting the sign convention wrong for a two-sided emitter produces light
   leaking through walls. The harness must include a two-sided case.
4. **This option makes the analytic-light question urgent**, because now you have two light systems
   with different quality levels. See the Recommendation.

#### What would prove it

The same standalone harness, extended: an emitter of known analytic shape (a 1×1 quad, area exactly
1 m²) at a known distance, sampled through the *real* `SampleShapeSurface` code transpiled to the
harness, with the analytic irradiance computed in closed form. Assert the mean matches to 3σ.

That is a stronger test than Option A's because the ground truth is independent of the sampler: for
a quad the expected solid angle is analytic, so an area-formula bug shows up as a bias the test
catches rather than as a subtly wrong-looking light.

### Option C — Triangle light buffer + dedicated light BVH

#### Approach

**This is the plan's literal §5.4 step 1, and it is the option I recommend *against* building now.**

Tessellate each emitter's `ProceduralMesh` (already memoised CPU-side and const-referenced,
`ProceduralMeshes.h:78-88`) into world-space triangles, emit a 32-byte triangle record, build a
second BVH over triangles, sample by area.

**The blocker is not the triangle buffer — it is that Nova has no triangle intersector.** I verified
this: `grep -n 'Triangle|IntersectTriangle'` over `Renderer3D_NovaRenderer.glsl` returns **zero
matches. MEASURED.** Every primitive in the ray tracer is analytic (`HitPrimitive`, `glsl:746-760`,
eight shapes). There is no triangle path to hang a triangle light buffer off.

So Option C produces a light buffer the ray tracer cannot shadow-test consistently: the sample point
comes from a triangle, the occluder test goes through `HitPrimitive` against the analytic shape.
Those disagree at the tessellation error, and the disagreement is a *bias*, not a rounding difference.
Fixing it properly requires adding a triangle intersector to Nova — which is §5.3's work, plus a
triangle BVH traversal, plus keeping the analytic path for non-emissive geometry.

#### Files touched

Everything in Option A, plus:

| Path | Status | Change |
|---|---|---|
| `Wasteland/src/Wasteland/Renderer/ProceduralMeshes.h` / `.cpp` | exist | Triangle extraction from the memoised `ProceduralMesh` (`Indices`, `:44`) |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | exists | Emit `LightTriangleBuffer` + build `LightBVHBuffer` |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.{glsl,hlsl}` | exist | Triangle intersector + triangle BVH traversal, **both twins** |
| `Wasteland/src/Platform/NVRHI/{DX12Nova,DX11Nova,VulkanNova}.*` | exist | 2 more structured buffers (triangle data + triangle BVH) |

Note this **adds two bindings** to a DX12 root signature that is already exactly full — see the
Option A backend notes. This is the single largest plumbing item in any of the three options.

#### Data layout

**Light triangle — 48 B** (32 B only if the normal is reconstructed from area, which is lossy and
not recommended):

```c
struct alignas(16) LightTriangle   // 48 B
{
    glm::vec3 p0;       // +0   world-space
    glm::vec3 n;        // +16  world-space face normal
    float     area;     // +32  m^2
    uint32_t  emitter;  // +36  index into EmissiveSurface[] for radiance
    glm::vec2 uv0;      // +40  UV at p0, for textured emission later
    glm::vec2 uv1;      // +48  UV at p1
};                           // = 56 B with full UVs; 48 B without the second UV pair
```

**Reservoir — unchanged, 32 B.** Same struct as A and B; a triangle sample stores
`(emitter, triangleIndex, bary)`, which is the same 8-byte `bary`/index payload.

**Light BVH — reuses `BVHNodeGPU` exactly, 32 B** (`Renderer3D.cpp:61-65`), and
`BuildBVHRecursive` (`:296-395`) is generic over `BVHPrim`, so the builder is reusable verbatim.
That is genuinely the one strong argument in Option C's favour: the BVH machinery is free.

#### Backend notes

| Backend | Tier | Notes |
|---|---|---|
| **DX12 / Vulkan** | TierA | Requires hardware RT for the visibility test to be affordable. Gated on §5.3 landing. NVRHI symbols verified present in `Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h`: `dispatchRays` (`:3451`), `buildBottomLevelAccelStruct` (`:3478`), `buildTopLevelAccelStruct` (`:3506`), `buildTopLevelAccelStructFromBuffer` (`:3524`), `class IAccelStruct` (`:1737`), `struct AccelStructDesc` (`:1714`), `InstanceDesc& setBLAS(IAccelStruct*)` (`:1690`), `BindingSetItem::RayTracingAccelStruct(uint32_t, rt::IAccelStruct*)` (`:2244`), `NVRHI_BINDING_LAYOUT_ITEM_INITIALIZER(RayTracingAccelStruct)` (`:1953`), `BufferDesc::setIsAccelStructBuildInput`/`setIsAccelStructStorage` (`:718-719`), `createShaderTable` (`:2881`), `createBuffer` (`:3693`), `struct ComputeState` (`:2727`). **All confirmed. MEASURED.** |
| **OpenGL / DX11** | TierC | Would brute-force every triangle per shadow ray — hopeless. Effectively unusable. |

**Critical NVRHI finding.** `DispatchRayQuery`, `TraceRay` as a command-list entry,
`RayQueryCandidateFlags` and `DispatchRaysIndirect` are **absent from this header — 0 matches
each. MEASURED.** Inline ray tracing and ray-query are therefore *not* reachable through this NVRHI
version's C++ surface. That directly contradicts `RenderCapabilities.cpp:54-55`, which asserts
`inlineRaytracing = true; rayQuery = true` unconditionally. See Corrections.

#### Effort and risks

**18–28 person-days, plus §5.3's 10–20. ESTIMATE.**

| Risk | Severity |
|---|---|
| Tessellation/analytic mismatch is a **bias**, not rounding | High — and unfixable without a triangle intersector in Nova |
| Two more bindings onto a full DX12 root signature | High — plumbing, but deterministic |
| Memory: a 48×32 sphere is ~3000 tris → 144 KB per emitter at 48 B | Medium |
| Per-scene-change triangle + BVH rebuild | Medium |
| Delivers TierA only; OpenGL — the TierC story's centrepiece — gets nothing | High for this engine |

#### What would prove it

**Nothing standalone.** There is no CPU-only test that proves a triangle light buffer is *useful*,
because its value depends entirely on hardware RT existing and being affordable. The honest answer is
that Option C cannot be evaluated until §5.3 lands, which is precisely why it should not be scheduled
ahead of it.

### Recommendation

**Build A, then B. Defer C.**

The ordering is forced by a fact about this repo, not by preference: **emissive contribution here is
a single `float` per instance, and the engine has no shader graph.** That makes §5.4b cheap (see
below), which in turn makes the *only* real cost of §5.4 the resampling math — which is published,
testable, and identical on all four backends. So §5.4 is **15 days, not 40**, and the plan's
headline risk is answered before the work starts.

Concretely:

1. **Phase 5 (light buffer) collapses from a risk to a 1–3 day task** — see §5.4b. The
   `LightIndices` list is already built at `Renderer3D.cpp:1397`. Do not re-plan around it.
2. **Phase 6a: Option A**, 6–10 days. Its acceptance test is the standalone reservoir harness, which
   runs before the engine links and settles unbiasedness.
3. **Phase 6b: Option B**, +6–8 days, *after* §5.10 settles the shader pipeline — because
   `SampleShapeSurface` is exactly the kind of large function that should be authored once.
4. **Option C only after §5.3 lands**, and only if the triangle path is being built anyway.
5. **Independently and cheaply: fix the analytic-light cap.** It is a 2-day fix (below) and it is a
   *correctness* bug users hit today, not a §5.4 concern. Do not let §5.4's schedule absorb it.

**Effort summary: §5.4 is ~12–18 person-days for A+B on all four backends, not 15–25 excluding a
light buffer that turns out to need ~2. ESTIMATE.**

### Corrections to the plan

1. **§4 / §5.4: "our material system is shader-graph-style, so emissive contribution often cannot be
   statically analysed" is FALSE for this repository.** `MaterialComponent`
   (`Wasteland/src/Wasteland/Scene/Components.h:640-714`) is a flat POD: `EmissionColor` is a
   `glm::vec4` at `:668`, `EmissionIntensity` is a `float` at `:677`, and the comment at `:669-673`
   states plainly that `EmissionIntensity == 0` is the gate and there is no separate flag.
   `grep -rl 'ShaderGraph|shadergraph|MaterialGraph|NodeMaterial'` over `Wasteland/src` and
   `DemonCore-Editor` returns **0 files. MEASURED.** There is no material graph, no node graph, no
   shader permutation. The `VisualBlocks/` directory is a behaviour-tree/logic graph
   (`NodeLibrary.cpp`, `GraphSerializer.cpp`), unrelated to shading.
   **Consequence: §5.4b is not a schedule risk. The "15 days or 40" framing should be struck.**

2. **§2 claims "No hardware ray tracing … Every sampling feature below is gated on this" — correct,
   but §5.4 then says ReSTIR "compiles unchanged in GLSL" with no RT.** These are in tension and the
   plan does not resolve it. The resolution: **Option A and B genuinely need no RT** (candidate
   generation is arithmetic + analytic surface sampling; visibility is the existing `IsOccluded`
   walk), while **Option C genuinely does.** The plan conflates them. Only §5.4 *option C* is gated
   on §5.3.

3. **§7 open question 3 and §4: the 8-light cap.** `MaxAnalyticLights = 8` is at
   **`Renderer3D.h:298`**, not `:209` as the plan states. **MEASURED.** The overflow behaviour is
   worse than "a cull": `PushAnalyticLight` (`Renderer3D.cpp:2849-2854`) **silently returns** when
   the vector is full — no counter, no log, no error. `Scene::SubmitSceneLights`
   (`Scene.cpp:1536-1567`) submits **all directionals, then all points, then all spots, then all
   areas**, so a scene with 8 point lights **starves every spot and area light in the scene**, and
   the entity iteration order is entt storage order, not authored priority — as the comment at
   `Scene.cpp:1529-1534` already admits. **The plan asks "which analytic lights will exceed that
   cap and need reservoir handling?" The answer is: none of them should — this needs a cull or a
   light-list build, not a reservoir.** ReSTIR DI resamples *emissive geometry*; it does not
   resample point/spot/directional lights. Plumbing analytic lights into the same reservoir would
   require sampling a point light's solid angle, which is a delta distribution — a different
   estimator entirely.

4. **§5.4 "new Nova passes 12 (temporal) and 13 (spatial, 5 taps)" — the IDs are free, confirmed.**
   The dispatch switch at `Renderer3D_NovaRenderer.glsl:2671-2680` uses 0, 1, 2, 4, 5, 6, 7, 8, 9,
   10. **MEASURED.** So 12 and 13 are available. Note **3 is also unused** — a gap in the switch, not
   a reserved ID. Either would work; 12/13 is the right choice for readability.

5. **§5.4 says the light buffer needs "a BVH over it".** For Options A and B that is **wrong and
   unnecessary** — the existing `BVHBuffer` already indexes every emitter, because emitters *are*
   instances. A second BVH is only needed for Option C. This is a material over-estimate of effort in
   the plan.

6. **`RenderCapabilities` claims capabilities it never probes.** `RenderCapabilities.cpp:53-57` and
   `:62-66` set `hardwareRT = true; inlineRaytracing = true; rayQuery = true; maxRecursionDepth = 31;
   tensorCores = true` **unconditionally** for DX12 and Vulkan, with no device query. **MEASURED.**
   Combined with the NVRHI finding above — this header has **no** `DispatchRayQuery` and **no**
   `RayQueryCandidateFlags` — `rayQuery = true` is currently a lie the plan's §5.3 step 6 ("gate on
   `GetRenderCapabilities().hardwareRT`") would gate on. **§5.3 must add a real capability probe
   before anything gates on it**, or the gate will pass on hardware that cannot run the feature.

7. **§5.4 "Files: `nova_restir_common.glsl`" implies a GLSL include.** There is no include mechanism
   in this shader pipeline — `Renderer3D_NovaRenderer.glsl` is a single 2682-line file with no
   `#include`, and the `.hlsl` twin is 2724 lines of hand-maintained parallel text. A new shared
   file needs a build step that concatenates it. **This is §5.10's problem, and §5.4 inherits it.**

8. **DX12's descriptor/root model is tighter than the plan assumes.** `rootParams[8]`
   (`DX12Nova.cpp:128`) is exactly full, and `Upload()` hard-returns at `slot >= 3`
   (`DX12Nova.cpp:470`). Any §5.4 buffer must be added as a **descriptor-table range**, not a new
   root parameter, or the DX11/DX12 flush ordering described at `Renderer3D.cpp:2902-2912` is at
   risk. The plan does not mention this constraint.

9. **A per-frame buffer hazard the plan does not anticipate:** `DX12Nova::Upload()` calls
   `WaitForGpuIdle()` then destroys and recreates the resource (`DX12Nova.cpp:474-476`). A reservoir
   changes every frame. **It must not use `Upload()`.** Using it would introduce a full GPU stall
   per frame — a performance regression far worse than the noise ReSTIR removes.

**Verification note:** this entire section is static analysis of a tree that **does not link**
(`Scene.cpp` fails on `Navigation::`; 41 `Renderer3D` symbols have no implementation — plan §7).
Nothing here is MEASURED-on-GPU. The only claims labelled MEASURED are ones I verified by reading
source or running `grep`/`wc` against it. Per plan §8, every option remains **compile-verified only**
until a screenshot exists, and the reservoir's unbiasedness remains unproven until the standalone
harness in §5.4b runs.

---

## §5.4b Emissive-triangle light buffer (the schedule risk)

The plan says: *"ReSTIR needs a light buffer of emissive triangles. Our material system is
shader-graph-style, so emissive contribution often cannot be statically analysed… Prototype this
first. It decides whether §5.4 is 15 days or 40."*

### ANSWER FROM THE CODE: the risk does not exist in this repository.

**Can emissive output be statically enumerated from CPU-side data? YES — trivially, and it is
already done today, on every backend, every frame.**

Three independent lines of evidence, all MEASURED by reading source:

**(1) The material is a flat POD struct, not a graph.**
`MaterialComponent` — `Wasteland/src/Wasteland/Scene/Components.h:640-714`:

```cpp
glm::vec4 EmissionColor = {1.0f, 1.0f, 1.0f, 1.0f};   // :668
float    EmissionIntensity = 0.0f;                    // :677
```

The comment at `:669-673` states the design outright: *"EmissionIntensity … 0 — the default — means
the entity emits no light, regardless of EmissionColor. This is the field that gates emission, not
a separate has-emission flag: there is no separate flag, so this doubles as one."* So the predicate
is `EmissionIntensity > 0`, evaluated on the CPU, with no shader involvement.

**(2) There is no shader graph, no material graph, and no node-based shading anywhere in the tree.**
`grep -rl 'ShaderGraph|shadergraph|MaterialGraph|NodeMaterial'` over both `Wasteland/src` and
`DemonCore-Editor` returns **0 files. MEASURED.** The only graph system in the engine is
`Wasteland/src/Wasteland/VisualBlocks/` — and it is a *behaviour tree* (`BehaviorTree.cpp`,
`NodeLibraryFlow.cpp`, `GraphSerializer.cpp`), which serialises gameplay logic, not shading. There is
no permutation explosion because there is no permutation.

**(3) The CPU→GPU transfer is one already-written line, and the shader consumes it in one already-
written line.**

CPU side, at every one of the four instance-push sites — `Renderer3D.cpp:2553-2556` (procedural
primitives), `:3172-3175`, `:3296-3299`, and `:3488` (the zero case):

```cpp
instance.Emission = glm::vec4(material.EmissionColor.x,
                              material.EmissionColor.y,
                              material.EmissionColor.z,
                              material.EmissionIntensity);
```

Shader side, `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl:1867` — the **only**
emission assignment in the entire 2682-line file:

```glsl
info.emission = inst.Emission.xyz * inst.Emission.w;
```

`grep -n 'emission' Renderer3D_NovaRenderer.glsl` returns four hits total: the struct field
declaration (`:1203`), this assignment (`:1867`), its consumption (`:1970` `directLight +=
h.emission;`), and one unrelated comment (`:2223`). **There is exactly one multiply and one add.**
Emission is a constant per instance. It is not computed in a shader. It cannot be "not statically
analysable", because there is nothing to analyse.

### And the light list is ALREADY BUILT

The plan's §5.4 step 1 asks for a light buffer as if it must be created. **It already exists, on all
three backends, rebuilt whenever the scene changes.**

`Renderer3D.cpp:1394-1398` (DX11 flush) and `:1499-1503` (DX12 flush), verbatim:

```cpp
s_Data.m_LightIndices.clear();
for(size_t i=0;i<s_Data.m_BVHPrimOrder.size();++i) {
    auto& v=s_Data.m_ReorderedInstances[i];
    if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
}
```

`Emission.w` **is** `EmissionIntensity`. That single comparison is the entire extraction. The GL path
does the same at `:1729-1734`, and all three upload it as `LightListBuffer` — GLSL binding 3
(`glsl:60-63`), HLSL `t3`, documented at `VulkanNova.h:60` as *"slot: 0 = Instances, 1 = BVHNodes,
2 = LightIndices"*.

The count reaches the shader as `u_LightCount` (`Renderer3D.cpp:1406`, `:1511`).

### The one genuine gap: there are no triangles to enumerate

This is the real finding, and it is **not** the risk the plan named.

`grep -n 'Triangle|IntersectTriangle' Renderer3D_NovaRenderer.glsl` → **0 matches. MEASURED.** The
ray tracer has no triangle intersector at all. `HitPrimitive` (`glsl:746-760`) dispatches on
`MaterialParams.z` to `HitCube`, `HitSphere`, `HitPlaneXZ`, `HitDisc`, `HitCylinder` and further
analytic shapes below — **eight analytic primitives, zero triangles.**

So "emissive **triangles**" is the wrong primitive for this engine, and building a triangle light
buffer would mean *first* building triangle intersection into Nova. Options A and B in §5.4 exist
precisely to avoid that.

### What CPU-side geometry does exist

| Source | Location | CPU-side triangle data? |
|---|---|---|
| `ProceduralMesh` | `ProceduralMeshes.h:25-58` | **Yes** — `Positions`, `Normals`, `TexCoords`, `Indices`, all `std::vector`, memoised and returned by const reference (`:78-88`). `TriangleCount()` at `:58`. |
| `MeshData` / `MeshVertex` | `MeshLoader.h:21-59` | **Yes** — de-indexed positions/normals/UVs, `Indices`, `BoundsMin/Max`, `RecomputeBounds()`. Explicitly designed for headless verification: *"a test can check vertex counts, normals, and triangle winding with no window at all."* |
| `VertexArray` | `VertexArray.h:29-77` | **No** — GPU binding state only. `GetVertexBuffers()`/`GetIndexBuffer()` return GPU objects; there is no CPU readback of vertex data. |
| `RayTracingInstance` | `Renderer3D.cpp:42-58` | **No** — 272 B of transform/AABB/material. It carries `Min`/`Max` AABBs, not vertices. |
| `AnalyticLightData` | `Renderer3D.cpp:186-205` | N/A — point/spot/directional/area lights have no geometry (the comment at `Components.h:1836-1838` says so explicitly: *"Unlike emissive materials they need no geometry."*) |

So the CPU-side triangle data **exists and is well-formed** — for procedural primitives and for
imported OBJ meshes. What does *not* exist is a triangle path in the tracer that could consume it.
That is a §5.3 problem wearing a §5.4 hat.

### Options

| Option | Summary | Effort | Forecloses |
|---|---|---|---|
| **A** — Use the existing `LightIndices` list as-is | Zero new CPU code. Emitters are already listed and uploaded. Add only area/normal metadata if Option B needs it. | **0.5–1 day** | Nothing. This is the floor. |
| **B** — Enrich each emitter with analytic surface metadata | Per-emitter `EmissiveSurface` (32 B) + alias table (8 B), computed from `MaterialParams.z` shape and transform. Enables area-correct sampling without triangles. | **4–7 days** | No per-triangle weighting; no textured emission. Requires `SampleShapeSurface` in both shader twins. |
| **C** — Tessellate to triangles from `ProceduralMesh` | Enumerate `ProceduralMeshes::*` triangle indices, emit world-space triangles + a triangle BVH. | **12–20 days + §5.3** | Nothing — but see below. |

### Option A — Use the existing light list as-is

#### Approach

`LightIndices` already contains every emitter, sorted by BVH leaf order, uploaded to all backends.
Option A is the assertion that this *is* the light buffer, plus a comment saying so. The only work
is the reservoir (§5.4 Option A).

The one thing worth adding even here: an explicit counter that reports how many emitters were found
and how many were dropped, so the `Emission.w > 0` filter is observable rather than silent. The
`MaxAnalyticLights` cap learned this lesson the hard way (see Corrections); do not repeat it.

#### Files touched

**None on the CPU side.** Shader-side files as in §5.4 Option A.

#### Data layout

Unchanged: `LightListBuffer`, 4 B per emitter (`int`), GLSL binding 3 / HLSL `t3`, already uploaded
at `Renderer3D.cpp:1401`, `:1506`, `:2611`.

#### Effort and risks

**0.5–1 day. ESTIMATE.** Risk: essentially none. The one real risk is a scene with *thousands* of
emissive instances — `LightIndices` is a flat `std::vector<int>` with no cap, and each candidate draw
is an indirect load. **ESTIMATE:** fine to ~10⁴ emitters; beyond that the alias table in Option B
becomes necessary for cache behaviour, not correctness.

#### What would prove it

A one-line log of `m_LightIndices.size()` per scene change, next to the existing
`WL_CORE_INFO` diagnostic in `RefreshRenderCapabilities()` (`RenderCapabilities.cpp:70-75`). If a
scene known to contain emissive entities reports zero emitters, extraction is broken.

### Option B — Enrich each emitter with analytic surface metadata

#### Approach

Keep Option A's list. Attach to each emitter the surface data area-correct sampling needs, derived
on the CPU where the transform is known:

1. Read the shape discriminator from `instance.MaterialParams.z` (set at `Renderer3D.cpp:2548` from
   `PrimitiveShape`) and the shape parameters from `Min`/`Max`/`MaterialParams.w`/`Min.w`
   (`:2549-2552`, documented in the comment at `:2549-2550`).
2. Compute world-space surface area. Exact for plane/grid/disc/quad (`w·h`, `πr²`); closed-form for
   sphere (`4πr²`); `2πr·h` for a cylinder side, `2πr·√(r²+h²)` for a cone lateral surface, `4π²Rr`
   for a torus. **Use `abs(det(J))` of the transform for the local→world area scale — do NOT use
   `GetWorldScale()`** (`Renderer3D.cpp:2558`), which averages the three axes and is wrong for area.
3. Emit an `EmissiveSurface` record and build a 64-entry-per-emitter alias table weighted by area.

#### Files touched

| Path | Status | Change |
|---|---|---|
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | exists | Area computation beside the `m_LightIndices` build (`:1394-1398`, `:1499-1503`, `:1729-1734`); surface + alias upload |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.h` | exists | `EmissiveSurface` struct, accessor |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.{glsl,hlsl}` | exist | `SampleShapeSurface()`; alias read |

#### Data layout

```c
struct alignas(16) EmissiveSurface   // 32 B, one per emitter
{
    glm::vec3 centroid;    // +0   world-space
    float     area;        // +12  m^2, pre-multiplied by |det J|
    glm::vec3 normal;      // +16  conservative world normal
    uint32_t  shapeAndTwo; // +28  shape:8 | twoSided:1 | pad:23
};

struct LightAliasEntry { uint32_t alias; float prob; };   // 8 B, one per emitter
```

Folding `EmissiveSurface` into `RayTracingInstance` (making it 288 B, +16) avoids growing the
already-full DX12 root signature. **ESTIMATE:** strongly preferred.

#### Backend notes

Identical to §5.4 Option B. The key property: **candidate generation needs no intersection**, so
this path works identically on OpenGL (GLSL 430 compute, same SSBOs), DX11 (`cs_5_0`,
`DX11Nova.h:191`), DX12 (`cs_5_0`, `DX12Nova.cpp:62-63`), and Vulkan (HLSL→SPIR-V via
`compile_nova_spirv.py`, shifts u=0/b=7/t=16/s=200). No hardware RT, no DXC, no NVRHI RT symbols.

#### Effort and risks

**4–7 days. ESTIMATE.** Risks: wrong area formula (mitigated by the harness asserting each shape's
area against a hand-computed value); non-uniform scale (mitigated by using `det(J)`, flagged above);
two-sided emitters leaking light through walls (mitigated by the `twoSided` flag + a two-sided test
case).

#### What would prove it

Extend the standalone harness to assert, for each of the 8 shapes, that the CPU area formula matches
a closed-form reference. This is a **pure-CPU test with no GPU and no engine** — it can run today,
while the engine does not link, and it settles the question the plan says must be settled first.

### Option C — Tessellate to triangles from `ProceduralMesh`

#### Approach

For each emitter, walk `ProceduralMesh::Indices` (`ProceduralMeshes.h:44`), transform the three
corners to world space, emit a triangle record, build a triangle BVH with the existing
`BuildBVHRecursive` (reusable as-is — it is generic over `BVHPrim`).

Then the problem the plan does not name: **the shadow ray must hit the same triangle.** Nova has no
triangle intersector (`glsl`, 0 matches for `Triangle`). So either the light triangles are also
added to the trace path — which is §5.3's BLAS/TLAS work — or the sample point comes from a triangle
while the occluder test hits the analytic shape, and the tessellation error between them becomes a
**bias**.

#### Files touched

Everything in §5.4 Option C, plus a triangle intersector in both shader twins and a triangle BVH
traversal. Both are §5.3 deliverables.

#### Data layout

32 B per triangle is achievable with an edge-difference encoding (`vec3 p0; float area; vec3 e1; uint
matIdx;` with `e2` reconstructed from area and normal — lossy, so 48 B with an explicit `vec3 n` is
the honest number). BVH nodes reuse `BVHNodeGPU` exactly (32 B, `Renderer3D.cpp:61-65`).

#### Backend notes

TierA on DX12/Vulkan only; GL/DX11 would brute-force. Requires §5.3. The NVRHI symbols verified
present (`nvrhi.h:3451` `dispatchRays`, `:3478` `buildBottomLevelAccelStruct`, `:3506`
`buildTopLevelAccelStruct`, `:1737` `IAccelStruct`, `:1714` `AccelStructDesc`, `:1690`
`InstanceDesc::setBLAS`, `:2244` `BindingSetItem::RayTracingAccelStruct`) are all in place — **but
`DispatchRayQuery`, `RayQueryCandidateFlags` and `DispatchRaysIndirect` are absent from this header.
MEASURED, 0 matches each.** So inline RT and ray-query are not available through this NVRHI version.

#### Effort and risks

**12–20 days on top of §5.3's 10–20. ESTIMATE.** Risks: the bias mismatch above; Tessellation
memory (a subdivided sphere at 48×32 is ~3000 triangles → 3000 × 32 B = 96 KB per emitter); and
per-scene-change rebuild cost.

#### What would prove it

Nothing short of §5.3 landing. There is no standalone test that proves a triangle light buffer is
*useful*, because its value depends entirely on hardware RT existing.

### Recommendation

**Option A now (0.5–1 day). Option B when §5.4 Option B is built. Option C never, until §5.3 lands
and the triangle path exists anyway.**

The decisive facts, all MEASURED:

- Emission is `EmissionColor.rgb × EmissionIntensity` — two struct fields, `Components.h:668` and
  `:677`. One multiply in the shader, `glsl:1867`.
- No shader graph exists anywhere: `grep -rl 'ShaderGraph|shadergraph|MaterialGraph|NodeMaterial'`
  → **0 files**.
- The light list is **already built and uploaded** on all three backends: `Renderer3D.cpp:1397` and
  `:1502` are a one-line `if (v.Emission.w > 0)`.
- `MaxAnalyticLights = 8` is at **`Renderer3D.h:298`** — not `:209`.

**The schedule risk the plan names does not exist here.** §5.4's cost is the resampling math, which
is published, testable standalone, and identical across all four backends. **§5.4 is 12–18
person-days for A+B, not "15 or 40". ESTIMATE.** The plan should strike the risk from §4, drop
Phase 5 as a separate gate, and move directly to Phase 6a.

The risk that *does* exist, and which the plan does not name, is that **there are no triangles in the
ray tracer at all.** That belongs in §5.3, and it is a larger risk than anything in §5.4.

**One independent correction, 2 days, orthogonal to §5.4:** the analytic-light cap. `PushAnalyticLight`
(`Renderer3D.cpp:2849-2854`) silently returns once 8 lights are collected — no log, no counter. And
`Scene::SubmitSceneLights` (`Scene.cpp:1536-1567`) submits all directionals, then all points, then
all spots, then all areas, so **8 point lights starve every spot and area light in the scene**, with
survival decided by entt storage order rather than authored importance. This is a live correctness
bug in the shipping renderer, it is not a §5.4 concern, and it should not wait on §5.4. The fix is a
distance/importance cull in `SubmitSceneLights` plus a logged overflow count — not a reservoir,
because a point light's solid angle is a delta distribution that ReSTIR DI's area estimator cannot
represent.

### Corrections to the plan

1. **"Our material system is shader-graph-style, so emissive contribution often cannot be statically
   analysed" — FALSE in this repository.** No shader/material/node graph exists (`grep` → 0 files).
   `MaterialComponent` (`Components.h:640-714`) is a flat POD; emission is
   `EmissionColor × EmissionIntensity` (`:668`, `:677`); the shader consumes it in one multiply
   (`glsl:1867`). **The §4 risk and §7 open question 4 are both void.** Strike them.

2. **"ReSTIR needs a light buffer of emissive triangles" — the wrong primitive.** Nova has zero
   triangle intersection (`grep 'Triangle'` over the GLSL → 0 matches). It traces eight *analytic*
   shapes via `HitPrimitive` (`glsl:746-760`). A triangle light buffer cannot be shadow-tested
   consistently by the current tracer. The correct unit is the **analytic emitter**, which is what
   `LightIndices` already contains.

3. **The light buffer already exists.** `Renderer3D.cpp:1394-1398` (DX11), `:1499-1503` (DX12),
   `:1729-1734` (GL) build `m_LightIndices` from `Emission.w > 0` and upload it as `LightListBuffer`.
   Phase 5 as a separate prototype gate is unnecessary; the prototype *is* shipped code.

4. **§5.4b's cost framing ("15 days or 40") should become a flat estimate.** §5.4 excluding the light
   buffer is 6–10 days (Option A) and 12–18 with area-correct sampling (Option B). ESTIMATE.

5. **`MaxAnalyticLights = 8` is at `Renderer3D.h:298`, not `:209`.** The plan cites `:209` in both §4
   and §7 open question 3. **MEASURED.**

6. **Open question 3 ("which analytic lights will exceed that cap and need reservoir handling?") is
   the wrong question.** ReSTIR DI resamples *emissive geometry*. Point/spot/directional lights have
   delta solid angles and are not reservoir candidates. The real defect is the silent overflow in
   `PushAnalyticLight` (`Renderer3D.cpp:2849-2854`) plus the type-ordered starvation in
   `Scene::SubmitSceneLights` (`Scene.cpp:1536-1567`). Fix it as a cull, in 2 days, independently.

7. **The plan's §2/§5.4 tension on RT gating is unresolved in the text.** ReSTIR Options A and B
   need **no** hardware RT and run on all four backends at TierC; only Option C needs §5.3. The plan
   states both "every sampling feature is gated on this" and "compiles unchanged in GLSL with no RT"
   without reconciling them. This map's options are separated on exactly that line.

8. **The plan should not gate on `GetRenderCapabilities().hardwareRT` until it is a real probe.**
   `RenderCapabilities.cpp:53-57` and `:62-66` set `hardwareRT`, `inlineRaytracing`, `rayQuery`,
   `maxRecursionDepth = 31` and `tensorCores = true` **unconditionally** for DX12 and Vulkan with no
   device query. **MEASURED.** Worse, the vendored NVRHI header has **no** `DispatchRayQuery` and
   **no** `RayQueryCandidateFlags` (**0 matches** in
   `Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h`), so `rayQuery = true` is currently
   unsupportable through that API. §5.3 step 6's gate would pass on hardware that cannot run the
   feature.

9. **DX12's root signature is exactly full and the plan does not account for it.**
   `CD3DX12_ROOT_PARAMETER1 rootParams[8]` (`DX12Nova.cpp:128`) has all eight slots consumed
   (`:131-150`), and `Upload()` hard-returns at `slot >= 3` (`DX12Nova.cpp:470`) with
   `structuredBuffers` a `std::array<..., 3>` (`DX12Nova.h:86`). Any new buffer must be a
   **descriptor-table range**, not a root parameter. The plan's "Files: `nova_restir_common.glsl`"
   understates this plumbing.

10. **A per-frame GPU-stall hazard the plan does not anticipate.** `DX12Nova::Upload()` calls
    `WaitForGpuIdle()` (`DX12Nova.cpp:474`) and destroys/recreates the resource (`:476`). A reservoir
    changes every frame; using `Upload()` for it would stall the GPU every frame — a regression worse
    than the noise ReSTIR removes. The reservoir needs a persistent double-buffered resource.

11. **`§5.4 "a BVH over it" is unnecessary for Options A and B.** Emitters *are* instances; the
    existing `BVHBuffer` already indexes them and `IsOccluded` already traverses it
    (`glsl:793-830`). A second BVH is only needed if triangles are introduced.

---

## Appendix — the acceptance test (investigation 5)

The plan's criterion: *"Validate unbiasedness before appearance: with a fixed camera and a uniform
emitter, mean frame irradiance must equal the analytic value."*

The engine does not link (`Scene.cpp` fails on `Navigation::`; 41 `Renderer3D` symbols unimplemented —
plan §7), so no in-engine test can run. But the reservoir's correctness is **backend-independent
arithmetic**, and the repo already has the exact vehicle for testing it without the engine:

**`tests/` is a separate premake workspace with its own `tests/premake5.lua`**, defining
ConsoleApp projects that compile individual engine `.cpp` files directly — e.g. `ProjectTests`
compiles `Project.cpp` + `Log.cpp` and links only `yaml-cpp`. The comment at `tests/premake5.lua:2-5`
says so explicitly: *"ProjectTests links the real Project.cpp, so it exercises the shipping code
rather than a copy… which is what makes it runnable in CI."* It is driven by `tools/t.sh native` →
`tools/tnative.sh`, which treats a **SKIP as a failure** and requires native `C:/…` paths with
`MSYS_NO_PATHCONV=1`. `tools/generate_blue_noise.py` is the other precedent: a standalone tool that
self-verifies and `sys.exit(1)`s on failure (`:258`).

**So: `tests/test_restir_reservoir.cpp` as a new ConsoleApp in `tests/premake5.lua`.**

**What it asserts**, mirroring the reservoir code line-for-line in portable C++ (no glm, no GL):

1. **Analytic agreement.** A 1×1 m quad emitter of radiance L=1 at distance d, Lambertian BRDF.
   Closed-form irradiance `E = L·cosθ_l·cosθ_s·A / d²`. Run 10⁷ reservoir iterations; assert
   `|mean/E − 1| < 3/√N`. **3σ, not 1%** — a 1% bound would pass a biased estimator that got lucky.
2. **The `wSum` normalisation specifically.** Same setup, but assert that a deliberately broken
   `W = 1/(p̂·M̂)` (dropping `wSum`) **fails** the test. A negative control, in the spirit of §5.1's
   white-noise control. Without it, the harness cannot distinguish "correct" from "biased but lucky".
3. **Combine invariance.** Merging two reservoirs of 5 candidates each must give the same mean as a
   single reservoir of 10. This is the property the temporal and spatial passes depend on.
4. **Temporal reuse is unbiased under reprojection.** Combine a reservoir with a *static* history;
   the mean must not drift over many frames. (Uses the reprojection arithmetic from
   `glsl:2246-2268`.)
5. **Spatial reuse with normal rejection.** Combine only from neighbours whose packed normal agrees;
   assert the mean is preserved when rejection is conservative, and assert it is *wrong* when
   rejection is dropped on a high-contrast edge — another negative control.

**How it is run:** `bash tools/t.sh native`, which already exists, already fails on SKIP, and
already uses the native-path convention. No new runner. **It runs today, engine or no engine** —
which is the point: it decides Option A's unbiasedness before a line of shader code exists.

**What it cannot prove:** anything about the GPU integration — descriptor binding, register shifts,
barriers, or the DX12 root signature. Those stay compile-verified-only until the engine links
(plan §7) and screenshot-verified per plan §8. The harness proves the *math*; the plan's own standard
still requires a captured frame afterwards.

---

## §5.5 DDGI — diffuse GI probes

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A — Camera-anchored DDGI cascade, full Majercik** | Textbook DDGI: 2D-array probe atlas + depth/normal moments, rolling update traced through Nova's existing analytic-primitive BVH. 3 cascades, ~13k probes. Replaces `wl_neural_ambient` outright. | All four. DX12/Vulkan TierA after §5.3; DX11/OpenGL TierC now. | **18–30 d** (above plan's 10–18) | The final Option C below; 8 probe UAVs spent; no triangle-mesh GI unless §5.3 lands; OpenGL image-unit 8 cap forces ping-pong or a merge |
| **B — Probe grid without moments (visibility by depth-only, 2 cascades)** | Same atlas, but 8×8 irradiance / 16×16 single-moment depth, half the cascades, half the rays. Keeps the leak fix but drops normal-based weighting. | All four | **10–16 d** | Normal-aware back-face rejection; second-cascade coverage beyond ~128 units; probing inside thin geometry |
| **C — Surface-attached 8×8×8 SH-9 probe clusters** | Not DDGI. Sparse SH-9 irradiance clusters placed on floors/walls from entity bounds, updated on the CPU-visible instance list. No new G-buffer, no moments, no octahedral map. | All four, RT-independent | **6–10 d** | Any view-independent leak-free GI. First-bounce-from-geometry accuracy; it is a shipping-quality cheap stopgap, not a DDGI substitute. Does not scale past ~2k clusters |

### Option A — Camera-anchored DDGI cascade (full Majercik)

#### Approach

Three nested camera-centred cascades, each a 3D grid flattened into 2D texture arrays (one array slice per grid layer), following Majercik et al. 2019 §4 and McGuire et al. 2017. Probes are re-centred on the camera and snapped to the grid lattice so probes do not swim; a probe whose world position changes by more than half a cell is invalidated rather than reprojected.

Per frame, one cascade's active probe set (≈1/8 of probes) is updated by tracing N rays per probe from a spherical Fibonacci set, shading the hits with the same direct-lighting routine pass 1 uses, and blending into history. Surfel G-buffer in the Majercik sense is optional: because this engine's tracer returns a full `HitInfo` (albedo/rough/metal/normal/emission) per hit rather than a barycentric hit, we can shade immediately and skip the surfel pass — see "Files touched".

**Why the tracer is the cheap part.** `TraceScene(Ray)` (`DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl:1872`) is a generic BVH traversal over an `SSBO` of `RayTracingInstance` records, with a 32-entry explicit stack and a slab test per node (`:1893-1899`). It is **not** specialised to fog or cloud media. The volumetric march is a separate function — `wl_march_fog_vol()` (`:836`) — which marches density only and never touches `BVHNodes`. So probe rays are `TraceScene()` calls with a different origin, and the traversal is reusable verbatim.

**The one real obstacle: `isPrimaryRay`.** `TraceScene` derives the primary-ray flag from the origin (`:1881`):

```glsl
bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01f;
```

A probe ray fired from a grid vertex is never within 1 cm of the camera, so it takes the non-primary path and skips displacement/water/BOM evaluation (`TestInstanceHit`, `:1664`). For probe shading that is *mostly* fine — you want the geometric normal, not the wave normal — but it is a semantic trap and must be replaced by an explicit parameter, not worked around. This is a one-line shader change in both `.glsl` and `.hlsl`.

**Second obstacle, and it is the expensive one: Nova traces analytic primitives, not triangles.** `HitCube` (`:454`), `HitSphere` (`:477`) and the shape dispatch at `:751-766` intersect boxes and spheres only. `TestInstanceHit` never reads a vertex or index buffer; the renderer has no triangle geometry in the `RayTracingInstance` path at all. The raster path has real meshes (`CubeVertexBuffer`, `SphereVertexArray`, `Renderer3D.cpp:2074-2090`) but Nova never sees them.

Consequence: **probe rays hit only procedural primitives — boxes, spheres, cylinders, cones, tori, planes.** Any imported triangle mesh contributes nothing to probe GI and, worse, contributes nothing to the *visibility* term, so probes will leak light straight through walls. This is the single largest quality risk in Option A and it is not a DDGI problem — it is a missing-tracer-primitive problem. It is only fully solved by §5.3.

#### Files touched

Existing, modified:
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — new `RunProbeUpdate()`, `RunProbeSample()`, `RunProbeMarkUsed()`; `traceScene()` gains an explicit `isPrimaryRay` parameter; two new `image2DArray`/`sampler2DArray` pairs; new `u_PassID` cases; probe query call inside the shading path.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — the twin. Both must change together (plan §5.10 is unresolved; this is the tax it warns about).
- `Wasteland/src/Platform/NVRHI/DX12Nova.h` / `.cpp` — probe textures, extra UAV descriptors, a `DispatchProbe()` that is *not* 8×8-screen-shaped.
- `Wasteland/src/Platform/NVRHI/DX11Nova.h` — same for DX11.
- `Wasteland/src/Platform/NVRHI/VulkanNova.cpp` — same for Vulkan.
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — pass-schedule insertion, probe texture allocation, cascade re-centring. **Contains the uncommitted D3 dead-uniform audit; stage selectively.**
- `Wasteland/src/Wasteland/Renderer/RenderCapabilities.h` — nothing strictly required; `TierFor(FeatureTier::HardwareRT)` (`:51-72`) already degrades DX11 to `ComputeEmulated`.

New:
- `Wasteland/src/Wasteland/Renderer/ProbeGrid.{h,cpp}` — grid geometry, cascade re-centring, probe→(slice, x, y) packing. **Requires `#include "wlpch.h"` in the `.cpp` plus a `premake5.exe vs2022` regen, or the TU silently does not compile while the build still exits 0.**

#### Data layout

Per Majercik's published packing (GTC 2019 talk, "PROBE PACKING" slide): sphere → octahedron → square, with a 1-texel gutter for bilinear and 4×4 write alignment.

Cascade table (spacing and counts are **ESTIMATE** — engineering judgement sized against this engine's ~1000-unit view distance, see "World extents" below):

| Cascade | Grid | Spacing | Probes | Extent | Update |
|---|---|---|---|---|---|
| 0 (fine) | 32 × 4 × 32 | 2.0 u | 4 096 | 64 × 8 × 64 u | every frame |
| 1 (mid) | 16 × 8 × 16 | 4.0 u | 2 048 | 64 × 32 × 64 u | 1 in 4 frames |
| 2 (coarse) | 8 × 8 × 8 | 8.0 u | 512 | 64³ u | 1 in 8 frames |
| | | | **6 656** | | |

Formats — all verified present in NVRHI's `Format` enum (`Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h`):

| Target | Format | Layout per probe | Bytes/probe |
|---|---|---|---|
| Irradiance | `R11G11B10_FLOAT` (`nvrhi.h:190`) | 6 × 6 incl. border | 144 |
| Depth moments | `RG16_FLOAT` (`nvrhi.h:195`) | 16 × 16 incl. border | 1 024 |
| Normal + offset | `RGBA16_FLOAT` (`nvrhi.h:201`) | 16 × 16 incl. border, xyz = view normal, w = plane offset | 2 048 |
| **Total** | | | **3 216** |

6 656 probes × 3 216 B = **21.4 MB** (`MEASURED` — arithmetic on the counts and format widths above).

Packing into 2D arrays: each grid layer becomes one array slice. Cascade 0 is 32 × 32 per layer × 4 layers; with 4×4 alignment and a 1-texel border a probe occupies 8 × 8 and 18 × 18 texels respectively in the two maps, so slice 0 of the depth atlas is 32 × 8 × 18 × 18 texels of `RG16F`. Probe index is `(layer * gridY + y) * gridX + x`, computed with bit operations because grid dimensions are powers of two (Majercik's own simplification).

Note the depth and normal maps must be **separate textures at different resolutions** — 16×16 vs 6×6 — so three textures per cascade, nine total, plus ping-pong copies for the temporal blend. That is the binding problem: see "Backend notes".

`img_TraceGBuffer` (`Renderer3D_NovaRenderer.glsl:15`) carries `r` = NDC depth, `gba` = packed world normal (`:2105-2109`; the `.gba` not `.rgb` convention is called out at `:2512-2517` and `:2537-2539`). Both are exactly what the DDGI visibility test needs, so no new frame G-buffer — consistent with the plan's already-verified finding.

#### Backend notes

**OpenGL.** This is the binding wall. `Renderer3D.cpp:1123-1126` documents it in-source:

> "Shares one RGBA32F image on unit 6 because this GL driver caps image bindings at 0-7 (units 0-5, 7 taken, 6 was dead)."

Units in use are 0, 1, 2, 4, 5, 6, 7 (`:1810-1816`) — **unit 3 is the only free image unit.** A DDGI cascade needs three write targets minimum. Options: (a) merge depth+normal moments into one `RGBA16F` texture at 16×16 and accept the loss of the separate 6×6 irradiance resolution; (b) ping-pong — the update pass writes moments in frame *n*, irradiance in frame *n+1*, which is legal because the blend is temporal anyway; (c) promote probe textures to `GL_TEXTURE_2D_ARRAY` bound through a *sampler* rather than an image unit, which moves the cost from image units to texture units where there are far more (`glActiveTexture(GL_TEXTURE0 + i + 15)`, `:1820`, already burns 15–46). **(c) is the right answer** — a probe atlas is exactly the case image units were not designed for, and it keeps the DX/Vulkan path uniform.

Sampler slots are also shared with the 32-entry scene texture array at `GL_TEXTURE0 + 15` (`:1818-1822`), so texture-unit pressure is real but there is room.

**DX11.** 128 SRV slots and 8 UAV slots (`DX11Nova.h:436-465`, `CSSetUnorderedAccessViews(0,8,...)` at `:477`). All 8 UAV slots are **already bound to `images[0..7]`**. There is no free UAV slot. Same fix as OpenGL: bind probe atlases as SRVs from a separate UAV set, or extend the `std::array<Image,8>` (`:66`) and re-lay the binding table — the latter is invasive and the comment at `:459-465` notes the table is already load-bearing. Prefer the SRV route plus a dedicated `DispatchProbe` that binds its own table; D3D11 allows a dispatch to bind any 8 UAVs, so a separate call site can use all 8 for probes if it does not also need the screen images.

**DX12.** Cleanest path. `kDescriptorsPerHeap = kUavBase + kUavCount` (`DX12Nova.h:107`) with `kUavCount = 9` (`:92`) gives 58 descriptors per frame slot; the heap is per-slot (`:189-192`) so growing it is safe. Root signature has 8 root parameters (`DX12Nova.cpp:534-560`); a probe texture array needs one more, or it can ride the existing table at 6.

**Vulkan.** `VulkanNova::CreateImages` (`VulkanNova.cpp:304-346`) is the model to copy. The binding contract log at `:274-280` states the invariant that must be preserved:

> "VulkanNova binding contract: offsets u=0 b=7 t=16 s=200; 5 sampled SRV + 5 read SRV = 24 bindings, all distinct"

With `t`-shift 16, a probe SRV at HLSL `t80` becomes Vulkan descriptor 96; `t72`–`t79` are free in the `t`-space (occupied: t1–t3 SSBO, t15–t46 scene textures, t48–t52 sampled, t64–t71 read) but the descriptor index must not collide with `u` (0–7), `b` (8), `s` (200). **Free `t` slots: t47, t53–t63, t72+.** `tools/compile_nova_spirv.py --check` re-verifies the whole table and will fail loudly if a new binding collides.

**What TierC does without hardware RT.** Traces the same analytic-primitive BVH via `TraceScene`, so probe GI on DX11/OpenGL is genuinely functional and genuinely leaks through triangle meshes. The honest interim: sample `wl_neural_ambient` (`Renderer3D_Basic.glsl:616-634`) for the moment term and use the probe grid only for the *visibility* weight — probes contribute occlusion, not radiance. That removes the flat-corner artefact without pretending to have bounce light. **(c) below does this properly.**

#### Effort and risks

**18–30 person-days (ESTIMATE).** Higher than the plan's 10–18 because the plan assumes tracer parity that does not exist: probe rays miss every triangle mesh.

| Risk | Severity | Mitigation |
|---|---|---|
| Leaks through triangle meshes (no triangle intersection) | **High** | §5.3 first. Interim: occlusion-only use of the grid. |
| `isPrimaryRay` inferred from ray origin (`:1881`) silently changes shading | Medium | Make it an explicit parameter; check both twins |
| OpenGL unit-3 cap | Medium | Sampler-bound array textures (c) |
| No UAV slot free on DX11 | Medium | Dedicated `DispatchProbe` binding table |
| Visibility weight is where quality is won or lost | Medium | Validate against a synthetic occlusion case first |
| Doubled per-backend binding tables, un-deduped | Medium | The `FlushDX11Nova`/`FlushDX12Nova` duplication (`Renderer3D.cpp:2593`/`:2701`) compounds with every pass |

#### What would prove it

Cheapest decisive experiment, standalone and runnable today with no MSBuild:

A Python harness that, for one probe, casts rays against a hand-built analytic scene (a floor and a box) through a *literal transcription* of `TraceScene`'s slab test and stack walk (`Renderer3D_NovaRenderer.glsl:1889-1914`), computes the six-by-six octahedral irradiance and the sixteen-by-sixteen Chebyshev-weighted moments, then samples a point behind the box and checks it is dark. **ESTIMATE 0.5 day to write, and it decides the whole option.** If occlusion does not read correctly there, no amount of integration will save it.

Second, in-engine: add pass 11 writing a constant colour to one probe texel and confirm on all four backends that the binding landed. Cheapest possible binding test.

### Option B — Probe grid without moments, two cascades

#### Approach

Same atlas and same update machinery as Option A, reduced. Irradiance at 6×6 `R11G11B10F`, depth moments at 8×8 `RG16F` (single mean distance + squared distance — this is the cheapest form that still permits a Chebyshev bound), **no normal moment map**. Two cascades only.

Probe count: cascade 0 = 24 × 4 × 24 at 2.5 u spacing (2 304 probes, 60 × 10 × 60 u); cascade 1 = 10 × 6 × 10 at 6 u spacing (600 probes, 60 × 36 × 60 u). Total **2 904 probes** (`MEASURED` arithmetic; spacings are ESTIMATE).

#### Files touched

Identical to Option A — same nine files. The only shader difference is the absence of the normal map and the lower depth-map resolution. This is a real advantage: it is the same integration, so the work done for B is not thrown away if it later grows into A.

#### Data layout

| Target | Format | Per probe | Bytes |
|---|---|---|---|
| Irradiance | `R11G11B10_FLOAT` (`nvrhi.h:190`) | 6 × 6 | 144 |
| Depth moments | `RG16_FLOAT` (`nvrhi.h:195`) | 8 × 8 | 256 |
| **Total** | | | **400** |

2 904 × 400 B = **1.16 MB** (`MEASURED`). Against Option A's 21.4 MB that is a 18× reduction, and it is the number that makes this affordable on every backend at once.

#### Backend notes

Binding pressure drops to two write targets per cascade. OpenGL still cannot fit two in unit 3, so sampler-bound array textures remain the answer — but there is now slack to put both cascades in one texture array and write with one unit. DX11 can use a single UAV for the whole grid. DX12 and Vulkan are unaffected.

Vulkan `t` slots consumed drop to two, which keeps the `compile_nova_spirv.py --check` contract easy to preserve.

#### Effort and risks

**10–16 person-days (ESTIMATE).** This is the plan's 10–18, and the only option here that lands inside it.

| Risk | Severity |
|---|---|
| Leaks through triangle meshes | **High** — identical to A, this is the tracer's fault |
| No normal-aware back-face rejection → more light on back faces than A | Medium |
| Only 60-unit coverage; beyond that everything reads cascade 1 | Medium |
| Coarse cascades miss the first-bounce-from-floor quality | Low |

#### What would prove it

The same Python harness as Option A, with the 8×8 moment map. If the Chebyshev test works at 8×8 the option is viable; the harness change is a constant, not a rewrite.

### Option C — Surface-attached SH-9 probe clusters (not DDGI)

#### Approach

Deliberately the cheap option. Spherical-harmonic order-2 (9 coefficients, 27 floats) probe clusters, ~4–16 clusters per room, placed from entity AABBs rather than a lattice. Placed and refreshed on the CPU from the same `m_SceneInstances` list the renderer already rebuilds each frame (`Renderer3D.cpp:1391-1404`); shading is evaluated on the CPU with the engine's existing analytic-light set (`MaxAnalyticLights = 8`, `Renderer3D.h:298`), and clusters are uploaded as one `std::vector` in an SSBO the shader reads and interpolates trilinearly.

No octahedral map. No depth moments. No visibility. No new G-buffer. No new pass in `RunNovaPassSchedule`.

It exists in this list because the plan asserts a TierC probe system already exists — it does not — and something must occupy the "diffuse GI" cell for Low/Medium presets before DDGI is affordable on four backends.

#### Files touched

- `Wasteland/src/Wasteland/Renderer/ProbeClusters.{h,cpp}` — **new**, cluster placement, SH projection, SSBO packing. Needs `wlpch.h` + premake regen.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` and `.hlsl` — one `sampleProbeClusters()` function plus a call site. No new pass, no new `u_PassID` case.
- `Wasteland/src/Platform/NVRHI/DX11Nova.h` — a 4th `StructuredBuffer` at `t4`. Note this is *outside* the three currently uploaded (`DX11Nova.h:436`, `t1`–`t3`) and does not collide with `t15+`.
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — cluster rebuild next to the existing `SceneInstancesChanged()` block.

#### Data layout

Per cluster: 9 SH coefficients × RGB as `vec3` = 108 B, plus a centre `vec3` and radius `float` = 16 B, plus a 4-coefficient validity/weight vec4 for fade-in = 16 B. **140 B per cluster, rounded to 144 for alignment** (`MEASURED` arithmetic). 2 000 clusters = 288 KB. One `StructuredBuffer`, one binding, both textures and buffers avoided entirely.

Voxel-cluster variants (9 colours / 9 directions) would be the higher-quality form; SH-9 is chosen because it needs no lookup table and no direction-loop in the shader.

#### Backend notes

Identical on all four, and that is the entire argument for it. It is pure SSBO reads — `RenderCapabilities` already routes all four backends to `ComputeEmulated` via `TierFor` (`RenderCapabilities.h:51-72`) — so there is no binding surgery, no `DispatchProbe`, no descriptor-table growth, no OpenGL image-unit problem, and no tier-dependent code path at all.

It also works today on Vulkan, which currently has no Nova backend wired (`Renderer3D.cpp:1620-1627`: "Vulkan has no Nova backend at all, so Vulkan always lands here regardless of RayTracingEnabled").

#### Effort and risks

**6–10 person-days (ESTIMATE).**

| Risk | Severity |
|---|---|
| No light leaking — clusters are static geometry, so a wall the cluster sits behind still receives its bounce | **High**, inherent |
| Placement heuristic quality dominates the result; a bad heuristic gives worse artefacts than the flat ambient it replaces | Medium |
| No dynamic GI — a moving light or opening door is invisible | Medium |
| Scale ceiling ~2–4 k clusters before CPU projection cost is felt | Low |

#### What would prove it

Run it on the CPU only, no GPU work: build clusters for a synthetic two-room scene, project an analytic area light into them, and check the second room's irradiance is dimmer than the first by roughly the right ratio. **ESTIMATE 1 day including a scene-graph dump**, and it needs no GPU and no build.

### Recommendation

**Option B, gated on §5.3 — and Option C only if a screenshot on Low/Medium is wanted before §5.3 exists.**

The reasoning is driven by one finding the plan does not account for: **Nova traces analytic primitives only** (`HitCube`/`HitSphere`, `Renderer3D_NovaRenderer.glsl:454`/`:477`). Probe rays will pass through every triangle mesh in the world. That makes Option A's "10–18 days" unreachable and makes *any* probe grid ship as a light-leak generator until §5.3 lands. Since §5.3 is already Phase 2 and §5.4–5.7 all queue behind it, the honest sequencing is: build Option C now (6–10 days, unblocks the diffuse-GI cell on all four backends with zero tracer dependency), then build Option B after §5.3 when the tracer can see the real scene.

Option A is the right *destination* — 3 cascades and normal moments are where the quality is — but doing it first means paying for three cascades of machinery to ship light that leaks through walls.

Two constraints to fix before any of this, both cheap:

1. **`TraceScene`'s `isPrimaryRay` must become an explicit parameter** (`Renderer3D_NovaRenderer.glsl:1881`). It is currently inferred from `ray.Origin ≈ u_CameraPosition`, which is silently false for every probe ray. Left alone, probe shading will take a code path nobody chose.
2. **Probe atlases must be sampler-bound `Texture2DArray`s, not image units, on OpenGL.** Unit 3 is the only free image unit (`Renderer3D.cpp:1123-1126`), which is fewer than a single cascade needs.

### Corrections to the plan

1. **"TierC (raymarch) diffuse GI probes" already exist for OpenGL/DX11 (§3 table) — FALSE.** There is no probe system in this repo. `ProceduralMeshes.h:86` mentions "Subdivided plane for terrain/GI probes" and `VisualBlocks/Compiler.cpp:309` mentions "Probes from a little ABOVE the requested foot level", but both are aspirational comments; neither is a probe implementation. `RenderCapabilities` has no diffuse-GI feature entry at all — the `FeatureTier` enum (`RenderCapabilities.h:21-27`) has four values and none is GI-specific. What exists instead is `wl_neural_ambient()` (`Renderer3D_Basic.glsl:616-634`), whose own comment says: *"Stands in for an irradiance probe. Cheap and always available, but it cannot capture occlusion — a corner receives the same ambient as an open field."* That is the actual starting state: unoccluded analytic ambient, no grid.

2. **"Reuse Nova's existing raymarch traversal for probe baking" (§5.5) — half right, and the wrong half.** The *raymarch* is not reusable: `wl_march_fog_vol()` (`:836`) marches density and is specialised to fog/cloud media. The reusable thing is `TraceScene()` (`:1872`), which the plan does not name. Reuse is real, but it comes with an `isPrimaryRay` origin test (`:1881`) that must be parameterised, and it traces analytic primitives only.

3. **The plan gives no world extents, and the repo contains none either.** No `docs/ClusterLOD.md` exists — `docs/` holds exactly four files (`RTX_KIT_LICENSES.md`, `NVRHI_SETUP.md`, `BUILD_ISOLATION.md`, `AGE_ASSURANCE.md`), and §5.12 of the plan cites a ClusterLOD doc that is not in the tree. There are no `.wlscene`/level files, no `WorldSize`/`GridSize` constant, and `Scene` exposes no world-bounds query. The only scale anchors are: perspective far plane 1000.0 u (`SceneCamera.h:190`), editor far clip 1000.0 u (`EditorCamera.h:145`), `RayTracingInstance::MaxDistance = 1000.0f` (`Renderer3D.cpp:2570`), navmesh `CellSize = 0.5f` with `AgentRadius = 0.4f` (`NavMesh.h:63`/`:72`), collider defaults `Radius = 0.5f`/`Height = 1.0f` (`Components.h:517`/`:542`), and a fog-volume comment describing "a 20-unit volume" (`Components.h:1283`). **Conclusion: the unit is roughly one metre and the usable radius is ~1000 u, but the engine has no declared world extent — so a probe grid must be camera-anchored with cascades, exactly as Option A/B specify.** A world-anchored grid has nothing to anchor to.

4. **§5.5's 10–18 days is optimistic by roughly a factor of two for the full algorithm.** ESTIMATE 18–30 for Option A. The driver is not the octahedral mapping (easy) or the Chebyshev blend (easy) — it is that the tracer cannot see triangle meshes, so the visibility term has to be rebuilt on a hardware-RT foundation that does not exist yet.

5. **Pass ID 11 is free; 3 is free too.** The switch in `main()` covers 0, 1, 2, 4, 5, 6, 7, 8, 9, 10 (`Renderer3D_NovaRenderer.glsl:2671-2680`). **Use 11** — it leaves 3 alone and reads as the next feature after god rays. `DX11Nova.h:447` binds a special slot only for `pass==9`, so a new pass ID inherits no hidden special cases.

6. **The DX flush's deliberate order 1, 0, 2 constrains where the probe update goes.** `RunNovaPassSchedule` (`Renderer3D.cpp:1373-1386`) runs pass 1 before pass 0 because pass 2's velocity reprojection needs the G-buffer pass 1 produced. A probe update must go **after pass 1** (it needs the scene traced and, for screen-space rejection, the depth/G-buffer from pass 0) and **before pass 2**, because pass 2 is what shades the frame that will sample the probes. Inserting as `pass(1); pass(0); pass(11); pass(2);` keeps the documented invariant intact. Note the OpenGL path orders 0, 1, 2 ascending (`Renderer3D.cpp:1938-1949`) — the 1,0,2 order is a DX-specific artefact of the snapshot-copy model, not a shared schedule, so the two paths need not agree and should not be "unified".

7. **The plan's binding headroom is better than it looks on DX12/Vulkan and much worse on OpenGL/DX11.** Free Vulkan `t` slots: t47, t53–t63, t72+ (`compile_nova_spirv.py` shifts t by 16; occupied are t1–t3, t15–t46, t48–t52, t64–t71). DX12 grows `kDescriptorsPerHeap` from 58 (`DX12Nova.h:107`). **But all 8 DX11 UAV slots are already bound** (`DX11Nova.h:477`) **and only OpenGL image unit 3 is free** (`Renderer3D.cpp:1123-1126`). Neither backend has room for probe atlases as image units on either backend.

8. **`FeatureTier` has no GI entry, so §3's tier table cannot currently be enforced.** `RenderCapabilities.h:21-27` defines `Unavailable`, `ComputeEmulated`, `DenoiseCompute`, `HardwareRT` — a per-feature capability list does not exist. The plan's "Diffuse GI probes → TierA/TierC" row has no field to resolve into. Adding one is a prerequisite for any of these options, and it is the same gap §5.2 already hit with `DenoiseCompute`.

---

#### Appendix — investigation answers

**World extents (investigation 1).** No declared world extent exists in the repo. Nearest anchors, all `MEASURED` as readings: perspective far 1000.0 (`SceneCamera.h:190`), editor far clip 1000.0 (`EditorCamera.h:145`), `MaxDistance = 1000.0f` (`Renderer3D.cpp:2570`), navmesh cell 0.5 / agent radius 0.4 (`NavMesh.h:63`, `:72`), unit-cube colliders with `Height = 1.0`–`2.0` (`Components.h:517`, `:542`, `:1958`), fog reference to a 20-unit volume (`Components.h:1283`). `docs/ClusterLOD.md` **does not exist** (the plan's §5.12 cites it). Implied probe counts at ~2 u spacing over the 1000 u view distance: a single cascade would need 500³ = 125 M probes — infeasible — which is why both A and B are camera-anchored cascades with 2.5–8 u spacing, giving 2 904–6 656 probes.

**TierC probe system (investigation 2).** **No.** No probe grid, no SH, no octahedral map, no irradiance volume anywhere in `Wasteland/src` or `DemonCore-Editor/assets/shaders`. The only probe-adjacent code is `wl_neural_ambient()` at `Renderer3D_Basic.glsl:616-634`, which self-describes as an occlusion-free stand-in. Two comments mention probes (`ProceduralMeshes.h:86`, `VisualBlocks/Compiler.cpp:309`) and neither implements one. **The plan's §3 tier-table row for "Diffuse GI probes" is wrong.**

**Raymarch reusability (investigation 3).** **The raymarch is not reusable; the traversal is.** `wl_march_fog_vol()` (`:836`) is media-only and never touches `BVHNodes`. `TraceScene()` (`:1872`) is a generic BVH walk with a 32-deep stack and slab test (`:1889-1914`) that probe rays can use verbatim — after replacing the `isPrimaryRay` origin test at `:1881` with an explicit parameter, and after accepting that it intersects analytic primitives (`HitCube` `:454`, `HitSphere` `:477`) and **not triangles**.

---

## §5.6 Filtered texturing

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A** | Analytic LOD clamp around the existing distance formula — replace the axis-aligned `grazing` guess with a real ray-differential footprint (Baranov minor-axis rule) in the shared `.glsl`/`.hlsl` twins | All four | **2–3 pd** | Nothing. Retains the CPU box-filter mip chain and `Mirror` addressing; no block-size awareness (irrelevant — see Corrections) |
| **B** | Option A + clamped anisotropic taps along the major axis (up to N taps, LOD from the *minor* axis only), replacing the single `textureLod` | All four (TierC) | **+2–3 pd** | The 1-sample albedo path at `Renderer3D_NovaRenderer.glsl:1748`; costs 1–8 extra texture reads per ray hit. Not viable on POM march taps (`:1524`, `:1537`) without a tap budget |
| **C** | Compressed-texture-ready footprint core: block-size-parameterised texel math, correct the day BC7 lands even though nothing is compressed today | All four | **1–2 pd** (header only) | Nothing today. Forecloses *not* thinking about block size — the parameter must be threaded through every sampler, so a later format change touches N call sites if the parameter is botched |

### Option A — Ray-differential minor-axis footprint

#### Approach

Replace the heuristic at `Renderer3D_NovaRenderer.glsl:1741-1744` with the standard anisotropic footprint construction. Today the shader computes LOD from camera **distance** plus a `grazing` fudge (verbatim, MEASURED):

```glsl
// Renderer3D_NovaRenderer.glsl:1741-1744
float ndv = abs(dot(info.normal, ray.Direction));
float grazing = 1.0f - clamp(ndv, 0.0f, 1.0f);
float mipLevel = log2(max(dist * scaleLen * 0.008f, 0.0001f)) + grazing * grazing * 2.0f;
mipLevel = clamp(mipLevel, 0.0f, 4.0f);
```

That is distance-proportional, not footprint-proportional. A steeply-angled surface at 10 m and a face-on one at 10 m get the same LOD unless `grazing` catches it, and `grazing*grazing*2.0f` is an unbounded guess — the trailing `clamp(...,0,4)` hides its range.

The correct input already exists in the tracer: the ray differential. Take `ddx`/`ddy` of the UV **in texel units**, form the footprint ellipse, and use the **minor** axis for LOD:

```
duvdx = dFdx(uv) * texDims   →  lx = length(duvdx)
duvdy = dFdy(uv) * texDims   →  ly = length(duvdy)
lod   = max(log2(min(lx, ly)), 0.0)
```

`min` is the axis that makes anisotropic filtering work: it is where the footprint is tightest, so aliasing is controlled there, and the major axis is recovered by taking more samples rather than a blurrier mip. This is the rule from Baranov et al., *Filtered Texture Sampling and the Role of Footprints*, HPG 2021 — derive the mip from the footprint ellipse and sample along the axis of anisotropy, instead of from raw screen derivatives.

Because Nova is a **compute** shader, `ddx`/`ddy` are legal with no quad-derivative requirement as in a fragment shader. `textureGrad` is the natural call; the repo currently uses `textureLod` exclusively (MEASURED: 17 `textureLod`, **0** `textureGrad`/`textureProj` in the `.glsl`).

**The clamp is the actual deliverable.** `ComputeMipLevels` (`NVRHITexture.cpp:40-56`) derives chain length from the **largest** axis, so a 2048² texture gets 12 levels. `clamp(mipLevel, 0.0f, 4.0f)` caps at 4 — levels 5–11 are allocated, uploaded by `UploadMipChain` (`:289`) and then **unreachable**, while sitting in VRAM. A real footprint lets the clamp use the real level count.

#### Files touched

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl:1741-1748`
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — the twin, same edit
- `Wasteland/src/Platform/NVRHI/NVRHITexture.cpp:216` — expose `mipLevels` so the clamp is real instead of `4.0f`

Note on §5.10: `tools/compile_nova_spirv.py` covers only the **Vulkan SPIR-V** artifact. The DX11/DX12 SM5 blob comes from `D3DCompileFromFile` on the `.hlsl` directly (`DX12Nova.cpp:53-57` region, `DX11Nova.h:48`), so the `.hlsl` copy must stay real and hand-maintained.

#### Data layout

The footprint is shader-local, but its *inputs* are per-instance. `RayTracingInstance` (`Renderer3D_NovaRenderer.glsl:30-42`) already carries `TextureScale` (vec4), and `:1736` derives `scaleLen = max(length(inst.TextureScale.xy) / worldExtent, 0.0001f)`. It does **not** carry the texture's texel dimensions, which the footprint needs to convert UV deltas into texels.

| Added field | Type | Bytes | Purpose |
|---|---|---|---|
| `TexelDims` | `vec2` | 8 | `texW`, `texH` at level 0, so `dFdx(uv)*TexelDims` is in texels |
| `MipCount` | `float` | 4 | `log2(mipLevels)`, for a correct clamp instead of `4.0f` |

Both are available CPU-side where instances are pushed; `TextureID` (`:34`) already indexes `u_SceneTextures[32]` (`Renderer3D_NovaRenderer.glsl:129`). **Re-run the SPIR-V binding audit after this change** — plan §7 records all 84 constant-buffer members matched fxc↔DXC, and adding fields invalidates that.

#### Backend notes

- **The three paths agree by construction.** The footprint is computed in shader code from the ray differential and texture dims, so GLSL, HLSL and SPIR-V evaluate the same formula. **There is no backend divergence to reconcile** — which removes the plan's stated risk and its reason for a per-API footprint. See Corrections.
- **No DXC needed.** DX11/DX12 are on SM5 `D3DCompileFromFile`; `ddx`/`ddy`/`textureGrad` are all SM4+. Unlike §5.3, this option is not gated on the DXC work.
- **Vulkan Nova is compiled but never wired** (plan §2 gap 3 — no `FlushVulkanNova`), so the Vulkan half is compile-verified only until Phase 4. `tools/compile_nova_spirv.py` still exercises the path.
- **Sampler state today (MEASURED):** `NVRHITexture.cpp:244-249` sets `minFilter`/`magFilter`/`mipFilter` = true (trilinear); `:256-258` sets `addressU/V/W = Mirror`; `:263` sets `maxAnisotropy = 16.0f`. OpenGL matches at `OpenGLTexture.cpp:79` (`min(maxAniso, 16.0f)`).
- **Nova bypasses all of it.** `textureLod` with an explicit LOD does not consult the sampler's aniso setting — hence the comment at `:1740` "*textureLod bypasses driver anisotropy, so compensate manually*". The 16× hardware aniso is spent on the raster path and is **dead weight on the Nova path**. That compensation is what this option finally makes correct.
- **Pre-existing inconsistency (not introduced here):** `VulkanNova.cpp:202-206` creates its sampler with `Wrap` addressing and `maxAnisotropy = 1.f`, while `NVRHITexture2D::CreateSampler` uses `Mirror` and 16×. Mirror-vs-Wrap changes results at tile boundaries. Any Vulkan-vs-DX12 screenshot comparison must account for it.

#### Effort and risks

**2–3 person-days (ESTIMATE).** Two edits in two shader twins plus an instance-field addition.

Risks, in order:
1. **Widening the clamp changes appearance everywhere.** Today LOD ≤ 4 is a hard floor on blur; a real footprint that legitimately selects level 7 looks sharper and, until §5.2's SVGF lands, noisier (no denoiser to absorb the extra high-frequency detail). **Sequence after SVGF, not before.**
2. **Compute derivatives are per-quad.** Legal, but the 2×2 grouping across `local_size_x = 8` means derivatives are well-defined yet not aligned with screen-space pixel quads. Acceptable for a stochastic path tracer whose rays are already jittered; **not** acceptable in the raster path, where it would show as 2×2 blocking. Apply to Nova only.
3. **Do not route the POM march taps** (`:1524`, `:1537`) through the footprint. They use a fixed macro-level `marchLOD = (1.0f - marchK) * 2.0f` (`:1469`), and the height field needs the *same* LOD across all march steps or the intersection breaks.

#### What would prove it

A standalone harness — the only executable verification available, since the engine does not link (plan §7 "Blocked — not ours"):
- CPU reference footprint vs. a brute-force area integral of the ellipse over a texel grid, over a grid of (distance, angle, texScale). Assert relative error < 5% (ESTIMATE threshold).
- Sweep `mipLevel` 0..12 on a synthetic high-contrast checker, counting high-frequency energy with and without the level-4 clamp. The current clamp should measurably under-filter past level 4 — that gap is the win.
- Once Phase 2 unblocks the engine: fixed-camera screenshot of a grazing-incidence floor across three backends, confirming levels > 4 are actually reached.

---

### Option B — Anisotropic multi-tap along the major axis

#### Approach

Extend Option A: clamp `ratio = major / minor` to the sampler's 16×, then take `ceil(ratio)` taps spread along the major axis at the **minor-axis** LOD. This is the actual filtered texturing the Baranov paper describes, and it is what `grazing*grazing*2.0f` crudely fakes.

```
lod   = max(log2(minor), 0.0)
ratio = min(major / max(minor, 1e-8), u_MaxAniso)     // 16.0f, matches NVRHITexture.cpp:263
dir   = normalize(majorAxisUV)
taps  = ceil(ratio)
for i in 0..taps-1:
    acc += textureLod(tex, uv + dir * (i/taps - 0.5) * majorUV, lod)
result = acc / taps
```

#### Files touched

Same as Option A, plus a loop around the `textureLod` at `Renderer3D_NovaRenderer.glsl:1748`.

#### Data layout

No new instance fields beyond Option A. `u_MaxAniso` becomes a constant uniform (16.0f) rather than per-instance.

#### Backend notes

All four, TierC. The loop compiles unchanged in GLSL and HLSL; no RT dependency.

**Where this is expensive, honestly:** Nova evaluates this **per ray hit, per bounce**, up to `u_MaxBounces`. A 16:1 footprint turns 1 texture read into 16 *per hit* — the opposite of what a path tracer wants. This is why the current code deliberately does not use hardware aniso. **This is the option with a real chance of being a net loss.**

#### Effort and risks

**+2–3 person-days on top of A (ESTIMATE).**

1. **Worst case 16 reads per hit.** Mitigate with a global tap cap (2 or 4), absorbing excess anisotropy as blur — i.e. deliberately accept a *lower* effective aniso than the sampler advertises.
2. **Divergent tap counts.** `ceil(ratio)` varying per pixel causes SIMD divergence and poor compute utilisation. Round the tap count to a power of two and branch on it so all lanes in a subgroup agree.
3. **Gain may be invisible** — SVGF (§5.2) blurs the result anyway, and temporal accumulation (pass 2) already averages across frames where the footprint rotates.

#### What would prove it

Before writing shader code: measure the **distribution of `major/minor` over a real frame**. If p95 anisotropy < 4, Option B is pointless — 4 taps is the maximum worth doing, and Option A alone captures most of the benefit. Cheap CPU-side computation over existing `RayTracingInstance` data; do it **first**.

---

### Option C — Block-size-parameterised footprint core

#### Approach

Write the footprint function once as a shared header with a texel-block-size parameter, passing `1.0` today:

```glsl
// wl_footprint.glsl / wl_footprint.hlsli  (new)
float2 wl_footprint_scale(vec2 uv, vec2 texDims, vec2 blockSize)
{
    vec2 duvdx = dFdx(uv) * texDims;   // texels per pixel, x axis
    vec2 duvdy = dFdy(uv) * texDims;   // texels per pixel, y axis
    return vec2(length(duvdx), length(duvdy)) / blockSize;  // blocks per pixel
}
```

The division by `blockSize` is the whole point: with a 4×4 block, a pixel covering 2×2 texels covers **half a block**, which is sharpening territory — so the LOD must be biased *down* by 2 or the surface over-blurs. Getting this wrong is the classic compressed-texture bug, and it is invisible until textures are actually compressed.

#### Files touched

One new header pair (`wl_footprint.glsl`, `wl_footprint.hlsli`) plus the two call sites.

#### Data layout

`blockSize` is a compile-time `#define` per backend for now — texture format is a property of the asset pipeline, not a uniform. When compression lands it joins `TexelDims` as a per-instance vec2.

#### Backend notes

**This costs nothing today because there is nothing to be correct about.** Every `nvrhi::Format::` used in `Wasteland/src` is uncompressed — MEASURED counts: `RGBA32_FLOAT` ×8, `RGBA8_UNORM` ×4, `R32_SINT` ×6, `DEPTH24STENCIL8` ×5, `SRGBA8_UNORM` ×2, `RG32_FLOAT` ×2, `BGRA8_UNORM` ×2, plus singles (`RGBA16_FLOAT`, `R8_UINT`, `R32_UINT`, `R32_FLOAT`, …). `stbi_load(..., STBI_rgb_alpha)` (`NVRHITexture.cpp:99`) produces raw RGBA8. So the block size is 1×1 and the parameter is dead.

It matters only if §3's texture-compression row ("an ASTC-class codec if we ever write one") is ever funded. Then it is the difference between filtering that works and filtering that over-blurs every surface by 2 mip levels.

#### Effort and risks

**1–2 person-days (ESTIMATE)** — a header and two call sites.

Risk is near zero now; the cost is **deferred ambiguity**. A `#define` threaded through three paths that is always `1.0` will be wrong in at least one place the day someone changes it, and nothing will test it. Mitigate by making `blockSize` a required function argument defaulting to `float2(1.0)`, so a wrong constant surfaces as a visible call-site diff rather than a silent shader bug.

#### What would prove it

CPU reference test: for block sizes {1×1, 4×4, 8×4} and footprints 0.25→64 texels, assert `lod >= log2(blockSize) - 1` always holds — i.e. the selected level never under-samples the block extent. Cheap, and it is the assertion that catches the real bug.

---

### Recommendation

**Option A, and only Option A.** ~2–3 person-days (ESTIMATE) for the largest real win in §5.6, and it closes a live defect rather than adding a feature: `clamp(mipLevel, 0.0f, 4.0f)` at `Renderer3D_NovaRenderer.glsl:1744` makes mip levels 5–11 unreachable on every texture, while `ComputeMipLevels` allocates them.

**Do Option B's measurement before writing any of B.** Compute the anisotropy distribution over a real frame first; if p95 < 4:1, drop B. On a path tracer, 16 taps per hit is likely a net loss and the existing `grazing` fudge may already suffice.

**Do not do Option C as a standalone task.** Fold `float2 blockSize = float2(1.0)` into A's signature as a one-line forward hook and move on. It is insurance against a compression decision nobody has made.

**Sequencing:** A is TierC and needs no hardware RT, so it is *not* gated on §5.3 — but land it **after** §5.2's SVGF, because widening the LOD clamp sharpens the image and there is currently no denoiser to absorb the extra variance.

### Corrections to the plan

1. **"A `ComputeTextureFootprint`-equivalent on DX12" — there is no such API in this NVRHI, and the premise is wrong.** `grep -n "TextureFootprint" Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h` returns **zero hits**. NVRHI wraps D3D12, Vulkan and D3D11 behind one abstraction and exposes **no footprint or LOD-query entry point at all**. The only mip-related surface in `nvrhi.h` is bookkeeping: `typedef uint32_t MipLevel` (`:383`), `uint32_t mipLevels = 1` on `TextureDesc` (`:413`), `setMipLevels` (`:448`), `TextureSubresourceSet::baseMipLevel`/`numMipLevels` (`:504-505`), `writeTexture(..., uint32_t mipLevel, ...)` (`:3250`). **No backend implements a footprint API, so there is nothing to diverge between them** — and a compute footprint in shader code is *more* portable than a native one, since it runs identically on OpenGL 4.3 and SM5. Rewrite §5.6's *"Risk: low, but backend divergence is real"*: the real risk is the opposite — a native per-API footprint **invites** divergence where a shared shader function removes it.

2. **"Texture compression | TierC (BC via NVRHI)" (§3 table) is false for this repo.** The plan asserts BC1/BC5 are in use "via NVRHI". MEASURED: `grep -rn "BC[1-7]|DXT[1-5]|BCONV"` across `Wasteland/src`, `tools/`, `premake5.lua` returns hits **only** inside `Wasteland/vendor/Box3D/extern/sokol/sokol_gfx.h` — a vendored debug renderer, not the engine. `premake5.lua` links `d3dcompiler.lib` (`:155`) and nothing texture-related: no `DirectXTex`, no `wav`, no BCONV, no `dxcompiler`. Textures decode through `stb_image` to raw RGBA8 (`NVRHITexture.cpp:99`, `OpenGLTexture.cpp:112`). **Every engine texture is uncompressed: 1 texel = 1 pixel = 4 bytes.** The entire "a compressed texel block can exceed one pixel" motivation for §5.6 is currently hypothetical. The *anisotropy* half is real and worth doing (A/B); the *block-size* half is not, until compression is funded.

3. **The plan's "Nova samples explicit LOD 0..4" warning is right but understates it.** `NVRHITexture.cpp:211-216` says "*Nova samples explicit LOD 0..4*" while allocating a full chain via `ComputeMipLevels`. A 2048² texture has 12 levels; 5–11 are allocated, uploaded (`UploadMipChain`, `:289`) and unreachable. For the 6 real textures (MEASURED — `ls DemonCore-Editor/assets/textures/`) at 4 bytes/texel, levels 5+ cost ~5.6 MB each, so **~33 MB of unreachable mip data** (ESTIMATE, assuming 2048² for 5 of them). Fixing the clamp is a free win and is the core of Option A.

4. **The RTXTF `uint16_t` warning is correct — and it also breaks fxc, which the plan implies is DXC-only.** The plan says the types "do not [predeclare]" in DXC and that NVIDIA's own test fails against DXC. MEASURED on this machine with `C:\VulkanSDK\1.3.296.0\Bin\dxc.exe` (1.8.0.4739) and Windows SDK `fxc.exe` (10.1, `10.0.26100.0`):

   | Probe | dxc `cs_6_0` / `cs_6_6` | fxc `cs_5_0` / `cs_5_1` |
   |---|---|---|
   | plain compute | PASS / PASS | PASS / PASS |
   | `uint16_t a; uint16_t2 b; uint16_t4 c;` | **FAIL** `error: unknown type name 'uint16_t'` | **FAIL** `error X3000` |
   | `#include <cstdint>` | **FAIL** `'cstdint' file not found` | **FAIL** `error X1507` |
   | `typedef uint16_t uint16_t;` | **FAIL** (still unknown) | — |
   | **`min16uint` / `min16uint2` / `min16uint4`** | **PASS / PASS** | **PASS / PASS** |
   | `typedef min16uint uint16_t;` + friends | **PASS / PASS** | **PASS / PASS** |

   Two conclusions. (a) The plan's rule — "*our own shader includes must not depend on someone else's preamble*" — is **verified and necessary**: a self-contained `typedef min16uint uint16_t;` preamble is the only thing that compiles, and it must be *ours*. (b) **The problem is not DXC-specific.** fxc fails identically, and fxc is what compiles Nova today (`D3DCompileFromFile`, SM5). §5.6's shader work therefore needs the preamble on the **SM5** path too. Practically: **do not use `uint16_t` at all** — use `min16uint` (native HLSL) and plain `uint` in GLSL, or write the shared header in terms of `min16uint`. Currently `grep -rn "uint16" DemonCore-Editor/assets/shaders/` returns **zero hits**, so the shaders are clean today; this is purely a constraint on new code.

5. **Effort 3–6 pd is right; the stated reason is wrong.** It is not backend divergence (see 1). It is that the change touches two hand-maintained shader twins with no shared header, widens the LOD clamp and therefore changes appearance everywhere, and lands on a path tracer where over-filtering costs sample budget. 3–6 pd stands (ESTIMATE); Option A alone is 2–3.

6. **Vulkan Nova cannot be validated for this component until Phase 4.** It compiles but is never invoked (plan §2 gap 3: no `FlushVulkanNova`). Filtered texturing's Vulkan half is compile-verified only until then.

7. **`TextureFootprint` must not be treated as an NVRHI dependency anywhere in §6.** Phase 8 lists "filtered texturing" with no API caveat; add the note that the footprint is pure shader maths and needs no NVRHI surface at all.

---

## §5.7 Opacity micromap baker

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A** | **Do not build it.** Record the measurement that gates it, close §5.7, and spend the 10–20 days on §5.3 | — | **0.5 pd** (write-up) | Nothing. Forecloses acceleration of *future* alpha-tested content until that content exists — which is the correct trade, because content is the binding constraint, not code |
| **B** | Degenerate all-opaque OMM using the **special-index** path (`FULLY_OPAQUE` = −2, zero micromap bytes). Wires the entire consumption path end-to-end so a real baker becomes a drop-in | **Vulkan works today**; DX12 needs an NVRHI rebuild | **3–5 pd** | Real per-micro-triangle opacity — but there is no opacity to encode, so nothing is lost. Forecloses claiming "OMM works" without content to prove it on |
| **C** | Full micro-triangle baker: micro-triangle grouping + micro-cluster layer storing min/max opacity per cluster, 2-state and 4-state | DX12 + Vulkan | **10–20 pd** | The plan's number, spent on **nothing measurable in this engine today**. This is the option the plan describes, and the measurement says not to build it |

### Option A — Do not build the baker

#### Approach

Close §5.7 with the measurement that gates it: **this engine has no alpha-tested geometry**, so an opacity micromap has nothing to accelerate. Then re-scope Phase 9.

An OMM exists so the ray traversal unit can reject a ray against an alpha-tested micro-triangle without invoking an any-hit shader. With zero alpha-tested triangles it has nothing to reject, and a *correct* OMM encodes "every triangle fully opaque" — which the hardware already knows, because the BLAS contains no alpha-tested geometry.

The evidence, each link MEASURED:

1. **No `discard` or `clip` in any 3D shader.** Per-shader counts over `DemonCore-Editor/assets/shaders/`:
   ```
   0  FlatColor.glsl                 1  FlatColor.hlsl
   7  Renderer2D_Circle.glsl         3  Renderer2D_Circle.hlsl
   0  Renderer2D_Line.glsl           0  Renderer2D_Line.hlsl
   0  Renderer2D_Quad.glsl           0  Renderer2D_Quad.hlsl
   0  Renderer3D_Basic.glsl          0  Renderer3D_Basic.hlsl
   0  Renderer3D_NovaRenderer.glsl   0  Renderer3D_NovaRenderer.hlsl
   ```
   The only hits are **2D UI sprites**. Both 3D paths — raster *and* path tracer — shade every triangle unconditionally.
2. **No alpha-cutout concept in the material system.** `grep -rn "AlphaTest|alphaTest|AlphaCutoff|alphaCutoff|Cutout|BLEND_MASK|BlendMode"` over `Wasteland/src` returns only `Animator`'s animation `BlendMode` (`Animator.h:155`) — pose blending, unrelated to opacity.
3. **The material system says outright that albedo alpha is unused.** `Components.h:641-644`, verbatim:
   > *The alpha channel is present for type symmetry with the primitive components' Color but is **unused** here: opacity in the PBR path comes from the transmission/thickness fields, not from albedo alpha.*

   `MaterialComponent` (`Components.h:640`) has no alpha-cutout field, no texture-alpha path, and no blend-mode enum.
4. **No scene uses anything but opaque analytic primitives.** Across all 5 `.wastescene` files: 30× `CubeRendererComponent`, 8× `SphereRendererComponent`, 1 each of Plane / Grid / Circle3D / IcoSphere / Cylinder / Cone / Torus. Textures referenced: `metal_floor.jpg` (13), `wood_floor.jpg` (2), `wood_wall.jpg` (1), `Checkerboard.png` (1), and 20 empty paths. **Zero `Alpha*` / `Cutout*` / `Opacity*` / `Blend*` keys in any scene file.**
5. **There are no mesh assets at all.** `DemonCore-Editor/assets/` contains `ai/ graphs/ scenes/ scripts/ shaders/ textures/ trees/` — **no `meshes/`**. A repo-wide find for `.obj/.fbx/.gltf/.glb` outside vendor returns only `obj/Debug/Bindings.obj` and `obj/Release/Bindings.obj`, which are **pygount LOC-analysis output**, not geometry.

**The decision rule this supports:** the binding constraint on §5.7 is *content*, not code. Folding 10–20 person-days into a baker for an engine whose entire geometry is cubes and spheres produces a system that is provably correct and provably useless. Revisit when foliage or cutout cards exist — which is also when §5.3's RT path will exist to consume it.

#### Files touched

None. This option produces a decision, not code.

#### Data layout

The degenerate OMM is worth spelling out, because it is the output a *correct* baker produces for this engine's content, and it quantifies the waste:

- **`inputBuffer`: 0 bytes.** There are no opacity states to encode.
- **`perOmmDescs`: `numTriangles × 8`** — `sizeof(VkMicromapTriangleKHR)` is 8 (`uint32 dataOffset` + `uint16 subdivisionLevel` + `uint16 format`), every entry `subdivisionLevel = 0`, `format = 2`, `dataOffset = 0`.
- **Better: the special-index path.** Set `indexBuffer[i] = VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_KHR` (−2). Per the KHR spec, *"If the `micromap` provided was `VK_NULL_HANDLE`, then all indices must be one of the special indices"* — so a fully-opaque scene needs **no micromap object and no state bytes at all**, just an index buffer of −2.

For the engine's densest single mesh — a `Plane` at max subdivisions, 2·64·64 = **8192 triangles** (MEASURED from `ProceduralMeshes.cpp`: `subdivisions = ClampCount(subdivisions, 0, 64)`, topology `2*sub*sub`) — the descriptor array alone is 65,536 bytes. Adding state data at subdivision level 2 (16 micro-triangles, 2-state) adds 16,384 bytes, **all zero, all meaning "opaque"**. Total **81,920 bytes (MEASURED — see "What would prove it")** to describe 8,192 triangles that are already 100% opaque in the BLAS.

#### Backend notes

**Every symbol the plan claims for §5.7 is REAL. Verified against this repo's vendored NVRHI** — `Wasteland/vendor/nvrhi/include/nvrhi/nvrhi.h`:

| Symbol | Line | Declaration |
|---|---|---|
| `OpacityMicromapFormat` | **1383-1387** | `enum class OpacityMicromapFormat { OC1_2_State = 1, OC1_4_State = 2 };` |
| `OpacityMicromapBuildFlags` | **1389-1395** | `enum class OpacityMicromapBuildFlags : uint8_t { None = 0, FastTrace = 1, FastBuild = 2, AllowCompaction = 4 }` |
| `OpacityMicromapUsageCount` | **1399-1407** | `struct { uint32_t count; uint32_t subdivisionLevel; OpacityMicromapFormat format; }` — **12 bytes** |
| `OpacityMicromapDesc` | **1409-1437** | `flags`, `counts`, `inputBuffer(+Offset)`, `perOmmDescs(+Offset)` + fluent setters |
| `IOpacityMicromap` | **1439-1445** | `getDesc()`, `isCompacted()`, `getDeviceAddress()` |
| `OpacityMicromapHandle` | **1447** | `typedef RefCountPtr<IOpacityMicromap> OpacityMicromapHandle;` |
| `ResourceStates::OpacityMicromapWrite` | **375** | `= 0x00400000` |
| `ResourceStates::OpacityMicromapBuildInput` | **376** | `= 0x00800000` |
| `GeometryTriangles::opacityMicromap` | **1506**, setter **1522** | `IOpacityMicromap*` |
| `GeometryTriangles::ommIndexBuffer` | **1507**, setter **1524** | + `ommIndexBufferOffset`, `ommIndexFormat` |
| `GeometryTriangles::pOmmUsageCounts` | **1510**, setter **1526** | `const OpacityMicromapUsageCount*` + `numOmmUsageCounts` |
| `PipelineDesc::allowOpacityMicromaps` | **2819**, setter **2828** | `bool` |
| `ICommandList::buildOpacityMicromap` | **3459** | `virtual void buildOpacityMicromap(rt::IOpacityMicromap*, const rt::OpacityMicromapDesc&) = 0;` |
| `IDevice::createOpacityMicromap` | **3752** | `virtual rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc&) = 0;` |
| `Feature::RayTracingOpacityMicromap` | **3050** | capability query |

**The plan's claim "the consumption side already exists in NVRHI" is CORRECT**, and the surface is complete enough to consume a baked OMM without modifying NVRHI.

**The RTXMU/OMM conflict the plan describes is REAL — and broader than stated.** `vendor/nvrhi/src/vulkan/vulkan-device.cpp:361-365`, verbatim:
```cpp
case Feature::RayTracingOpacityMicromap:
#ifdef NVRHI_WITH_RTXMU
    return false; // RTXMU does not support OMMs
#else
    return m_Context.extensions.EXT_opacity_micromap;
#endif
```
So the Vulkan conflict is exactly as claimed. **But the plan implies it is Vulkan-only, and it is not**: `vendor/nvrhi/src/d3d12/d3d12-device.cpp:388-396` has the same `#ifdef NVRHI_WITH_RTXMU → m_OpacityMicromapSupported = false` under **both** the DXR-1.2 path and the NVAPI path. And `vulkan-device.cpp:243-246` emits a *user-visible warning* at device creation: `m_Context.warning("Opacity micro-maps are not currently supported by RTXMU.")`.

**Both flags are OFF in this build** — MEASURED in `vendor/nvrhi/build/CMakeCache.txt`: `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP:BOOL=OFF` (line 352), `NVRHI_WITH_RTXMU:BOOL=OFF` (line 401), `NVRHI_WITH_NVAPI:BOOL=OFF` (line 398). So the conflict is currently hypothetical on both backends — which means §5.8's "leave RTXMU off until §5.7 exists" advice is **free right now**: there is nothing to be exclusive with.

**DX11 has no OMM at all, confirmed by symbol table, not by inference.** `nm -C vendor/nvrhi/build/Release/nvrhi_d3d11.lib` shows `createOpacityMicromap@Device@d3d11` exists as a symbol, but the body is `utils::NotSupported(); return nullptr;` (`vendor/nvrhi/src/d3d11/d3d11-device.cpp:308-312`) and `buildOpacityMicromap` is `utils::NotSupported();` (`d3d11-commandlist.cpp:211-214`). Not an oversight — a deliberate stub.

**The plan's "OpenGL and DX11 have no hardware RT, so there is nothing to accelerate" is CORRECT and worth keeping verbatim.** An OMM is consumed by the RT traversal unit (`PipelineDesc::allowOpacityMicromaps`, `nvrhi.h:2819`). OpenGL 4.3 compute and SM5 have no traversal unit, so an OMM has no consumer. Nothing to accelerate, nothing to build.

#### Effort and risks

**0.5 person-days (ESTIMATE)** — a decision and this write-up.

Risk: essentially none, **provided the decision is recorded in the plan rather than in a comment**. The failure mode is that §5.7 stays on the board because it looks like work, and someone builds it in a year when the content finally exists — against a stale assumption about which NVRHI flags were on. Write the decision into §7 "Known constraints" with the measurement that justifies it and the condition for revisiting.

#### What would prove it

The measurement above *is* the proof, and it is re-runnable:

```
# Zero alpha-tested geometry (the gate for §5.7)
for f in DemonCore-Editor/assets/shaders/*3D*.glsl DemonCore-Editor/assets/shaders/*3D*.hlsl; do
  echo "$(grep -cE '\bdiscard\b|\bclip\s*\(' "$f")  $f"; done
# => all zero

grep -rn "AlphaTest\|alphaCutoff\|Cutout" Wasteland/src/ DemonCore-Editor/assets/scenes/
# => no hits
```

`omm_layout.py` (written alongside this analysis, `scratch/rtxmap/omm_layout.py`) reproduces the byte-size table below with **27/27 assertions passing**, including a negative control for the "4-state always costs 2× 2-state" assumption that byte rounding breaks at N=0 and N=1.

---

### Option B — Degenerate all-opaque OMM via special indices

#### Approach

If the team wants the plumbing proven before content exists, this is the minimum honest version. Build an OMM array in which **every** triangle is fully opaque, and consume it through the real NVRHI path.

Three levels, in increasing cost:
1. **Special-index only** (cheapest): an `ommIndexBuffer` full of −2, `micromap = nullptr`. No micromap object is created at all. Verifies only the index-buffer wiring.
2. **Degenerate micromap**: create an `IOpacityMicromap`, fill `inputBuffer` with zeros, `perOmmDescs` with `subdivisionLevel=0`. Verifies the full build path including `buildOpacityMicromap`.
3. **Trivial subdivision**: build at N=2, 2-state, all-zero states. Verifies the traversal-side hardware path actually consumes it.

#### Files touched

- New `Wasteland/src/Wasteland/Renderer/OpacityMicromap.{h,cpp}` — `#include "wlpch.h"` (forced PCH) and **must be followed by `premake5.exe vs2022`**, or the TU silently does not compile and the build still exits 0 (plan §7 gotcha #2).
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — BLAS geometry descriptors (⚠ contains the user's uncommitted D3 dead-uniform audit; stage selectively).
- `RenderCapabilities.{h,cpp}` — add an `opacityMicromapCapable` bool, set from `queryFeatureSupport(Feature::RayTracingOpacityMicromap, ...)`.

#### Data layout

For the engine's densest mesh (`Plane` sub=64, 8192 tris), N=2, 4-state:

| Region | Element | Bytes | Total |
|---|---|---|---|
| `perOmmDescs` | `VkMicromapTriangleKHR` = `u32 dataOffset` + `u16 subdivisionLevel` + `u16 format` | 8 | 65,536 |
| `inputBuffer` (state) | 16 micro-tri × 2 bits ÷ 8 | 4 | 32,768 |
| `counts` | `OpacityMicromapUsageCount` = `u32` + `u32` + `int` | 12 | 12 |
| `ommIndexBuffer` | `int32` per triangle (special index −2) | 4 | 32,768 |
| **total** | | | **131,084** (MEASURED) |

Cost per triangle: **16.0 bytes** for 100% opaque geometry.

#### Backend notes

**Asymmetric, and the asymmetry is the interesting part.**

- **Vulkan: works today, no rebuild.** `nm -C vendor/nvrhi/build/Release/nvrhi_vk.lib | grep " T .*OpacityMicromap"` shows **4 real code symbols**, including `createOpacityMicromap@Device@vulkan` and `buildOpacityMicromap@CommandList@vulkan`. The Vulkan implementation at `vendor/nvrhi/src/vulkan/vulkan-raytracing.cpp:284-321` and `:608+` is **not `#if`-gated on any option** — it calls `getMicromapBuildSizesEXT` and `createMicromapEXT` unconditionally. It compiled into the shipped `nvrhi_vk.lib` with the current flags.
- **DX12: the implementation is `#if`-gated and currently compiles to nothing.** `d3d12-raytracing.cpp:783` and `:1797` are both `#if NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP || NVRHI_WITH_NVAPI_OPACITY_MICROMAP`. Both evaluate to 0 (CMakeCache: DXR12 flag OFF at line 352, `NVRHI_WITH_NVAPI` OFF at line 398). The symbols still appear in `nvrhi_d3d12.lib` — because the stubs are `[[maybe_unused]]` shells that link fine. **Do not read symbol presence as feature availability; only the Vulkan path is real.**
- **Enabling DX12 requires a CMake flag change plus a full NVRHI rebuild of `nvrhi_d3d12.lib`** (`cmake -DNVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=ON`), and per `nvrhi.h:3457-3459` the D3D12 native path *"maps to NvAPI_D3D12_BuildRaytracingOpacityMicromapArray and requires NVAPI"* — i.e. an NVIDIA SDK dependency, which is precisely the class of dependency plan §1 forbids. **This is a licence-posture regression hiding inside a build flag.**
- **A latent NVRHI bug worth reporting upstream:** `d3d12-device.cpp:393` and `d3d12-raytracing.cpp:1801` test `NVRHI_D3D12_WITH_NVAPI_OPACITY_MICROMAP`, a macro that is **never defined anywhere** — `grep -rn "NVRHI_D3D12_WITH_NVAPI_OPACITY_MICROMAP" vendor/nvrhi/` returns only those two `#if` sites, no CMake option, no `target_compile_definitions`. The real macro is `NVRHI_WITH_NVAPI_OPACITY_MICROMAP` (`d3d12-backend.h:47-55`). Undefined in `#if` evaluates to 0, so the NVAPI OMM branch is **dead code today**. Harmless, but it means the NVAPI OMM path in this NVRHI revision has never been compiled. Do not enable it expecting it to work.
- **Extension mismatch to plan for:** this NVRHI checks `VK_EXT_opacity_micromap` (`vulkan-device.cpp:87`, `:365`), while the current Khronos spec is `VK_KHR_opacity_micromap` (promoted from EXT). The vendored Vulkan headers on this machine (`C:\VulkanSDK\1.3.296.0`) have `VK_EXT` (3 hits) and **zero** `VK_KHR` (MEASURED). So NVRHI here targets the older extension name; a driver advertising only `VK_KHR` would report OMM unsupported. Verify against the target driver before relying on it.

#### Effort and risks

**3–5 person-days (ESTIMATE)** — and that is **Vulkan-only**. DX12 adds an NVRHI rebuild plus the NVAPI licence problem above, which pushes it out of scope entirely.

1. **It proves the plumbing and nothing else.** A screenshot comparison will show zero visual difference, because the correct output for opaque geometry *is* identical to no OMM. Anyone reviewing this needs that stated up front or the work reads as a failure.
2. **DX12 cannot be done without an NVAPI dependency** — a licence-posture regression. If DX12 OMM is required later, `VK_EXT`/`VK_KHR` is the clean path and DX12 is not, which inverts the plan's "DX12 and Vulkan" framing.
3. **Consumption needs §5.3.** Nothing in the current engine builds a BLAS or calls `TraceRay`. `GeometryTriangles::setOpacityMicromap` (`nvrhi.h:1522`) has no caller. **This option cannot be validated until §5.3 lands**, so it is not truly 3–5 days of independent work — it is 3–5 days plus a dependency on a phase that does not exist.

#### What would prove it

- **Vulkan, no engine needed:** create a micromap from a synthetic 8192-triangle all-opaque mesh via `createOpacityMicromap` + `buildOpacityMicromap` in a standalone NVRHI program, and assert the build succeeds and `getDeviceAddress()` is non-zero. This exercises the exact code NVRHI ships.
- **Conservative correctness check:** build the same mesh twice, once with every triangle `FULLY_OPAQUE` and once with a known 50% alpha-tested variant, and assert the resulting images differ only within the expected coverage error.
- **After §5.3:** `D3D12_RESOURCE_STATE`/`ResourceStates::OpacityMicromapWrite` barrier correctness, via the ImGui tier readout the plan already proposes in §6 Phase 10.

---

### Option C — Full micro-triangle baker (what the plan describes)

#### Approach

The plan's §5.7 design, stated precisely:

1. **Per mesh, choose a subdivision level N** (micro-triangle count is 4^N per triangle, per `nvrhi.h:1403-1404`: *"Micro triangle count is 4^N, where N is the subdivision level"*).
2. **Classify each micro-triangle** by sampling the opacity function at its centroid (and conservative bounds) into one of four states: `Transparent`, `Opaque`, `UnknownTransparent`, `UnknownOpaque` — 2-state collapses the unknowns.
3. **Micro-cluster layer**: group adjacent micro-triangles into clusters, store min/max opacity per cluster, so a ray can reject a whole cluster with one test.
4. **Serialise**; **invalidate on geometry change**.
5. **Consume** through the existing NVRHI descriptors.

#### Files touched

- `Wasteland/src/Wasteland/Renderer/OpacityMicromapBaker.{h,cpp}` — `wlpch.h` + `premake5.exe vs2022`
- Serialisation: a new binary format under `DemonCore-Editor/assets/`
- `MeshLoader.cpp` — hook the bake at import; `Renderer3D.cpp` — attach `GeometryTriangles::setOpacityMicromap`
- `RenderCapabilities.{h,cpp}` — resolve `FeatureTier`

#### Data layout

**Measured, self-verified** by `scratch/rtxmap/omm_layout.py` (27/27 assertions pass). Derived from `VkMicromapTriangleKHR` = `{u32 dataOffset; u16 subdivisionLevel; u16 format}` (8 B) and `OpacityMicromapUsageCount` = `{u32 count; u32 subdivisionLevel; Format format}` (12 B, `nvrhi.h:1399-1407`):

**Bytes per triangle** = state bits ÷ 8, plus 8 for the descriptor:

| Geometry | Tris | N | Format | Micro-tris | State B | Desc B | Total B | B/tri |
|---|---|---|---|---|---|---|---|---|
| Cube | 12 | 2 | 2-state | 16 | 24 | 96 | 120 | 10.00 |
| Cube | 12 | 3 | 4-state | 64 | 192 | 96 | 288 | 24.00 |
| Grid 10×10 | 200 | 2 | 2-state | 16 | 400 | 1,600 | 2,000 | 10.00 |
| IcoSphere sub=2 | 320 | 3 | 4-state | 64 | 5,120 | 2,560 | 7,680 | 24.00 |
| IcoSphere sub=3 | 1,280 | 3 | 4-state | 64 | 20,480 | 10,240 | 30,720 | 24.00 |
| Sphere seg=32 | 2,048 | 3 | 4-state | 64 | 32,768 | 16,384 | 49,152 | 24.00 |
| **Plane sub=64** | **8,192** | **3** | **4-state** | **64** | **131,072** | **65,536** | **196,608** | **24.00** |
| Plane sub=64 | 8,192 | 2 | 2-state | 16 | 16,384 | 65,536 | 81,920 | 10.00 |

Triangle counts are MEASURED from `ProceduralMeshes.cpp` formulas (`IcoSphere` = 20·4^N, clamped 0..4 at `ProceduralMeshes.cpp:506`; `Plane` = 2·sub², clamped 0..64 at `:268`; `Sphere`/`Torus` = segs²·2). Byte counts are MEASURED by the harness, which self-verifies.

**The load-bearing finding from that harness** is a negative control worth keeping: the tempting assumption *"4-state costs exactly 2× 2-state"* is **false at N=0 and N=1**, where byte rounding makes both formats cost 1 byte per triangle. It only holds from N=2 up, where 2^(2N) bits is always a byte multiple. **Consequence: at N≤1 the 4-state format is free, so there is never a reason to choose 2-state below N=2.** A baker that picks 2-state at low subdivision silently gives up the `Unknown` states — which are exactly the states that make an OMM conservative and therefore safe.

**Conservative-but-tight is the hard part, and the plan is right to flag it.** Getting it wrong is *worse than no baker*, because it deletes visible geometry. The safe default is 4-state everywhere with `Unknown` on any micro-triangle whose alpha sampling is not provably all-above or all-below the cutoff.

#### Backend notes

DX12 and Vulkan only, and the two are **not** symmetric:
- **Vulkan** — implemented and linked (see Option B). Gated at runtime on `m_Context.extensions.EXT_opacity_micromap` (`vulkan-device.cpp:365`).
- **DX12** — requires `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=ON` **plus** NVAPI (`nvrhi.h:3457-3459`), which plan §1 forbids. Effectively **DX12 OMM is unavailable to this project.**
- **DX11 / OpenGL** — no traversal unit, so no consumer. `d3d11-device.cpp:308` is `NotSupported()`. Correct to exclude.

**So §5.7's stated backend set — "DX12 and Vulkan" — is really just Vulkan.** The plan should say so.

#### Effort and risks

**10–20 person-days (ESTIMATE, the plan's number), spent on nothing measurable in this engine today.**

1. **No content to bake.** The 8,192-triangle `Plane` above is the worst case and it is fully opaque. A baker over this content produces 196,608 bytes of "everything is opaque" — correct, useless, and a pure memory cost.
2. **Blocked on §5.3.** There is no BLAS build, no `TraceRay`, no `DispatchRayQuery` in the engine. `GeometryTriangles::setOpacityMicromap` has no caller. **The consumption side cannot be exercised.**
3. **No per-vertex opacity exists to bake from.** `MeshVertex` (`MeshLoader.h`) has exactly `Position[3]`, `Normal[3]`, `TexCoord[2]` — **no alpha field**. Alpha would have to come from sampling a texture by UV, and `MaterialComponent` (`Components.h:640`) does not carry a cutout texture or a cutoff threshold. So the baker's *input* does not exist: the content schema would need extending first, which is content work, not rendering work.
4. **Invalidation has no event to hang off.** There is **no geometry-change event system** — `grep` for `GeometryChanged|MeshChanged|onGeometry|BufferReloaded` over `Wasteland/src` returns nothing relevant. The only dirty flag is `s_Data.m_SceneDirty` (`Renderer3D.cpp:490`), set at `:2574`, `:3190`, `:3319`, `:3512` and consumed at `:1718`. That tracks *instance transform* changes, not mesh content changes. A cache keyed on it would silently serve stale OMMs after a mesh edit.
5. **The micro-cluster layer is the expensive half** and is what separates this from the "simpler conservative design" the plan already offers as acceptable. Ship the flat version first — or, per Option A, ship neither.

#### What would prove it

- **Correctness is testable without an RT path, and it must be tested first.** Generate synthetic alpha masks with known coverage (a disc, a checker, a gradient), bake, and assert that the OMM's decoded coverage matches the source alpha within tolerance — and specifically that **no visible texel is ever classified `Transparent`**. A single false-transparent classification is a hole in the render.
- **The byte table above** is reproducible via `python scratch/rtxmap/omm_layout.py`; it self-verifies and refuses to print on failure.
- **End-to-end needs §5.3.** Until a BLAS exists, the honest statement is compile-verified only — which per plan §7's own standard ("rendering claims require screenshots") is not "done".

---

### Recommendation

**Option A. Do not build the baker. Close §5.7 and move its 10–20 person-days to §5.3.**

The measurement is unambiguous: **zero `discard`/`clip` in any 3D shader, zero alpha-cutout in the material system, zero mesh assets in the repo, and 38 opaque analytic primitives across all 5 scenes.** `MaterialComponent` states in its own comment that albedo alpha is unused and opacity comes from transmission/thickness. An opacity micromap accelerates alpha-tested traversal; there is no alpha-tested traversal. Building one now produces a system that is provably correct and provably useless, on a phase that cannot even be consumed until §5.3 exists.

**The one exception worth building is Option B's special-index path (level 1 only, ~1 pd)** *if* the team wants the index-buffer wiring proven before content exists. It is the cheapest possible demonstration that the NVRHI consumption surface is real, and it costs almost nothing. Do not build a degenerate micromap on top of it — the zero-state micromap adds VRAM and proves nothing the special index does not.

**The decision is about content, and it should be recorded as such.** Write into §7 "Known constraints": §5.7 is deferred on content grounds, with the greps above as the re-check. Revisit when (a) foliage or cutout cards exist, (b) `MaterialComponent` gains a cutout texture + threshold, and (c) §5.3's RT path can consume it. All three must hold.

**Correct the plan's backend row regardless of the decision.** "DX12 and Vulkan" is really just **Vulkan**: DX12 OMM requires NVAPI (`nvrhi.h:3457`), which §1 forbids. And §5.8's RTXMU-vs-OMM conflict is a **non-problem today** — both flags are OFF in `CMakeCache.txt`, so there is nothing to be mutually exclusive with. Delete the "decide explicitly" item; it is currently a decision between two things that are both disabled.

### Corrections to the plan

1. **"The consumption side already exists in NVRHI" — VERIFIED CORRECT.** Every symbol is real: `rt::OpacityMicromapFormat` (`nvrhi.h:1383`), `rt::OpacityMicromapUsageCount` (`:1399`), `ResourceStates::OpacityMicromapWrite` (`:375`) and `::OpacityMicromapBuildInput` (`:376`), plus `OpacityMicromapDesc` (`:1409`), `IOpacityMicromap` (`:1439`), `OpacityMicromapHandle` (`:1447`), the five `GeometryTriangles` setters (`:1522-1526`), `PipelineDesc::allowOpacityMicromaps` (`:2819`), `ICommandList::buildOpacityMicromap` (`:3459`), `IDevice::createOpacityMicromap` (`:3752`), and `Feature::RayTracingOpacityMicromap` (`:3050`). The plan is accurate here. No correction.

2. **The RTXMU/OMM conflict is REAL — and the plan understates its scope.** `vulkan-device.cpp:361-365` is quoted correctly. **But it is not Vulkan-only**: `d3d12-device.cpp:388-396` applies the same `m_OpacityMicromapSupported = false` under **both** the DXR-1.2 and NVAPI paths. The conflict exists on **both** backends, and §5.8 should say so. Related: `vulkan-device.cpp:243-246` emits a runtime `warning("Opacity micro-maps are not currently supported by RTXMU.")`, so enabling RTXMU with an OMM-capable driver produces a log warning, not a silent failure — worth knowing before debugging.

3. **"RTXMU and OMM are mutually exclusive" is currently moot.** MEASURED in `vendor/nvrhi/build/CMakeCache.txt`: `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP:BOOL=OFF` (line 352), `NVRHI_WITH_RTXMU:BOOL=OFF` (line 401), `NVRHI_WITH_NVAPI:BOOL=OFF` (line 398). Both are off, so §5.8's "Pick one" and §7's constraint bullet describe a conflict between two disabled features. Delete both and re-add them only when one is actually enabled.

4. **The plan's backend set for §5.7 is wrong: DX12 OMM is unavailable to this project.** `nvrhi.h:3457-3459` states the D3D12 path *"Maps to NvAPI_D3D12_BuildRaytracingOpacityMicromapArray and requires NVAPI"* — and NVAPI is an NVIDIA SDK dependency, forbidden by §1's rule. `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=ON` would not give a clean path; it would give an NVAPI-dependent one. **§5.7 is a Vulkan-only component**, and the §3 tier table's "Opacity micromap | Unavailable | Unavailable | TierA | TierA" row should read `Unavailable | Unavailable | Unavailable* | TierA` with the asterisk explained. The same applies to §5.8's RTXMU row.

5. **NVRHI's Vulkan OMM is compiled unconditionally — so "OFF" is misleading.** The plan's framing (from §2 item 1) that the OMM flag is OFF implies no OMM support. MEASURED: `nm -C vendor/nvrhi/build/Release/nvrhi_vk.lib | grep " T .*OpacityMicromap"` returns **4 real code symbols** (`createOpacityMicromap@Device@vulkan`, `buildOpacityMicromap@CommandList@vulkan`, plus internal helpers). `vulkan-raytracing.cpp:284-321` and `:608+` are **not `#if`-gated on any CMake option** and call `getMicromapBuildSizesEXT`/`createMicromapEXT` directly. **The Vulkan consumption path is live in the shipped library today, and needs no NVRHI rebuild.** Contrast D3D12, where `d3d12-raytracing.cpp:783`/`:1797` are `#if`-gated to nothing — yet the symbols *still* appear in `nvrhi_d3d12.lib` as `[[maybe_unused]]` stubs. **Symbol presence in the `.lib` is not evidence of a working feature; check the `#if`.**

6. **A latent NVRHI bug: the NVAPI OMM path is dead code.** `d3d12-device.cpp:393` and `d3d12-raytracing.cpp:1801` test `NVRHI_D3D12_WITH_NVAPI_OPACITY_MICROMAP`, which is **never defined** — `grep -rn` over `vendor/nvrhi/` finds only those two `#if` sites, no CMake option, no `target_compile_definitions`. The real macro is `NVRHI_WITH_NVAPI_OPACITY_MICROMAP` (`d3d12-backend.h:47-55`, with the deliberate comment *"There's no version check available in the nvapi header… we look for one of the defines it adds"*). Undefined in `#if` evaluates to 0, so that branch has never been compiled. Harmless today; do not enable it expecting it to work. Worth an upstream issue.

7. **Extension-name drift to plan for.** NVRHI checks `VK_EXT_opacity_micromap` (`vulkan-device.cpp:87`, `:365`), while the current Khronos spec is `VK_KHR_opacity_micromap` (promoted from EXT). The Vulkan headers on this machine (`C:\VulkanSDK\1.3.296.0`) contain `VK_EXT` (3 references) and **zero** `VK_KHR` (MEASURED). Drivers advertising only the KHR name will report OMM unsupported through this NVRHI. Verify against the actual target driver before relying on the Vulkan path.

8. **The plan's "invalidate on geometry change" has no mechanism to invalidate on.** There is **no geometry-change event** in the engine — `grep` for `GeometryChanged|MeshChanged|onGeometry|BufferReloaded` over `Wasteland/src` returns nothing. The only relevant flag is `s_Data.m_SceneDirty` (`Renderer3D.cpp:490`), set at `:2574`, `:3190`, `:3319`, `:3512` and consumed at `:1718` — and it tracks *instance transform* churn, not mesh content. A mesh-edit-invalidation scheme needs a new event, or a content hash in the mesh cache key. This is part of why §5.7's "Risk: medium-high" is right, and it is worth stating explicitly: the risk is not that the baking algorithm is hard, it is that the **cache-coherence substrate does not exist**.

9. **The baker's input data does not exist either.** `MeshVertex` (`MeshLoader.h`) carries `Position[3]`, `Normal[3]`, `TexCoord[2]` and **no alpha field**; `MaterialComponent` (`Components.h:640`) has no cutout texture and no cutoff threshold. Alpha would have to be inferred by sampling a texture by UV at bake time, which means the baker needs its own notion of the material's cutout parameters — data the engine does not currently record. **This is content-schema work that must precede any baker**, and it reinforces the recommendation.

10. **Add a §7 "Known constraints" entry recording the §5.7 deferral**, with the gate stated as three simultaneous conditions: (a) alpha-tested content exists, (b) `MaterialComponent` carries a cutout texture and threshold, (c) §5.3's RT path exists to consume the OMM. Include the two greps above so the next implementer can re-check the gate in ten seconds rather than re-deriving it.

---

# §5.9 Temporal supersampling and §5.8 RTXMU — implementation option map

Scope: read-only analysis of `C:/dev/Wasteland` on branch `master`. No engine file was
created, edited or deleted. The engine does not link (§7 "Blocked — not ours"), so every
verification below is **static** (source reading, symbol/string inspection of prebuilt
libraries, a `git ls-remote` reachability probe). Nothing here is a screenshot.

**Labelling.** `MEASURED` = I ran something and the command is named. `ESTIMATE` =
engineering judgement. No third category is used.

---

## §5.9 Temporal supersampling

### What already exists, established from source

The plan (§5.9, §2, §5.1) describes this component as 3–6 person-days of largely new work,
and §5.1 describes the blue-noise jitter as "DONE". Both claims need qualifying. The
measured inventory:

| Asset | Status | Evidence |
|---|---|---|
| Temporal accumulation pass | **exists**, velocity-reprojected | `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl:2246-2323` (`RunTemporalAccumulation`), HLSL twin at `.hlsl:2297` |
| Velocity buffer | **exists** | `glsl:1169-1193` (`RunVisibilityAndVelocity`); format `RG16F` GL (`Renderer3D.cpp:1116`), `RG32_FLOAT` Vulkan (`VulkanNova.cpp:312`), `DXGI_FORMAT_R32G32_FLOAT` DX12 (`DX12Nova.cpp:243`) |
| Depth + packed normal for reprojection | **exists** | `img_TraceGBuffer` binding 6, written at `glsl:2104-2110`; `glsl:2265-2267` already does a depth-difference history rejection at threshold `0.02` |
| History buffer | **exists**, 2-deep ping-pong, full-size | `Renderer3D.cpp:576` `uint32_t AccumulationTextures[2]`; read/write flip at `Renderer3D.cpp:1771-1772`; allocated at full `RayTracingWidth/Height` at `Renderer3D.cpp:1140` |
| Bilinear upscale from subregion | **exists**, inline in composite | `glsl:2401-2418` inside `RunComposite`; extracted helper `BilinearAccum` at `glsl:2370-2387` |
| A second upscale pass | **exists in both shader twins, never scheduled** | `RunUpscale` at `glsl:2637-2665` and `.hlsl:2672`, dispatched as `case 8` (`glsl:2678`, `.hlsl:2720`). MEASURED: no `Dispatch(8`, no `pass(8`, no `u_PassID, 8` anywhere in `Wasteland/src`. `RunNovaPassSchedule` (`Renderer3D.cpp:1378-1386`) emits 1,0,2,[7×4],6,[4,5],[10]; the GL path emits the same set. **Dead code.** |
| Sharpen / RCAS / contrast-adaptive filter | **absent** | MEASURED: grep for `sharpen\|Sharpen\|upscale\|Upscale\|TAA\|resolve\|Resolve` across `Wasteland/src/Wasteland/Renderer` returns no sharpen implementation and no RCAS. `RunBilateralBlur` (`glsl:2491`) is a denoiser, not a sharpener. |
| Dilation / depth-clip / locking | **absent** | `RunTemporalAccumulation` has none of the three. |
| Blue noise asset + sampler | **exists, and is good** | `BlueNoise.{h,cpp}`, `BlueNoise.cpp:105-114`; volume `128×128×64 R8` = `128*128*64*1 = 1,048,576 B` (exactly 1 MiB). |

### Three findings that change the option set

**(1) The blue-noise jitter never reaches the shader. MEASURED.**

`Renderer3D::GetCurrentJitter` (`Renderer3D.cpp:2953-2966`) correctly delegates to
`BlueNoise::Jitter` — the plan's §5.1 claim is true *at the C++ layer*. But:

- `Renderer3D.cpp:1769` computes `currentJitter` and `Renderer3D.cpp:1839` uploads it via
  `glUniform2f(s_Data.Loc_Jitter, ...)` — **OpenGL only**. `FlushDX11Nova`
  (`Renderer3D.cpp:1387-1490`) and `FlushDX12Nova` never call `GetCurrentJitter` at all.
- The uniform is declared and **never read**. `grep -c u_Jitter` on
  `Renderer3D_NovaRenderer.glsl` returns **1** (the declaration at `glsl:87`); on the HLSL
  twin the only hit is the declaration at `.hlsl:111`. MEASURED.
- The jitter that actually displaces the primary ray is the shader-local
  `GetJitter(u_FrameIndex, u_CameraMoved)` at `glsl:2092` / `.hlsl:2142`, and that function
  is still **Halton, indexed `effectiveIndex % 16`** — `glsl:1104-1109`, `.hlsl:1277-1282`:

  ```glsl
  vec2 GetJitter(int frameIndex, float cameraMoved) {
      int effectiveIndex = (cameraMoved > 0.5f) ? 0 : frameIndex;
      float x = Halton(effectiveIndex % 16 + 1, 2);
      float y = Halton(effectiveIndex % 16 + 1, 3);
      return vec2(x, y) - 0.5f;
  }
  ```

**This is the exact 16-frame repeating sequence §5.1 says was removed.** It was removed
from C++ and left in the shader, where it is the only thing that matters. §5.1's measured
~11000× improvement is real but currently applies to a uniform nothing samples.

**(2) Jitter is applied as a UV offset, not in the projection matrix — and the matrices are already unjittered. MEASURED.**

`glsl:2093-2095`:

```glsl
vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(traceSize)) * 2.0f - 1.0f;
Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0f, 1.0f)).xyz));
```

`u_ViewProjection` and `u_InverseViewProjection` are uploaded verbatim from
`s_Data.CurrentViewProjection` / `glm::inverse(viewProj)` (`Renderer3D.cpp:1284-1285`,
`1330-1331`, uploaded at `1412-1414`, `1517-1519`). No jitter is ever folded into a matrix.

Consequences, both favourable:

- §7's "Known constraints" bullet — *"Reprojection needs unjittered matrices"* — is
  **already satisfied structurally**, not merely a rule to remember. There is no jittered
  matrix to accidentally feed anywhere.
- AMD's FSR 2 integration contract puts jitter *in the projection matrix*, and the plan
  copies that framing. This engine does not need to follow it. The engine's velocity is
  computed on the unjittered grid and says so at `glsl:1187-1191`:

  > `// NOTE: intentionally the UNjittered grid UV. History is stored per-pixel`
  > `// on that grid, so velocity = uv - prevUV lands the lookup on the texel`
  > `// that saw this surface point. Biasing by the frame jitter (tempting,`
  > `// tried) offsets the lookup by a per-frame-varying amount and smears.`

  That is exactly the convention FSR 2 wants for motion vectors, arrived at independently.

**(3) The dynamic-resolution scaler is not a ring buffer, and it does not touch the projection matrix. MEASURED.**

`UpdateDynamicResolution()` at `Renderer3D.cpp:2190-2244` is a **continuous float scaler**:
adjust every 20 frames (`if (++s_Data.DRSFramesSinceAdjust < 20) return;` at
`2208-2210`), step by `0.15f / 0.10f / 0.05f` depending on overshoot (`2221`), clamped to
`[DynamicMinScale, DynamicMaxScale]` both clamped to `[0.25, 1.0]` (`2213-2216`), with a
dead band (`ema > targetMs * 1.15` down, `ema < targetMs * 0.6` up). There is no ring
buffer of depth 3 anywhere in it.

The `0.5 / 0.66 / 0.8 / 1.0` values are real but they are **preset ceilings** set by
`ApplyQualityPreset()` (`Renderer3D.cpp:2126-2181`): Low `0.5f` (`2139`), Medium `0.66f`
(`2149`), High `0.8f` (`2160`), Ultra `1.0f` (`2172`), each immediately copied into
`s_Data.DynamicMaxScale` at `2176`.

The scale is applied by `UpdateComputeResolution()` (`Renderer3D.cpp:2103-2111`):

```cpp
s_Data.ComputeWidth  = (uint32_t)(s_Data.RayTracingWidth  * s_Data.RenderScale);
s_Data.ComputeHeight = (uint32_t)(s_Data.RayTracingHeight * s_Data.RenderScale);
```

The projection matrix is untouched. The full-res projection is used for every dispatched
subregion pixel; the shader maps the full-size image onto the dispatched subregion via
`wl_trace_extent()` (`glsl:203-207`) and `wl_buf_uv()` (`glsl:212`). **An upscaler therefore
needs no camera, projection, or resize changes at all** — it only needs to read the
subregion and write the full-size target. That is a large reduction in the work the plan
implies. Every scale change also calls `ClearAccumulationBuffers()` and zeroes
`FrameIndex` (`2230`, `2241`), which already gives the upscaler a correct history-invalidate
hook.

**(4) `BlueNoise::Jitter` cannot drive a per-pixel FSR 2 jitter as written. MEASURED.**
`BlueNoise.cpp:105-114`:

```cpp
void BlueNoise::Jitter(uint32_t frameIndex, uint32_t width, uint32_t height,
                       float &outJitterX, float &outJitterY)
{
    float a = 0.0f, b = 0.0f;
    Sample(width, height, frameIndex, 0, a, b);
    outJitterX = (a - 0.5f) * 2.0f / static_cast<float>(width  ? width  : 1);
    outJitterY = (b - 0.5f) * 2.0f / static_cast<float>(height ? height : 1);
}
```

It passes **the viewport dimensions as the sample's pixel coordinates** (`BlueNoise.cpp:109`),
so the volume lookup lands on one fixed texel, `(width % 128, height % 128)` — at 1920×1080
that is `(0, 56)` — and only the frame index varies. A single global per-frame offset is a
legitimate TAA jitter, but the spatial blue-noise structure is never used, and FSR 2's
sample-position dithering wants the value **per pixel**. `BlueNoise::Sample(x, y, frame, i, …)`
at `BlueNoise.cpp:77-103` already supports that and is correct; only this wrapper is wrong.

### Options

| Option | Summary | Backends | Est. effort | What it forecloses |
|---|---|---|---|---|
| **A — Sharpen the existing bilinear upscale** | Keep subregion trace + inline bilinear composite; add a contrast-adaptive sharpen after it. No temporal change. | All four (compute on all four) | 0.5–1.5 d | All temporal reprojection. No ghosting control, no edge-adaptive stability, no per-pixel jitter. Fixes the §5.1 shader-jitter bug only incidentally. |
| **B — FSR 2, full stage set** | Per-pixel blue-noise jitter in the tracer + luminance pyramid, dilate, depth clip, lock, reproject+accumulate at output res, RCAS. | All four | 4–7 d (ESTIMATE) | Requires an output-resolution ping-pong pair and a second sample-count regime. Locks in a per-frame alpha policy that interacts with the §5.2 SVGF plan. |
| **C — Render-res TAA with dilated reprojection + sharpened spatial upscale** | Keep history in the existing full-size allocation, add 3x3 nearest-depth dilation of depth+velocity, upgrade the composite's bilinear to a sharpened spatial filter. | All four | 2.5-4 d (ESTIMATE) | No output-resolution temporal stability; shimmer at render scales below 0.7 is bounded by the spatial filter alone. Forecloses ever moving the accumulate stage to output res without a rewrite. |
| **D — Fix the jitter plumbing only; defer the upscaler** | Land the blue-noise jitter into the shader (replacing Halton %16) and make it per-pixel; change nothing else. | All four | 0.5-1 d (ESTIMATE) | Upscaling as a feature. Leaves the bilinear upscale in place. This is the honest floor: it is a bug fix that belongs in the tree regardless of which option wins. |

### Option A — Sharpen the existing bilinear upscale

#### Approach

Leave the pipeline as it is. `RunTraceAndDenoise` (pass 1) traces into the `RenderScale`
subregion, `RunTemporalAccumulation` (pass 2) accumulates, `RunComposite` (pass 6) dispatches
at full resolution and bilinearly samples `img_Accumulation` (`glsl:2401-2418`). Add one
full-resolution filter after the composite: a contrast-adaptive sharpen with a per-pixel
contrast measure from a 3x3 luma neighbourhood, weighting an unsharp kernel
`(sum of neighbours / 8 - centre)` by `saturate(contrast - threshold)`.

Two-thirds of the perceived blur at render scale 0.66 is bilinear reconstruction softness,
which a sharpen pass removes immediately. It also fixes the finding-1 regression as a side
effect.

Implemented from the published algorithm only, per §5.9's rule: Jimenez et al.,
*Practical Temporal Supersampling*, EGSR 2022, §4 on the contrast-adaptive filter, plus
FSR 2's "Robust Contrast Adaptive Sharpening (RCAS)" description in the MIT documentation.
No FSR code is copied.

#### Files touched

Existing files only, no new engine TU:

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — `GetJitter` at `1104-1109` replaced; new `RunSharpen()`; new `case` in `switch(u_PassID)` at `2667-2681`.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — same three edits at `.hlsl:1277-1282` and `.hlsl:2720`.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` — regenerated by `tools/compile_nova_spirv.py` (per §5.10; register-space shifts u=0, b=7, t=16, s=200 apply to any new binding).
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — one dispatch in `RunNovaPassSchedule` (`1378-1386`) and one in the GL block (`1963-1972`); uniforms in `FlushDX11Nova` (`1387+`) and `FlushDX12Nova` (`1492+`).
- `Wasteland/src/Wasteland/Renderer/BlueNoise.cpp` — `Jitter` at `105-114` corrected, or bypassed by sampling per pixel in-shader.

No new `.cpp`, so **no premake regeneration** is required. That is the point of this option:
it sidesteps the §7 gotcha that a new TU silently does not compile while the build exits 0.

#### Data layout

No new render targets. Jitter stays CPU-side, 8 bytes/frame (`vec2` in NDC, the existing
`u_Jitter` convention from `BlueNoise.cpp:112-113`).

If the composite output cannot be read and written in one dispatch on a given backend, add
one full-resolution ping-pong pair: 2 x `RGBA16F` = 2 x `1920*1080*8 = 16,588,800 B` =
**31.6 MiB** at 1080p. Prefer `RGBA16F` over `RGBA32F` (2 x `33,177,600 B` = **63.3 MiB**)
because the input is already tonemapped and clamped to `[0,1]` at `glsl:2473`.

#### Backend notes

- **OpenGL**: `glBindImageTexture` units are capped at 0-7 on this driver — stated in the
  source at `glsl:2102-2103` ("Image units are capped at 0-7 on this GL driver"). Binding 6
  is `img_TraceGBuffer`, 7 is `img_Bloom_Temp`. A sharpen pass needing a 9th image unit
  **cannot** be expressed on GL. Write it as a `sampler2D` fetch instead, or fold it into
  `RunComposite`. Hard constraint, not a preference.
- **DX11**: a UAV cannot sample the resource it writes; `VulkanNova.cpp:336-339` documents
  the snapshot-copy workaround, and `DX12Nova.cpp:258-264` allocates one per image. A
  read-modify-write sharpen needs that snapshot.
- **DX12 / Vulkan**: same restriction, same existing snapshot copy.
- All four: no DXC needed, SM5 `D3DCompile` suffices. This is the only option that stays
  entirely inside the current toolchain (§2 gap 2 untouched).

#### Effort and risks

0.5–1.5 person-days (ESTIMATE): half a day for the jitter fix plus the sharpen function in
both languages, half a day for the two dispatch sites, the rest for binding shifts and
screenshot comparison.

Risks in order:

1. **Halos.** Unsharp masking on a noisy path-traced image amplifies fireflies. The
   contrast-adaptive weight is the mitigation and must be tuned first.
2. **GL image-unit ceiling** above. Fails as a compile error, which is safe, but costs time
   if found late.
3. **This is a sharpen, not a reconstruction.** At `RenderScale` 0.66 calling it temporal
   supersampling would be dishonest.

#### What would prove it

Fix `GetJitter` to the per-pixel blue-noise sequence, then render a **fixed camera at frame
16** at `RenderScale` 0.66, Release and Debug, OpenGL first. Frame 16 is reproducible
because the scaler resets `FrameIndex` on every change (`Renderer3D.cpp:2229`, `2240`) — the
protocol §6 Phase 10 already mandates. Pass condition: no visible 16-frame pattern, which
the current Halton `%16` build does show.

Cheapest experiment in §5.9, and a prerequisite for B and C. Do it first regardless.

### Option B — FSR 2, full stage set

#### Approach

Implement the FSR 2 stage list from the MIT documentation (gpuopen
`FidelityFX-SDK/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md`, MIT) and the
GDC 2021 deck: luminance pyramid, reconstruct & dilate, depth clip, create locks, reproject
& accumulate, RCAS. Reference algorithm is Jimenez et al., EGSR 2022.

1. **Per-pixel jitter** in the tracer: `jitter = Sample(pixelCoords, frame) - 0.5` in sub-pixel
   units, applied at `glsl:2093`. This is the change that makes the volume's spatial
   structure matter.
2. **Reconstruct & dilate**: 3x3 neighbourhood, keep the depth nearest the camera, take its
   velocity. `glsl:1179-1181` already reconstructs world position from depth, so
   nearest-depth is one extra compare. This is what stops edge smearing — the current raw
   bilinear tap at `glsl:2270` relies entirely on the `0.02` depth rejection at `2267`.
3. **Depth clip** the history sample against reconstructed depth.
4. **Create locks** on high-contrast / high-velocity pixels to shorten history and prevent trailing.
5. **Reproject & accumulate at output resolution**, sampling the subregion with a sharpened
   Lanczos-style filter instead of the plain bilinear at `glsl:2401-2418`.
6. **RCAS** as the final edge-adaptive sharpen.

Jitter does **not** go into the projection matrix, unlike AMD's documented contract. This
engine does not jitter matrices (finding 2), the velocity buffer is already unjittered-grid
and says why (`glsl:1187-1191`), and jittering the projection would require threading a
per-frame offset through `Camera` and invalidating the raster path for no benefit.

#### Files touched

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — `GetJitter` (`1104-1109`) becomes a per-pixel blue-noise fetch; six new pass bodies; `switch(u_PassID)` at `2667-2681` gains entries. **IDs 11, 12, 13 are free** — the schedule uses 0,1,2,4,5,6,7,8,9,10, and case 8 (`glsl:2678`) is dead code available for reuse. The plan's proposed 12/13 are indeed free.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — full mirror. Per §5.10 this doubling is the standing tax and is why §5.10 must be settled first; **Option B is the option most damaged by deferring §5.10.**
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` — regenerated via `tools/compile_nova_spirv.py`.
- `Wasteland/src/Platform/NVRHI/VulkanNova.cpp` — `CreateImages` (`304-315`) and `kImageCount` (`VulkanNova.h:100`) both need new entries. **The binding contract is positional and hand-maintained**: `VulkanNova.cpp:307-314` lists formats by index with comments naming each `img_*`, so adding images means renumbering.
- `Wasteland/src/Platform/NVRHI/DX12Nova.cpp` — `formats[]` (`237-246`), the UAV descriptor loop (`251-276`), and the `kUavBase` arithmetic.
- `Wasteland/src/Platform/NVRHI/DX11Nova.h` — image/descriptor table (see the `DX11Nova.h:258` note on velocity channel packing).
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — dispatch sites (`1378-1386`, `1963-1972`), uniforms (`1387+`, `1492+`), GL `glBindImageTexture` block (`1807-1816`).

This crosses the **GL 8-image-unit ceiling** (`glsl:2102-2103`): six stages cannot fit in 8
units alongside existing bindings. Resolve by sampling through `sampler2D` + `texture()`
instead of `image2D`/`imageLoad` — legal, and `RunTemporalAccumulation` already does this
for `s_Accumulation` at `glsl:2270`. Budget it as a real design constraint.

#### Data layout

Per 1920x1080 output. Note the accumulation buffer is already allocated at **full**
resolution (`Renderer3D.cpp:1140` allocates `AccumulationTextures[i]` at
`RayTracingWidth/Height` regardless of `RenderScale`), so full-res allocation is the norm.

| Target | Format | Bytes/px | Bytes @1080p | Notes |
|---|---|---|---|---|
| History colour ping-pong (new) | `RGBA16F` | 8 | 16,588,800 (15.82 MiB) x2 = **31.6 MiB** | output res; `RGBA32F` would be 63.3 MiB |
| History metadata (new) | `RG16F` + `R16F` | 6 | 12,441,600 (11.86 MiB) | lock in `.rg`, stability in `.b` |
| Luminance mip 0 (new) | `R16F` | 2 | 4,147,200 (3.96 MiB) | 5-mip chain to 120x68 |
| Luminance mips 1–4 (new) | `R16F` | 2 | ~0.55 MiB | sum of 1/4+1/16+1/64+1/256 of mip 0 |
| Dilated velocity (new) | `RG16F` | 4 | 8,294,400 (7.91 MiB) | needs its own target |
| Depth for clip/dilate | **reuse `img_TraceGBuffer`** | — | **0** | `.x` is already NDC depth, `.yzw` packed world normal (`glsl:2104-2110`) |

**New total: ~73 MiB at 1080p. ESTIMATE**, derived from the per-pixel byte arithmetic above;
no allocation was measured.

Reuse is the lever: `img_TraceGBuffer` already carries exactly the depth the clip and
dilation stages need, in the existing `RGBA32F` allocation, saving 33 MiB and one binding.

Jitter: 8 bytes/frame CPU-side, plus the 1 MiB `128x128x64 R8` volume already loaded at
`Renderer3D.cpp:914`.

#### Backend notes

- **OpenGL 4.3**: works — the whole Nova path is GL compute here. Constraints are the
  8-unit ceiling and `GL_RG16F` availability. `GL_RG16F` is proven on this driver
  (`Renderer3D.cpp:1116`); `R16F` and a sampled mip chain are not yet exercised and are the
  risk.
- **DX11**: `R16F` needs feature level 9.1, compute needs 11.0 — already satisfied since the
  Nova compute path runs here. UAV-read-same-texture forces snapshot copies.
- **DX12**: `R16F` mip chain via `CreateCommittedResource` with
  `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, matching `DX12Nova.cpp:254-255`. UAV mip
  access needs the resource created with the full mip count. No DXC needed — all SM5.
- **Vulkan**: `nvrhi::Format::R16_FLOAT`, following the `VulkanNova.cpp:306-315` pattern. The
  `tools/compile_nova_spirv.py` register-space shifts (u=0, b=7, t=16, s=200) apply to every
  new binding, and §5.10 records 4 existing collisions — 6 stages will add more, each needing
  audit.
- All four: output-res accumulation changes the role of `UpdateComputeResolution()`
  (`2103-2111`), which today sizes only the input subregion. The output side must stay fixed
  at `RayTracingWidth/Height` — it already is, since pass 6 dispatches with `full=true`
  (`Renderer3D.cpp:1380`).

#### Effort and risks

4–7 person-days (ESTIMATE). The extra day over the plan's 3–6 is the GL image-unit ceiling
plus positional binding renumbering in three backends, neither of which the plan budgets.

Risks:

1. **Shader duplication (§5.10).** 6 new passes x 2 languages = 12 bodies to keep in
   lockstep. §6 places §5.10 at Phase 1 and §5.9 at Phase 8, so this ordering is already
   correct — landing §5.9 first would violate it.
2. **Interaction with §5.2 SVGF.** Both want to own the temporal blend;
   `glsl:2318-2319` computes `alpha = mix(u_AccumulationAlpha, 0.8f, motionFactor)`. An FSR 2
   accumulate stage *replaces* this, leaving SVGF's variance guidance nowhere to apply. They
   must be sequenced, not stacked: SVGF is Phase 3, §5.9 is Phase 8, so §5.9 must replace
   pass 2 rather than feed it.
3. **Halation/ringing** from the Lanczos-style resample, mitigated by clamping — same risk
   §5.4 flags for reservoirs.
4. **No in-engine verification** while the tree does not link (§7). This option cannot meet
   its own screenshot acceptance bar until `Scene.cpp` compiles.

#### What would prove it

Standalone, before touching the engine: a CPU reference implementation of the six stages in
Python/numpy driven by synthetic input — a known depth+velocity field with a moving edge, a
constant-colour field, and a high-contrast checker. Assert (a) reprojection lands on the
correct source texel across the moving edge, (b) the accumulate is stable on the static field
after 32 frames, (c) the sharpen does not ring on the checker.

This is the repo's existing precedent: `tools/generate_blue_noise.py` self-verifies with a
negative control, and the plan credits it as one of only two executable verifications in the
tree. A stage-by-stage reference harness is the only source of signal before the link is fixed.

### Option C — Render-res TAA with dilated reprojection + sharpened spatial upscale

#### Approach

The middle path: keep the temporal accumulation where it already is — in the `RenderScale`
subregion — fix its two weaknesses, then improve the spatial upscale that follows.

1. **Per-pixel jitter**, identical to Option B step 1. This is what makes the existing
   accumulation worth having, and it is common to A, B and C.
2. **3x3 nearest-depth dilation** of depth and velocity before the history tap, replacing the
   raw bilinear at `glsl:2270`. Highest-value change to the existing pass: it fixes edge
   smearing with no output-resolution machinery.
3. **Depth clip** the history sample, turning the binary reject at `glsl:2267` into a clip.
4. **Sharpen the spatial upscale**: a mild 3x3 unsharp over the bilinear taps in
   `RunComposite` (`glsl:2401-2418`) rather than a full Lanczos resample.

A genuine TAA-quality resolve reusing the existing buffer layout almost entirely. It is
deliberately **not** full FSR 2: no luminance pyramid, no locking, no output-res accumulation.

#### Files touched

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — `GetJitter` (`1104-1109`); dilation + clip in `RunTemporalAccumulation` (`2246-2323`); sharpened taps in `RunComposite` (`2401-2418`).
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — mirrors at `.hlsl:1277-1282`, `.hlsl:2297`, and the composite body.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` — regenerated.
- `Wasteland/src/Wasteland/Renderer/BlueNoise.cpp` — only if per-pixel jitter is done on the CPU rather than in-shader.
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — uniforms only. **No dispatch-site changes**, because both touched passes are already scheduled at passes 2 and 6.

No new image slots, so **no `kImageCount` change** and no Vulkan/DX12 binding-table edit.
Dilation reads `img_TraceGBuffer` (binding 6) and `img_Velocity` (binding 5), both already
bound. This is the structural advantage over Option B.

#### Data layout

**No new allocations.** Reuses:

| Existing target | Format | Bytes @1080p | Evidence |
|---|---|---|---|
| `img_TraceGBuffer` | `RGBA32F`, 16 B/px | 33,177,600 (31.64 MiB) | binding 6; `.x` NDC depth, `.yzw` packed normal (`glsl:2104-2110`) |
| `img_Velocity` | `GL_RG16F` / `RG32_FLOAT` / `R32G32_FLOAT` | 8,294,400 (7.91 MiB) GL; 15.82 MiB VK/DX12 | `Renderer3D.cpp:1116`, `VulkanNova.cpp:312`, `DX12Nova.cpp:243` |
| `AccumulationTextures[2]` | `RGBA32F` ping-pong | 2 x 31.64 MiB = 63.3 MiB | `Renderer3D.cpp:576`, allocated `1140` |
| Dilation scratch | **none** | **0** | 3x3 x (4+4) B = 72 B of stack per thread, MEASURED from the formats above |

Jitter: 8 bytes/frame CPU-side. Blue noise volume: 1,048,576 B (1 MiB), already on disk.

**Total new bytes: 0.** That is the whole argument for this option.

#### Backend notes

- **OpenGL**: the binding set is already saturated and this option adds none — the decisive
  practical advantage. Dilation uses `imageLoad` on bindings 5 and 6 exactly as
  `RunTemporalAccumulation` does today (`glsl:2252`), so the 8-unit ceiling is respected.
- **DX11**: `RWTexture2D<float4>` and `Texture2D<float2>` already exist for these slots. No new
  descriptor, so no snapshot-copy problem.
- **DX12**: formats at `DX12Nova.cpp:237-246` suffice. Note velocity differs per backend
  (`R32G32_FLOAT` vs GL's `GL_RG16F`) while the shader declares `layout(rg16f, binding = 5)`
  (`glsl:14`); the SPIR-V path reconciles this. Untouched.
- **Vulkan**: no `kImageCount` change (`VulkanNova.h:100` stays 8), no register-space shift
  changes, no SPIR-V binding audit. **The only option that does not touch
  `tools/compile_nova_spirv.py` at all.**

#### Effort and risks

2.5–4 person-days (ESTIMATE), inside the plan's 3–6.

Risks:

1. **Stability ceiling.** Temporal accumulation at render res converges, but the output is
   still a spatial upscale of a subregion. At 0.66 (Medium) the sharpen has real work to do
   and will ring on path-tracer fireflies. This option will not match FSR 2's stability at
   aggressive scales — that is the trade being made.
2. **Firefly amplification** by the sharpen, same as Option A.
3. **Dilation correctness** — nearest-depth must use the *reconstructed* depth, not the traced
   normal, or thin geometry dilates wrongly. Fiddly, and the most likely source of a subtle
   wrong-pixel bug.
4. Still gated on §5.10 for the dual-language cost, but ~2 new bodies rather than 6.

#### What would prove it

The same fixed-camera frame-16 protocol as Option A, then a **camera-pan sweep**: capture 32
consecutive frames during a constant-velocity pan. Pass condition: no trailing edge behind
moving silhouettes. That specifically tests dilation, which the depth-rejection-only path
(`glsl:2267`) cannot pass alone. Cheap to judge from 32 captured frames, and the cheapest way
to tell whether this option suffices or Option B is required.

### Option D — Fix the jitter plumbing only; defer the upscaler

#### Approach

Not really an upscaler — it is the **floor**. Replace the shader's Halton `%16` at
`glsl:1104-1109` / `.hlsl:1277-1282` with the per-pixel blue-noise value from
`BlueNoise::Sample` (`BlueNoise.cpp:77-103`), and change nothing else. Optionally delete the
dead `RunUpscale` (case 8) or wire it up and drop the duplicate bilinear in `RunComposite`.

This exists because option A's cheapest experiment *is* this change, and because it is a
correctness fix that should land regardless of which upscaler wins. §5.1 currently claims the
Halton sequence is gone; it is gone from C++ and still live in both shader twins.

#### Files touched

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — `GetJitter` body only.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — `.hlsl:1277-1282` body only.
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — optionally remove the dead `u_Jitter` upload at `1839` and `Loc_Jitter` at `1031` once the uniform is gone from both shaders.

Three edits in two files. No new dispatch, no new binding, no new allocation, no premake
regeneration.

#### Data layout

Unchanged. Blue noise volume 1,048,576 B (1 MiB) on disk; the jitter itself is 8 bytes/frame.
Optionally removes the dead `img_Bloom_Temp` read in `RunUpscale` (`glsl:2655-2662`) — that
image is still live elsewhere, so the allocation stays.

#### Backend notes

Identical across all four, because the jitter is CPU-side or a pure ALU change: no binding
changes, no format changes, no image-unit pressure on GL. This is the property §5.1 already
claims for blue noise ("Works on every backend including OpenGL, because the jitter is a
CPU-side uniform rather than a GPU resource", `BlueNoise.h:37-38`) — but only true once the
shader actually reads it, which it currently does not.

#### Effort and risks

0.5–1 person-day (ESTIMATE).

Risks: essentially none technically. The governance risk is the real one — shipping a
half-§5.9 that §5.1 already describes as complete. Fixing the shader jitter while the plan
reports §5.1 DONE is a documentation correction, and it should be made explicitly rather than
folded into a larger commit.

#### What would prove it

The Option A experiment. If the fixed-camera frame-16 capture at `RenderScale` 0.66 shows no
16-frame pattern where the current build does, this is done. Same command, same pass
condition, and it is the cheapest signal in the entire §5.9 analysis.


### Recommendation

**Option C, gated on Option D landing first as a separate one-day fix.**

The reasoning is that the plan's 3–6 days is roughly right but its starting point is wrong.
§5.9 reads as greenfield "temporal supersampling, DLSS replacement"; measured, roughly
**70% of the data path already exists** — velocity, depth, a reprojecting history pass with
variance clamping, a full-resolution composite with an inline bilinear upscale, a working
dynamic-resolution scaler, and a genuinely excellent 1 MiB jitter volume. What is missing is
(a) the jitter never reaches the shader at all, (b) no dilation, (c) bilinear instead of
sharpened.

Option C gets all three with **zero new render targets** and no new dispatch sites, which is
why it fits the plan's own budget while Option B does not. Option B's six stages need ~73 MiB
of new allocations, a `kImageCount` change that renumbers a positional hand-maintained
binding table in three backends, and it crosses the OpenGL 8-image-unit ceiling. Option B is
the better upscaler; it is not the right next thing.

Sequence, in order:

1. **Option D now** (0.5–1 d). It is a bug fix, it is a prerequisite for A, B and C, and its
   experiment is the cheapest in this entire analysis.
2. **Option C** (2.5–4 d) after §5.10 (Phase 1) lands, per the plan's own ordering.
3. **Option B** only if the pan-sweep experiment shows C's render-res stability is
   insufficient at the render scales the Low preset actually selects.

One caution on sequencing against §5.2: SVGF (Phase 3) lands before §5.9 (Phase 8) and both
want to own the temporal blend. Whatever is chosen must **replace** `RunTemporalAccumulation`
rather than feed it, or `alpha = mix(u_AccumulationAlpha, 0.8f, motionFactor)` at
`glsl:2318-2319` becomes a blend of two competing temporal filters.

### Corrections to the plan

1. **§5.1 overstates what blue noise is wired into.** `GetCurrentJitter` does delegate to
   `BlueNoise::Jitter` (`Renderer3D.cpp:2953-2966`), but the uniform it feeds, `u_Jitter`,
   is declared and **never read** — one occurrence in each shader twin, the declaration
   (`glsl:87`, `.hlsl:111`), MEASURED. The jitter that actually displaces the primary ray is
   the shader-local `GetJitter` at `glsl:2092`, still `Halton(effectiveIndex % 16 + 1, 2/3)`
   (`glsl:1104-1109`, `.hlsl:1277-1282`). **The 16-frame repeating sequence §5.1 says was
   removed is still the one in use.** §5.1's ~11000x measurement is real but currently
   describes a code path with no GPU consumer. This should be corrected in §5.1 itself, not
   only here.
2. **`u_Jitter` is OpenGL-only.** `Renderer3D.cpp:1839` is the sole upload site;
   `FlushDX11Nova` (`1387-1490`) and `FlushDX12Nova` (`1492+`) never call
   `GetCurrentJitter`. So "works on all four backends" is currently false for this uniform.
3. **Dynamic resolution is not a 3-deep ring buffer.** §2 and §5.1 both say so.
   `UpdateDynamicResolution()` (`Renderer3D.cpp:2190-2244`) is a continuous float scaler:
   20-frame interval, steps of `0.15/0.10/0.05` (`2221`), clamped to `[0.25, 1.0]`
   (`2213-2216`), with a 1.15/0.6 dead band. The `0.5/0.66/0.8/1.0` values are real but are
   **preset ceilings** from `ApplyQualityPreset()` (`2139`, `2149`, `2160`, `2172`), not
   ring-buffer slots. The ring-buffer description should be deleted.
4. **"How the scale is applied to projection matrices" — it is not.** `UpdateComputeResolution()`
   (`2103-2111`) sizes `ComputeWidth/Height`; the projection matrix is untouched and the full-res
   projection is used for subregion pixels via `wl_trace_extent()` (`glsl:203-207`). This is
   *good news* the plan does not claim: **no upscaler option requires any camera, projection
   or resize change.**
5. **§7's "reprojection needs unjittered matrices" is already satisfied structurally.** Jitter
   is a shader-side UV offset (`glsl:2093`); `u_ViewProjection`/`u_InverseViewProjection` are
   uploaded verbatim (`Renderer3D.cpp:1412-1414`, `1517-1519`). There is no jittered matrix in
   this engine to accidentally feed. Downgrade from "single most likely failure of this whole
   task" to a property that already holds — while keeping the rule, because §5.3's hardware RT
   pass may introduce projection-level jitter that would break it.
6. **§5.9's "Works on all four backends" needs a caveat for OpenGL.** The GL path is capped
   at 8 image units (`glsl:2102-2103`) and a 6-stage FSR 2 pipeline does not fit. New passes
   on GL must sample via `sampler2D`/`texture()` rather than `image2D`/`imageLoad`.
7. **§5.4's proposed pass IDs 12/13 are free but not needed as stated.** Occupied IDs are
   0,1,2,4,5,6,7,8,9,10 (`glsl:2667-2681`); 3, 11, 12, 13 are all free. ID 3 is a gap, not a
   reserved slot.
8. **An existing pass 8 `RunUpscale` is dead code** in both shader twins (`glsl:2637-2665`,
   `.hlsl:2672`), MEASURED: no dispatch of pass 8 anywhere in `Wasteland/src`. It duplicates the
   inline bilinear in `RunComposite` but reads `img_Bloom_Temp` instead of `img_Accumulation`.
   Either wire it up or delete it — do not leave a third upscale implementation to compound
   §5.10's duplication tax.
9. **`BlueNoise::Jitter` cannot drive per-pixel jitter as written** (`BlueNoise.cpp:105-114`):
   it passes the viewport **dimensions** as the sample's pixel coordinates (line 109), so the
   lookup lands on one fixed texel, `(width % 128, height % 128)` = `(0, 56)` at 1920x1080,
   and only the frame index varies. A single global per-frame offset is valid TAA, but FSR 2's
   sample-position dithering needs the value per pixel. `BlueNoise::Sample` (`77-103`) already
   supports that; only the wrapper is wrong.
10. **§5.9's effort should be split.** Option D is 0.5–1 d and is a correctness fix; C is
    2.5–4 d; B is 4–7 d, above the plan's 3–6 and for the two reasons in the table above.

---

## §5.8 RTXMU — enable or not

### What was verified, claim by claim

Every RTXMU claim in §5.8 was checked against `Wasteland/vendor/nvrhi/`. **All eight code
claims are confirmed, at the exact line numbers stated.** The plan's RTXMU research is solid.
What the plan got wrong is the build and supply-chain picture.

| Plan claim | Verdict | Real code |
|---|---|---|
| `option(NVRHI_WITH_RTXMU ... OFF)` at `CMakeLists.txt:38` | **CONFIRMED**, exact line | `option(NVRHI_WITH_RTXMU "Use RTXMU for acceleration structure management" OFF)` |
| FetchContents RTXMU at tag `v1.4` | **CONFIRMED** | `CMakeLists.txt:61-62`: repo `https://github.com/NVIDIA-RTX/RTXMU.git`, tag `v1.4`; the FetchContent calls are `183-190` |
| Mirrors backend options, links PUBLIC | **CONFIRMED** | `CMakeLists.txt:175-176`: `option(RTXMU_WITH_VULKAN "" ${NVRHI_WITH_VULKAN})`, `option(RTXMU_WITH_D3D12 "" ${NVRHI_WITH_DX12})` |
| `vulkan-device.cpp:232-238` constructs `rtxmu::VkAccelStructManager` | **CONFIRMED**, exact lines | see quote below |
| `d3d12-device.cpp:173` / `:237` | **CONFIRMED** at both, but `:173` is **garbage collection, not construction** | `:173-188` is `CommandListLifetimeTracker::runGarbageCollection`; construction is `:237-244` |
| `vulkan-raytracing.cpp:756+` routes AS builds | **CONFIRMED**, exact | `:756-781` `PopulateBuildCommandList`, `:786+` `PopulateUpdateCommandList` |
| `d3d12-raytracing.cpp:444+` routes AS builds | **WRONG LINE.** `:444-452` is the **destructor**. Actual build routing is `:2039-2065` | `:442-453` is `AccelStruct::~AccelStruct()` |
| `d3d12-device.cpp:260` Enhanced-Barrier caveat | **CONFIRMED**, exact line | see quote below |
| `vulkan-raytracing.cpp:330-333` `isManaged = desc.isTopLevel` | **CONFIRMED**, exact lines | see quote below |
| `vulkan-device.cpp:361-365` OMM conflict | **CONFIRMED**, exact lines | see quote below |

Quoted real code:

```cpp
// Wasteland/vendor/nvrhi/src/vulkan/vulkan-device.cpp:232-246
#ifdef NVRHI_WITH_RTXMU
    if (m_Context.extensions.KHR_acceleration_structure)
    {
        m_Context.rtxMemUtil = std::make_unique<rtxmu::VkAccelStructManager>(desc.instance, desc.device, desc.physicalDevice);

        // Initialize suballocator blocks to 8 MB
        m_Context.rtxMemUtil->Initialize(8388608);

        m_Context.rtxMuResources = std::make_unique<RtxMuResources>();
    }

    if (m_Context.extensions.EXT_opacity_micromap)
    {
        m_Context.warning("Opacity micro-maps are not currently supported by RTXMU.");
    }
#endif
```

```cpp
// Wasteland/vendor/nvrhi/src/d3d12/d3d12-device.cpp:237-244
#ifdef NVRHI_WITH_RTXMU
    if (m_RayTracingSupported)
    {
        m_Context.rtxMemUtil = std::make_unique<rtxmu::DxAccelStructManager>(m_Context.device5);

        // Initialize suballocator blocks to 8 MB
        m_Context.rtxMemUtil->Initialize(8388608);
    }
#endif
```

```cpp
// Wasteland/vendor/nvrhi/src/d3d12/d3d12-device.cpp:258-263
if (SUCCEEDED(m_Context.device->QueryInterface(&m_Context.device10)) && hasOptions12 && desc.enableEnhancedBarriers)
{
#ifndef NVRHI_WITH_RTXMU // RTXMU doesn't implement Enhanced Barriers, and we need to interop with it.
    m_EnhancedBarriersSupported = m_Options12.EnhancedBarriersSupported;
#endif
}
```

```cpp
// Wasteland/vendor/nvrhi/src/vulkan/vulkan-raytracing.cpp:330-334
#ifdef NVRHI_WITH_RTXMU
    bool isManaged = desc.isTopLevel;
#else
    bool isManaged = true;
#endif
```

```cpp
// Wasteland/vendor/nvrhi/src/vulkan/vulkan-device.cpp:361-366
case Feature::RayTracingOpacityMicromap:
#ifdef NVRHI_WITH_RTXMU
    return false; // RTXMU does not support OMMs
#else
    return m_Context.extensions.EXT_opacity_micromap;
#endif
```

One finding **beyond** the plan's list: `vulkan-device.cpp:243-246` emits a runtime
`warning("Opacity micro-maps are not currently supported by RTXMU.")` whenever
`EXT_opacity_micromap` is present. So on Vulkan, enabling RTXMU does not merely make
`Feature::RayTracingOpacityMicromap` return false — it logs a warning on every device init on
any driver exposing the extension. The plan does not mention this.

### Is enabling it possible without a network fetch? No.

This is the finding that changes the recommendation.

- **`CMakeCache.txt` in the existing build tree says OFF.** MEASURED:
  `grep -i rtxmu CMakeCache.txt` -> `NVRHI_WITH_RTXMU:BOOL=OFF`, with the option's help string
  `//Use RTXMU for acceleration structure management`.
- **RTXMU is not vendored.** MEASURED: `find . -iname "*RTXMU*"` across `C:/dev/Wasteland`
  returns nothing. `_deps/` contains only `directx_headers-{build,src,subbuild}` and
  `vulkan_headers-{build,src,subbuild}`. No `rtxmu-src`.
- **RTXMU is not in the prebuilt libraries.** MEASURED: `strings -a` over
  `Release/nvrhi_vk.lib` and `Release/nvrhi_d3d12.lib`, grepped for `rtxmu` (case-insensitive),
  returns **zero hits** in both. The `#ifdef NVRHI_WITH_RTXMU` blocks are not compiled in.
- **So enabling requires a network fetch at configure time.** `CMakeLists.txt:178-190` does a
  `FetchContent_Declare`/`FetchContent_MakeAvailable` against GitHub. Network *is* available
  here — MEASURED: `git ls-remote https://github.com/NVIDIA-RTX/RTXMU.git v1.4` returns
  `0c9ce1177000d5923e2cc6a35ae9cb7ff03748d2  refs/tags/v1.4`. So the fetch would succeed on
  this machine today.

Why that is a problem for this project specifically:

1. **Reproducibility.** A pinned tag is better than nothing, but `git clone` at configure time
   means an offline or firewalled build of this open-source engine fails at the CMake step. The
   entire rest of `vendor/` is checked in — `Box2D`, `entt`, `glm`, `nvrhi`, `stb_image`,
   `spdlog`, `yaml-cpp` are all present as source trees. RTXMU would be the only third-party
   component fetched at build time. That is an inconsistency a reviewer will notice.
2. **It is a full NVRHI rebuild, not a flag flip.** The libs are prebuilt and committed
   (`premake5.lua:102` points at `Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}`, and both
   `Debug/` and `Release/` contain all four `.lib` files). Turning the option on rebuilds
   `nvrhi_d3d12` and `nvrhi_vk` — and `docs/BUILD_ISOLATION.md`'s one-MSBuild-at-a-time rule
   applies. §4's "**1** person-day / one CMake flag" is an underestimate: the flag is one line,
   but it is a fetch plus a four-lib rebuild in two configurations plus a re-verification that
   the raster paths still work, because the D3D12 barrier model changes (`:258-263`).
3. **It gains nothing until §5.3 lands.** RTXMU's entire value is acceleration-structure
   memory. There is no `RaytracingAccelStruct`, no BLAS, no TLAS anywhere in this repo today
   (§2 gap 1; §7 confirms no hardware RT exists). Enabling RTXMU today optimises a subsystem
   with zero acceleration structures in it. It would be pure risk with no measurable benefit,
   and no way to validate it — the engine does not link.
4. **It forecloses §5.7 on Vulkan permanently, for zero present gain.** Per
   `vulkan-device.cpp:361-366`, enabling RTXMU makes `Feature::RayTracingOpacityMicromap`
   return false. §7's Open Question 5 asks "Should RTXMU or the OMM baker be built?" and §6
   Phase 9 says "or RTXMU - not both on Vulkan". Deciding this **before** either exists is
   deciding blind: the OMM baker (§5.7, 10–20 days) targets a shipped feature class —
   alpha-tested foliage — while RTXMU targets a memory characteristic nobody has measured.

### Options

| Option | Summary | Effort | What it forecloses |
|---|---|---|---|
| **A — Enable now** | Flip `NVRHI_WITH_RTXMU=ON`, let CMake fetch RTXMU v1.4, rebuild nvrhi_d3d12 + nvrhi_vk in Debug and Release. | 1–3 d (ESTIMATE), not the plan's 1 | Any chance to build §5.7 OMM on Vulkan (§6 Phase 9 picks one). Adds a build-time network dependency. Buys nothing measurable until §5.3 lands. |
| **B — Defer behind §5.3; vendor RTXMU when it earns its place** | Leave the option OFF. When hardware RT lands and AS memory is measured, vendor RTXMU as source (MIT, OSI-clean) alongside the other vendor trees and rebuild. Revisit with data. | 0 now; 1–2 d later (ESTIMATE) | Nothing. Preserves both §5.7 and §5.8 as live options and keeps the tree offline-buildable. |
| **C — Never** | Drop RTXMU. If AS memory ever bites, write our own suballocator, which is straightforward next to a compaction implementation. | 0 | The compaction win. NVIDIA's own README claims compaction "is proven to reduce the total memory footprint by more than a half"; that claim is theirs, not MEASURED here. |

### Option A — Enable now

#### Approach

Set `NVRHI_WITH_RTXMU=ON` in `Wasteland/vendor/nvrhi/CMakeLists.txt` (or pass `-D` at
configure), reconfigure, rebuild `nvrhi_d3d12` and `nvrhi_vk` for both Debug and Release,
then confirm `strings` finds `rtxmu` symbols and that the raster path is unchanged.

The integration is genuinely written, which is the plan's strongest point and is confirmed
above at `vulkan-device.cpp:232-246`, `d3d12-device.cpp:237-244`,
`vulkan-raytracing.cpp:756-790`, `d3d12-raytracing.cpp:2039-2065`. There is no stub to write.

#### Files touched

- `Wasteland/vendor/nvrhi/CMakeLists.txt` — line `38` option, or the cache.
- `Wasteland/vendor/nvrhi/build/CMakeCache.txt` — regenerated.
- `Wasteland/vendor/nvrhi/build/_deps/rtxmu-{src,build,subbuild}/` — **new, fetched from GitHub**.
- `Wasteland/vendor/nvrhi/build/{Debug,Release}/nvrhi_{d3d12,vk}.lib` — rebuilt. These are committed artifacts at `premake5.lua:102`; both configurations would change.
- No engine source file changes. NVRHI's `target_compile_definitions` at `CMakeLists.txt:391-392` and `434-435` inject `NVRHI_WITH_RTXMU=1` into the backend targets.

#### Data layout

No engine-side data layout. RTXMU allocates 8 MiB suballocator blocks — confirmed at
`vulkan-device.cpp:238` and `d3d12-device.cpp:243`, both `Initialize(8388608)` = **8,388,608 B**.
Its compaction results are opaque to this engine; NVRHI returns handles
(`as->rtxmuId = accelStructsToBuild[0]` at `vulkan-raytracing.cpp:774` and
`d3d12-raytracing.cpp:2051`).

#### Backend notes

- **DX12**: Enhanced Barriers are force-disabled under RTXMU (`d3d12-device.cpp:258-263`), so
  `desc.enableEnhancedBarriers` is silently ignored. This is the plan's caveat 1, confirmed, and
  it is the one that actually bites: `m_EnhancedBarriersSupported` stays false and every D3D12
  barrier falls back to the legacy model. Affects DX12 only.
- **Vulkan**: only the TLAS is RTXMU-managed (`vulkan-raytracing.cpp:330-333`), BLAS stays on
  the standard path. Plan caveat 2, confirmed.
- **DX11**: no RT at all, so RTXMU is irrelevant. `CMakeLists.txt:175-176` derives
  `RTXMU_WITH_D3D12` from `NVRHI_WITH_DX12`, not from DX11.
- **OpenGL**: unaffected entirely.

#### Effort and risks

1–3 person-days (ESTIMATE), against §4's stated **1**. The extra time is the fetch, the
four-library rebuild under `docs/BUILD_ISOLATION.md`, and re-verification that nothing
regressed — the last being impossible right now, since the engine does not link (§7).

Risks: permanently forecloses §5.7 on Vulkan; adds a build-time network dependency to an
otherwise fully-vendored tree; changes the D3D12 barrier model for a subsystem with no
users; and ships untestable because there is no AS to compact.

#### What would prove it

`strings -a nvrhi_vk.lib | grep -i rtxmu` returning non-empty after the rebuild. That is the
whole proof, and it is already the check I ran to establish the current state. It proves the
code is in; it proves nothing about benefit.

### Option B — Defer behind §5.3; vendor when it earns its place

#### Approach

Leave `NVRHI_WITH_RTXMU=OFF`. Revisit after §5.3 delivers a working BLAS/TLAS, and decide from
a measurement: build the RT path, record peak AS memory against scene complexity, and compare
with and without compaction. If AS memory is a real constraint, vendor RTXMU as source — MIT
and OSI-clean, so this is exactly the licence-clean exception §1 describes — placing it under
`Wasteland/vendor/rtxmu` next to the other vendored trees, and flipping the option with
`NVRHI_RTXMU_FETCH_DIR` (`CMakeLists.txt:63`) pointed at it so no network access is needed.

This makes the option *reproducible* rather than fetch-dependent, which is the actual defect
in Option A.

#### Files touched

Nothing today. Later, and only if measured to be worth it:

- `Wasteland/vendor/rtxmu/` — new vendored source tree, added to the repo (MIT).
- `Wasteland/vendor/nvrhi/CMakeLists.txt` — `NVRHI_WITH_RTXMU=ON` plus `NVRHI_RTXMU_FETCH_DIR` set to the vendored path (`CMakeLists.txt:63` already provides this knob, and `:180-186` uses it).
- `Wasteland/vendor/nvrhi/build/{Debug,Release}/*.lib` — rebuilt.
- `Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp` — possibly surface AS memory in the tier readout §6 Phase 10 asks for.

#### Data layout

Unchanged. 8 MiB suballocator blocks when it does land.

#### Backend notes

Same caveats as Option A when it eventually lands (DX12 Enhanced Barriers off, Vulkan TLAS-only
management). The point of deferring is that both are *measurable* once there are AS to manage.

#### Effort and risks

Zero now. 1–2 days later (ESTIMATE): vendor, rebuild both configurations, measure. The
measurement itself is the deliverable and it is the thing §5.8 currently says to do and does
not ("Measure before enabling").

Risks: RTXMU might be needed before §5.3 completes and the deferral costs schedule. Mitigation:
§6 already places §5.8's work at Phase 9, after Phase 2's RT pass, so this ordering is the
plan's own.

#### What would prove it

The measurement: with §5.3's BLAS/TLAS in place, record total AS bytes for a representative
scene at Low/Medium/High/Ultra with the option OFF, then ON. If the ON case is not
meaningfully smaller, RTXMU stays off forever. This is a two-run experiment, not a project.

### Option C — Never

#### Approach

Drop RTXMU permanently. If acceleration-structure memory ever becomes a measured bottleneck,
implement suballocation directly in the RT pass: an offset allocator over one large buffer is
a few hundred lines, and it captures the "tightly packing acceleration structure buffers that
are less than 64 KB" half of RTXMU's value on its own.

What this option gives up is **compaction** — running `vk::CopyAccelerationStructureKHR` with
`eCompact` after a build to shrink the AS in place. That is genuinely more work: it requires
knowing which parts of an AS are unused, which is format-specific and driver-dependent. NVIDIA
claims "more than a half" footprint reduction from it (their README, **not MEASURED here**).

#### Files touched

Nothing — this is the current state. `NVRHI_WITH_RTXMU:BOOL=OFF` in the existing
`CMakeCache.txt` is already exactly this option.

#### Data layout

Unchanged, and no 8 MiB block suballocators.

#### Backend notes

No D3D12 barrier-model change, no Vulkan OMM feature loss, no build-time network dependency.
The most stable of the three.

#### Effort and risks

Zero. Risks: forfeits a real, published memory optimisation. If a future title has 20 GB of
acceleration structures, that decision gets expensive to reverse — but §5.3 does not exist, so
the scenario is hypothetical.

#### What would prove it

Nothing needs proving; it is the status quo. The trigger that would falsify it is the same
measurement as Option B's.

### Recommendation

**Defer. Option B — leave `NVRHI_WITH_RTXMU=OFF`, and revisit after §5.3 with a measurement.**

The plan's own instruction is "**Measure before enabling**", and the measurement is
impossible today: there is no `RaytracingAccelStruct`, no BLAS and no TLAS anywhere in this
repo, so there is nothing for RTXMU to compact. Enabling it now is a build-time network
fetch plus a four-library rebuild, for zero measurable benefit, that permanently forfeits
§5.7's OMM baker on Vulkan (`vulkan-device.cpp:361-366`) and changes the D3D12 barrier model
(`d3d12-device.cpp:258-263`). Every one of those costs is certain and the benefit is
hypothetical.

This also resolves §7's Open Question 5 — *"Should RTXMU or the OMM baker be built? They are
mutually exclusive on Vulkan"* — and the answer is **neither, yet**, because §6 Phase 9 lists
both as later than §5.3. The question is premature, not hard. When it becomes real, prefer the
OMM baker: it targets alpha-tested foliage, which is content this engine has, whereas RTXMU
targets a memory characteristic nobody has measured on a scene that does not yet exist in
hardware-RT form.

Two things to fix in the plan regardless of the decision:

1. **§4's "1 person-day, one CMake flag" is wrong.** It is a fetch plus a rebuild of
   `nvrhi_d3d12` and `nvrhi_vk` in both configurations, plus re-verification. 1–3 days
   (ESTIMATE). And it cannot be re-verified while the tree does not link.
2. **§5.8 should record that RTXMU is not vendored and not prebuilt.** MEASURED: zero `rtxmu`
   strings in `nvrhi_vk.lib` and `nvrhi_d3d12.lib`; nothing under `_deps/`; no RTXMU tree
   anywhere in the repo. §5.8's "Do not vendor it manually" is right about the *code* and
   silent about the *source*, which is the part that affects reproducibility.

If and when it is enabled, use `NVRHI_RTXMU_FETCH_DIR` (`CMakeLists.txt:63`, consumed at
`180-186`) to point at a vendored copy. That removes the network dependency while keeping
NVRHI's integration, and is strictly better than the default FetchContent.

### Corrections to the plan

1. **`d3d12-raytracing.cpp:444+` is not the build routing.** `:444-452` is
   `AccelStruct::~AccelStruct()`, which removes unmanaged AS at destruction. The actual
   `PopulateBuildCommandList` / `PopulateUpdateCommandList` routing is at **`:2039-2065`**.
   The Vulkan counterpart (`:756+`) is correct.
2. **`d3d12-device.cpp:173` is garbage collection, not construction.** `:173-188` is inside
   `CommandListLifetimeTracker::runGarbageCollection`; `DxAccelStructManager` is constructed
   at `:237-244`. Both lines are real RTXMU sites, but only `:237` is the one §5.8 describes.
3. **"One CMake flag / 1 person-day" understates the cost.** Enabling performs a
   `FetchContent` git clone (`CMakeLists.txt:178-190`) and rebuilds `nvrhi_d3d12` + `nvrhi_vk`
   in Debug and Release — the committed `.lib` files at `premake5.lua:102`. 1–3 days
   (ESTIMATE), and unverifiable in-engine while §7's link failure stands.
4. **RTXMU is not present in the vendored tree or the prebuilt libs.** MEASURED:
   `NVRHI_WITH_RTXMU:BOOL=OFF` in `CMakeCache.txt`; zero `rtxmu` strings in `nvrhi_vk.lib` and
   `nvrhi_d3d12.lib`; `_deps/` holds only directx-headers and vulkan-headers; no RTXMU
   directory exists anywhere under `C:/dev/Wasteland`. §5.8 reads as though the integration is
   merely switched off — it is absent, and turning it on requires network access.
5. **The OMM conflict has a second, noisier symptom.** `vulkan-device.cpp:243-246` logs
   `"Opacity micro-maps are not currently supported by RTXMU."` via `m_Context.warning`
   whenever `EXT_opacity_micromap` is present — on every device init, on any driver exposing
   the extension. Worth knowing before enabling, since it looks like a driver fault.
6. **The conflict is Vulkan-only, which sharpens the decision.** On D3D12, OMM is a separate
   `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP` option (`CMakeLists.txt:53`, default OFF,
   currently OFF per §2). So RTXMU and OMM could coexist on DX12 but not on Vulkan — meaning
   "pick one" is a per-backend choice, not a global one. If RTXMU is ever enabled, enabling it
   **only for the D3D12 NVRHI target** would preserve §5.7 on Vulkan.
7. **The MIT claim is confirmed, from RTXMU's own README:** "RTXMU is licensed under the
   [MIT License](LICENSE.txt)." Fetched from
   `raw.githubusercontent.com/NVIDIA-RTX/RTXMU/main/README.md`. Note the file is `LICENSE.txt`,
   not `LICENSE` — a bare `LICENSE` URL returns 404.

---
