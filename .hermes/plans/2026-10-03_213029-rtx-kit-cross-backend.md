# Cross-Backend RTX Kit Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Integrate NVIDIA RTX Kit (RTXDI/RTXPT/RTXGI/NRD/etc.) on the hardware-ray-tracing backends, and build compute-shader equivalents of the same features for OpenGL and DX11 so all four backends expose one feature set.

**Architecture:** A capability-tiered feature layer (`RenderFeatureSet`) that queries the active backend once and resolves each RTX-class feature to one of three tiers: `TierA_HardwareRT` (real RTX Kit SDK via DXR/VK-RT), `TierB_DenoiseCompute` (NRD, which supports D3D11), or `TierC_ComputeEmulated` (Nova-style software tracing in plain compute shaders). Backends never branch on API enum in feature code — they ask the tier.

**Tech Stack:** C++17, NVRHI 1.0.0 (vendored), DXC/libdxcompiler, DX12 + Vulkan RT, OpenGL 4.3 compute, Dear ImGui, premake5 + MSBuild.

---

## Current context (verified against the repo)

**What already exists — do not rebuild:**

| Asset | Location | Notes |
|---|---|---|
| Compute path tracer "Nova" | `Wasteland/src/Platform/NVRHI/DX11Nova.h`, `DX12Nova.cpp` | Software RT in compute. DX11 compiles `cs_5_0`. **No hardware RT anywhere.** |
| OpenGL Nova | `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` | GLSL compute, passes 0–10 |
| DX12 Nova shader | `...Renderer3D_NovaRenderer.hlsl` | Hand-maintained twin of the GLSL |
| Temporal accumulation | `Renderer3D_NovaRenderer.glsl:2207` `RunTemporalAccumulation()` | velocity-reprojected history |
| Bilateral blur denoise | `Renderer3D_NovaRenderer.glsl:2380` `RunBilateralBlur()` | 4 step sizes `{1,2,4,8}` |
| Neural rendering | `Renderer3D.h:225-233` | GLSL MLP: neural texture + neural indirect light. Homegrown analog of RTXNS/RTXNTC |
| Volumetrics | `Renderer3D_NovaRenderer.glsl` raymarch | Fog + clouds, both backends |
| Dynamic resolution | `Renderer3D.cpp` `UpdateDynamicResolution()` | 3-deep `GL_TIME_ELAPSED` ring |
| NVRHI full RT support | `vendor/nvrhi/include/nvrhi/nvrhi.h:3048-3052` | BLAS/TLAS, clusters, opacity micromap, `setRayTracingState` — **vendored but unused** |
| RTXMU (RTX Kit component) | `vendor/nvrhi/CMakeLists.txt:38` | `NVRHI_WITH_RTXMU` exists, defaults **OFF** |

**The four backends (`RendererAPI.h:16-21`):** `OpenGL`, `NVRHI_DX11`, `NVRHI_DX12`, `NVRHI_Vulkan`. All link `nvrhi`, `nvrhi_d3d11`, `nvrhi_d3d12`, `nvrhi_vk` (`premake5.lua:115-118`).

**Gap analysis — the actual problem:**

1. **No hardware ray tracing at all.** `DX12Nova.cpp:53-57` uses `D3DCompileFromFile`; `DX11Nova.h:48` uses `D3DCompileFromFile(..., "cs_5_0", ...)`. Both are SM5. No `RaytracingAccelStruct`, no `TraceRay`, no `DispatchRayQuery` in either Nova shader. Every RTX Kit component except NRD and STBN is gated on hardware RT, so **none of them can be integrated today** without Phase 2.
2. **No DXC.** `premake5.lua:155` links only `d3dcompiler.lib`. RTX Kit samples build on DXC→SPIR-V. SM5 `D3DCompile` cannot emit ray-tracing or SPIR-V.
3. **Vulkan has no Nova path.** `Renderer3D.cpp:2893` dispatches `FlushDX11Nova` then `FlushDX12Nova`; there is no `FlushVulkanNova`. Vulkan falls through to NVRHI raster. The comment at `:2930` ("Nova remains GL-only") is already stale — DX11/DX12 have it too.
4. **Shader duplication is manual.** `.glsl` and `.hlsl` are maintained by hand. `Renderer3D.cpp:2593` and `:2701` already flag `FlushDX11Nova`/`FlushDX12Nova` as "VERBATIM DUPLICATE". Phase 3 doubles this to 4 copies unless it is fixed first.
5. **Denoise is hand-rolled.** Bilateral blur + accumulation, not NRD. This is the single biggest quality gap and the cheapest win — NRD supports **D3D11, D3D12, and Vulkan** but *not* OpenGL.

**Hard constraint — OpenGL cannot run RTX Kit.** No DXR, no `VK_KHR_ray_tracing`, no DXC. NRD, RTXDI, RTXPT, RTXGI, RTXMG, RTXNTC are all unavailable there. Any OpenGL support must be compute-shader emulation. The Nova path tracer is the proof this is already viable in this engine — it does software ray traversal on OpenGL today.

**Build constraints (`docs/BUILD_ISOLATION.md`):** one MSBuild at a time, never mix Debug/Release into the same `bin/` tree, kill `MSBuild.exe`/`cl.exe`/`mspdbsrv.exe` to recover from a killed build. `premake5.lua:59-60` globs `src/**.h` and `src/**.cpp`, so new engine sources are picked up automatically; new third-party SDK sources need explicit `files` entries plus a premake re-run to regenerate the checked-in `.vcxproj`.

