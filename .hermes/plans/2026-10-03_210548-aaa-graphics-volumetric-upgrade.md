# AAA Graphics + Volumetric Upgrade Roadmap

> **For Hermes:** implement phase-by-phase. Each phase expands into bite-sized
> tasks at execution time; the phase boundaries below are the real contract.

**Goal:** Move Wasteland's rendering from a software compute path tracer over
analytic primitives to an architecture that can host Cyberpunk-2077 /
Crimson-Desert-class techniques — hardware ray tracing, real mesh geometry,
clustered lighting, froxel-grid volumetrics, temporal reconstruction — with
quality tiers that stay honest on the hardware we actually have.

**Architecture:** Two-track. Track A replaces the SM 5.0 / software-tracer
foundation with SM 6.6 + hardware RT, keeping Nova as a verified fallback.
Track B layers the deferred G-buffer + clustered-light + froxel-volumetrics
stack on top. No technique is added until the pass it plugs into has a real
GPU representation.

**Tech Stack:** NVRHI (vendored, RT API already compiled in), DXC/SPIR-V
toolchain (Vulkan SDK 1.3.296 present), GLSL/HLSL dual-source shaders,
CPU BVH → TLAS, Dear ImGui editor.

---

## STATUS — updated 2026-10-04

**Line references below are STALE.** This document was written 2026-10-03 against
a working tree that has since changed substantially. Treat every `file:line` as a
hint about *where to look*, never as a location. Re-verify against the source.

### Verified working (build `MSBUILD_RC=0`, runtime evidence)

| Item | Evidence |
|---|---|
| DX12 Nova initializes and dispatches | `DX12 Nova active: api=3 rtEnabled=1 ... uavCount=9`, ~250 frames, no device-removed, no fallback |
| D1 frame ring + fencing | 3 slots, per-slot allocators/lists/descriptor heaps; drains before GPU-state destruction |
| D2 GLSL normal channel | `.rgb` → `.gba` on both bilateral-blur sites |
| D3 dead-uniform cleanup | `u_MaxBounces` / `u_SamplesPerPixel` annotated dead but RETAINED as packed placeholders (removing them would shift every later CPU-side offset). `u_IndirectRays` is the live control. Lying SPP slider removed. |
| D4 `Clear()` | Root signature + heap + UAV root table bound; clears 8 pass images + temporal history |
| DX12 `u_PassID` | Was **never** uploaded — the whole pass schedule collapsed to pass 0. Fixed. |
| Scene-texture SRV format | Now taken from the resource; zero Format made D3D12 infer a mismatched format |
| Pass 9 material-map input | Bound at `t52` (`kSamplerBase+4`, NOT `+5` — that equals `kImageSrvBase` and would clobber the first image SRV) |
| ImGui DX12 display | `srv=0` → `0x65678A00190020` (raw `ID3D12Resource*` was passed where a descriptor handle is required) |
| Editor startup | Loads `StartScene`; `Project::Bootstrap` opens the project *before* `OnAttach`, so `OnProjectChanged` never fires for `WASTELAND_PROJECT` |
| Settings persistence | `Project::Settings::RenderSettings` under a nested YAML `Render:` map; 20/20 round-trip assertions (seed → open → mutate → save → reopen) |
| Post-processing | Bloom threshold/intensity, vignette, chromatic aberration, grading, film grain implemented in BOTH GLSL and HLSL, uploaded on all 3 backends |

### RETIRED — do not act on this

- **"D3D12 rejects DXBC"** — asserted from the API contract, never observed.
  `DX12Nova` compiles `cs_5_0` via `D3DCompile` and runs fine. **No `dxcompiler`
  dependency is needed.** Adding one would have been a large change made for no
  reason. Lesson: capture the HRESULT before adding a dependency on the strength
  of an argument.

### Known-wrong state to be aware of

- **`Renderer3D.cpp` is a RECONSTRUCTION.** An accidental `git checkout --` on
  2026-10-03 destroyed ~2,800 lines of uncommitted work (6,354 → 3,078 lines).
  The file compiles and runs but a substantial part of it is a rebuild, not the
  original. **Diff it against intent before trusting it.** No backup, stash or
  dangling object contained the original.
- **`EditorLayer.cpp` was also damaged** by diagnostic cleanup (a lost
  `dockingEnabled` wrapper orphaned a closing brace) and has been repaired.

### Still open

- **DX12 frame capture.** Initialization and dispatch are proven; the rendered
  *image* is not. Needs a real staging-buffer readback path.
- **Settings validation** — malformed / out-of-range YAML values are unvalidated.
- **`VulkanNova` (~790 lines) is implemented but never constructed.**
- **Phases 0–7 not started.** Nova is still a software tracer over analytic
  primitives; no mesh path, BLAS/TLAS/SBT, clustered lighting, froxel
  volumetrics, TAA, SSAO, SSR/SSGI, or HDR pipeline.

---

## Current state — verified, not assumed

Everything below was read out of the working tree on 2026-10-03. Line
references are load-bearing: several of these are traps that have already
cost days once.

### What Nova actually is

A **software path tracer in a compute shader**, not hardware ray tracing.
All dispatches are `glDispatchCompute(w/8, h/8, 1)` —
`Renderer3D.cpp:3438-3510`, 8×8 workgroups, 1D grid. Backend-specific
ports exist for DX11 (`FlushDX11Nova`, `Renderer3D.cpp:2424`) and DX12
(`FlushDX12Nova`, `:2751`), dispatched at `:2893` / `:2920`.

**Vulkan has no ACTIVE Nova compute path** — it falls through to raster.

> **CORRECTED 2026-10-04.** This originally read "Vulkan has no Nova
> compute path at all". That was wrong. `VulkanNova.h` / `VulkanNova.cpp`
> / `VulkanDispatch.cpp` exist (~790 lines) and implement the same
> interface as the DX11/DX12 backends, and `RunNovaPassSchedule` is a
> template written for all three. What is true is narrower: **no
> VulkanNova instance is ever constructed anywhere in the tree**, so the
> code never executes. Wiring it up is Phase 0/1 work, not an accident.
> The same false claim was also removed from the comment in
> `Renderer3D.cpp`'s backend-selection branch.

Pass order per frame (`Renderer3D.cpp:3428-3512`):

| # | Pass | Shader fn | Line |
|---|------|-----------|------|
| 0 | Visibility + velocity | `RunVisibilityAndVelocity` | `.glsl:1130` |
| 1 | Hybrid trace + denoise | `RunTraceAndDenoise` | `.glsl:2045` |
| 2 | Temporal accumulation | `RunTemporalAccumulation` | `.glsl:2207` |
| 7 | Bilateral blur (≤4×) | `RunBilateralBlur` | `.glsl:2380` |
| 6 | Composite + bilinear upscale | `RunComposite` | `.glsl:2327` |
| 4/5 | Bloom threshold / blur | `RunBloomThreshold`, `...BlurVertical` | `.glsl:2286`, `2311` |
| 10 | God rays (screen-space radial) | `RunGodRays` | `.glsl:1007` |

`RunUpscale` (`.glsl:2517`) exists separately; `img_TraceGBuffer` is a single
RGBA32F intermediate (`.glsl`), **not** a real G-buffer.

### Scene representation

**Analytic primitives only.** Intersection is `HitCube` (`.glsl:415`),
`HitSphere` (`.glsl:438`), plus a thin-box branch (`.glsl:712-727`,
discriminator constants documented `.glsl:457`). **No triangles. No mesh
BVH.** `MeshLoader.cpp` exists (12 KB, loads OBJ) but its only consumer is
Python `LoadOBJ` (`Bindings.cpp:528`) — it is never wired into the tracer.

The scene is a flat array of `RayTracingInstance` (272 bytes,
`Renderer3D.cpp:127`), a CPU BVH is built by `BuildBVHRecursive` (`:521`) and
flattened into 32-byte nodes (`BVHNodeGPU`, `:156`) uploaded to SSBO
binding 2. Static-scene skip is a `memcmp` against a previous copy
(`SceneInstancesChanged`, `:1432`) — and **that memcmp depends on the struct
having zero padding**; see the trap list.

### Light transport

- **Max ray depth is effectively 2** (primary + 1 bounce), hardcoded by a loop
  clamp in the shader — not by the `MaxBounces` preset value, which is dead
  (D3). A third ray fires only for metals at Low preset.
- **No MIS. No ReSTIR. No Russian roulette. No BSDF importance sampling** —
  diffuse uses uniform hemisphere sampling and the specular lobe is direction
  *lerping*, not VNDF/GGX-pdf.
- Indirect = one bounce ray (`.glsl:2132`) or the neural fallback.
- **One BRDF only**: isotropic Cook-Torrance/GGX + Lambert diffuse. No
  anisotropic, sheen, clearcoat lobe, thin-film, subsurface, or transmission.
  There is no refraction anywhere — water is opaque.
- **Nova is 100% ray-traced shadows.** There are no shadow maps in the engine;
  every shadow is a marched ray.

### "Neural rendering" is not neural

`wl_neural_tex_detail` (`.glsl:183`), `wl_neural_micro` (`:204`),
`wl_neural_indirect` (`:223`), `wl_neural_material` (`:249`) are 4-unit
MLPs whose **weights are hardcoded float literals in the GLSL source**,
fed by `sin()` features and a `softsign`. There is no weight buffer, no
texture, no training, no inference path. They are hand-tuned procedural
functions wearing an MLP's shape.

This matters for planning: there is no learned model to improve, so "tune
the neural weights" is a shader edit, not a training run. If we want real
neural rendering later it is a from-scratch subsystem.

### Volumetrics — current implementation

`wl_march_fog_vol` (`.glsl:797`), `wl_march_cloud_vol` (`.glsl:883`).

- Fixed 32-iteration loops with `if (s >= steps) break` — required by
  GLES-era compilers, and the reason `u_VolFast` / render-scale tiers work
  by changing the *break* threshold rather than the bound.
- Caps: **4 fog volumes, 2 cloud volumes** (`Renderer3D.h:123-124`).
- **Marched inside the trace pass**, on the primary hit and sky-miss rays
  only — at `t = distance(camera, worldPos)`. There is no separate volumetric
  pass and therefore no froxel grid, no 3D texture, no temporal
  reprojection of volume data, no bilateral upsample of a half-res volume.
- **One shadow ray per analytic light per march step**
  (`IsOccluded(lRay, dL, -1)`, `.glsl:865`). This is the cost wall — it is
  why "volumetric noise is the true bottleneck" notes understate the real
  multiplier.
- Single Henyey-Greenstein phase (`.glsl:809`). **No multiple-scattering
  approximation, no octaves, no shadowed-scattering integration.**
- Cloud self-shadow = single-octave noise tap toward the sun.
- Dither is **static per-pixel and must stay that way** — a frame-cycling
  dither was tried (2026-09-06) and reverted because it defeated temporal
  accumulation and shimmered forever. Recorded in
  `skills/native-engine-rendering/references/volumetric-fog-clouds.md`.
- Noise cost is gated: `wl_vol_cheap` (1 hash) replaced `wl_vol_fbm` (24
  hashes) almost everywhere. Do not regress this.

### Sky / atmosphere

`GetSkyColor(dir)` (`.glsl:2040`) is literally
`mix(u_SkyBottomColor, u_SkyTopColor, t)` — a two-color vertical gradient.
No Rayleigh/Mie scattering, no Preetham/Hosek, no aerial-perspective LUT,
no time-of-day system anywhere in the engine.

### Confirmed absent

Grepped the whole `Wasteland/src` tree; these return nothing:

- Shadow maps / cascades / depth prepass — **every shadow in the engine is a
  traced shadow ray**.
- G-buffer, deferred shading, MRT. The editor framebuffer's second colour
  attachment is **entity-ID picking** for `ReadPixel`, not a G-buffer
  (`EditorLayer.cpp:396`, `Renderer3D_Basic.hlsl:274-275`). The Nova path's
  "G-buffer" is one compute-written RGBA32F image (`.glsl:2070`).