**Verification standard:** rendering claims require **screenshots**, not clean logs. Every phase below ends in a captured frame compared against a known baseline.

---

## Tier model

| Feature | OpenGL | DX11 | DX12 | Vulkan |
|---|---|---|---|---|
| Path tracing | TierC (Nova, exists) | TierC (exists) | TierA (RTXPT) | TierA (RTXPT) |
| Denoise | TierC (SVGF-style) | TierB (NRD) | TierB (NRD) | TierB (NRD) |
| Direct lighting | TierC | TierC | TierA (ReSTIR DI) | TierA (ReSTIR DI) |
| Indirect lighting | TierC | TierC | TierA (ReSTIR GI/PT) | TierA (ReSTIR GI/PT) |
| Diffuse GI probes | TierC (raymarch) | TierC | TierA (DDGI) | TierA (DDGI) |
| Neural shading | TierC (exists) | TierC (exists) | TierA (RTXNS) | TierA (RTXNS) |
| Neural texture | TierC (exists) | TierC | TierA (RTXNTC) | TierA (RTXNTC) |
| Blue noise sampling | TierC (STBN) | TierC | TierC | TierC |

TierC is not a consolation prize — it is the existing Nova tracer, which is a real software path tracer. TierC work is genuine engine development, not stubbing.

---

## Phase 0 — Capability layer (foundation for everything)

### Task 0.1: Define the capability query

**Objective:** One place that answers "what can this backend do", so no feature code ever branches on `RendererAPI::GetAPI()`.

**Files:**
- Create: `Wasteland/src/Wasteland/Renderer/RenderCapabilities.h`
- Create: `Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp`

**Step 1: Write the header**

```cpp
#pragma once
#include <cstdint>
#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland
{
    // Feature tiers. Every RTX-class feature resolves to exactly one.
    enum class FeatureTier : uint8_t
    {
        Unavailable = 0,  // feature absent; caller must use its raster path
        ComputeEmulated = 1,  // TierC: software tracing in plain compute
        DenoiseCompute = 2,  // TierB: NRD-class, works on D3D11 too
        HardwareRT = 3,  // TierA: real RTX Kit SDK
    };

    struct RenderCapabilities
    {
        RendererAPI::API api = RendererAPI::API::OpenGL;

        // Hardware ray tracing (DXR / VK_KHR_ray_tracing).
        bool hardwareRT = false;
        // SM 6.x features: Inline Raytracing, RayQuery, WaveOps.
        bool inlineRaytracing = false;
        bool rayQuery = false;
        uint32_t maxRecursionDepth = 0;
        // Cooperative vectors / tensor cores (RTXNTC, RTXMU).
        bool tensorCores = false;

        // Compute-shader software tracing (Nova). True on every backend today.
        bool computeRaytracing = false;
        // Can NRD be integrated (D3D11/D3D12/Vulkan)?
        bool nrdCapable = false;

        FeatureTier TierFor(FeatureTier probe) const
        {
            // probe is the TierA expectation; degrade from there.
            if (probe == FeatureTier::HardwareRT)
            {
                if (hardwareRT) return FeatureTier::HardwareRT;
                if (computeRaytracing) return FeatureTier::ComputeEmulated;
                return FeatureTier::Unavailable;
            }
            if (probe == FeatureTier::DenoiseCompute)
            {
                if (nrdCapable) return FeatureTier::DenoiseCompute;
                if (computeRaytracing) return FeatureTier::ComputeEmulated;
                return FeatureTier::Unavailable;
            }
            return probe;
        }
    };

    const RenderCapabilities &GetRenderCapabilities();
    // Re-probe after a device switch (Renderer3D::RecreateResources).
    void RefreshRenderCapabilities();
}
```

**Step 2: Implement — probe once, cache**

`RenderCapabilities.cpp` sets fields per backend:

```cpp
#include "Wasteland/Renderer/RenderCapabilities.h"
#include "Platform/NVRHI/NVRHIContext.h"

namespace Wasteland
{
    static RenderCapabilities s_Caps;

    void RefreshRenderCapabilities()
    {
        RenderCapabilities c{};
        c.api = RendererAPI::GetAPI();
        c.computeRaytracing = true; // Nova runs on all four backends today

        switch (c.api)
        {
        case RendererAPI::API::OpenGL:
            // No DXR, no DXC, no NRD. Everything is TierC.
            c.hardwareRT = false;
            c.nrdCapable = false;
            break;
        case RendererAPI::API::NVRHI_DX11:
            c.nrdCapable = true;  // NRD supports D3D11
            break;
        case RendererAPI::API::NVRHI_DX12:
            c.hardwareRT = true;
            c.inlineRaytracing = true;
            c.rayQuery = true;
            c.maxRecursionDepth = 31;
            c.tensorCores = true;
            c.nrdCapable = true;
            break;
        case RendererAPI::API::NVRHI_Vulkan:
            c.hardwareRT = true;
            c.inlineRaytracing = true;
            c.rayQuery = true;
            c.maxRecursionDepth = 31;
            c.nrdCapable = true;
            break;
        }

        s_Caps = c;
    }

    const RenderCapabilities &GetRenderCapabilities()
    {
        return s_Caps;
    }
}
```

**Step 3: Wire the probe**