- SSAO / GTAO, SSR, SSGI. Water reflection is analytic sky + one traced ray;
  no planar reflection, no SSR, no refraction, no caustics.
- TAA. There is **no projection-matrix jitter** — the 16-sample Halton(2,3)
  offset is applied to the traced *ray*, so it is not TAA. DLSS / FSR / XeSS /
  any learned upscaler. Upscaling is bilinear (`.glsl:2346-2356`).
- Clustered or tiled light culling.
- LoD / impostors / hierarchical level subdivision — see D10; `LODLevel` is
  hardcoded 0.
- Bindless descriptors. 32 scene texture slots, fixed (`Renderer3D.cpp:691`).
- 3D textures **anywhere** in the engine (`Texture.h` has `Texture2D` only) —
  which blocks the froxel grid in Phase 5.
- Tone mapping: Reinhard `x/(x+1)` + gamma 2.2 (`.glsl:2363-2364`). No ACES,
  no AgX, no exposure. Swapchain is `BGRA8_UNORM`/`SBGRA8_UNORM`
  (`NVRHIContext.cpp:1656`, `1720`, `1963`) — LDR-clamped before display.
- Any time-of-day or weather system.

### Two more things worth knowing before planning against this

**Nothing in the graphics UI persists.** Every control mutates `s_Data` and is
lost on exit; there is no `SaveSettings`/`LoadSettings` anywhere in the repo.
`Project::Settings` (YAML) has `QualityPreset` and `RendererAPI` fields that
round-trip but are **never read** — nothing calls `SetQualityPreset` or
`SwitchRendererAPI` from them. The only renderer-adjacent test in the entire
suite (`SeedingSmokeTest`) verifies shader *files* exist, never that they
compile. Any new Phase 3-6 setting arrives with no persistence behind it
unless that is built too.

**`tests/` is untracked by git** (0 files under `git ls-files`). Add to the
renderer work deliberately or it will not be version-controlled.

---

### Defects found during this audit — fix these regardless of AAA scope

These are not "upgrade" work. Three are live correctness bugs, verified in the
working tree, and two of them are load-bearing for everything below: you
cannot build a temporal/volumetric pipeline on a frame that has undefined
resource hazards or a corrupted normal buffer.

**D1 — `DX12Nova` has no CPU/GPU synchronisation at all.** `fence` and
`fenceEvent` are created (`DX12Nova.cpp:39-41`) and then never used: no
`Signal`, no `GetCompletedValue`, no `WaitForSingleObject` anywhere in the
file (grepped). `FlushCommandList` (`DX12Nova.cpp:282-289`) does
`Close` → `ExecuteCommandLists` → **`commandAllocator->Reset()`** →
`commandList->Reset()` with no wait. The allocator and command list are
recycled while the GPU may still be executing them. Every DX12 resource
hazard in the Nova path is undefined behaviour.

This compounds with two more DX12Nova facts: ~45 `CreateShaderResourceView`
calls rewrite a descriptor heap the GPU is actively reading, on **every
dispatch** (`DX12Nova.cpp:390-426`); and 8 full-image `CopyResource` runs
**per dispatch** purely to break UAV/SRV aliasing (`:401-405`). Both are
meant to be safe only if execution ordering is guaranteed — which it is not.

**D2 — the bilateral denoiser reads the wrong channels for the normal.**
`img_TraceGBuffer` stores `(r = NDC depth, gba = packed normal)` — written at
`.glsl:2070`, documented in the layout comment at `.glsl:15`. It is bound to
sampler unit 10 as `s_NormalBuffer` (`Renderer3D.cpp:3167-3168`). But
`RunBilateralBlur` reads `.rgb` and treats it as a normal
(`.glsl:2401`, also `:2421`, `:2460`). So the edge-stopping normal weight is
computed on a depth-contaminated vector — the denoiser is smoothing across
the wrong edges. Should be `.gba`.

**D3 — two uniforms are uploaded every frame and read by nothing.**
`u_MaxBounces` (uploaded `Renderer3D.cpp:2542`) and `u_SamplesPerPixel` are
declared in both shaders and never read. Consequence: **the `MaxBounces = 1`
you see in all four presets (`:3697-3736`) is not the real bounce count** —
the real depth is hardcoded by the indirect-ray loop clamp in the shader, and
`SamplesPerPixel = 1` is not a sample count at all because there is no
per-pixel sample loop. The Settings UI exposes a `Samples Per Pixel` slider
that provably cannot do anything.

**D4 — `DX12Nova::Clear()` is a no-op** (`:262-268`: two empty command-list
flushes, zero clears). `DX11Nova::Clear` *does* clear (`DX11Nova.h:63`). So DX12
accumulation is never reset where DX11's is — a behavioural divergence
between two implementations of the same pass list.

**D5 — the Vulkan raster path is latently broken.** `NVRHIShader.cpp:36-48`
avoids swapping `.glsl`→`.hlsl` on Vulkan, which means a `.glsl` file loaded
under Vulkan is handed to `D3DCompile` as DXBC. It cannot compile. Vulkan
raster is either untested or running something other than what the source
suggests.

**D6 — pass order diverges between backends.** DX11/DX12 dispatch
**1 → 0 → 2**; OpenGL dispatches **0 → 1 → 2** (`Renderer3D.cpp:2705-2716` vs
`:3437-3447`). Pass 1 writes the buffer pass 0 consumes. Three hand-maintained
copies of one pass list (`:2717`, `:2851`, `:3437-3510`) with no single source
of truth.

**D7 — `HLSL` has no parallax occlusion mapping at all.** `ApplyPOM` in the
HLSL port computes `doMarch` / `marchK` / `numLayers` and never uses them;
`finalHeight` is hardcoded to `0.5f` (`.hlsl:1610` and surrounding). The GLSL
runs a real 6–32 layer march. **DX11/DX12 get bump-only relief; OpenGL gets
parallax.** The same port-drift class as the "neural blocks" finding above.