In `Renderer3D::Init()` (`Renderer3D.cpp`, alongside the block at `:1608` that picks `s_Nova`/`s_Nova12`) add `RefreshRenderCapabilities();`. In `Renderer3D::RecreateResources()` add the same call.

**Step 4: Build**

```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"
cd C:/dev/Wasteland
"$MSB" Wasteland/Wasteland.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```
Expected: `Wasteland.vcxproj -> ...\bin\Release-windows-x86_64\Wasteland\Wasteland.dll`. No new file list edits needed — the `src/**.cpp` glob at `premake5.lua:60` covers it.

**Step 5: Log the resolved tiers**

Add one line to `Renderer3D::Init` after the probe:
```cpp
WL_CORE_INFO("RTX Kit tier map: RT={0} InlineRT={1} RayQuery={2} NRD={3}",
    c.hardwareRT ? "yes" : "no", c.inlineRaytracing ? "yes" : "no",
    c.rayQuery ? "yes" : "no", c.nrdCapable ? "yes" : "no");
```

**Step 6: Commit**

```bash
git add Wasteland/src/Wasteland/Renderer/RenderCapabilities.h Wasteland/src/Wasteland/Renderer/RenderCapabilities.cpp Wasteland/src/Wasteland/Renderer/Renderer3D.cpp
git commit -m "feat(renderer): add per-backend render capability tier query"
```

### Task 0.2: Collapse the duplicated pass-order code

**Objective:** `FlushDX11Nova` and `FlushDX12Nova` are flagged verbatim duplicates (`Renderer3D.cpp:2593`, `:2701`, `:2715`, `:2731`, `:2745`). Five phases of new passes would multiply this across four backends. Extract the shared schedule first.

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`

Extract the pass schedule into a table both flushes iterate, matching the existing GL order (`Renderer3D.cpp:3428-3512`): pass 0 visibility+velocity → 1 hybrid trace → 2 temporal accumulation → 7 bilateral blur ×N → 6 composite → 4/5 bloom → 10 god rays.

**Note the non-ascending order `1, 0, 2` in the DX flushes is deliberate** (`:2715` explains why) — preserve it exactly. A refactor that "sorts" it is a regression.

**Step 1:** Add a shared `NovaPassSchedule` descriptor + a `DispatchNovaPass(NovaBackend&, int pass)` helper beside `FlushDX11Nova` (`:2424`).
**Step 2:** Rewrite both flushes to loop it. **Step 3:** Build both Release and Debug (this touches hot frame code — Debug catches uninitialized reads).
**Step 4:** Screenshot-verify DX11 and DX12 renders are pixel-equivalent to the pre-refactor baseline.
**Step 5:** Commit.

---

## Phase 1 — NRD denoising (Tier B: DX11 + DX12 + Vulkan)

Highest quality-per-hour ratio in the whole plan. NRD needs **no** hardware RT and supports **D3D11**, so it lands on three of four backends immediately — including DX11, which currently gets the worst denoise in the engine.

### Task 1.1: Vendor NRD

**Files:**
- Add to `premake5.lua`: NRD source globs + include dir + links
- Regenerate `Wasteland/Wasteland.vcxproj` via premake

**Step 1: Fetch**
```bash
git clone --depth 1 --branch v3.0 https://github.com/NVIDIA-RTX/NRD.git Wasteland/vendor/NRD
```
Confirm the tag exists before relying on it; if `v3.0` is absent, list tags and pin the newest.

**Step 2: Add to `premake5.lua`** (inside the `Wasteland` project block, near the other vendor globs at `:61-68`):
```lua
IncludeDir["NRD"] = "Wasteland/vendor/NRD/Include"
IncludeDir["NRD_SHADERS"] = "Wasteland/vendor/NRD/Shaders/Include"
```
and add `"Wasteland/vendor/NRD/Source/**.cpp"` plus the prebuilt lib dir to `libdirs` (`premake5.lua:99`).

**Step 3:** Add `dxcompiler.lib` and `dxguid.lib` to `links` (`premake5.lua:106`) — NRD compiles with DXC, which is the Phase 2 dependency anyway.

**Step 4: Regenerate and build the engine before wiring NRD:**
```bash
premake5 vs2022
"$MSB" Wasteland/Wasteland.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```
Expected: clean build. **Do not proceed until this passes** — a broken vendor drop here contaminates every later phase.

**Step 5: Commit.**

### Task 1.2: Port the existing G-buffer to NRD's requirements

NRD requires: linear (not view-space) **world-space normals**, **linear roughness**, view-space depth, and motion vectors, with **non-jittered** matrices.

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (writes `img_MaterialPacked` at binding 3, `img_TraceGBuffer` at binding 6)
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` (add NRD input/output UAV slots)

**Step 1:** Confirm what `img_MaterialPacked` currently stores. Read `Renderer3D_NovaRenderer.glsl:22` and the writing code. If normals are view-space, add a world-space normal channel.

**Step 2:** Add a dedicated NRD input G-buffer (`NRD_Inputs`: normal.xy octahedral + roughness + viewZ + motion.xy + hitMask) rather than unpacking the packed buffer — packing formats differ per backend and NRD has a documented unpacker.

**Step 3:** **NRD requires non-jittered matrices.** Nova jitters (`GetCurrentJitter`, `Renderer3D.h:236`). Feed NRD the unjittered `ProjView` as a separate constant; do not remove the jitter from the main path.

**Step 4:** Build + screenshot-verify the frame is unchanged before denoising is switched on.