**D8 — raster fallback draws cubes and spheres only** (`Renderer3D.cpp:2972-3000`).
Plane, ico-sphere, cylinder, cone, torus, grid and water have no NVRHI raster
path. On a D3D/Vulkan backend most of the scene does not draw in the
non-ray-traced path.

**D9 — `FrustumPlanes` is read but never written.** Four call sites consume it
(`Renderer3D.cpp:4082-4160` region); no path populates it. Documented in-source
at `:2321-2337`. Editor-viewport culling does not match what is drawn.

**D10 — `RayTracingInstance::LODLevel` is hardcoded to 0** at all four write
sites (`:5222`, `:5560`, `:5767`, `:6343`). There is no LOD selection logic
anywhere, in engine or component layer.

### Correcting one claim from the earlier draft

This document previously said to "keep `wl_vol_cheap` where it is." That was
wrong. `wl_vol_cheap` is **defined and never called** in both sources
(`.glsl:319`, `.hlsl:521` — zero call sites). The actual volumetric perf lever
is `wl_vol_fbm2` (2 octaves) vs `wl_vol_fbm` (4 octaves), selected by
`u_VolFast` and `u_RenderScale` at `.glsl:835` and `.glsl:913`, plus
`wl_vol_noise` for cloud detail (`:918`, `:933`). Those are what must not be
regressed. `wl_vol_cheap` is dead code that can be deleted or wired up.

### The two findings that determine sequencing

### Finding 1 — NVRHI's hardware RT is already built and already linked. The engine never calls it.

`Wasteland/vendor/nvrhi` contains the **complete** ray tracing API:
`createAccelStruct` (`nvrhi.h:3753`), `setRayTracingState` (`:3441`),
`dispatchRays` (`:3451`), `RayGeneration`/`AnyHit`/`ClosestHit`/`Miss`/
`Intersection` group types (`:787-790`), `RayTracingAccelStruct` binding set
item (`:1953`, `:2244`), and meshlet/NVMS sources
(`src/d3d12/d3d12-meshlets.cpp`, `src/vulkan/vulkan-meshlets.cpp`).

Those sources are in the generated projects and the symbols are in the
shipped binaries — `nvrhi_d3d12.lib` and `nvrhi_vk.lib` both contain
`createAccelStruct`, `dispatchRays`, and `setRayTracingState`.

**`Wasteland/src` contains zero call sites.** `grep -rn "createAccelStruct\|dispatchRays\|setRayTracingState" Wasteland/src/` → nothing.

The device side is also not asking for it: D3D12 requests only feature level
`11_0`/`11_1` (`NVRHIContext.cpp:330-332`) with no
`CheckFeatureSupport(RaytracingTier)` query, and the Vulkan device extension
list is `{VK_KHR_swapchain}` plus timeline (`:935-936`, `:1060`) — **no
`VK_KHR_acceleration_structure`, no `VK_KHR_ray_tracing_pipeline`, no
`VK_KHR_deferred_host_operations`, no `VK_KHR_buffer_device_address`.**

This is good news: the RT substrate is a *wiring* problem, not a vendor
integration project.

### Finding 2 — The actual blocker is Shader Model 5.0.

`NVRHIShader.cpp` compiles every stage through `D3DCompile` at
**`vs_5_0` / `ps_5_0` / `cs_5_0`** (`:147`, `:184`, `:237`).

SM 5.0 has no wave intrinsics, no bindless, no 64-bit atomics, no
ray tracing. **No amount of NVRHI RT wiring can be reached from these
profiles.** Every AAA technique in this plan is gated behind moving the
compiler to SM 6.6 (DXC → DXIL for D3D12, SPIR-V for Vulkan) first.

This is why the phase order below is not "add ray tracing first." The
compiler is the gate.

### Hardware reality — read this before setting expectations

| | |
|---|---|
| GPU | RTX 3060 Ti, **8 GB** VRAM, compute capability 8.6 (Ampere) |
| Driver | 591.59 |
| Toolchain | Vulkan SDK 1.3.296.0 — `dxc`, `glslc`, `glslangValidator`, `spirv-cross` all present |

Consequences that must shape the design rather than be discovered later:

- **8 GB is a hard budget.** A deferred G-buffer plus a froxel volume grid
  plus history plus 3D noise textures is not a small-memory pipeline. Budget
  it explicitly per pass before writing the pass.
- **Ampere has RT cores**, so hardware RT is genuinely available — but
  consumer Ampere does not support mesh shaders / NVMS at the same maturity
  as Ada/Hopper. Verify mesh-shader support on *this* card before designing
  around it; assume a compute-shader-culling fallback.
- **Crimson Desert runs on PS5 / RTX 40-series / 60fps at 4K.** We are not
  reproducing its frame budget and should not pretend otherwise. What is
  achievable is its *architecture*: the techniques exist, tiered, and the
  quality presets separate feature count from resolution.
- On this GPU the Ultra tiers will not hold native resolution. That is what
  the existing dynamic-resolution scaler (`Renderer3D.cpp`, `UpdateDynamic-
  Resolution`) is for. **Verification on this hardware proves correctness,
  not performance** — any performance number must come from the in-app
  `Tracer: N fps / M ms GPU` readout (`EditorLayer.cpp:1086`), never from
  ALU arithmetic. There is no headless GPU measurement in this repo.

---

## Reference points — what we are actually chasing

Not feature checklists; these are the specific techniques.

**Cyberpunk 2077 (RT Overdrive / path tracing)**
- One unified ray-traced path replaces SSR + SSAO + GI as separate systems.
- **ReSTIR DI** and **ReGIR** for light sampling — this is what makes
  thousands of small emissive sources tractable.
- RTXDI: every neon sign, streetlamp, billboard and headlight is a
  ray-traced light.
- DLSS 3.5 + **Ray Reconstruction** replacing several hand-tuned denoisers.
- Volumetric fog and light shafts as the primary Night City look, with
  particles lit by dynamic lights and visible in reflections.

**Crimson Desert (BlackSpace)**
- Full path tracing, **not** hybrid — all lighting unified in one
  mathematical model, including indoors.
- **Volumetric fog with fluid simulation**, froxel raymarching, reacting to
  player movement; density driven by wind and time of day.
- **Unified atmospheric scattering with volumetric clouds** — clouds are
  volumetric data you can fly through, not skybox.
- **Volumetric water**: FFT ocean + shallow-water simulation, water as a
  volume that flows around obstacles rather than a displacement map.
- Hierarchical level subdivision, proxy LoDs, impostors for seamless open
  world.
- Five presets plus DLSS 4.5 / FSR Redstone / PSSR / MetalFX.

The two overlaps worth calling out, because they are where our current
architecture is furthest from the target:

1. **Volumetrics as a structured pipeline** (froxel grid + temporal +
   multiple scattering), not as a raymarch inside the surface tracer.
   Everything Crimson Desert does with fog requires this.
2. **Many-light sampling** (clustering + ReSTIR/ReGIR). Our 8-light cap is
   a Night City with 8 streetlights in it.

---

## Phase plan

Seven phases plus a pre-phase of correctness fixes. Each states its gate, its
exit test, and what it must not break. Phase -1 and Phase 0 are load-bearing;
nothing after them is worth starting early.

---

### Phase -1 — Correctness fixes (do these first, independent of AAA scope)

**Gate:** none. **This precedes Phase 0.**
**Why:** D1 makes DX12 execution undefined, and D2 corrupts the normal buffer
every Phase 4-6 technique depends on. Building temporal reconstruction or a
froxel grid on top of either is building on sand. These are also cheap —
hours, not days — and they are verifiable by screenshot.

Tasks:
1. **D1** — make `DX12Nova` safe. Either fence per dispatch
   (`Signal` → `WaitForSingleObject` before `commandAllocator->Reset()`), or
   move to a persistent ring of N allocators + command lists with a
   per-allocator fence. Prefer the ring: the current
   descriptors-rewritten-per-dispatch pattern (`:390-426`) should become
   persistent descriptors updated only when the texture set actually changes.
   This is also a large performance win — ~45 descriptor writes and 8 image
   copies per dispatch is the single most obviously wasteful thing in the
   engine.
2. **D2** — `RunBilateralBlur` should read `.gba` of `s_NormalBuffer`
   (`.glsl:2401`, `:2421`, `:2460`). Verify by screenshot: denoised edges
   should stop at geometry boundaries. Cross-check the HLSL port
   (`.hlsl:2456` reads `.rgba`, same bug).
3. **D3** — either wire `u_MaxBounces` and `u_SamplesPerPixel` into real
   sample/bounce loops, or delete them and remove the dead `Samples Per Pixel`
   slider. Do not leave a control in the UI that provably does nothing.
4. **D4/D6** — one pass list, one order, shared by all three backends. This is
   the structural prerequisite for Phase 0's codegen: two divergent
   hand-maintained copies cannot survive a compiler migration.