### Task 1.3: Wire NRD into the DX11/DX12/Vulkan flushes

**Files:**
- Create: `Wasteland/src/Platform/NVRHI/NVRHINovaDenoise.h`
- Create: `Wasteland/src/Platform/NVRHI/NVRHINovaDenoise.cpp`

`DX12Nova.cpp:517-548` already builds SRVs for `images[i]` and `images[i].snapshot`, and `DX12Nova.h:145` takes a `const std::vector<ID3D12Resource*>&`. NRD goes between the trace pass and the composite, replacing the bilateral blur (`Renderer3D_NovaRenderer.glsl:2380`).

**Step 1:** Instantiate NRD once per device in `NVRHINovaDenoise::Init`, `Resize(w,h)` on resize.
**Step 2:** Per frame: `NewFrame` → `SetCommonSettings` → `SetDenoiserSettings` → `Denoise`.
**Step 3:** Gate on `GetRenderCapabilities().nrdCapable`; when false, keep bilateral blur. **Both paths must stay live** — OpenGL never leaves TierC.
**Step 4:** Expose a quality selector mapping to NRD's denoiser modes; **Default = REBLUR**, not OPTIX — REBLUR is the temporal mode that survives reprojection on moving cameras, which is what this engine does.
**Step 5:** Build Release **and** Debug.
**Step 6:** **Screenshot-verify at 4K** on DX11, DX12, Vulkan: same camera, same frame index, before/after. Expect visibly less noise at equal spp, and **no ghosting or smearing under camera motion** — that is the REBLUR-correctness check, and it is the single most likely failure.
**Step 7:** Commit.

---

## Phase 2 — Hardware ray tracing on DX12 + Vulkan

This is the gate for every remaining RTX Kit component. Until it lands, RTXPT/RTXDI/RTXGI/RTXNS/RTXNTC cannot be integrated at all.

### Task 2.1: Add DXC

**Files:**
- Modify: `premake5.lua`

**Step 1:** Vendor `dxcompiler` + `dxil` into `Wasteland/vendor/dxc/` (release zip from GitHub releases; take `bin/x64/dxcompiler.dll`, `dxil.dll`, `libdxcompiler.lib`, `dxil.lib`, `dxc.exe`).
**Step 2:** Add `IncludeDir["DXC"]`, lib dir, and `dxcompiler.lib` to `links`.
**Step 3:** Verify SPIR-V codegen works before building on it — RTX Kit's Vulkan path needs DXC→SPIR-V:
```bash
./Wasteland/vendor/dxc/bin/x64/dxc.exe -spirv -T cs_6_6 -E main test.hlsl -Fo test.spv
```
Expected: `test.spv` produced, exit 0.
**Step 4:** Commit.

### Task 2.2: Confirm NVRHI RT is actually wired

NVRHI vendors full RT (`nvrhi.h:3048-3052`) but the engine never calls it. Verify the built `nvrhi_d3d12.lib`/`nvrhi_vk.lib` export the RT symbols and that `vendor/nvrhi/build/%{cfg.buildcfg}` (premake5.lua:102) contains a build made with RT enabled. If NVRHI was built RT-off, rebuild it with the RT option **on** before continuing — otherwise Task 2.3 cannot link.

### Task 2.3: Build a BLAS/TLAS for the existing scene

**Files:**
- Create: `Wasteland/src/Platform/NVRHI/NVRHIAccelStruct.h`
- Create: `Wasteland/src/Platform/NVRHI/NVRHIAccelStruct.cpp`

**Step 1:** BLAS per mesh. Geometry buffers come from `VertexArray`; `ProceduralMeshes.h` primitives already submit analytic Nova instances — those need either real triangles or exclusion from the BLAS.
**Step 2:** One TLAS over all instances, rebuilt when transforms change. Reuse RTXMU (`vendor/nvrhi/CMakeLists.txt:38`, `NVRHI_WITH_RTXMU`, currently OFF) rather than hand-rolling compaction — that is what the flag is there for.
**Step 3:** Enable `NVRHI_WITH_RTXMU=ON` and rebuild NVRHI.
**Step 4:** Enable the DX12/Vulkan RT device feature in `NVRHIContext.cpp` `CreateDeviceVulkan` / DX12 creation.
**Step 5:** Validate with a plain `TraceRay` shader: a hardcoded triangle, 1 sample/pixel, render to `img_Output`.
**Step 6:** **Screenshot-verify** the triangle is on screen with correct depth. A black or NaN frame means the TLAS refit is wrong.
**Step 7:** Commit.

### Task 2.4: Add a hardware-RT trace pass alongside Nova

**Files:**
- Create: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRT.hlsl` (SM 6.6, raygen + miss + closesthit + anyhit)
- Modify: `Wasteland/src/Platform/NVRHI/DX12Nova.cpp`

**Step 1:** New pass ID (11) for the RT trace. **Keep Nova's pass 1 intact** — Nova remains the OpenGL/DX11 path and the fallback when RT init fails.
**Step 2:** Inline Raytracing (`RayQuery`) for shadows, full `TraceRay` for the primary bounce.
**Step 3:** Gate: `GetRenderCapabilities().hardwareRT`.
**Step 4:** Extend `s_Nova12` into a variant that holds both, or add a sibling class. **Prefer a sibling** — `DX12Nova.cpp:517-548` is resource-binding-dense and one throw corrupts the DX11 fallback ordering described at `Renderer3D.cpp:2902-2912`.
**Step 5:** **Screenshot-verify** DX12 and Vulkan against the Nova output on the same frame — they should match closely at high spp. Divergence means a transform-space or material mismatch, not "just sampling".
**Step 6:** Commit.

---

## Phase 3 — Fix shader authoring before it becomes a 4-way problem

`.glsl` and `.hlsl` are hand-maintained twins. Phase 2 adds SM 6.6 raygen/closesthit, taking the count to 3 files per feature. Phase 4 adds more. Every feature from here is a 3-file edit until this is solved.

**Options, in order of preference:**
1. **glslang + SPIR-V cross-compile** — author once in GLSL, compile to SPIR-V, feed NVRHI's Vulkan path; keep a thin HLSL path. Matches how RTX Kit's own samples work (DXC→SPIR-V).
2. **Slang** — NVIDIA's own choice for RTX Kit; single source targeting HLSL + SPIR-V + GLSL.
3. Accept duplication and triple the review burden.

**Deliverable:** a short `docs/SHADER_PIPELINE.md` recording the chosen path, the exact commands, and which backend consumes which artifact. Pick one and verify a round-trip on the Nova shader before building a feature on it.

---

## Phase 4 — RTX Kit components (Tier A only)

Each task is independent; order by value.

| Task | Component | Prereq | Notes |
|---|---|---|---|
| 4.1 | **STBN** blue noise | none | **DONE.** Vendor + `BlueNoise.h/.cpp` + `tools/extract_stbn.py`. Works on every backend including OpenGL (TierC) because the jitter is a CPU-side uniform. Replaced `GetCurrentJitter`'s Halton(2,3). |

### RTX Kit feasibility audit (findings, not speculation)

The RTX Kit umbrella (`NVIDIA-RTX/RTX-Kit`) lists 14 components. They are **not** uniform, and several are effectively locked away from a custom NVRHI engine. Graded by what is actually adoptable here:

| Component | Status for a custom NVRHI engine |
|---|---|
| **STBN** | **Adopted.** Only data files needed; no build system change. Done above. |
| **NRD** | **Adopted.** Vendored + built + link-verified (Task 1.1). Needs a hand-written NVRHI instance wrapper (NRD's own integration is NRI-bound). |
| **RTXTF** | **Adoptable.** Near-header-only shader library — no engine build changes. Verified on import. |
| **RTXMU** | **Via NVRHI**, not vendored standalone. `NVRHI_WITH_RTXMU=ON` exposes compaction through the abstraction this engine already uses. Rebuild NVRHI rather than hand-rolling. |
| RTXNTC / RTXNS | Neural models (ONNX weights) fetched separately, neural shading is Vulkan-only. Heavy. |
| RTXTS | Distributed via NVIDIA SDK Manager with an account login. |
| RTXMG | Unreal/NvRTX first. Large; deferred per plan. |
| RTXDI / RTXGI / RTXPT | Reference implementations built around NRI + Unreal integration patterns. Substantial adaptation effort; the algorithms (ReSTIR reservoirs, DDGI probes) are implementable directly against NVRHI instead, which is what Phase 5 does for TierC. |
| RTXCR | Character rendering (hair/skin) — needs RTXPT + Unreal integration. Out of scope. |
| Streamline | Its own plugin framework; DLSS/MFG integration, not a rendering feature. |
| OMM | Builds on RTXMG clustering. |

**Bottom line:** the kit is not a menu of drop-in modules. STBN, NRD, RTXTF and (via NVRHI) RTXMU are genuinely usable now; the rest are Unreal/NRI-shaped, and the practical route for their algorithms is to implement the well-documented techniques directly against NVRHI -- which is already the plan's Phase 5 strategy.

| 4.2 | **RTXDI** ReSTIR DI | 2.4 | Reservoir + neighbor-offset buffers. `Renderer3D_NovaRenderer.glsl` already enumerates analytic lights (`MaxAnalyticLights = 8`, `Renderer3D.h:209`) — that light list is the DI input. |
| 4.3 | **RTXGI** DDGI | 2.4 | Irradiance probes. Replaces the current sky/ambient model on DX12/Vulkan; TierC keeps raymarched probes. |
| 4.4 | **RTXPT** | 2.4 + 4.2 | Reference real-time path tracer. Supersedes Nova on TierA. |
| 4.5 | **RTXNTC / RTXNS** | 2.1 | Replaces the hand-rolled GLSL MLP (`Renderer3D.h:225-233`). Keep the MLP as the TierC path. |
| 4.6 | **RTXMG** | 2.3 | Mega Geometry clusters. Large; defer until scenes need it. |

**Per-task verification (non-negotiable):** Release **and** Debug builds; 4K screenshots on DX12 **and** Vulkan; the TierC path re-screenshotted to prove it did not regress.

---

## Phase 5 — TierC: close the OpenGL/DX11 feature gaps

The OpenGL path must track the DX12 feature set. Each entry maps a TierA feature to its software equivalent.

| TierA feature | TierC equivalent | Approach |
|---|---|---|
| NRD | **SVGF-style** denoise | Temporal reprojection is already built (`RunTemporalAccumulation`, `.glsl:2207`) with velocity. Add variance estimation + à-trous wavelet filtering driven by that same reprojection. This is a real algorithm, not a stub. |
| RTXDI ReSTIR DI | **Reservoir resampling** in compute | Reservoir math is pure arithmetic — **implementable in GLSL with no RT**. Trace with Nova's software rays. This is the highest-value TierC work. |
| RTXGI DDGI | **Raymarched probe grid** | Nova already raymarches volumetrics; reuse that traversal for probe baking. |
| RTXPT | **Nova + better sampling** | STBN blue noise (4.1) + stratified sequences + reservoir-guided paths (`Halton`, `Renderer3D.h:235`, is the current sampler). |
| RTXNTC/RTXNS | **Extend the existing MLP** | The GLSL MLP already exists. Add a decode weight set + a quality ladder. |

**Standing rule:** a TierC feature is done only when it produces a comparable screenshot on OpenGL. "It compiles" is not done.

### Task 5.1: Fill the Vulkan Nova gap

Vulkan is listed in `Renderer3DNVRHIAvailable()` (`Renderer3D.cpp:2086-2091`) but has **no Nova instance and no flush** — `FlushDX11Nova` (`:2424`) and `FlushDX12Nova` (`:2751`) are the only two, and neither is Vulkan. Vulkan therefore renders raster-only today. Since Vulkan is a TierA backend, this is a correctness gap in the tier map: it claims `hardwareRT` but cannot even do TierC.

**Files:**
- Create: `Wasteland/src/Platform/NVRHI/VulkanNova.h`
- Create: `Wasteland/src/Platform/NVRHI/VulkanNova.cpp`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp`
- Modify: `premake5.lua` (add the two `.cpp`; the `src/**.cpp` glob at `:60` already covers them, so likely no edit)