5. **D9** — either populate `FrustumPlanes` or remove the culling that reads it.
6. **D5** — decide whether Vulkan raster is supported. If yes, fix the
   `.glsl`-to-DXBC path (Phase 0's SPIR-V output fixes this for free). If no,
   remove it from the backend combo rather than shipping a selection that
   cannot work.

**Exit test:** DX12 runs N consecutive frames under a GPU validator or
RenderDoc capture with no resource-hazard warnings; denoiser edge-stopping
verified by screenshot before/after.

---

### Phase 0 — Shader toolchain: SM 5.0 → SM 6.6 (DXC/SPIR-V)

**Gate:** none. This is the root.
**Why first:** hardware RT, bindless, wave intrinsics, 64-bit atomics, and
most modern texture ops are unreachable from `cs_5_0`. Every later phase
depends on this.

Tasks:
1. Add a DXC compile path alongside `D3DCompile` in
   `Wasteland/src/Platform/NVRHI/NVRHIShader.cpp`. Target `vs_6_6`/`ps_6_6`/
   `cs_6_6`. Keep the SM 5.0 path selectable for the OpenGL backend, which
   still needs GLSL and has no DXC.
2. `dxc` is at `/c/VulkanSDK/1.3.296.0/Bin/dxc`. Decide and document:
   runtime spawn vs. build-time codegen. **Recommend build-time codegen into
   `assets/shaders/`** — runtime DXC adds a process spawn and a temp dir to
   every editor launch, and this repo already has an asset-staleness trap
   (below) that a second output directory would worsen.
3. Vulkan: emit SPIR-V via `glslc` for the NVRHI Vulkan backend so it is not
   second-class. Today Vulkan has no Nova path at all.
4. Verify both configs: `tools/msb.sh Wasteland/Wasteland.vcxproj` for
   Release **and** Debug.

**Exit test:** a SM 6.6 compute shader using a wave intrinsic (e.g.
`WaveActiveSum`) compiles, dispatches through `NVRHIRendererAPI::DispatchCompute`
(`NVRHIRendererAPI.cpp:423`), and produces a visibly different image than
its SM 5.0 counterpart.

**Must not break:** SM 5.0 fallback for OpenGL; the existing
`.glsl`→`.hlsl` extension swap (`NVRHIShader.cpp:39-41`).

---

### Phase 1 — Enable hardware ray tracing through NVRHI

**Gate:** Phase 0.
**Why:** the substrate exists and is linked. This phase is wiring.

Tasks:
1. **Device capability queries.** D3D12: `CheckFeatureSupport` for
   `D3D12_FEATURE_D3D12_OPTIONS12` / `RaytracingTier` at device creation
   (`NVRHIContext.cpp:417-427`). Vulkan: enumerate and enable
   `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`,
   `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`,
   `VK_KHR_buffer_device_address`. Log the result in the existing
   pre-`createDevice` diagnostic dump so a failure names the missing
   extension rather than surfacing as a later crash.
2. **BLAS/TLAS ownership.** One BLAS per static mesh instance, refcounted and
   rebuilt only on transform/material change. Reuse the existing
   `SceneInstancesChanged()` gate (`Renderer3D.cpp:1432`) semantics — but it
   is a `memcmp` that **only works because `RayTracingInstance` has zero
   padding** (see the comment at `:105-120`). Adding a field to that struct
   will silently insert padding, make the memcmp report a change every frame,
   and permanently defeat the static-scene optimisation. Put any new
   per-instance data in a **parallel** array, not in `RayTracingInstance`.
3. **SBT** (shader binding tables): raygen / miss / closest-hit, built once
   and invalidated on pipeline change only.
4. **First raygen shader**: one ray per pixel against the existing analytic
   primitives via a real BLAS. Render side-by-side with the current software
   tracer.

**Exit test:** a real `dispatchRays` produces an image that matches the
software tracer's primary-hit output, verified by screenshot diff at fixed
camera — not by "it compiled."

**Must not break:** the Vulkan device-creation path. Enabling RT extensions
changes `createDevice` input; the timeline-semaphore rules in
`references/vulkan-timeline-semaphore-crash.md` (VUID-VkDeviceCreateInfo-pNext-02830)
still apply and are easy to trip when adding to the same `pNext` chain.

**Also add:** Vulkan currently has **no** Nova compute path. Either give it
one or make the Vulkan entry in the backend combo say so. Shipping four
backends where one silently has no ray tracing is a trap.

---

### Phase 2 — Real geometry in the tracer (mesh BLAS)

**Gate:** Phase 1.
**Why:** until triangles exist, every lighting technique downstream is being
demonstrated on boxes and spheres. CP2077 and Crimson Desert are both about
arbitrary geometry.

Tasks:
1. Wire `MeshLoader.cpp` into the tracer. It already loads OBJ; it is only
   bound to Python today (`Bindings.cpp:528`).
2. Per-mesh vertex/index buffers → BLAS geometry descriptors.
3. Material parameterisation for real meshes (UV sets, tangent frame,
   normal-map orientation) — the current `TextureScale` convention tiles
   ~1 texture per world metre everywhere, which is fine for analytic boxes
   and wrong for meshes.
4. Vertex-displacement parity: Nova displaces in-shader today; real meshes
   need the displacement in the BLAS build input or the silhouette will not
   match the shading.

**Exit test:** an imported mesh renders correctly through *both* the
software tracer and hardware RT, and matches within the displacement
tolerance already used for the analytic path.

---

### Phase 3 — Deferred G-buffer + clustered lighting

**Gate:** Phase 2.
**Why:** this is what removes the 8-light cap and gives every screen-space
technique something to read.

Tasks:
1. **Real G-buffer** — depth, normal, albedo+ao, roughness+metal+reflectance,
   emissive, velocity, material ID. The Nova path's current "G-buffer" is one
   compute-written RGBA32F image holding NDC depth + a packed normal
   (`.glsl:2070`), and the raster path has none. Define a `GBuffer` layout in
   one shared header consumed by both the `.glsl` and `.hlsl` sources — the
   dual-source drift is already a documented cost (see D6, D7).
   **Watch the channel convention:** that image stores depth in `.r` and the
   normal in `.gba`, with sky at depth 1.0, which is the *opposite* of GL's
   window-space `[-1,1]` range. Phase -1's D2 fix is in this same format, so
   settle the layout before writing more consumers of it.
2. **Clustered light culling** with a froxel light grid — this is what
   removes `MaxAnalyticLights = 8` (`Renderer3D.h:209`) and the
   first-N-wins starvation at `Scene.cpp:1470-1475`. Fixing the cap by
   raising the number is the wrong fix; the ordering starvation is the
   actual bug.
3. Depth prepass — feeds velocity and SSAO, and lets the G-buffer reject
   overdraw. Note the engine currently has **no depth prepass at all**
   (grepped: no shadow-map, cascade, or prepass code anywhere in
   `Wasteland/src`).

**VRAM budget (this is the constraint, decide it now):** on 8 GB, a
full-precision G-buffer at 4K native is not viable. Fix the internal
render scale for the deferred path and let the existing dynamic-resolution
scaler own it. Budget per pass before writing it:

| Resource | @1080p RGBA16F | @4K RGBA16F |
|---|---|---|
| G-buffer (6 targets) | ~96 MB | ~384 MB |
| Froxel volume grid (160×90×64) | ~7 MB | ~7 MB |
| 3D noise (128³ tiled) | ~16 MB | ~16 MB |
| History + reprojection | ~48 MB | ~192 MB |

**Exit test:** a scene with 200+ analytic lights renders with no light
culled incorrectly and no per-type starvation; G-buffer contents verified by
debug visualisation.

**Must not break:** the `memcmp`-based static-scene gate (Phase 1 trap), and
the velocity-must-stay-on-unjittered-grid rule (see trap list).

---

### Phase 4 — Screen-space + RT lighting stack

**Gate:** Phase 3.
**Why:** the non-RT baseline. CP2077 keeps SSR layered *under* RT
reflections rather than replacing it outright.

In dependency order:
1. **SSAO / GTAO** — horizon-based, reads depth+normal from Phase 3.
2. **SSR** — ray-marched screen-space reflections with roughness-aware
   cone jitter, falling back to the analytic sky env that already exists
   (`GetSkyColor`, `.glsl:2040`).
3. **SSGI** — one-bounce diffuse GI, which is the direct replacement for the
   `wl_neural_indirect` hand-authored fallback (`.glsl:223`).
4. **Ray-traced shadows** via Phase 1, replacing the per-light
   `IsOccluded(aShadowRay, …)` at `.glsl:1976`. This is where Night City
   gets its look: hard, correct, contact-hardening shadows from thousands
   of small sources.

**Exit test:** each technique individually toggled on/off, screenshot at
fixed camera, with the known fingerprint for its failure mode (see traps).

**Must not break:** the albedo-squared class of bug — two sites are already
documented (`.glsl` composite at `:2332-2336` and trace at `:2147`). Any new
lighting stage that multiplies albedo again reintroduces it.

---

### Phase 5 — Volumetrics rebuilt as a real pipeline

**Gate:** Phase 3 (froxel grid needs the G-buffer + light clusters).
**This is the phase that answers the volumetric half of the request.**

The current implementation marches volumes *inside the surface tracer*, which
caps quality hard: no temporal reprojection of volume data, no half-res
volume with bilateral upsample, and one shadow ray per light per step
(`.glsl:865`) as the cost wall. Rebuild as a standalone pipeline:

1. **Froxel grid** — a 3D texture in view-frustum space holding
   scattering + transmittance, sliced along depth. This is the standard
   structure and the direct prerequisite for everything below. It also
   requires a `Texture3D` abstraction: the engine's texture class is 2D only
   (`Wasteland/src/Wasteland/Renderer/Texture.h` has `Texture2D` only, and
   `NVRHITexture.h` is `NVRHITexture2D` only). **There is no 3D texture
   support anywhere in the engine** — add it as its own task.
2. **Injection / scattering pass** — a compute pass over the grid. Shadow
   lookup becomes a *froxel cluster lookup*, not a marched shadow ray per
   light. This is where the cost wall comes down.
3. **Temporal reprojection with velocity** — reproject last frame's froxels
   through the velocity buffer from Phase 3. Reuse the existing
   reprojection machinery at `.glsl:2213-2222` (velocity → `prevUV` →
   history lookup), which is already correct including the
   `histUV` clamp. Do **not** re-derive it.
4. **Bilateral upsample** to full resolution, depth-aware.
5. **Multiple-scattering approximation** — the multi-octave approach that
   turns single-scattering fog into believable thick fog. Today there is
   exactly one HG phase (`.glsl:809`).
6. **Atmosphere** — replace `GetSkyColor`'s two-colour gradient
   (`.glsl:2040`) with a real scattering model (Rayleigh/Mie, Preetham or
   Hosek), plus an aerial-perspective term so distant terrain gets correct
   in-scatter. Add a time-of-day system; none exists in the engine.
7. **Volumetric clouds** with proper self-shadowing and light shafts, and
   cloud shadows on the ground.
8. **Fluid-simulated fog** (Crimson Desert's signature). This is a
   separate, larger subsystem — a coarse 3D velocity/density grid updated
   on CPU or GPU, with the froxel grid sampling it. Recommend **last within
   this phase**, as its own plan document. It is a system, not a shader pass.

**Exit test:** player camera walked through a fog volume shows the fog
reacting to movement (Crimson Desert's demonstrable feature); volumetric
shadows visible *inside* the medium; screenshot at fixed camera with the
medium on/off.

**Must not break:**
- **The static per-pixel dither stays static.** A cycling dither was tried
  and reverted (`references/volumetric-fog-clouds.md`). With temporal
  reprojection in place the temptation is stronger than ever — do not.
- **Keep the noise-octave tiering.** The lever is `wl_vol_fbm2` (2 octaves)
  vs `wl_vol_fbm` (4 octaves), gated on `u_VolFast` and `u_RenderScale`
  (`.glsl:835`, `.glsl:913`), plus `wl_vol_noise` for cloud detail
  (`.glsl:918`, `.glsl:933`). A 24-hash → cheaper-noise swap was a ~20× win
  and is load-bearing for frame time. (`wl_vol_cheap` itself is dead code —
  see the correction above — so it is not the thing to preserve.)
- The `for (s = 0; s < 32; s++) { if (s >= steps) break; }` shape is a
  deliberate fixed-trip-count workaround. Preserve it if you can; if you
  move to dynamic bounds, expect the compiler issues in
  `references/dx11-nova-validation.md`.

---

### Phase 6 — Temporal reconstruction + output

**Gate:** Phase 4.
**Why:** turns a noisy correct image into a clean one, and is what makes
higher sample counts affordable.

1. **Real TAA** — the existing `RunTemporalAccumulation` (`.glsl:2207`) is
   accumulation, not TAA: it needs motion vectors, neighbourhood clipping
   in YCoCg (already present at `:2234-2276`), and a proper resolve. The
   YCoCg variance clamp is already a large part of what TAA needs — extend
   it rather than replacing it.
2. **Upscaler** — replace the bilinear upscale in `RunComposite`
   (`.glsl:2339-2359`) with DLSS / FSR / XeSS behind a runtime backend
   selection. The dynamic-resolution scaler already exists and is the right
   host for it.
3. **Ray reconstruction** — replaces the hand-tuned bilateral blur
   (`RunBilateralBlur`, `.glsl:2380`) with a learned denoiser. This is the
   single biggest perceived-quality-per-millisecond win available, and it is
   why the bilateral blur becomes redundant rather than merely tuned.
4. **Tone mapping + HDR** — ACES or AgX, an exposure control, and an HDR
   swapchain format. Swapchain is currently `BGRA8_UNORM`/`SBGRA8_UNORM`
   (`NVRHIContext.cpp:1656`, `1720`, `1963`), so the entire pipeline is
   LDR-clamped before it reaches the screen. Colour grading exists as
   post-process knobs (`Renderer3D.h:190-204`) but has no HDR input to work
   with.

**Exit test:** no visible ghosting or smear under camera motion at every
quality preset; upscaler active and toggleable; HDR values above 1.0 survive
to the display path.

---

### Phase 7 — Content scale: LOD, foliage, and the last mile

**Gate:** Phases 3-4.
**Why:** Crimson Desert's open world is hierarchical level subdivision,
proxy LoDs and impostors. We have none (the `LODLevel` field at
`Renderer3D.cpp:130` selects tessellation tier only).

1. **Hierarchical LoD + impostors** for distance rendering.
2. **GPU-driven culling** — the engine has frustum culling on the CPU
   (`Renderer3D.cpp:2148-2186`) with an inlined comment that culling *could*
   feed visible geometry. Move to compute culling with indirect draws.
3. **Foliage** — Crimson Desert is foliage-heavy and renders animated trees at
   distance with maintained lighting.
4. **32-texture-slot limit** (`Renderer3D.cpp:691`) — bindless or texture
   arrays will be required well before a real scene hits it.

**Exit test:** a large scene renders with stable frame time and no visible
LoD popping.

---

## Cross-cutting requirements

### The `memcmp` trap — read before touching `RayTracingInstance`

`SceneInstancesChanged()` (`Renderer3D.cpp:1432`) uses `memcmp` to decide
whether to rebuild the BVH and re-upload the SSBO. It is exact **only
because the struct has no padding**, which in turn depends on a single `int`
landing `TextureScale` on a 16-byte boundary. The warning is written into the
struct comment at `Renderer3D.cpp:105-120`.

Adding one `int` to that group inserts 12 bytes of never-written padding, and
the memcmp then reports a change every frame — permanently defeating the
static-scene optimisation with no visible symptom other than frame time.

**Rule:** new per-instance data goes in a **parallel array** indexed the
same way, never appended to `RayTracingInstance`.

### The dual-source shader problem

Every `.glsl` change needs a matching `.hlsl` port for D3D11/D3D12
(`NVRHIShader.cpp:39-41` swaps the extension), and the two drift. Regenerators
live in `%LOCALAPPDATA%/Temp/hlsl_port/` per the skill and are **re-creatable
but not durable**. With Phase 0 landing SM 6.6 codegen, re-evaluate whether
two hand-maintained sources survive; a single source compiled to both is the
only configuration that will hold up across seven phases.

### Verification discipline

Per `native-engine-rendering`'s evidence rules, report these **separately**:
source edited → shader compiled → engine built (Release + Debug) → editor
relinked → backend exercised → **pixels verified by screenshot**.

A green build plus a clean `glslangValidator` run proves the code compiles.
It proves nothing about what a frame looks like. When a feature has no
screenshot, say so as its own line rather than letting green checks imply
visual correctness.

- Build: `tools/msb.sh Wasteland/Wasteland.vcxproj` (serialises MSBuild;
  never pipe MSBuild output).
- Shaders validate per-stage with `glslangValidator -S comp <tmp>` and
  **no `--target-env` flag** (see traps).
- After a green build: `git checkout -- Wasteland/vendor` to drop tracked
  `.lib`/`.idb` churn.
- Performance claims come from the in-app `Tracer: N fps / M ms GPU`
  readout (`EditorLayer.cpp:1086`) with the editor holding the GPU. Never
  from ALU estimates — there is no headless GPU measurement in this repo.
- **Stale assets are a known failure mode.** The editor's bin copy of
  `assets/` can lag the source, and `.glsl` edits require an editor restart
  (shaders compile at startup). Verify with
  `diff bin/<Config>/DemonCore-Editor/assets/shaders/ <source>` before
  concluding an edit did nothing.

### Traps that will recur

Condensed from the skill, all of them hit at least once already:

| Trap | Fingerprint |
|---|---|
| Albedo applied twice in composite/trace | Distance-graded darkness, faint lit pools surviving |
| SSBO struct drift C++ ↔ GLSL | Silently wrong values, no error |
| Velocity biased by frame jitter | Diagonal drag while moving / 16-frame shimmer while still |
| Cycled (not static) dither | Permanent shimmer; accumulation never converges |
| Skip threshold when RenderScale < 1 | Ghost duplicate of the viewport |
| `textureLod` on single-mip textures | Black at distance, moiré at grazing |
| POM bump gradient × uvScale | Near-black shading, saturated normals |
| New branch deleting `HitInfo.emission` write | Neon garbage on *all* geometry |
| Water/instance packing (`MaterialParams.w`) | Ocean collapses to a sphere |
| Bloom threshold skipped when scaled | Smear across the output |
| Enabling Vulkan 12 + KHR timeline together | VUID-VkDeviceCreateInfo-pNext-02830 |
| D3D12 `D24S8` with `isShaderResource` | `E_INVALIDARG` at `CreateTexture2D` |
| NVRHI buffer/texture without `initialState` | `Device Removed!` on D3D12 |

---

## Explicitly out of scope (and why)

- **Frame generation / DLSS 4 multi-frame gen.** Needs Ada. On a 3060 Ti it
  is unavailable; FSR 3 frame generation is the realistic option and is a
  Phase 6 follow-up.
- **Mesh shaders / NVMS.** Ampere consumer support is not at Ada/Hopper
  maturity. Verify before designing; assume compute-shader culling.
- **Matching Crimson Desert's frame budget.** PS5 / RTX 40 at 60fps 4K. We
  are building the architecture, not the performance.
- **A from-scratch neural renderer.** The existing "neural" blocks have
  baked weights and no training path. Real learned denoising/restoration is
  a separate project; Phase 6's ray reconstruction is the pragmatic version.

---

## Open questions for you

1. **Phase 0 compiler strategy** — build-time DXC codegen (recommended) or
   runtime compile? Build-time avoids a process spawn per editor launch but
   needs the postbuild asset sync wired correctly.
2. **Vulkan parity** — give Vulkan a real compute path in Phase 1, or is
   DX12 the priority backend with Vulkan explicitly deferred? Affects Phase 1
   scope significantly.
3. **Scope of the fluid-sim fog** (Phase 5.8) — this is genuinely a
   separate multi-week subsystem. Confirm it stays in this roadmap or gets
   its own plan.
4. **Target hardware beyond this machine.** The 8 GB budget above assumes
   the 3060 Ti is the floor, not just the dev box. If the real target is
   12-16 GB, the VRAM budget relaxes and Phases 3-5 get much more headroom.

---

**Suggested first action**

**Phase -1 task 1 (D1, the DX12 fence) and task 2 (D2, the normal-channel
read).** Both are small, both are independently verifiable by screenshot, and
D1 is currently making every DX12 frame undefined behaviour. Do not start
Phase 0 until Phase -1 is done — the compiler migration is wasted effort if it
lands on a frame that has resource hazards.

**Phase 0 follows immediately after.** One SM 6.6 compute shader using a wave
intrinsic, compiled through the DXC path and dispatched on DX12.

Nothing after that is worth attempting until Phase 0 works.