**Step 1:** Mirror `DX12Nova.h:139-147`'s interface exactly — `Init/Resize/Clear/Set/Upload/Unbind/Dispatch/CommitHistory/CommitDenoise` — implemented on `nvrhi::ICommandList` + `nvrhi::IDevice` from `NVRHIContext::GetDevice()` (`NVRHIContext.h:27`).

**Step 2:** Compile `Renderer3D_NovaRenderer.glsl` to SPIR-V (needs Phase 2.1 DXC). Load via `nvrhi::createShader` with a compute entry point, matching `NVRHIShader.cpp:263`.

**Step 3:** Add `s_NovaVK` as a `static std::unique_ptr<VulkanNova>` beside `s_Nova`/`s_Nova12` (`Renderer3D.cpp:998-999`) and a `FlushVulkanNova()` in the dispatch chain at `:2893-2927`.

**Step 4:** **Also fix the stale comment at `Renderer3D.cpp:2930`** — it says "Nova remains GL-only", which stopped being true when DX12Nova landed. Leaving it will mislead the next reader.

**Step 5:** Fix the stale `Renderer3D.h` comment at `:166` if it overstates backend parity.

**Step 6:** Build, then **screenshot-verify Vulkan against DX12 on the identical frame** — same camera, same frame index, spp matched. They should match closely; divergence means a descriptor-binding or image-layout bug (Vulkan layouts are stricter than D3D12's).

**Step 7:** Commit.

### Task 5.2: ReSTIR DI in compute (TierC)

**Files:**
- Create: `DemonCore-Editor/assets/shaders/nova_restir_common.glsl` (shared reservoir structs + resampling math)
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (new passes 12 = temporal resample, 13 = spatial resample)

**Step 1:** Define the reservoir: `vec4 target` (packed light position/direction), `vec4 w`, `uint M_hat`, `uint w_sum`, `uint lightId`. 32 bytes, matching `RTXDI_PackedGIReservoir` closely enough that the two implementations can be diffed.

**Step 2:** Reservoir arithmetic is arithmetic — it compiles unchanged in GLSL. The only RT-dependent part is candidate generation, and Nova's software rays supply that.

**Step 3:** Pass 12: temporal resample using the existing velocity/reprojection path (`.glsl:2207`). Pass 13: spatial resample over a neighbor-offset buffer, 5 taps.

**Step 4:** Validate **unbiasedness first**: with a fixed camera and a uniform emitter, the mean irradiance over the frame must equal the analytic value. Then screenshot at 4K against `spp=64` with reservoir sampling vs. `spp=1024` uniform — the images should match closely. If reservoir sampling is dimmer, the `w_sum` normalization is biased; fix before touching visuals.

**Step 5:** Commit.

### Task 5.3: SVGF-class denoise for OpenGL (TierC)

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (new passes 14 variance, 15 à-trous; retire pass 7 bilateral blur)

**Step 1:** Accumulate first/second moments of luminance in the history buffer; derive per-pixel variance.
**Step 2:** 4–5 à-trous iterations (edge-stopping on depth, normal, and variance), reusing the step ladder `{1,2,4,8}` from `RunBilateralBlur` (`.glsl:2380`).
**Step 3:** Blend with history using the existing velocity-weighted alpha (`.glsl:2279-2280`), widened where variance is high.

**Step 4:** Screenshot-verify at 4K: OpenGL at **1 spp** with SVGF should be roughly as clean as the *current* OpenGL at 8–16 spp. That is the bar; if it is not visibly better, the denoiser is eating too much detail (blurring edges) — tune `φ` (the normal/depth stopping threshold) before anything else.

**Step 5:** Confirm DX11/DX12/Vulkan are unaffected — NRD is still selected there via `nrdCapable`.

**Step 6:** Commit.

---

## Phase 6 — Quality tiering and validation

### Task 6.1: Wire tiers into `QualityPreset`

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` (`SetQualityPreset`, around the `QualityPreset` enum in `Renderer3D.h:18-24`)
- Modify: `DemonCore-Editor/src/EditorLayer.cpp` (the panel that edits these settings — 13 RT references already there)

**Step 1:** Each preset resolves to a tier set: Low/Medium = TierC only, High = TierC + NRD, Ultra = TierA where `hardwareRT`. Auto-detect GPUTier (`GetGPUTierName`, `Renderer3D.h:100`) to avoid offering TierA on hardware that lacks it.

**Step 2:** Never let a preset raise a tier above `GetRenderCapabilities()` allows — guard every assignment.

**Step 3:** Add an ImGui readout showing **resolved tier per feature**, not just the preset name. This is the single most useful debugging surface for the whole system and costs one table.

**Step 4:** Commit.

### Task 6.2: Full backend × preset validation matrix

**Objective:** Prove all four backends render correctly at all four presets.

**Step 1:** Release **and** Debug builds, one MSBuild at a time, per `docs/BUILD_ISOLATION.md`.

**Step 2:** For each of {OpenGL, DX11, DX12, Vulkan} × {Low, Medium, High, Ultra}: capture a 4K screenshot at a fixed camera and fixed frame index. 16 screenshots.

**Step 3:** Check each for: NaN/black frames, ghosting under camera motion, banding in volumetrics, water artifacts, broken tonemapping.

**Step 4:** Confirm graceful degradation: force `hardwareRT=false` on DX12 and confirm it lands on NRD+TierC cleanly rather than erroring. Force NRD off and confirm bilateral-blur fallback. Both fallbacks must be live paths, not `#ifdef` dead code.

**Step 5:** Run `tests/test_visual_blocks.py` (54 tests, per `docs/BUILD_ISOLATION.md`) to confirm no scripting regressions.

**Step 6:** Commit.

---

## Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| **OpenGL can never run RTX Kit** | Feature parity on OpenGL is approximation, always | Accepted by design. TierC is a real software path tracer, not a stub — but be honest in docs about what each tier actually is. |
| **SM5 → SM6.6 rewrite of Nova** | Large | Nova stays as the TierC/fallback path. Only new passes need SM 6.6. |
| **Vulkan descriptor/image-layout bugs** | Subtle visual corruption | Screenshot-diff Vulkan against DX12 per feature (Task 5.1 pattern). |
| **RTXMU/NVRHI rebuild** | Link breakage across the board | Isolate in Task 2.2/2.3; build engine before wiring features. |
| **Genuine duplication drift** | Silent feature divergence | Phase 3 gates Phases 4–5. |
| **RTXMU/NRD/DXC vendored binaries** | Repo bloat, supply chain | Pin versions, record licenses + SHAs in `docs/`. |
| **NVIDIA driver/arch variance** | Blackwell-only paths (coop vectors, SHaRC) | Capability-probe, never assume. RTXNTC coop-vectors needs the D3D12 Agility SDK. |

**Open questions to resolve during Phase 0:**
1. Was the vendored NVRHI built **with** RT enabled? (`Task 2.2` — gates everything in Phase 2.)
2. What is the current `img_MaterialPacked` layout — are normals view-space? (`Task 1.2`, and NRD requires world-space.)
3. Are the `ProceduralMeshes` analytic Nova instances (`:40-54`) real triangles available for a BLAS, or must they be excluded? (`Task 2.3`.)
4. Is `Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}` (`premake5.lua:102`) populated for both configs?
5. Which of the analytic lights (`MaxAnalyticLights = 8`, `Renderer3D.h:209`) will exceed that cap and need reservoir handling?

**Out of scope:** Streamline/DLSS (needs NVRHI Streamline integration + a licensing review); RTXCR digital humans (no character content yet); RTXTS texture streaming (no streaming system); Mega Geometry LOD (deferred to 4.6).

---

## Implementation progress

### Resolved open questions

1. **NVRHI WAS built with ray tracing.** `d3d12-raytracing.cpp` and `vulkan-raytracing.cpp` are compiled into `build/{Debug,Release}/nvrhi_d3d12.lib` and `nvrhi_vk.lib`; both vcxprojs list the sources. **Task 2.2's suspected rebuild is not needed** — Phase 2 goes straight to BLAS/TLAS. (`NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP` is OFF, which only disables the *micromap* extension, not RT itself.)
2. **NRD needs no new G-buffer.** `img_TraceGBuffer` (GLSL binding 6) already stores world-space packed normal + NDC depth — exactly NRD's requirement. **Correction to an earlier assumption in this plan:** `img_MaterialPacked` (binding 3) is *not* a frame G-buffer; it is a relief-map authoring buffer written by `GenerateMaterialMaps()` — rgba8, screen-space-derivative normals. Task 1.2 Step 1 is resolved as "no change needed"; skip the dedicated NRD input G-buffer unless integration proves the packing inconvenient.
3. **Vulkan truly has no Nova backend** — confirmed in code, not just by absence of `FlushVulkanNova`. Only `FlushDX11Nova`/`FlushDX12Nova` exist, so Vulkan always reaches the NVRHI raster fallback.

### Done

- **Task 0.1 — capability layer** (`8299a86`). `RenderCapabilities.h/.cpp`; `RefreshRenderCapabilities()` in `Init()`. Two gotchas worth keeping: new `.cpp` files need `#include "wlpch.h"` (forced PCH) **and** a `premake5.exe vs2022` regen — without the regen the TU silently does not compile and the build still exits 0. Release + Debug clean; object freshness verified.
- **Task 0.2 — dedupe flushes** (`dc5d6d3`). `RunNovaPassSchedule<NovaT, TexVec>` replaces the byte-identical 6-line schedule in both DX flushes. Pass order `1, 0, 2` documented as deliberate at the definition.

- **SPIR-V pipeline for the Vulkan Nova path** (`dc5d6d3` follow-up). `tools/compile_nova_spirv.py` compiles `Renderer3D_NovaRenderer.hlsl` to SPIR-V with register-space shifts (u=0, b=7, t=16, s=200) because NVRHI derives Vulkan bindings as `offset + slot` and the two otherwise collide at 4 points. Verified: 24 bindings distinct, all 84 constant-buffer member offsets identical between fxc and DXC. Side effect: fixed a real DXC incompatibility (`float3(1.0f / max(x, y))` nested in `pow()`), which was blocking DXC engine-wide.
- **Vulkan Nova backend** (`6ed933d`). `VulkanNova.h/.cpp`; mirrors the DX11/DX12 Nova interface so `RunNovaPassSchedule` drives all three. Compile-verified Release + Debug; **not linked or run** (see Blocked).
- **Task 1.1 — vendor NRD**. Cloned **v4.9.4** (plan said v3.0; that is stale — latest tag is v4.9.4) into `Wasteland/vendor/NRD`, with the `MathLib` and `ShaderMake` submodules. `tools/build_nrd.py` drives CMake+MSBuild and produces `NRD.lib`. NRD compiles **243 shaders x 3 targets (DXBC/DXIL/SPIR-V) = 729 embedded blob headers**; `_Bin/`, `_Shaders/` are gitignored. `premake5.lua` links `NRD` and adds `Include/` + `_Shaders/`.
  - **Verified at link+run level**, not just build: `tools/nrd_link_test.cpp` + `tools/test_nrd_link.sh` create a real NRD instance and read back the pipeline descriptors -> `pipelines=24 permanentPool=5 transientPool=6 samplers=2`, DXIL and SPIR-V bytecode both present. This matters because `NRD.lib` links cleanly even when it contains **zero** shaders (the blobs are generated at build time), so a link-only check cannot detect that failure mode.
  - NRD's own `Integration/NRDIntegration` depends on **NRI**, which this engine does not use (it is NVRHI). The 4.x C API is API-agnostic, so the integration must be written against NVRHI directly.

### Known NRD integration constraint (for Task 1.2/1.3)

NRD is a "bring your own pipelines" library: the host creates an `Instance` describing denoisers, NRD returns a texture pool plus per-frame `GetComputeDispatches`, and the host binds and executes them. Assembling the full `InstanceDesc` (24 pipelines, permanent+transient pools, samplers) is the bulk of the work. `NRDDenoise.h/.cpp` currently compiles and gates correctly on `RenderCapabilities::nrdCapable`, but `Init()` returns false by design until the instance wrapper is written -- **the bilateral blur stays the live path**, per Task 1.3 Step 3.

**Critical correctness constraint:** NRD requires **non-jittered** view/proj matrices. Nova jitters every frame for TAA. Feeding NRD the jittered matrices desynchronises reprojection and appears as ghosting under camera motion -- the single most likely failure of this integration. `NRDDenoiseInput` carries the unjittered matrices for this reason.

### Blocked (not mine)

`Scene.cpp` fails to compile on `Navigation::` (27 errors) — an in-flight navmesh refactor. Consequently the editor does not link, and it also fails on **41 `Renderer3D` symbols declared in `Renderer3D.h` with no implementation** (`DrawPlane`, `DrawTorus`, `DrawCylinder`, `DrawCone`, `DrawIcoSphere`, `DrawCircle`, `DrawGrid`, and every bloom/vignette/chromatic/film-grain/color-grade/bilateral accessor). Verified via `git show HEAD` and `git log -S` that none of these were ever in git history. Until that lands, **every phase in this plan is compile-verified only** — no phase can produce a screenshot, which this project's standard requires before a feature counts as working.

### Precedence warning for the next implementer

When the next task edits `Renderer3D.cpp`, check `git diff Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` first — the D3 dead-uniform audit is deliberately left uncommitted in that file. Stage selectively (`git apply --cached` with a patch built from `HEAD`) or that work will be swept into an unrelated commit.

---

## Verification standard

Per the project convention: **rendering claims require 4K screenshots, not clean logs.** Every task ending in a visual change ends with a captured frame compared against a known baseline. Both Release and Debug builds must pass before a phase is called done. `docs/BUILD_ISOLATION.md` rules apply — one MSBuild at a time, never mix Debug/Release into the same `bin/` tree.

## Execution order

```
Phase 0  capability layer + dedupe flushes   ← do first, unblocks everything
Phase 1  NRD (DX11 + DX12 + Vulkan)           ← best quality-per-hour, needs no RT
Phase 3  shader pipeline decision             ← gates Phases 4–5
Phase 2  DXC + BLAS/TLAS + RT pass            ← the gate for all RTX Kit
Phase 4  RTX Kit components (Tier A)
Phase 5  TierC equivalents + Vulkan Nova gap
Phase 6  quality tiering + full validation matrix
```

Phase 1 before Phase 2 deliberately: NRD ships visible quality on three backends without any hardware RT, and validating the NRD integration path early de-risks the more complex RT work that follows.