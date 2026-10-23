# Nova FPS: GL <4 fps vs DX ~20 fps — diagnosis and plan

Scope: Wasteland / Nova path tracer, 4K viewport, DemoGame scene.
Evidence: static analysis of the pass schedule, both Nova shader twins, and the
per-frame cost model, cross-checked against the observed GL/DX FPS split.
Phases 0-4 address the frame rate; Phase 5 adds a selectable anti-aliasing
method ladder, which is coupled to render scale and therefore to the FPS work.

**Correction to the first revision of this document.** It treated the GL/DX
difference as a minor twin divergence and ranked the fog march as a general
"too expensive" item. That was wrong. The fog march is not merely expensive —
its light cap differs between the twins, and that difference *is* the GL/DX gap.

---

## 1. Root cause of the GL/DX split

`wl_march_fog_vol` runs a full BVH shadow-ray loop inside the volume step loop,
for every primary ray that enters a fog box.

`Renderer3D_NovaRenderer.glsl:1673-1759`

```glsl
if (u_QualityLevel >= 1 && u_ALightCount > 0)
    lightEnd = min(u_ALightCount, 32);      // GL:  32 lights

for (int s = 0; s < 32; s++)                // up to 32 march steps
{
    ...
    for (int li = lightStart; li < lightEnd; li++) {
        if (IsOccluded(lRay, dL, -1)) continue;   // full BVH walk
    }
}
```

`Renderer3D_NovaRenderer.hlsl:1257` — the same loop, **capped at 8**:

```hlsl
if (u_QualityLevel >= 1 && u_ALightCount > 0)
    lightEnd = min(u_ALightCount, 8);       // HLSL: 8 lights
```

`IsOccluded` (`glsl:1549`) is a complete BVH traversal over every instance in
the scene. So per pixel in fog:

| | fog steps | lights | BVH traversals |
|---|---|---|---|
| GL | 32 | 32 | **1024** |
| DX | 32 | 8 | **256** |

**Ratio: 3.98x.** The observed gap is GL <4 fps vs DX ~20 fps — about 5x. The
4x divergence plus per-backend overhead accounts for it.

### Why the fog march covers the whole frame

| Quantity | Value | Source |
|---|---|---|
| Fog volume | 10000 x 1000 x 10000 m, centred y=500 | `DemoGame.wastescene:39980` |
| Camera height | 25 m | same scene |
| Fog `Enabled` | true | `:39981` |
| Fog `Density` | 0.0005 | `:39982` |
| Fog `MaxSteps` | 64 -> clamped to 32 | `:39989`, shader `:1648` |
| `u_ALightCount` GL / DX | up to 32 / up to 8 | `SelectRenderedLights` cap 32; Nova cap 8 |
| Analytic lights in scene | 677 point + 1 spot + 1 area + 1 dir | component census |

A 10 km fog box at y=25 means every pixel — sky included — enters the volume.
The march is reached from both call sites: sky pixels at `glsl:3653`, surface
pixels at `glsl:3822`. With `dt ~= 312 m`, `a = 1-exp(-0.0005*312) = 0.144`,
well above the `a > 0.001f` guard that would skip the light loop. The
transmittance early-out does not rescue it: `trans = 0.856^n` only reaches the
0.02 break at n ~ 25.

**Per-pixel traversals at Ultra:**

| Pixel type | GL | DX |
|---|---|---|
| Sky pixel | 1026 | 258 |
| Ground pixel | 12 | 12 |

At 4K / Ultra (trace res 2880x1620 = 4.67 Mpx, 50/50 sky-ground):

| | traversals/frame | sustained rate at 4 fps | at 20 fps |
|---|---|---|---|
| GL | 2.42 B | 9.7 G/s | 48 G/s |
| DX | 0.63 B | 2.5 G/s | 12.6 G/s |

GL at 4 fps is ~10 G traversals/s of sustained work — at the hardware limit for
this shader. DX at 20 fps is ~12.6 G/s, the same order, on 4x less volume work.

### This is also a correctness bug

The two twins compute different lighting for the same scene, same preset, same
frame. The `verify_nova_glsl.py` guard does not catch it: it keys on
`validHistory` / `histUV` / `mipLevel` / `GetJitter`, none of which appear in
the fog march. A systematic scan of every numeric loop bound in both twins found
this as the **only** divergent hot-loop constant:

| Pattern | GLSL | HLSL | |
|---|---|---|---|
| **`lightEnd` cap** | **32** | **8** | **divergent** |
| fog/cloud steps clamp | 2..32 | 2..32 | match |
| analytic light loop | 8 | 8 | match |
| fog/cloud step divisors | 16 / 8 | 16 / 8 | match |
| torus samples / bisection | 32 / 24 | 32 / 24 | match |
| POM layer + range ladder | 6/16, 8-16, 20-32, 25/35-50 | same | match |
| traversal stack depth | 32 | 32 | match |
| stack push guard | 30 | 30 | match |

---

## 2. Why "below the highest settings" does not help on GL

At Low quality (`u_QualityLevel == 0`) the fog light loop is correctly gated off
on both backends, so GL *should* recover. It does not, and there are three
reasons.

**2a. The GLSL traversal stack is private memory; the HLSL one is shared.**

```
GLSL:  int stack[32];                        // per-invocation LOCAL array
HLSL:  groupshared int gs_trav_stack[64*32]; // on-chip shared memory
```

The array is indexed dynamically (`stack[stackPtr++]`), which in GLSL forces it
out of registers into local/private memory. Every `IsOccluded` and `TraceScene`
call in the GL shader pays that, and the HLSL shader does not. This is a
GL-specific penalty that stacks multiplicatively with the 4x light divergence
and survives every quality preset, because the trace itself always runs.

**2b. The mid-frame overflow flush re-traces the whole scene.**

`MaxIndices = 1,500,000`. One 24x24 sphere costs 3456 indices, and DemoGame has
678 spheres = 2,343,168 indices — before the 64x64 water grid (24,576). So the
batch overflows, `DrawSphere` calls `Flush()`, then `FlushAndReset()`, the
remaining draws accumulate, and `EndScene()` calls `Flush()` again.

`FlushAndReset` (`Renderer3D.cpp:5895`) resets the batch cursors but **not**
`m_SceneInstances`. The instance list is cleared only in `BeginScene`. So the
second flush sees every instance again and rebuilds the BVH and re-runs the
entire pass schedule. **The frame is traced twice.** This is backend-independent
and doubles the cost of everything above.

**2c. The dynamic-resolution scaler cannot react to a per-pixel cost.**

`UpdateDynamicResolution` (`:4552`) is on by default, `TargetFPS = 30`,
`DynamicMinScale = 0.4`. Even pinned at its floor, 4K @ 0.4 = 1536x864 = 1.33
Mpx. At 1024 traversals per sky pixel that is still **~0.7 B traversals/frame**
on GL. Lowering resolution cuts the pixel count; it cannot cut the per-pixel
blow-up. So the scaler sits at its floor with the FPS readout still far below 30
— which is the reported symptom.

Two further scaler defects:

1. **The GPU timer is GL-only.** `DRSTimerQueries` are created at the end of the
   OpenGL branch of `Init()` (`:2516`); the NVRHI path returns long before, so
   on DX11/DX12 `DRSGpuSamples` stays 0 forever and the scaler falls back to
   `AppFrameTimeEMA` — wall-clock time that includes ImGui and the editor UI.
2. **The timer closes before god rays.** `glEndQuery` (`:4264`) precedes the
   pass-10 dispatch (`:4288`), and `UpdateDynamicResolution` runs at `:4281`.
   Pass 10 is outside the measured region on the one backend where the timer
   exists.

---

## 3. Secondary costs

Ranked by expected impact once section 1 is fixed.

**S1 — Distance LOD is dead code.** `RayTracingInstance::LODLevel` is written by
the CPU (`:5177-5182`) and read by **no shader in either twin** — `grep LODLevel`
in both files returns only the struct declarations and comments. The intended
large-world lever does nothing.

**S2 — Sky pixels pay a full trace.** Pass 1 (`:3578`) runs `TraceScene` and
`ComputeDirectLighting` before the `if (!primary.hit)` early-out at `:3634`. For
a sky-heavy 4K frame that is a BVH walk plus 10 shadow rays per pixel on pixels
that resolve to a two-`mix` gradient.

**S3 — Full-resolution post chain.** Composite, both bloom blurs and god rays
all dispatch at full target resolution while the trace runs at 0.75x: four
full-res passes at 4K = 33.2 Mpx/frame against 14.0 Mpx of trace work.

**S4 — Torus raymarch is ungated.** `HitTorus` (`:1363`) does 32 samples + 24
bisection iterations per instance test with no quality or distance gate
(~768 SDF evaluations per torus per ray).

**S5 — CPU per-frame traffic.** `SceneInstancesChanged` (`:1990`) `memcmp`s
1700 x 288 B = 490 KB every frame, and a dirty frame additionally copies 490 KB
and uploads ~590 KB (instances + BVH nodes). Correct for a static scene, but it
is ~1 MB/frame of memcpy plus the GL upload path.

**S6 — Dead uniforms.** `u_MaxBounces`, `u_SamplesPerPixel` (documented D3),
`u_PrevJitter`, `u_LODLevel`, and `u_TSR*` (no dispatch ever sets `u_PassID`
9, 15 or 16). No perf cost, but they mislead anyone tuning by uniform.

---

## 4. Plan

Each phase is independently shippable. Phase 0 is the one that addresses the
observed GL/DX split.

### 4.1 Implementation status — 2026-10-21

Steps are numbered as in the phases below. "Done" means the code is in the tree
and compiles; it does **not** mean the FPS acceptance test has been run, which
needs the editor open on both backends.

| Step | State | Where |
|---|---|---|
| Phase 0.1 — named cap constant | **done** | `#define WL_VOL_LIGHT_CAP 8` at `glsl:1019` / `hlsl:270`; `lightEnd = min(u_ALightCount, WL_VOL_LIGHT_CAP)` at `glsl:1699` / `hlsl:1275` |
| Phase 0.2 — hoist the shadow test | **done** | `float lightVis[WL_VOL_LIGHT_CAP]` at `glsl:1721` / `hlsl:1297`; one `IsOccluded` per light per pixel from the volume entry; per-step `if (lightVis[li] < 0.5f) continue;` |
| Phase 0.3 — light-count quality ladder | not done | — |
| Phase 0.4 — parity guard for the cap | **done** | `tools/verify_nova_glsl.py` checks 8 / 8a / 8b + 2 negative controls; `RESULT: PASS`, 24 PASS / 0 FAIL |
| Phase 1 — stop the double trace | **done** | `OverflowFlushWouldRetrace()`; on a ray-tracing backend the overflow resets the batch cursors without dispatching `Flush()`, so `EndScene` traces the frame once. Raster path unchanged |
| Phase 5 — AA methods | **partial** | see below |

**Resulting traversal count per fog pixel** (32 march steps, 8 lights):

| | before | after |
|---|---|---|
| OpenGL | 32 steps x 32 lights = **1024** | 8 hoisted traversals + 256 cheap culls |
| DX11 / DX12 / Vulkan | 32 steps x 8 lights = 256 | 8 hoisted traversals + 256 cheap culls |

The twins now compute identical volume lighting, and GL's fog cost is ~128x
lower. Verified independently: the GLSL compiles to SPIR-V under
`glslang -G --aml` (307,456 bytes, 0 errors) and the HLSL compiles under
`fxc /T cs_5_0 /E main` (632,736 bytes, 0 errors). Both shaders must be run
through a loader-directive strip first — `#type compute` at `glsl:6` is not GLSL,
and glslang 430 core needs `--aml` because the source declares its non-opaque
uniforms without explicit locations.

**Phase 1 detail.** The batch holds `MaxVertices` / `MaxIndices` = 1,500,000
indices; DemoGame needs 2,343,168 (678 spheres at 3,456 indices each = 2,343,168
before the water grid), so the overflow fires mid-frame, `EndScene` flushes
again, and `FlushAndReset` clears the batch cursors but not `m_SceneInstances` —
so the second flush re-traced the whole scene. The tracer reads
`m_SceneInstances`, never the CPU tessellation batch, so on a ray-tracing
backend the overflow only needs to reset the cursors. Raising `MaxIndices` to
cover the scene instead was rejected: it costs ~125 MB of CPU+GPU vertex buffers
and only defers the problem. Diagnostic: `Renderer3D::GetMidFrameFlushCount()`
/ `Statistics::MidFrameFlushes` (cumulative, deliberately not reset per frame),
logged once from `EndScene` when the count changes.

**Phase 5 detail — what is real and what is not.** The two enums
(`SamplePattern`, `ReconstructionFilter`), their editor combos, and the uniform
upload on all four dispatch paths are in the tree. Beyond that the two axes are
at different stages, and this is easy to misread:

- `SamplePattern` is **live**. The pattern is selected CPU-side in
  `Renderer3D::GetCurrentJitter` and expressed through the existing `u_Jitter`
  uniform — which is why it costs no extra pass. All six values work: BlueNoise
  (STBN, the default), Halton(2,3), RotatedGrid/RGSS, Quincunx, OrderedGrid and
  Centre. No shader declares `u_SamplePattern`; the upload is a documented
  no-op kept so the shader can take the decision over later.
- `ReconstructionFilter` is **plumbed but not consumed**. Neither twin declares
  `u_ReconstructionFilter` and `RunComposite` still hardcodes bilinear upscale.
  Picking a non-default value in the editor changes nothing on screen yet; the
  filter bank that acts on it is Phase 5 proper (steps 17-36). The enum, the
  combo and the upload are real, so this is a shader-side job, not a plumbing
  one.

### 4.2 Measured, plus two findings that change how the test must be run

**Finding 1 -- Quality Preset Low makes the entire fix inert.** The volumetric
light loop is gated on `u_QualityLevel >= 1` (`glsl:1700` / `hlsl:1278`), and Low
is quality 0, so `lightEnd` stays 0 and the loop never executes. **Any FPS number
taken at Low measures a renderer that is not running the code Phase 0 touched.**
This is why an early reading of "9 fps at Low" was not evidence about the fix
either way.

**Finding 2 -- the project file's `QualityPreset` was never applied.**
`ApplyRenderSettings()` applied the render scale, post-processing and ray tracing
but not the preset, even though `Project.h` documents `QualityPreset` as a string
"not the renderer's QualityPreset enum". So a project declaring
`QualityPreset: Ultra` silently came up on the engine default (Low). Fixed in
`EditorLayer::ApplyRenderSettings`; the mapping now runs *before* the render
scale, so an explicit project scale still wins over the preset's own.

**Finding 3 -- DX12 Nova could not initialise (root cause found and fixed).**
`DX12Nova` threw `E_INVALIDARG` (0x80070057) from `DX12Nova.h:191` during
construction and `Renderer3D` fell back to raster. Confirmed **pre-existing, not a
regression**: reverting the HLSL to HEAD and re-running produced the identical
HRESULT.

The cause was a root-signature/shader register mismatch, not a driver or shader
compile problem. The HLSL declares UAVs at `u0..u7, u9, u10, u11` -- u8 is a hole
nothing declares -- but:

- `kUavCount` was **11**, so the UAV descriptor table covered `u0..u10` only. The
  shader's `img_HistoryBuffer` lives at **u11**, which the root signature never
  declared. `CreateComputePipelineState` rejects that with `E_INVALIDARG`, which is
  exactly the HRESULT logged.
- Separately, the history UAV was bound at heap slot `kUavBase + 8` -- the hole at
  `u8` -- instead of `kUavBase + 11`, so even with the range widened the history
  descriptor would have been bound to a register nothing reads while `u11` stayed
  unbound.

Fixed: `kUavCount` 11 -> 12 (`DX12Nova.h:156`) and the history UAV moved to
`kUavBase + 11` (`DX12Nova.cpp:558`). The rule the code now documents: the UAV
count must be derived from the shader's highest UAV register, never from "the slot
after the image block", because a descriptor range is contiguous and the hole at
u8 still costs a slot.

The consequence for the original report is unchanged and important: **before this
fix, the "~20 fps on DX" reading could not have been Nova -- it was the raster
fallback.**

**Verified fixed.** After the change the editor logs, on the same machine and the
same scene:

```
DX12 Nova active: api=3 rtEnabled=1 renderTarget=2332x1451 framesInFlight=3 descriptorsPerSlot=64 uavCount=12
```

`uavCount=12` and `descriptorsPerSlot=64` are the two numbers the fix moves, so the
root signature now covers u11 and DX12 Nova initialises instead of falling back.

**What that exposed: a device removal on the first heavy frame.** The run then
logged `NVRHI: Device Removed!` followed by 48 repetitions of
`Device recovery for DX12 requires engine-level device recreation. Please restart.`,
and the Nova readback failed with `0x887a0005` (DXGI_ERROR_DEVICE_REMOVED). It
fires right after DemoGame finishes deserialising -- the first frame the tracer
actually runs on the full 7627-instance scene. This is a GPU TDR (watchdog reset),
the same class of failure already noted for Ultra on this project, and it is
consistent with the GL measurement: at Ultra the tracer costs over a second per
frame, which is past the D3D12 watchdog budget.

The binding itself is not the cause: the history resource is created
`R32G32B32A32_FLOAT` + `ALLOW_UNORDERED_ACCESS`, matching both the
`RWTexture2D<float4>` declaration and the UAV descriptor format, so the view is
well-formed. The device removal is the tracer being too heavy, not the fix being
wrong -- but it does mean **the DX side still cannot produce a frame time**; it
crashes the GPU instead. That is the next blocker, and it is the same underlying
problem as the GL result: the per-pixel cost is too high for this scene at Ultra.

**Measured -- OpenGL, DemoGame, 4K, Quality Preset Ultra:**

| | |
|---|---|
| DRS | drove the scale 0.75 -> 0.55 -> 0.40 and pinned at its floor, still above the 33.3 ms target |
| GL timer | GPU 301.1 ms, then 326.0 ms |
| Wall clock | frame 20 at 21:00:42 -> frame 250 at 21:05:32 = 230 frames / 290 s = **~1.26 s/frame (0.8 fps)** |

The GL timer reads about 4x below the wall clock. That is the defect already
noted in Phase 2: `glEndQuery` closes before the god-ray dispatch, so pass 10 and
everything after it sit outside the measured region. The wall clock is the number
that matches what a user sees, so it is the one to trust.

**What this means for Phase 0.** The traversal cut (1024 -> 8 per fog pixel) is
real and verified in the source, but at Ultra on this scene it is not sufficient
-- GL lands at ~0.8 fps with DRS at its floor. The remaining cost is dominated by
per-step work Phase 0 does not touch: `wl_vol_fbm` is evaluated every step
regardless of light count, and the god-ray pass is not measured by the GL timer at
all. Phase 0 is correct and worth keeping; it is not the whole answer, and Phase 2
(the unmeasured passes) needs to be answered before the 30 fps target is reachable.

**Correction, and it matters.** The "~0.8 fps" above was a bad wall-clock
estimate -- I divided a frame count by an interval that was not that many frames.
A periodic frame-time log added to `UpdateDynamicResolution` now reports the wall
clock and the GPU timer side by side:

```
Frame time: app 270.7ms (3.7 fps)  gpu 243.9ms  cpuSubmit 0.1ms  scale 0.40  preset 3
Frame time: app 262.3ms (3.8 fps)  gpu 243.9ms  cpuSubmit 0.0ms  scale 0.40  preset 3
```

**The real figure is ~4 fps, not 0.8**, and `app` tracks `gpu` closely, so the
bottleneck is the renderer, not the editor or present. `cpuSubmit` at ~0.1 ms
rules out CPU-side dispatch cost. This also lines up with the originally reported
"less than 4 fps", which was right.

**Finding 4 -- the cost is the trace, and only the trace.** Per-segment GPU
timing (`WASTELAND_NOVA_PROFILE=1`, four segments replacing the whole-chain query
because GL forbids two active `TIME_ELAPSED` queries at once):

```
Nova segments (ms): trace+restir 219.2  accum+blur 13.9  denoise+composite 2.8  bloom 0.0  | total 235.9
```

| segment | passes | ms | share |
|---|---|---|---|
| trace + ReSTIR | 0, 1, 3, 11, 14 | 219.2 | **93%** |
| accumulation + bilateral | 2, 7 | 13.9 | 6% |
| denoise + composite | 12, 13, 6 | 2.8 | 1% |
| bloom | 4, 5 | 0.0 | off |

The total (235.9) reconciles with the whole-chain DRS measurement (243.9), so
nothing is unaccounted for. ReSTIR is **off by default** (`RestirEnabled = false`),
so passes 3/11/14 never ran and the 219.2 ms is essentially passes **0 + 1** --
visibility/velocity and the hybrid trace.

**This reframes the target.** Everything except the trace costs 16.7 ms combined.
Optimising the denoiser, the accumulation or the composite cannot move the needle
at any budget; the trace is the entire problem. At 4 fps the frame is ~250 ms and
the trace is ~219 ms of it, so reaching 30 fps needs the trace at roughly 20 ms --
a **~11x** cut, not the 7.5x the headline number suggests. Phase 0's 8-traversal
cut is a step in that direction but is nowhere near sufficient, which the
measurement confirms rather than contradicts.

### Phase 0 — Remove the light-loop divergence (fixes the GL/DX gap)

1. **Make the cap one named constant in both twins.** Define
   `WL_VOL_LIGHT_CAP` (or read `u_MaxLights`) in both `.glsl` and `.hlsl` and
   use it in `lightEnd`. Target value: **8**, matching the twin that is
   currently cheaper. Expected: GL drops from ~1024 to ~256 traversals per sky
   pixel — the 4x.
2. **Hoist the shadow test out of the step loop.** For a given receiver the
   light's visibility changes far more slowly than the sample position does.
   Evaluating `IsOccluded` once per light per pixel instead of once per step is
   visually indistinguishable for this low-frequency term and is worth another
   32x: **~1024 -> ~8 traversals per sky pixel**. This is the single largest
   available win on both backends.
3. **Add a quality ladder to the light count** (Low 0, Medium 4, High 8,
   Ultra 8) so "below the highest settings" genuinely gets cheaper, and gate it
   on render scale so a DRS drop also cuts the light count.
4. **Extend `verify_nova_glsl.py`** to compare the fog/cloud light-cap
   expression between twins, so this class of divergence is caught rather than
   re-found. The current guard misses it because it keys on temporal/mip
   symbols that do not appear in the volume code.

Verification: build Release x64 (`wasteland-build-verification`), run DemoGame at
4K / Ultra, and compare the Stats panel "Tracer: N fps" readout plus the
`Dynamic resolution: ... scale ...` log lines before and after, on **both**
OpenGL and DX12. Acceptance: GL and DX report the same FPS within ~10%, and both
are above 30.

### Phase 1 — Stop the double trace

5. **Clear the instance list on the overflow flush**, or make the overflow
   flush a no-op for the Nova path (batch the CPU tessellation instead of
   flushing the GPU trace). Either halves the frame's trace cost on every
   backend and every preset.
6. Confirm the overflow actually fires at runtime — log a warning when `Flush()`
   is called from inside a `Draw*` — before and after the fix, since the trigger
   is camera-dependent.

### Phase 2 — Make the scaler trustworthy

7. Add a GPU timer path for the NVRHI backends (DX12 timestamp queries / Vulkan
   timestamp queries), or fall back to `CPUFlushTimeEMA` explicitly rather than
   mixing in `AppFrameTimeEMA`.
8. Move `glEndQuery` and the `UpdateDynamicResolution()` call to after the
   god-ray dispatch so pass 10 is inside the measured region.
9. Lower `DynamicMinScale` toward 0.3 and add a per-preset floor, so the scaler
   has room to react once Phase 0 removes the per-pixel blow-up.

### Phase 3 — Remove dead per-pixel work

10. Make `LODLevel` real: skip `HitPrimitive` for `LODLevel == 2` instances in
    `TestInstanceHit`/`TestInstanceOcclusion` (AABB-only), or drop them from the
    BVH entirely.
11. Early-out sky pixels in pass 1 before `TraceScene` when the ray cannot hit
    anything; at minimum skip `ComputeDirectLighting` and the emissive-light
    subset for a miss.
12. Dispatch the composite/bloom chain at trace resolution and let the existing
    upscale carry it, rather than at full resolution.
13. Reduce the GLSL traversal stack depth to what the BVH actually needs (a
    median-split tree with leaf size 4 over ~1700 instances is depth ~10, so a
    16-deep stack with an explicit skip guard is safe) to shrink the private
    array in the hottest loop. This partially mitigates 2a; it cannot eliminate
    it in GLSL.

### Phase 4 — Per-shape budgets and diagnostics

14. Gate `HitTorus`'s sample/bisection counts on quality and distance, matching
    every other shape.
15. Measure actual BVH depth for the DemoGame scene and size the stacks to it,
    replacing the silent-skip guard with a bounded, checked one.
16. Add per-frame diagnostics to the Stats panel: instances submitted, BVH nodes
    and depth, leaf count, overflow-flush count, and an estimated
    traversals/pixel — so the next regression is visible without a profiler.

### Phase 5 — Anti-aliasing method selection

Nova already contains most of the machinery this needs; the problem is that it
is unreachable. This phase turns it into a selectable, verified method ladder.

#### 5.0 What exists today

| Capability | Where | State |
|---|---|---|
| Jittered sampling | STBN blue noise, `GetCurrentJitter` (`Renderer3D.cpp:5816`) | **Working.** 8x8 volume, 16 phases, 128 frames. Replaced Halton(2,3). |
| Temporal accumulation (TAA) | pass 2, `RunTemporalAccumulation` | **Working.** Velocity reprojection, depth rejection at 0.02, YCoCg variance clipping (3x3 at Ultra, 5-tap cross below), adaptive alpha. |
| Upscale | pass 8, `RunUpscale` | **Dead code.** Never dispatched by `Renderer3D::Flush`. |
| Bilinear upscale | inline in `RunComposite` | **Working, and the only upscale in use.** `if (u_RenderScale < 1.0f)` block does a 4-tap bilinear fetch. |
| TSR | passes 15/16, `RunTSRResolve` / `RunTSRCopy` | **Scaffold only.** Written, never dispatched, `u_TSREnabled` never set. |
| Spatial denoise | passes 12/13 (ATrous), pass 7 (bilateral) | **Working** — but used as denoise, not selectable as AA. |
| MSAA | `VK_SAMPLE_COUNT_1_BIT` (`NVRHIContext.cpp:3306`) | **Absent.** |

Two consequences matter. First, at Ultra (`RenderScale = 0.75`) the only thing
standing between the tracer and the screen is a **bilinear fetch** — so the
"highest settings" image is soft by construction, and no amount of tracer
quality fixes it. Second, the TSR and upscale passes are already written, so
this phase is mostly *wiring and finishing*, not greenfield shader work.

#### 5.1 Target design

Add one enum and one uniform; keep the heavy paths on pass IDs.

```cpp
// Renderer3D.h
enum class AntiAliasingMethod : int
{
    TAA = 0,          // temporal accumulation + bilinear upscale (current default)
    FSR1 = 1,         // EASU + RCAS spatial upscaler
    TSR = 2,          // temporal super resolution (passes 15/16)
    SpatialDenoise = 3, // ATrous only, no temporal
    SSAA = 4          // RenderScale > 1, no upscale
};
static void SetAntiAliasingMethod(AntiAliasingMethod m);
static AntiAliasingMethod GetAntiAliasingMethod();
```

- `u_AAMethod` (int) is uploaded alongside `u_QualityLevel` and drives the
  upscale branch inside `RunComposite`.
- TSR keeps its own pass IDs (15/16) and its own `u_TSREnabled`, so enabling it
  is a dispatch change, not a uniform change.
- Every method must be **twin-symmetric**: same enum, same pass IDs, same
  uniform name and semantics in `.glsl` and `.hlsl`. Add it to
  `verify_nova_glsl.py`.

#### 5.2 Method A — TAA (default, existing)

Keep as the default; harden rather than rewrite.

17. Fix the accumulation floor. `u_AccumulationAlpha` is currently
    `max(0.02, 1/(FrameIndex+1))` (`:2980`), a running average that decays to a
    0.02 floor. That is correct, but the floor is applied to a *running* average
    whose effective window is 1/alpha, so at 60 fps a static pixel converges in
    ~0.8 s. Expose the floor as a setting (0.01-0.2) so a user can trade ghosting
    for sharpness.
18. Add a **jitter-sequence validation** to the twin guard: assert that
    `GetJitter()` reads `u_Jitter` in both twins and that the sequence period
    (128) and phase count (16) match. The current guard checks the read but not
    the sequence parameters.
19. Verify the depth-rejection threshold (0.02 NDC) against the scene's depth
    range; at DemoGame's 10 km fog box a 0.02 NDC threshold may be too tight or
    too loose depending on the near/far split. Make it a uniform
    (`u_HistoryDepthBias`) so it is tunable without a shader edit.

#### 5.3 Method B — FSR1-style spatial upscaler (EASU + RCAS)

The highest value-per-effort item: it replaces the bilinear fetch that is
currently the only upscale at Ultra.

20. Implement **EASU** (edge-adaptive spatial upsampling) as the upscale branch
    of `RunComposite`, selected by `u_AAMethod == 1`. EASU is a 12-tap
    edge-directed filter with a 4-tap lanczos-2 fallback; it is a published,
    well-specified algorithm.
21. Implement **RCAS** (robust contrast-adaptive sharpening) as a follow-up pass.
    Add pass ID **17** for it (15/16 are taken by TSR), dispatched after the
    composite when `u_AAMethod == 1`.
22. **Licensing note.** Implement EASU/RCAS from the published algorithm
    description, clean-room, as was done for NRD/RTXTF/STBN. Do **not** vendor
    AMD's FSR source or binaries — the project's stance is no vendor SDK
    components. The algorithm is simple enough that a clean-room version is
    both possible and sufficient.
23. Wire the existing but dead **pass 8** (`RunUpscale`) to the EASU path, or
    delete it if the composite branch supersedes it. Leaving it dead while
    adding a second upscale path is how the fog-light divergence happened.

#### 5.4 Method C — TSR (finish and wire passes 15/16)

The scaffold exists; it needs to be completed and dispatched.

24. `RunTSRResolve` currently does a bilinear history blend with depth rejection
    — it has no reactive mask, no auto-exposure, and no sharpening, so it is not
    yet a real TSR. Add: reactive mask from the disocclusion/confidence term,
    auto-exposure from the previous frame's luminance histogram, and an RCAS
    pass.
25. Set `u_TSREnabled` from the CPU and dispatch passes 15/16 when
    `u_AAMethod == 2`. Today `u_TSREnabled` is declared and never set.
26. TSR and TAA both consume history; they must be **mutually exclusive** in the
    dispatch logic. Add an assertion or a comment stating the invariant, since
    the pass IDs are independent and it is easy to enable both.

#### 5.5 Method D — Spatial denoise as AA

27. Expose passes 12/13 (ATrous) as a selectable AA method for static or
    low-motion shots, where temporal accumulation cannot help and a 4-iteration
    edge-aware filter is the right tool. This is a dispatch change plus a
    `u_AAMethod` branch; no new shader code.

#### 5.6 Method E — SSAA

28. Allow `RenderScale > 1.0` (the editor slider currently caps at 1.0) for a
    quality-over-speed mode. This is the one AA method that needs no new code at
    all — it is the existing path at higher internal resolution. Note that the
    DRS scaler clamps to 1.0, so SSAA must be excluded from DRS adjustment.

#### 5.7 Explicitly rejected

- **MSAA.** The only rasterised output is depth and velocity (pass 0); the image
  is produced by a compute ray tracer. MSAA would resolve nothing. `VK_SAMPLE_COUNT_1_BIT`
  is correct as-is.
- **DLSS / FSR3 / XeSS.** Vendor SDK components. The project removed the NVIDIA
  RTX SDK and reimplements clean-room; do not reintroduce a vendor binary. FSR1's
  EASU/RCAS (5.3) is reimplemented from the published spec, not vendored.
- **FXAA / SMAA.** Post-process edge detection on a raytraced image fights the
  tracer's own edges and adds a full-res pass for little gain. If a cheap
  fallback is ever needed, FXAA is a ~20-line addition to the composite, but it
  should not be the default at any preset. **Partially superseded by 5.11.3**,
  which keeps them as explicit legacy options for old GPUs while agreeing they
  are wrong as a default.

#### 5.8 Editor UI and persistence

29. Add an `ImGui::Combo("Anti-Aliasing", ...)` next to the Quality Preset combo
    (`EditorLayer.cpp:1352`), with the five methods. Persist the choice to
    `imgui.ini` alongside the quality preset so it survives restarts.
30. Show the active method in the Stats panel next to the tracer FPS, so a
    screenshot of the panel is enough to know what was measured.

#### 5.9 Verification

31. Extend `verify_nova_glsl.py` to assert that `u_AAMethod` exists in both twins
    with the same type and that every `u_AAMethod` branch has a counterpart.
    The fog-light divergence survived because the guard only checked temporal
    and mip symbols; the AA enum is exactly the kind of thing it would miss.
32. Add a **method-parity test**: for each `u_AAMethod` value, assert the GLSL
    and HLSL twins contain the same set of referenced pass IDs. This catches a
    method that works on one backend and silently no-ops on the other.
33. Build Release x64 (`wasteland-build-verification`), then for each method
    capture a screenshot at 4K / Ultra of a high-contrast edge (a streetlight
    against the sky, a building silhouette) and compare. Acceptance: FSR1 and
    TSR both visibly sharper than the current bilinear, with no visible
    ghosting on a slow camera pan.

#### 5.10 Interaction with the FPS plan

AA choice and render scale are coupled, and the coupling must be explicit.

34. **A spatial upscaler tolerates a lower internal resolution than bilinear
    does.** Once FSR1 lands, `DynamicMinScale` (Phase 2, step 9) can go lower
    than 0.3 without visible softness — which is the headroom the 30 fps target
    needs after Phase 0 removes the per-pixel blow-up. Measure the two together,
    not separately.
35. **TSR is not free.** It adds two passes at full resolution plus a luminance
    histogram. Budget it as part of the AA cost, and do not enable it by default
    at Ultra until Phase 0 and Phase 1 have landed.
36. **SSAA and DRS are mutually exclusive** (5.6). Make the UI refuse the
    combination rather than silently letting one override the other.

#### 5.11 Legacy and classic AA methods

Methods 5.2-5.6 are the modern set. This subsection adds the classic raster-era
techniques. Two of them are already half-present in the tree, and one earlier
rejection in 5.7 is corrected here.

**5.11.1 Reframe 5.1 as two independent axes.**

The enum in 5.1 conflates two orthogonal choices: *how the sub-pixel offset is
chosen* and *how samples are combined*. They should be separate enums, because
the combinations are meaningful — RGSS + TAA and RGSS + FSR1 are both valid, and
folding them into one enum forces a cross-product into the UI:

```cpp
enum class SamplePattern : int      // where the sub-pixel offset comes from
{
    BlueNoise = 0,  // current default, STBN
    Halton = 1,     // classic radical inverse -- code already present, unused
    RotatedGrid = 2,// RGSS, the classic 4x pattern
    Quincunx = 3,   // 5-sample: 4 corners + centre
    OrderedGrid = 4,// 2x2 axis-aligned
    Centre = 5      // no jitter
};

enum class ReconstructionFilter : int // how samples are combined
{
    None = 0,        // raw, RenderScale 1.0
    Nearest = 1,     // 1 tap, point sampling
    Bilinear = 2,    // 4 tap -- the current implicit default
    Box = 3,         // 4 tap unweighted
    Bicubic = 4,     // 16 tap Catmull-Rom
    Lanczos = 5,     // windowed sinc
    FXAA = 6,        // see 5.11.3
    SMAA = 7,
    FSR1 = 8,
    TSR = 9,
    Temporal = 10,   // TAA
    SpatialDenoise = 11
};
```

`AntiAliasingMethod` in 5.1 becomes a thin preset that sets both, so existing
call sites keep working while the UI exposes the two axes.

**5.11.2 Sample patterns — no shader work required.**

Every pattern is a CPU-side choice expressed through the existing `u_Jitter`
uniform. Adding them costs no new pass and no new uniform.

37. Add `SamplePattern` as above and route it through `GetCurrentJitter`
    (`Renderer3D.cpp:5816`), which already takes `frameIndex` and `viewportSize`.
38. **Re-enable Halton** for the `Halton` pattern. The implementation is still
    present and correct at `Renderer3D.cpp:5796`; it was retired in favour of
    STBN because a low-discrepancy sequence produces structured, repeating
    artefacts once accumulated. That is a reason to prefer blue noise, not a
    reason to delete the code — restore it behind the enum so the comparison is
    available.
39. Implement **RGSS** (rotated grid): a 2x2 grid rotated 45 degrees. This is
    the classic 4x pattern and the one most people mean by "4x AA".
40. Implement **Quincunx**: 5 samples — four corners plus the centre, with the
    centre sample weighted 4x. The classic 5x pattern; note that it needs a
    *multi-sample* accumulation path, not a single `u_Jitter`, so it requires
    either an accumulation loop in pass 2 or a per-sample pass ID.
41. Implement **OrderedGrid** (2x2 axis-aligned) and **Centre** (no jitter).
42. **Verify the pattern is twin-symmetric.** `verify_nova_jitter_contract.py`
    already checks the blue-noise source contract; extend it to assert that the
    pattern enum and its dispatch exist in both twins. A pattern that only
    jitters on one backend is the same class of bug as the fog-light divergence.

**5.11.3 Post-process edge AA — the FXAA/SMAA family.**

These are the classic "old" methods and the most worth adding, because they are
cheap, backend-independent, and need no temporal state.

| Method | Cost | Notes |
|---|---|---|
| **FXAA 3.11** | 1 full-res pass, ~20 lines | The classic cheap fallback. Luma-based edge detection with a 3x3 search. |
| **SMAA** | 2-3 full-res passes | Edge detection, blend-weight precomputation, then blend. The standard since ~2012; visibly better than FXAA on geometric edges. |
| **MLAA** | 2-3 passes | SMAA's ancestor. **Reject** — superseded, and there is no reason to implement both. |

43. Implement **FXAA** as `ReconstructionFilter::FXAA`, a branch in the composite
    or a new pass. It is the correct fallback for pre-Volta hardware: one
    full-res pass, no temporal state, no history buffer.
44. Implement **SMAA** if FXAA proves insufficient. Precompute the blend-weight
    texture once at startup rather than per frame.
45. **Correct the 5.7 rejection.** 5.7 rejects FXAA/SMAA as "fighting the
    tracer's edges". That is true for the *default at Ultra* — the tracer already
    produces a converged image, and edge-detection AA can only blur it. It is not
    true as a blanket rejection: as **explicit fallback options for old GPUs**
    they are exactly right, because they are the only methods in this document
    that cost one full-res pass and no temporal state. Keep them out of the
    default preset, not out of the engine.

### Phase 6 — BVH culling and LOD: the Nanite analogue

Raised by the question "can we have UE5-Nanite-style virtualized geometry here?".
The honest answer: **Nanite's rasterizer half does not apply, but its culling half
does — and half of that is already built and switched off.**

**What the engine already has. Reuse these; do not rebuild them.**

| Asset | Where | State |
|---|---|---|
| `LODLevel` field | `RayTracingInstance` (288 B, uploaded) | Declared in **both** twins (`glsl:117`, `hlsl:37`), written CPU-side with distance bands, **read by no shader** |
| Distance bands | `LODNearDistance` 500, `LODFarDistance` 2000, `MaxDrawDistance` 10000 (`Renderer3D.cpp:557-559`) | Active |
| `MaxGeometryLODLevels` | `Renderer3D.cpp:1144` (=4) | Declared, unused |
| Per-instance frustum cull | `Renderer3D.cpp:5405-5435` | Active |
| Hard distance cull in shader | `glsl:2972`, `glsl:1541`, `hlsl:2027`, `hlsl:1187` | Active |
| BVH | spatial median split, binary tree, 32 B GPU nodes, `stack[32]` walk | Active |
| Analytic shape dispatch | `HitPrimitive` (`glsl:1495`) | Active |

The telling detail: the CPU comment at `Renderer3D.cpp:5421` already says *"The
shader can use LODLevel to skip detailed geometry"*. Documented intent, never
implemented. `LODLevel` is written every frame for every instance and ignored.

**Why Nanite's rasterizer half does not transfer.** Nanite's two mechanisms are
(a) eliminating vertex processing — no vertex shader, no vertex fetch, no index
decode — and (b) cluster LOD so cost tracks screen coverage rather than authored
complexity. This tracer has **no triangles** (`HitCube`/`HitSphere`/
`HitCylinder`/`HitCone`/`HitTorus` are analytic), so (a) has nothing to eliminate
and (b) has nothing to cluster. A meshlet BVH plus a software rasterizer would add
cost to a path that already avoids both.

**What does transfer** is (b)'s principle applied to the BVH: *do not traverse
instances whose screen footprint is negligible.* That is the design below.

46. **Consume `LODLevel` — cheap proxy substitution. Do this first.**
    `HitTorus` runs **32 samples plus bisection** (`glsl:1403`), and its own
    comment records that *"torus intersection cost is fixed regardless of quality
    preset -- unlike the other shapes, it is not on the `u_QualityLevel` dial."*
    That fixed cost is exactly what `LODLevel` exists to switch off. In
    `HitPrimitive`, gate the expensive shapes on the field that is already
    uploaded:

    ```glsl
    // LOD 2 (beyond LODFarDistance): the torus bisection is 32 samples + a
    // bisection search, paid per hit with no distance dependence. A box proxy
    // is indistinguishable at that range. LODLevel is already in the struct
    // and already written by the CPU -- it is currently read by nothing.
    if (inst.LODLevel >= 2 && (shape == 5))
        return HitCube(localRay, outNormal);
    ```

    Apply the same to `HitCylinder`/`HitCone` if their cost shows up. This is a
    **shader-only change**: no new uniform, no CPU work, no ABI change, and it is
    a no-op at LOD 0/1 so the near-field image is untouched.

    **Precondition: ANSWERED — this is not worth doing for this scene.** DemoGame
    contains 1024 cubes, 678 spheres, 3 cylinders, 1 cone, **1 torus**, 1 plane
    (1729 entities total). The defect and the LOD hook touch **5 instances, 0.29%
    of the scene**. `HitTorus`'s 32-sample bisection is real but it runs on one
    object; gating it cannot move a 226 ms frame. Keep the change as a
    correctness/robustness improvement for future scenes with many toruses, and
    do not spend measurement time on it.

47. **Screen-space instance culling — the real Nanite analogue.**
    Today the BVH is built from every instance inside `MaxDrawDistance`, and the
    traversal (`glsl:3315-3369`) has **no screen-size rejection at all**: its only
    pruning is the AABB slab test and `tNear > info.t`. Nanite's core idea — do
    not process what is sub-pixel — has no equivalent here.

    Implement it **CPU-side, in the existing cull loop** (`Renderer3D.cpp:5405`),
    which already computes each instance's world AABB centre:

    - Project the AABB to NDC with the current view-projection.
    - Derive its pixel footprint at the **current trace resolution** (not the
      display resolution — DRS changes it every few frames, so a footprint
      computed against the wrong size either culls visible geometry or culls
      nothing).
    - Skip submission below a threshold (start at 4 px; make it a named constant).
    - Keep it **conservative**: an instance straddling the threshold must be kept,
      and the cull must never hide an instance whose AABB touches the view.

    This removes instances from the BVH entirely, which shrinks both the node
    count and every leaf loop — the traversal win Nanite gets from cluster LOD.

    **Ordering constraint: SUPERSEDED.** This step originally said to do it after
    step 48, "a screen-space cull built on incorrect bounds is not merely slow, it
    hides geometry". Step 48 turned out to describe no defect (see its retraction),
    so there was nothing to order against -- the cull was built and measured
    without it, and the bounds it relies on are correct.

48. **RETRACTED -- the "BVH bounds defect" does not exist.** This step claimed
    `ComputeWorldAABBMax` (`Renderer3D.cpp:306` / `:400`) reads only the xyz of
    `Min`/`Max` and ignores their w components, so "those shapes' BVH bounds are
    wrong". Checked against the call sites and it is wrong:

    - `Min.w` carries the shape's SECOND DIMENSION **as a scalar for the analytic
      hit function** -- height for a cylinder/cone, minor radius for a torus. The
      shader passes it straight through: `HitCylinder(localRay, inst.MaterialParams.w,
      inst.Min.w, outNormal)` (`glsl:1650`). It is a parameter, not a bound.
    - `Max.w` is a literal `1.0f` (`Renderer3D.cpp:5804`) and no shader reads it.
    - The local AABB **already spans** that dimension, because the `Draw*` helpers
      pass it in the y extent: `DrawCylinder` uses
      `(-radius, -height*0.5, -radius)` .. `(radius, height*0.5, radius)`, and
      `DrawTorus` uses `(-outer, -outer, -minorRadius)` .. `(outer, outer, minorRadius)`.

    So using `.xyz` only is **correct**, and folding w into the bound would
    actively break the AABB. The instinct behind the step -- "a shape dimension
    that the bounds function cannot see is a bounds bug" -- is a good one and is
    worth keeping as a check; it just does not apply here, because the dimension
    is visible to the bounds function through the local box rather than through w.

    **Before "fixing" a claimed bounds bug, read the shape's `Draw*` call site and
    establish whether the dimension lives in the local AABB or in a shader
    parameter. Only one of those is a bound.**

49. **BVH quality: spatial median split -> SAH.** `BuildBVHRecursive`
    (`Renderer3D.cpp:348-450`) splits on the longest axis at the centroid
    midpoint — balanced by *count*, ignoring surface area. A surface-area
    heuristic reduces expected node visits for the same tree, which is a direct
    multiplier on the traversal term. This is the highest-risk item here
    (it rebuilds the builder and the flatten step) and it is independent of 46-47,
    so it goes last.

    **Re-measure before doing this.** The traversal term this item would optimise
    was **226.4 ms before the traversal-stack fix and is 15.1 ms after it** -- the
    earlier figure of "173 ms" in this paragraph was a mid-session reading and is
    also superseded. The SAH win is a multiplier on whatever the traversal costs
    *now*, so its absolute value has fallen by roughly the same 15x as the term
    itself. It is still the right thing to do for a scene whose BVH is rebuilt
    often, but it is no longer the emergency it was planned as, and the expected
    gain should be recomputed from the current 15.1 ms before committing to the
    rebuild.

**Where this does and does not land.** With the scene counts now known, the
ranking changes: **step 47 is the only item here that can move a 226 ms frame**,
because it is the only one that touches the 1729 instances that actually get
traversed. Step 46 is real but small (5 instances). Step 48 **does not exist** --
it was retracted (see above). Step 49 is the traversal-quality lever for the bulk
of the scene, and its expected gain must be recomputed against the current 15.1 ms
traversal rather than the 226 ms this paragraph was written against. None of them
is a cluster rasterizer, and none should be: the traced path has no triangles, so
the part of Nanite that would have to be invented is precisely the part that
cannot pay.

**Superseded ranking.** This paragraph ranks items by their effect on a 226 ms
frame. That frame no longer exists: the traversal-stack fix (step 53) took it to
30.3 ms and the frame is now 20-24 ms. Steps 47/49 remain correctly ordered
relative to each other, but their absolute value should be re-derived from the
current frame before any of them is scheduled.

### Phase 6 results — what the Nanite analogue actually bought

Steps 46-48 were implemented as one shared CPU cull, `CullInstanceForTrace`
(`Renderer3D.cpp`, defined after `s_Data`), applied at all five submission paths
via `WASTELAND_SCREEN_CULL=<px>` (0 = off). The implementation surfaced a defect
worth more than the cull itself:

**`DrawCube`, `DrawSphere`, `DrawWaterPlane` and `DrawTerrainChunk` submitted
unconditionally.** The frustum / distance-LOD / `MaxDrawDistance` cull existed
only inside `DrawProceduralPrimitive`, whose own comment calls frustum culling
*"the single most impactful optimization -- a 2500x2500m city may have 1000+
buildings but only ~50 are visible"*. That comment was true of a function that
handles 5 instances, not of the two that handle **1702 of DemoGame's 1729
entities, 97% of the scene**. They had no frustum test, no distance LOD and no
distance cull. The shader's per-ray `MaxDistance` test still rejected them, but
inside the traversal, for every ray -- exactly the work a CPU cull exists to
avoid.

**Measured (OpenGL, DemoGame, 4K, Ultra, `WASTELAND_NOVA_PROFILE=1`):**

| Configuration | visibility | trace | accum | denoise | total |
|---|---|---|---|---|---|
| Baseline (cull on 1 of 5 paths) | 1.1 | 226.4 | 15.1 | 3.1 | 245.7 |
| **Fog volumes disabled** | 1.5 | **173.2** | 13.6 | 2.8 | **191.2** |
| Cull on all 5 paths | 1.1 | 213.0 | 12.9 | 3.3 | 230.3 |

Read the middle row as the camera-independent one; the other two are superseded
by the pinned-camera A/B at the end of this section.

50. **The fog march is the largest real lever found: 36.3 ms, 17% of the frame.**
    **Correction to this step's first revision**, which reported 53.2 ms from the
    per-segment profiler. That profiler is unreliable (see step 51); the number
    that has been reproduced against the wall-clock frame time is **36.3 ms**
    (216.7 -> 180.4 ms with the scene's two volume components disabled, pinned
    camera, no profiler). The segment figure was an overestimate.

    Mechanism, still correct: the march is per-pixel rather than per-instance --
    32 steps, each evaluating FBM noise and running the light loop, which is up to
    8 `IsOccluded` calls -- **256 BVH traversals per pixel** from the light loop
    alone. `WL_VOL_LIGHT_CAP` already cut the light loop from 1024.

    Note what the step cap experiment showed: halving the step count saved only
    3.3 ms of the 36.3, so the cost is **not** in the step count. The per-pixel
    fixed cost -- the box intersection, the transmittance bookkeeping, the noise
    evaluation -- dominates it. That is where to look next.

51. **Instance count is not a lever, and neither is the frustum cull.** The
    original version of this step claimed a 4x instance cut barely moved the
    frame; the next revision claimed a pinned-camera cull was worth 16.5%. **Both
    are withdrawn.** Measured against the wall-clock frame time with the profiler
    off, the cull moved 213.1 -> 214.2 ms -- it does nothing.

    The reason is worth keeping: every earlier version of this result was read
    from the renderer's GPU EMA (`gpu` in the frame-time log). That number moved
    by 32 ms under the same change while the frame did not, and it read 243.5 ms
    in a 213.1 ms frame -- which cannot happen if the frame waits on the GPU. The
    GPU EMA is not a frame-cost measure and must not be used as one.

    What does move the frame time, measured the same way:

    | Change | app |
    |---|---|
    | Nova on (baseline) | 216.7 |
    | fog + clouds disabled | **180.4** |
    | march steps 32 -> 16 | 213.6 |
    | CPU cull on (211 -> 203 instances) | 214.2 |
    | **Nova disabled** | **17.3** |

    So the fog is worth 36 ms, the step cap 3 ms, the cull nothing, and the Nova
    path itself 199 ms. The trace pass is where the remaining ~163 ms lives.

52. **The fog's cost is the per-pixel shadow traversals, not the per-step loop —
    and hoisting the range cull above them is worth 20-25 ms.** This is the one
    change found so far that is both large and free.

    The per-step loop was the obvious suspect, but halving the step count moved
    only 3.3 ms of the fog's 36.3. What identified the real cost was cutting the
    *light cap* instead: 8 lights -> 1 measured

    | Arm | app |
    |---|---|
    | 8 lights, no range cull | 216.7 |
    | 1 light, no range cull | **188.0** |

    28.7 ms. The per-step loop runs once per step; the shadow test runs once per
    *light per pixel*. With 8 lights that is 8 complete `IsOccluded` BVH walks
    over all 1729 instances, per pixel, before the march even starts — 79% of the
    fog's cost, and it does not scale with the step count, which is exactly what
    the 3.3 ms step-cap result says.

    The hoist already existed; what it lacked was a range test. It traversed
    every selected light unconditionally, and with 677 point lights in DemoGame
    most are far outside their own range. The fix lifts the per-step range cull
    (`arange > 0 && dL >= arange`) above the traversal:

    | Arm | app |
    |---|---|
    | range cull off (`WASTELAND_VOL_NOCULL=1`) | 223.8 |
    | range cull on (default) | **198.5** |

    **25.3 ms, 11.3% of the frame.**

    **The test must be against the whole segment, not the entry point.** Testing
    only the volume's entry point is an approximation and a wrong one — the box
    is 10000 m deep, so a light beyond range of the entry can be well inside
    range of a sample further along the ray, and culling it would silently drop
    real in-scattering. What is exact is the closest point on the segment
    `[t0, t1]`: if that is beyond the range, no sample in the volume can be
    within it. The test is conservative in the other direction (a light inside
    range of the closest point but outside every discrete sample still pays its
    traversal), which is the previous behaviour rather than a regression.

    **Exactness verified numerically, not visually.** Screenshots cannot be used
    here: a control run of the *same* arm twice produced different images (the
    fog is advected by `u_Time`), so any two-run image comparison is invalid.
    Instead the two predicates were extracted and checked against each other over
    400,000 random ray/volume/light configurations: 70,690 were culled by the
    segment test and **0** of them had a sample within range. The cull cannot
    change the answer.

53. **The BVH traversal stack was in local memory on OpenGL, and moving it to
    shared memory took the frame from 198.5 ms to 30.3 ms — 33 fps. This is the
    single largest finding in the whole investigation, and it is the answer to
    the original problem.**

    The GLSL twin declared `int stack[32]` in both `IsOccluded` and `TraceScene`.
    Every index into it is a runtime value, so the compiler cannot keep it in
    registers and places it in **local memory** — thread-private storage backed
    by global memory. Every push and pop of the traversal was therefore a
    global-memory access, in the hottest code in the shader: these two functions
    run for every primary ray and every shadow ray.

    The HLSL twin never had this problem, and the reason is recorded in its own
    comment: *"groupshared: fxc cannot compile a dynamically-indexed local array
    inside the traversal call graph"*. DX was **forced** onto
    `groupshared int gs_trav_stack[64 * 32]`; GLSL silently got the slow path.
    That asymmetry is why the OpenGL backend was slower than the DX backends at
    every quality level, which was the original symptom.

    The fix is to mirror the HLSL layout in GLSL, which `#version 430 core`
    supports:

    ```glsl
    shared int gs_trav_stack[64 * 32];          // 8x8 group, 32 slots per thread
    ...
    int stackBase = int(gl_LocalInvocationIndex) * 32;
    gs_trav_stack[stackBase + stackPtr++] = 0;
    ```

    64 threads matches `local_size 8x8` and the HLSL `numthreads(8, 8, 1)`. Each
    thread touches only its own 32 slots, so no barrier is required — the same
    argument the HLSL makes. `IsOccluded` and `TraceScene` were both converted.

    **Measured (OpenGL, DemoGame, 4K, Ultra, pinned camera, no profiler):**

    | Arm | app | fps | DRS scale |
    |---|---|---|---|
    | private stack (before) | 198.5 | 5.0 | pinned at 0.40 floor |
    | **shared stack (after)** | **30.3** | **33.0** | 0.57, converging |

    **168 ms removed, 6.5x.** The DRS is no longer pinned at its 0.40 floor — it
    walks 0.75 -> 0.69 -> 0.63 -> 0.57 while converging on its 33.3 ms target,
    which is what "the target is met" looks like from the inside. Zero GL errors,
    zero shader compile errors, guard PASS, glslang SPIR-V 309,944 B.

    Verified visually as well: the render is correct, and the volumetric fog is
    clearly present — a misty layer above the ground plane and haze between the
    structures, with no black voids, missing geometry, garbage pixels or noise.

    **What this means for the 30 fps target.** It is met at 33 fps with the DRS
    running at 0.57 and still converging, so there is headroom rather than a
    knife-edge. The remaining work is headroom, not survival: the DRS can rise
    further as more is removed, and the ~9 ms of in-range volumetric traversals
    plus whatever remains in the trace pass are the next candidates.

54. **The per-segment split after the fix -- the trace pass is now the whole of it.**

    With the stack in shared memory and the profiler on:

    ```
    Nova segments (ms): visibility 0.2  trace 15.1  accum+blur+restir 1.3  denoise+composite+bloom 0.3  | total 16.8
    ```

    Against the same measurement before the fix (`trace 226.4 ... total 245.7`),
    the GPU work of the whole Nova path fell **245.7 -> 16.8 ms, 14.6x**. The
    trace pass is 15.1 ms of that, so 90% -- the denoiser, accumulation and
    composite are together 1.6 ms and cannot matter at any budget.

    The frame is 28.1 ms while the Nova GPU work is 16.8 ms, so roughly 11 ms of
    the frame is editor and driver overhead rather than rendering. That is the
    same ordering the `WL_FRAME_TIMING` split showed (`imguiEnd` holding the wait),
    and it means the next 30 fps-adjacent win is as likely to be in the editor
    loop as in the renderer.

    This also confirms the fix is not a profiling artefact: the frame time fell by
    170 ms and the segment total fell by 229 ms, in the same direction and by the
    same order.

55. **After the fix the frame is no longer GPU-bound, and the remaining fog cost
    is not worth taking.** With the shared stack in place, an A/B of the two
    volume components:

    | Arm | app | gpu | DRS scale |
    |---|---|---|---|
    | fog + clouds ON | 28.0 ms (35.7 fps) | 17.3 ms | 0.40 (floor) |
    | fog + clouds OFF | 28.4 ms (35.2 fps) | 12.9 ms | 0.40 (floor) |

    The fog is **4.4 ms of GPU and nothing in frame time** — the two arms are
    within noise of each other and the arm WITH the fog is nominally faster. The
    GPU work fell but the frame did not, because the frame is now split roughly
    evenly between the renderer and the editor loop.

    `WL_FRAME_TIMING=1` shows where the rest sits:

    ```
    Frame sections (ms/frame): begin 0.0  update 5.0  imguiBegin 1.8  imguiDraw 2.4  imguiEnd 15.1  window 0.1  total 24.4
    Frame sections (ms/frame): begin 0.0  update 2.4  imguiBegin 0.1  imguiDraw 3.0  imguiEnd 18.6  window 0.1  total 24.1
    ```

    `imguiEnd` is 15-19 ms, which tracks the 13-17 ms of GPU work — it is where
    the wait for the renderer lands. `update` + `imguiDraw` + `imguiBegin` is
    another 5-10 ms of real editor work.

    **So further GPU reduction has diminishing returns until the editor loop is
    addressed.** This is the point at which the investigation stops being about
    the renderer: the renderer is at 16.8 ms of GPU work for the whole Nova path
    and the frame is 24-28 ms.

56. **Defect found while measuring: the DRS is being driven by a broken
    measurement.** Its log reads

    ```
    Dynamic resolution: GPU 577.6ms (target 33.3ms) scale 0.75 -> 0.55
    Dynamic resolution: GPU 360.5ms (target 33.3ms) scale 0.55 -> 0.40
    ```

    in a run whose frame time is 22-28 ms. The whole-chain query it reads back
    reports 577 ms against a 28 ms frame — a 20x discrepancy — so the scaler
    drives the resolution to its 0.40 floor for no reason. The correct response
    to a 17 ms frame would be to RAISE the scale, not pin it.

    This is a quality defect rather than a speed defect (the frame already meets
    30 fps), but it means the renderer is currently running at a quarter of the
    resolution it could afford. Fixing the query would raise image quality at no
    frame-time cost. Not fixed here: it is in `UpdateDynamicResolution` and the
    query's open/close pairing, and it needs its own measurement.

57. **Root cause of the DRS defect, found by instrumenting the query.**

    Two independent faults, both evidenced.

    **Fault 1 -- the GPU query almost never produces a sample.** With
    `WL_DRS_DEBUG=1`, a 200-frame run logged:

    ```
    DRS query raw: 16.89 ms  (EMA 16.89 ms, sample #1)
    ```

    and **199** `query NOT available this frame` lines. So the poll succeeds
    once in two hundred. The one sample it did get read **16.89 ms**, which is
    correct against a 22-28 ms frame -- the query measurement itself is sound.
    The fault is in the polling, not the measurement.

    The consequence is in `UpdateDynamicResolution`:

    ```cpp
    // GPU time is preferred only after 10 real samples
    if (s_Data.DRSGpuSamples > 10 && s_Data.GPUFrameTimeEMA > 0.0)
        ema = s_Data.GPUFrameTimeEMA;
    ```

    One sample in two hundred never satisfies `> 10`, so the correct GPU signal
    is **never used at all**. The scaler is driven entirely by the app-time
    branch.

    **Fault 2 -- `AppFrameTimeEMA` is seeded by the startup frame and converges
    far too slowly.** It is fed from `EditorLayer.cpp:736` by
    `NotifyFrameTime(ts.GetMilliseconds())`, and blends at 0.95/0.05 -- a
    ~20-frame time constant -- with the first sample seeding it outright. The
    first sample is the shader-compilation frame, so the EMA starts at
    roughly 600-900 ms and decays 5% per frame while the scaler adjusts every 10
    frames. That is exactly the observed trajectory: `GPU 556.8ms` then
    `GPU 348.1ms`, both in a run whose frame time was 22-28 ms.

    The two faults compound through the "slowest signal wins" policy:

    ```cpp
    if (s_Data.AppFrameTimeEMA > 0.0 && s_Data.AppFrameTimeEMA > ema)
        ema = s_Data.AppFrameTimeEMA;
    ```

    Taking the maximum of a good-but-unavailable GPU signal and a stale app
    signal guarantees the stale one wins, so the scaler pins the render scale to
    its 0.40 floor for the first ~50 frames of every session and then acts on a
    value still several times too large.

    **The fix is not to trust the GPU EMA more -- it is to stop gating it behind
    a sample count the poll can never reach, and to seed the app EMA from a
    wall-clock frame time rather than the first timestep of a session that is
    still compiling shaders.** Both are small changes in
    `UpdateDynamicResolution` and `NotifyFrameTime`, and both are untested here.

58. **Fault 2 is FIXED; fault 1 is not, and the fix is partial.** `NotifyFrameTime`
    now clamps its seed to 66.7 ms (15 fps) before believing it:

    ```cpp
    if (s_Data.AppFrameTimeEMA <= 0.0)
    {
        s_Data.AppFrameTimeEMA = glm::min(ms, 66.7);
        return;
    }
    ```

    Measured, same scene and configuration:

    | | before the fix | after the fix |
    |---|---|---|
    | first DRS reading | `GPU 556.8ms` | **`GPU 67.2ms`** |
    | second | `GPU 348.1ms` | **`GPU 55.0ms`** |
    | third | — (pinned at floor) | **`GPU 41.6ms`** |
    | scale trajectory | 0.75 -> 0.55 -> 0.40, stuck | 0.75 -> 0.55 -> 0.43 -> 0.40, converging |

    The starting value fell **8.3x** and the trajectory now converges instead of
    being pinned. The seed clamp is safe in both directions: it only applies while
    the EMA is zero, so a genuine stall later in a session is still tracked, and
    66.7 ms is a slow-but-real frame rather than a guess at the steady state.

    **What is still wrong.** The scale still ends at the 0.40 floor, and the
    third reading is 41.6 ms while the same run's `app` readings are 21.8-24.1 ms.

    **Correction to an earlier draft of this step.** It claimed the DRS was fed
    `ts.GetMilliseconds()` while `app` was a different quantity measured in
    `Application.cpp`. That is wrong: the frame-time log's `app` field prints
    `s_Data.AppFrameTimeEMA` directly, so the log and the scaler share ONE signal.
    There is no second, better measurement sitting unused.

    So the gap is not a signal mismatch. It is the EMA still converging: the
    first reading is the clamped seed (67.2 ms), the blend is 0.95/0.05, and the
    scaler adjusts every 10 frames, so for the first ~30-50 frames it acts on a
    value between the seed and the true frame time. At frame 30 the reading is
    41.6 ms; by frame 100+ the log shows 21.8-24.1 ms.

    **The remaining fix is therefore to converge faster or seed better, not to
    change the signal.** Options, none applied here: shorten the blend, or seed
    from a short burst of early frames rather than clamping a single one. Both
    need their own before/after A/B, and the honest position after this step is
    that the seed fault is fixed and verified while the convergence-rate fault is
    identified but untouched.

60b. **Fault 1 root-caused: `glBeginQuery` is failing with GL_INVALID_OPERATION.**
    The poll diagnostic that settles it logs the GL error alongside the pool:

    ```
    DRS poll miss: readQ idx 2 avail 0 | pool avail [0 0 0] | glError 1282
    DRS poll miss: readQ idx 1 avail 0 | pool avail [1 1 0] | glError 1282
    DRS poll miss: readQ idx 2 avail 0 | pool avail [1 1 0] | glError 0
    DRS poll miss: readQ idx 0 avail 0 | pool avail [0 1 0] | glError 0
    ```

    `1282` is `GL_INVALID_OPERATION` (0x0502). So `glBeginQuery(GL_TIME_ELAPSED,
    ...)` is **failing** -- the query is never started, and a query that is never
    started can never become available. That is the entire 199/200.

    Two corroborating details. The read index rotates 2, 1, 2, 0, 1, 2 rather
    than 0, 1, 2, which means `DRSTimerIndex` is not advancing every frame -- the
    open is being skipped, which is what the `!DRSTimerActive` guard does when a
    previous query was never ended. And the pool sometimes reads `[1 1 0]`, so
    queries 0 and 1 do complete on the occasions the begin succeeds; the machinery
    works, it is just almost never started.

    **This is a real defect in the timer, not a driver quirk to code around**: a
    `glBeginQuery` whose failure is never checked, on a guard (`DRSTimerActive`)
    that the failure then wedges. The fix is to check `glGetError()` immediately
    after the begin and clear `DRSTimerActive` (and roll the index back) when it
    fails, so the next frame retries instead of starving the timer for the rest
    of the session. **Not applied here** -- it needs its own before/after, and the
    DRS's visible symptom is already resolved by the warmup gate, which is why
    the scaler is driven by the app-EMA path.

60c. **That hypothesis was WRONG, and the fix is a no-op.** The error check was
    applied and measured: **zero** `DRSBeginFailures`, so `glGetError()` is
    `GL_NO_ERROR` after `glBeginQuery` and the begin is not failing. The 1282
    observed at the poll site is raised by `glGetQueryObjectiv` on a query that
    has no result -- a *consequence* of the timer starving, not the cause of it.

    What the follow-up diagnostics did establish:

    - The read index now rotates regularly (2, 0, 1, 2, 0, 1, ...), so
      `DRSTimerIndex` advances every frame and the open is not being skipped.
    - A static scan of the region between the open and the close finds no early
      `return`, `continue`, `break` or `goto`, so `glEndQuery` is reached.
    - The query still never becomes available: 119 misses in 120 frames, with
      the single success reading 16.83 ms -- correct against a ~21 ms frame.

    So the query is opened and closed correctly, on the right schedule, with a
    clean error state, and its result is nonetheless never available. **That is
    unexplained.** The error check is left in place as a defensive measure (it
    costs one `glGetError()` per frame and would prevent a wedged timer if the
    begin ever does fail), but it fixes nothing here and should not be read as a
    fix.

    **The consequence that matters for anyone reading the logs:** the `gpu` field
    in the `Frame time:` line is **not a real measurement in this engine**. It is
    whatever the last successful query happened to return, which in practice is
    one sample at startup and nothing thereafter. The DRS therefore runs on the
    app-time signal alone, and any perf number read off `gpu` -- including
    several quoted earlier in this document before the practice was understood --
    is not a frame-cost measure.

61. **DX12 re-tested after the traversal fix: still TDRs.** The traversal-stack
    fix was the one change that could plausibly have caused or cured a DX12
    device-removal, so the backend was re-run end to end:

    ```
    [10:42:14] DX12 Nova active: api=3 rtEnabled=1 renderTarget=2332x1451 framesInFlight=3 descriptorsPerSlot=64 uavCount=12
    [10:42:15] NVRHI: Device Removed!
    [10:42:15] DX12 Nova readback staging buffer: 2332x1451 fmt=2 rowPitch=37376 total=54232512
    [10:42:15] DX12 Nova readback: staging buffer creation failed (0x887a0005) for 2332x1451
    ```

    Nova initialises correctly (the `uavCount=12` register-coverage fix holds),
    then the device is removed **one second later**. The staging-buffer failure
    is `DXGI_ERROR_DEVICE_REMOVED`, a consequence of the removal, not a cause --
    so the readback is not the bug. The symptom is a >2 s GPU stall on the first
    Nova frame, which is what a TDR is.

    One plausible cause was checked and eliminated: the HLSL twin's traversal
    stack is `groupshared int gs_trav_stack[64 * 32]` indexed by
    `sBase = g_tid * 32`, and `g_tid` is set as
    `(dispatchThreadID.y % 8) * 8 + (dispatchThreadID.x % 8)` -- group-local,
    0-63. The maximum index is therefore 2016 + 30 = 2046, inside the 2048-element
    array. The stack is not out of bounds, and an out-of-bounds groupshared write
    is exactly the kind of thing that would hang a GPU.

    **Diagnosing this needs a GPU debugger (PIX) or a WDDM driver log, and is
    out of reach from here.** The honest position: the traversal fix is confirmed
    on OpenGL only, and DX12 remains unverified for reasons that have nothing to
    do with the changes in this document -- it was already failing before them.

62. **The reconstruction-filter ladder is now real, and it was dead before.**
    The AA ladder requested in section 5.11 was plumbed end to end -- enum,
    editor combo, `glGetUniformLocation`, `glUniform1i`, and all three NVRHI
    `Set()` calls -- and **consumed by nothing**. Neither Nova twin declared
    `u_ReconstructionFilter` or `u_SamplePattern`, so `glGetUniformLocation`
    returned -1 and every one of those uploads was silently dropped. The editor's
    AA dropdown did nothing at all, on every backend.

    **An enum plus a `Set()` call is not a feature.** The uniform has to be
    *declared in the shader*, or the whole chain is a no-op -- and silently so,
    which is the worst kind. This is the same failure mode as the `gpu` field
    above: a value that is computed, uploaded and then never read.

    Both twins now declare both uniforms, and `RunComposite` carries a real
    branch per filter, weighted from the same fractional coordinate bilinear
    used so switching filters changes only the reconstruction kernel:

    | value | filter | taps | character |
    |---|---|---|---|
    | 0 | None | - | no upscale (only meaningful at RenderScale 1.0) |
    | 1 | Nearest | 1 | cheapest, most aliased; the "no AA" baseline |
    | 2 | Bilinear | 4 | the previous implicit default |
    | 3 | Box | 4 | unweighted 2x2; softer, no interpolation |
    | 4 | Bicubic | 16 | Catmull-Rom, separable, weight-normalised |
    | 5 | Lanczos | 12 | 3-lobe windowed sinc; best edges, most ringing |
    | 6-11 | Temporal/FSR1/TSR/SMAA/FXAA/SpatialDenoise | - | separate passes, not reconstruction filters -- fall through to bilinear rather than being silently ignored |

    The kernels are textbook forms written from the published definitions:
    Catmull-Rom is the cubic B-spline with a=-0.5, Lanczos-3 is
    `sinc(x)*sinc(x/3)` truncated at |x|<3. Both are applied separably, so a 2D
    filter costs its tap count rather than its square.

    **Verification, in two parts.** The kernel math was checked by transcribing
    the shader's own formulas and evaluating them:

    - Catmull-Rom: `k(0)=1.000000`, `k(-2)=k(-1)=k(1)=k(2)=0`, continuous across
      the `x=1` seam (`0.000050` / `-0.000050`), and the four weights sum to
      **1.000000** at f = 0.0, 0.25 and 0.5. Interpolating and correctly
      normalised.
    - Lanczos-3: weight sums 1.000000 / 0.996972 / 0.994299 at f = 0.0 / 0.25 /
      0.5 (the caller normalises), and `k(3.0) = 0.000000` -- correctly truncated.

    The branch was verified by running all four spatial filters through the
    headless harness with `WL_AA_FILTER=<n>` (a test hook added for this), fog
    disabled, camera pinned, screenshot at the same frame index. Screenshot A/B is
    not usable here -- even a control run of the same arm twice differs, because
    the scene is time-animated beyond the fog -- so the comparison is against the
    control's own noise floor, restricted to the content region:

    | pair | mean abs diff | vs control |
    |---|---|---|
    | **control** (nearest, run twice) | **1.032** | 1.00x |
    | Nearest vs Lanczos | 1.582 | **1.53x** |
    | Bilinear vs Lanczos | 1.345 | 1.30x |
    | Bicubic vs Lanczos | 1.159 | 1.12x |
    | Nearest vs Bicubic | 0.974 | 0.94x |
    | Nearest vs Bilinear | 0.704 | 0.68x |

    The decisive evidence is the **ordering**: difference from Nearest runs
    Lanczos > Bicubic > Bilinear, which is exactly what the kernels imply
    (Lanczos is furthest from point sampling, bilinear nearest to it). A filter
    that were not being applied would give no such ordering. The effect is
    modest -- at most 1.5x the scene's own temporal variation -- because the
    scene is smooth and the upscale mild, so this is a correctness verification
    of the ladder, not a claim that the filters are visually dramatic.

62b. **The sample-pattern axis is verified too, and it needed no shader work.**
    The filter axis above was the half that had never been consumed. The other
    half -- `SamplePattern` -- is genuinely CPU-side: `GetCurrentJitter`
    (`Renderer3D.cpp:6463`) picks the pattern and the shader only ever sees the
    resulting `u_Jitter`, so a pattern costs no extra pass and adds no shader
    branch. All six branches are present and were checked by transcribing them
    and evaluating the sequences over 16 frames against the documented unit
    contract (pixels, inside [-0.5, 0.5]):

    | pattern | distinct offsets | max abs offset | in bounds | cycle |
    |---|---|---|---|---|
    | Centre | 1 | 0.0000 | yes | 1 |
    | OrderedGrid | 4 | 0.2500 | yes | 4 |
    | Quincunx | 5 | 0.2500 | yes | 16 |
    | RotatedGrid | 16 | 0.3534 | yes | never repeats |
    | Halton | 16 | 0.4688 | yes | 16 |

    Every offset satisfies the contract -- **zero violations** -- and each pattern
    behaves as designed: OrderedGrid cycles over its 4 frames, Quincunx yields its
    5 offsets (four corners plus the centre), RotatedGrid's golden-angle
    re-rotation (`atan(1/2) + frameIndex * 2.3999632`) means it never settles into
    a fixed 4-phase recurrence the way a static rotation would, and Halton(2,3) is
    correctly centred on the pixel.

    The unit contract is the thing that matters here: the call site adds the
    result to `pixelCoords` BEFORE dividing by trace size, so an offset outside
    [-0.5, 0.5] would move the sample into a neighbouring pixel and silently
    corrupt the accumulation. Worth checking explicitly for any new pattern.

63. **CORRECTION -- the "fog off" A/B runs never disabled the clouds.** Every
    arm in this document that disables volumetrics used

    ```
    sed -i '39981s/true/false/; 40000s/true/false/' DemoGame.wastescene
    ```

    The two volume blocks are at lines **39979** (`VolumetricFogComponent`) and
    **39998** (`VolumetricCloudsComponent`), so their `Enabled` fields are at
    **39981** and **39999**. The first substitution is correct; the second targets
    line **40000**, which is the fog volume's `Color:` field -- not the clouds'
    `Enabled`. So every "fog OFF" arm in this document disabled the **fog volume
    only**, with `VolumetricCloudsComponent` still enabled and still marching.

    What this does and does not change:

    - **The headline numbers are unaffected.** The traversal-stack result
      (198.5 -> 30.3 ms, 6.5x) was measured with the volumes in their default
      state and does not depend on which volume was toggled.
    - **The "fog is 36.3 ms (17%)" figure is mislabelled.** It is the cost of the
      fog VOLUME with the clouds still rendering, not of both volumes. The
      clouds' share of that 36.3 ms has never been measured separately.
    - **The "fog is no longer a lever" conclusion is unaffected**, and if
      anything is strengthened: with the clouds still on, removing 4.4 ms of GPU
      moved the frame 0.4 ms, so the clouds being on does not change the finding
      that the frame is no longer GPU-bound.
    - **The cloud march has no `IsOccluded` in either twin** (established
      independently by counting call sites), so the clouds do not pay the
      per-pixel shadow-traversal cost that made the fog volume expensive. That
      is why the light-cap and range-cull results above are attributed to the
      fog volume specifically.

    The correct substitution, for anyone repeating the measurement, is
    `'39981s/true/false/; 39999s/true/false/'`.

64. **The split, done properly -- and the DRS was confound**

    Two things had to be fixed before the fog/clouds attribution could be
    measured at all.

    **(a) The substitution.** Corrected to `'39981s/true/false/; 39999s/true/false/'`
    (step 63).

    **(b) The DRS is a confound, and every post-fix A/B in this document was one.**
    The scaler adapts to the very quantity being measured: a faster arm is handed
    a higher `RenderScale` and can end up *slower* than the arm it beat. Measured
    directly -- with the fog volume disabled the frame went 20.6 -> 26.6 ms while
    the scale went 0.45 -> 0.57. The saving was more than eaten by the extra
    pixels the scaler bought with it.

    A `WL_DRS_OFF=1` hook was added to pin the scale. **Note the pre-fix fog A/B
    (216.7 vs 180.4 ms) was valid, but for a reason nobody checked at the time:**
    the scale was pinned at its 0.40 floor in both arms, so the scaler had nowhere
    to move. Post-fix runs are not safe the same way, because the scale is no
    longer saturated. Any future cost attribution in this engine must pin the
    scale first.

    The 2x2, with the scale pinned at 0.75 by `WL_DRS_OFF=1`, camera pinned,
    203 instances in all four arms:

    | arm | app (ms) | fps | cpuSubmit |
    |---|---|---|---|
    | A: both volumes on | 51.4 | 19.4 | 46.7 |
    | B: fog volume OFF, clouds ON | 43.7 | 22.9 | 39.0 |
    | C: fog volume ON, clouds OFF | 51.3 | 19.5 | 46.5 |
    | D: both OFF | 43.4 | 23.0 | 38.8 |

    **Fog volume: 7.7 ms (15% of the frame). Clouds: 0.1 ms -- free.**

    Three things fall out of this.

    - **The clouds cost nothing.** `VolumetricCloudsComponent` can be left enabled
      with no measurable frame-time consequence in this scene. That is consistent
      with the earlier finding that the cloud march has no `IsOccluded` in either
      twin: it never pays the per-pixel shadow traversal that makes the fog
      volume expensive.
    - **The earlier "fog is 36.3 ms (17%)" figure is withdrawn.** It was measured
      at the 0.40 scale floor, BEFORE the traversal-stack fix -- so it was
      inflated by the same bug that made the whole frame 216.7 ms. At a pinned
      0.75 scale, after the fix, the fog volume is 7.7 ms.
    - **`cpuSubmit` is NOT a CPU measure, and must not be read as "the frame is
      CPU-bound".** This bullet was written claiming the frame was CPU-bound at
      0.75 scale, on the strength of `cpuSubmit` reading 46.7 ms of a 51.4 ms
      frame. That is wrong, and the reason is in the instrumentation:
      `drsFlushStart` is taken at the top of the Nova block *before any GPU
      work* (`Renderer3D.cpp:4162`), and the timer is closed after the last
      dispatch. So the figure spans every `glDispatchCompute`, every
      `glMemoryBarrier` and every image bind in the Nova sequence -- and a GL
      dispatch call returns immediately only while the command queue has room.
      Once the GPU falls behind, the driver blocks the CPU inside those calls, so
      the "CPU submit" figure is largely **the wait for the GPU**, not work done
      by the CPU.

      This is the same phenomenon already established for `ImGuiLayer::End()`
      ("the GL driver throttling the CPU once the Nova render fills the command
      queue") -- it shows up wherever the frame happens to block, and
      `cpuSubmit` is just the place it blocks in the Nova path. The earlier
      profiler result is the corroborating evidence: with
      `WASTELAND_NOVA_PROFILE=1` the same field read ~212 ms against a 0.0 ms
      reading with it off, because the profiler's own `glGetQueryObject` stalls
      the CPU.

      **What is actually established:** at a pinned 0.75 scale the Nova block
      costs ~34 ms of wall time (51.4 ms frame vs 17.3 ms with Nova disabled),
      and it dominates the frame. Whether that time is spent on the CPU or the
      GPU **cannot be determined from `cpuSubmit`** -- and any claim either way
      that rests on it is unsupported. Separating the two needs a GPU profiler.

    This does not change the headline result -- the traversal-stack fix
    (198.5 -> 30.3 ms, 6.5x) was measured with the volumes in their default
    state and the scale saturated, so it is unaffected. It corrects the
    attribution of what the fog costs, which was the point of the exercise.

65b. **The project's `RendererAPI` field was parsed, serialized and never read.**
    `Project::Settings::RendererAPI` is an `int` with `-1` as the "never chose"
    sentinel, numbered exactly as `RendererAPI::API` (None=0, OpenGL=1, DX11=2,
    DX12=3, Vulkan=4). It was read only by the YAML parser
    (`Project.cpp:676`) and written only by the serializer (`Project.cpp:868`) --
    no logic consumed it, so `RendererAPI: 3` in `project.wasteland` was silently
    ignored on every backend while the file faithfully round-tripped a value that
    did nothing. Third instance of the same class of defect in this session (the
    undeclared `u_ReconstructionFilter`, the never-available GL query).

    Fixed in `EditorLayer::ApplyRenderSettings`: the field is read first, guarded
    on `>= 0` (unset keeps the process default) and on `!= current` (a project that
    already matches does not pay for a switch). `CaptureRenderSettings` writes it
    back ONLY when it was already set -- writing the current API unconditionally
    would turn an unset field into a real id on the first save, so "I never chose"
    would silently become "I chose whatever was running".

    Verified by replacing the field with `2` and running the editor: the log shows
    `project pins renderer API 2 (was 1) -- switching` followed by
    `Switching renderer API to 2`. The field is now honoured.

    Two things worth knowing for anyone repeating this:

    - **The project file already contains `RendererAPI: -1`.** Appending a second
      `RendererAPI:` key produces a duplicate that YAML resolves to the FIRST, so
      an appended value is silently ignored. The value must be replaced in place.
    - **`ApplyRenderSettings` runs twice at startup**, so the switch is logged
      twice. Harmless -- the second call switches to the same API -- but it means
      the guard is not sufficient to make the operation once-only, because
      `GetAPI()` still reports the pre-switch value on the second call (the switch
      is asynchronous).

65. **Instrumentation flaw found and fixed: the perf log was behind the DRS gate.**
    While running the above, `WL_DRS_OFF=1` produced no `Frame time:` lines at
    all. The periodic report lived at the bottom of `UpdateDynamicResolution`,
    *after* its `if (!DynamicResolutionEnabled) return;` -- so the only way to
    read `app` was to leave running the very scaler that confounds the
    measurement. A diagnostic disabled by the thing it is diagnosing is not
    usable.

    The report moved above that early return and onto a struct member
    (`FrameTimeLogCount`) so it fires regardless of the scaler's state. Verified:
    with `WL_DRS_OFF=1` the report now appears and the scale stays pinned at 0.75
    in all four arms.

59. **Warmup gate applied and measured.** The scaler now refuses to act until
    the app EMA has absorbed 30 samples:

    ```cpp
    if (s_Data.AppFrameSamples < 30)
        return;
    ```

    placed after the every-10-frames interval check in `UpdateDynamicResolution`.
    30 samples is roughly one 0.95-blend time constant (1/0.05), where the EMA
    has absorbed ~95% of the seed's influence.

    | | seed clamp only | seed clamp + warmup gate |
    |---|---|---|
    | DRS readings | 67.2 -> 55.0 -> 41.6 | **56.9 -> 50.7 -> 41.9** |
    | scale trajectory | 0.75 -> 0.55 -> 0.43 -> 0.40 | **0.75 -> 0.63 -> 0.51 -> 0.45** |
    | final scale | 0.40 (floor) | **0.45** |
    | frame time | 21.8 / 24.1 ms | **21.0 / 20.0 ms** |
    | fps | 42-46 | **47.6 / 50.0** |

    The final scale moved off the floor, which is **27% more pixels** for the
    same frame time -- the quality win this whole line of work was for.

    **Still not fully correct, and the limit of what is claimed.** The scaler
    still steps DOWN during startup (0.75 -> 0.63 -> 0.51 -> 0.45) rather than
    holding, because at frame 30 the EMA is still ~57 ms against a true frame
    time of ~20 ms. The gate delays the step-down; it does not prevent it.

60. **The decay anomaly is EXPLAINED, and the gate was raised to 100.** The
    diagnostic that settles it prints the sample counts next to the reading:

    ```
    DRS adjust: ema 66.85 ms  appSamples 10  drsGpuSamples 1
    DRS adjust: ema 60.72 ms  appSamples 20  drsGpuSamples 1
    DRS adjust: ema 56.68 ms  appSamples 30  drsGpuSamples 1
    DRS adjust: ema 50.52 ms  appSamples 40  drsGpuSamples 1
    DRS adjust: ema 41.82 ms  appSamples 50  drsGpuSamples 1
    DRS adjust: ema 33.80 ms  appSamples 60  drsGpuSamples 1
    DRS adjust: ema 28.20 ms  appSamples 70  drsGpuSamples 1
    DRS adjust: ema 24.88 ms  appSamples 80  drsGpuSamples 1
    ```

    `appSamples` advances by exactly 10 per adjustment and the adjustments are
    10 frames apart, so `NotifyFrameTime` **is** called once per frame. The
    earlier estimate of a 0.99/frame decay was wrong: the EMA is decaying at the
    specified 0.95, and it is high because the INCOMING samples are high --
    the startup frames really do cost 60-90 ms while shaders compile, and the
    EMA is correctly tracking them. There is no blend bug. The seed clamp was
    the right fix and the residual is simply that 30 samples is not enough for
    the EMA to forget a genuinely slow startup.

    The gate was therefore raised from 30 to **100** samples (3.3 s at 30 fps),
    chosen because the measured EMA reaches 24.88 ms -- within ~4 ms of the
    ~21 ms steady state -- at sample 80.

    | | gate 30 | gate 100 |
    |---|---|---|
    | scale trajectory | 0.75 -> 0.63 -> 0.51 -> 0.45 | 0.75 -> 0.63 -> 0.51 -> 0.45 -> **0.50** |
    | final scale | 0.45 | **0.50** (recovered) |
    | frame time | 21.0 / 20.0 ms | 20.5 / 20.0 / 23.8 ms |
    | fps | 47.6 / 50.0 | 48.7 / 49.9 / 42.1 |

    The scale now steps back UP off the floor once the EMA converges, which is
    the behaviour the scaler was supposed to have all along.

    **The residual, stated plainly.** The EMA at frame 100 read 51.4 ms in this
    run and 24.88 ms at sample 80 in the previous one -- the same code, the same
    scene, a 2x difference. Startup cost varies run to run (shader compilation,
    PSO creation, BVH build), so **no fixed sample count can be robust**: a gate
    tuned to one run's startup is wrong for another's. The correct fix is to stop
    using a single seeded EMA as the scaler's only signal -- a short window, a
    median, or gating on the variance rather than the count would all be robust
    to a variable startup. That is a design change, not a constant, and it is not
    made here.

    What this redirects: the per-pixel traversal-plus-lighting term and the fog
    march were both per-pixel. They have since been resolved -- see step 53 for
    the traversal-stack fix (which was per-pixel by construction, and which
    removed 168 ms) and step 52 for the volumetric range cull (25 ms).
    The measurements worth taking next are inside `RunTraceAndDenoise`, where
    `ApplyPOM`'s ladder search and the analytic primitive evaluation are the
    documented suspects, and in the editor loop, where ~11 ms of the frame now
    sits outside the renderer entirely.

**Measurement caveat — the editor camera is not deterministic across runs.**
`EditorLayer` builds `m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 10000.0f)`
with `SetZoomLevel(5.0f)` and no persisted transform, then runs
`m_CameraController.OnUpdate(ts)` every frame. The camera therefore moves during a
run and settles differently each time: one baseline run reported instance counts
of 612, 862 and 825 within a single run, and the 64 px run settled at a stable 203.

**Resolved.** The camera is not merely noisy, it drifts between runs: an unpinned
run reported `cam (0.0,0.0,19.0)` where the constructor default is `z = 10`. That
single difference is worth ~24 ms (235.6 ms vs 211.3 ms gpu in the same binary),
which is why every earlier cross-run comparison in this document was unreliable.
The "203 instances" anomaly was the same cause -- that run had the camera at the
default `z = 10`, matching a pinned run at `z = 10` exactly.

**Fixed by `WASTELAND_PIN_CAMERA=<fx,fy,fz>,<distance>,<pitch>,<yaw>`** (EditorLayer
`OnProjectChanged`, plus `SetFocalPoint`/`SetPitch`/`SetYaw` on `EditorCamera` and
an `m_CameraPinned` flag that skips `EditorCamera::OnUpdate`). The camera is now
logged in the frame-time line, so any future cross-run comparison can be checked
before it is believed. `WASTELAND_CPU_CULL=0` gates the frustum/distance cull for
the same purpose.

**The corrected A/B — pinned camera, same binary, cull the only difference.**

| Arm | instances | gpu (EMA) | imguiEnd | app (frame time) |
|---|---|---|---|---|
| A: `WASTELAND_CPU_CULL=0` | 211 | 243.5 | 194.1 | **213.1** |
| B: cull on | 203 | 210.9 | 195.2 | **214.2** |

**The CPU cull does not improve the frame rate.** It moved the frame time by
+1.1 ms (the wrong direction, and inside noise) and `imguiEnd` by 1.1 ms. The GPU
EMA moved 32.6 ms, but the GPU EMA is not a frame-cost measure: in arm A it read
243.5 ms while the frame took 213.1 ms, which is impossible if the frame waits for
the GPU. **The earlier "41.8 ms / 16.5%" figure is withdrawn.** The only measure
that has tracked reality throughout this investigation is the wall-clock frame
time (`app`), taken with `WASTELAND_NOVA_PROFILE` off.

The mechanism claim in the previous revision -- that traversal cost tracks ray
coverage rather than instance count -- may still be true of the GPU work, but it
does not translate into frame time here, so it is not a lever.

**Volumetric march step cap, measured the same way.** `WASTELAND_VOL_STEPS=<n>`
(uniform `u_VolStepCap`, both twins) exposes the march's step bound, which was
hardcoded 32.

| Arm | app |
|---|---|
| 32 steps (the old hardcoded bound) | **216.9** |
| 16 steps | **213.6** |

**Halving the march saves 3.3 ms, not the 17 ms the GPU EMA suggested.** The step
count is therefore not where the fog cost lives -- the march's per-pixel fixed
cost dominates it.

**The fog volumes are the largest lever found, and it is a real one.** With the
camera pinned and no profiler, disabling the scene's two volume components
(`VolumetricFogComponent` and `VolumetricCloudsComponent` in DemoGame) moves:

| Arm | imguiEnd | app |
|---|---|---|
| fog + clouds on | 198.7 | **216.7** |
| fog + clouds off | 162.7 | **180.4** |

**36.3 ms, 17% of the frame, and it moves the app time.** This is the only
change found so far that improves the frame rate.

**The Nova render is the whole problem, and the rest is nearly free.** With
`RayTracingEnabled: false` the same scene, same camera and same 4K viewport runs
at:

| Arm | imguiEnd | app |
|---|---|---|
| Nova off | 10.8 | **17.3** |
| Nova on | 198.7 | **216.7** |

So the Nova path costs **199 ms** of a 217 ms frame, and everything else in the
editor -- scene update, ImGui draw, window pump, present -- totals **17 ms**. The
remaining ~163 ms after removing the fog is the trace pass.

**Why the frame waits in `imguiEnd`.** `m_ImGuiLayer->End()` for OpenGL is a
single call, `ImGui_ImplOpenGL3_RenderDrawData`, and the draw data is tiny: 6
command lists, 5,758 vertices, 9,327 indices -- under a millisecond of actual
drawing. It nonetheless holds ~195 ms because the GL driver throttles the CPU
once the Nova render has filled the command queue: the next GL call after the
scene submission is the ImGui draw, so the block lands there. That is why
`imguiEnd` tracks the Nova GPU work and why `update` -- which only submits --
reads 2.5 ms.

66. **The procedural primitives rendered nothing at all: `RayTracingInstance::MaxDistance`
    was never assigned on that one submission path.** Reported as "the procedural meshes no
    longer render at all". They had not merely degraded -- four shape types (`DrawPlane`,
    `DrawCylinder`, `DrawCone`, `DrawTorus`, all routed through
    `DrawProceduralPrimitive`) produced no visible geometry, while cubes, spheres, water and
    terrain were unaffected.

    **The mechanism, in three links.** Every submission path builds its instance with
    `RayTracingInstance instance = {};`, and the struct field had **no default
    initializer**, so value-initialization set it to exactly `0.0f`. `DrawProceduralPrimitive`
    never assigned it (the other four paths each do). Then:

    1. `CullInstanceForTrace` tests `if (camDist >= instance.MaxDistance) return false;`.
       With `MaxDistance == 0.0f` that is `camDist >= 0.0f` -- **unconditionally true**, so
       not one instance from that path was ever pushed into `m_SceneInstances`.
    2. Even had it been pushed, both shader twins reject it: `TestInstanceHit` (glsl:3152,
       hlsl:2109) and `TestInstanceOcclusion` (glsl:1669, hlsl:1240) both do
       `if (dot(camToInstance, camToInstance) > inst.MaxDistance * inst.MaxDistance)`,
       which at `MaxDistance == 0.0f` is `> 0.0` -- true for anything not exactly at the eye.

    So the geometry was dropped on the CPU before the BVH ever saw it, and would have been
    dropped by the shader as well. Two independent rejections, both from the same zero.

    **Not a regression from this session's work, but it was invisible to every check used
    so far.** At HEAD the same function culled with `camDist < s_Data.MaxDrawDistance` and
    likewise never assigned the field, so the zero predates the refactor. The refactor did
    not introduce it; it did make the field load-bearing on the CPU side for the first time.

    **The fix.** A single named constant, declared before the struct so the field can
    default to it, with `MaxDrawDistance` now *derived* from it rather than written as a
    second literal:

    ```cpp
    static constexpr float TraceMaxDistance = 10000.0f;   // before RayTracingInstance
    ...
    float MaxDistance = TraceMaxDistance;                 // in the struct
    ...
    static constexpr float MaxDrawDistance = TraceMaxDistance;  // in Renderer3DData
    ```

    The comment already on the field said "KEEP THIS IN STEP WITH MaxDrawDistance below" --
    the instruction was there and the field was still missed on one of five paths. A rule
    that a human has to remember is not a fix; deriving one from the other is.

    **Swept the whole struct for the same defect class.** Every field of
    `RayTracingInstance` across all five `= {}` construction sites: `WaveParams` and the
    four `Terrain*` fields are legitimately zero on the non-water / non-terrain paths, and
    `LODLevel` is assigned centrally by `CullInstanceForTrace`. **`MaxDistance` was the only
    field that was both unassigned on some path and consumed as a rejection threshold.**

    **Measured, with a purpose-built scene and a deliberate A/B.** A scene containing only
    the four procedural shapes plus a ground cube (so nothing else can be mistaken for
    them), camera pinned at the same place in both arms:

    | Arm | instances submitted | `TRACE DEFECT` log |
    |---|---|---|
    | `MaxDistance = 0.0f` (the bug, rebuilt) | **1** | **`TRACE DEFECT: 4 instance(s) submitted with MaxDistance <= 0`** |
    | `MaxDistance = TraceMaxDistance` (fixed) | **5** | *absent* |

    The four dropped instances are exactly the four procedural shapes; only the ground cube
    survived. **Visually:** the bug arm renders an empty viewport -- ground and sky, zero
    objects -- while the fixed arm shows the red cylinder (parallel straight sides, flat
    circular top), the magenta cone (tapering to a point) and the green torus.

    **A diagnostic was added with the fix, because the bug was silent.** `CullInstanceForTrace`
    now counts instances arriving with `MaxDistance <= 0` into
    `Statistics::ZeroMaxDistanceInstances`, reset per frame by `ResetStats()`, and the
    periodic report emits `TRACE DEFECT: N instance(s) submitted with MaxDistance <= 0` when
    it is non-zero. Any future path that forgets the assignment now announces itself in the
    log instead of quietly rendering nothing. **It fired with exactly 4 during the bug arm
    above, which is what makes that arm self-verifying rather than inferred.**

    **The general rule.** `= {}` plus a `>= 0`-style threshold comparison is a silent cull:
    value-initialization is not "leave it alone", it is "set it to a value that usually
    fails the test". Any struct field that a shader or a cull treats as a *limit* needs
    either a default initializer or an assignment on every construction path -- and the
    assignment form has now been missed once.



## 5. The vertex question, closed

Asked directly: *is the cost the number of vertices being rendered?* **No — and
on the Nova path the question cannot even arise, because nothing is rasterized.**

`Renderer3D::EndScene` gates the whole raster fallback behind
`if (s_Data.RayTracingEnabled)` (`Renderer3D.cpp:3842`), and the Nova branch
`return`s at `4544` before the two `DrawIndexed` calls at `4585`/`4598` are
reached. With Nova active — which is every measurement in this document — those
draw calls do not execute. There is no vertex shader, no vertex fetch and no
index decode in the frame.

The depth the temporal pass consumes is not rasterized either: `s_DepthBuffer`
(`glsl:55`) is a sampler bound to **binding 4**, which the shader shares with the
image `img_AlbedoHit` (`glsl:78`) — an image the tracer writes, not a depth
buffer a raster pass produces.

Two independent confirmations:

- The compute chain, which contains **zero** vertex work, accounts for the entire
  frame: 245.7 ms of a ~244 ms frame. There is no headroom for a vertex cost.
- Pass 0, the only pass that reads depth, is a 1.1 ms reprojection — it samples
  the depth image and reprojects (`glsl:2231-2271`); it performs no intersection
  at all.

**The consequence for the Nanite question.** High polygon counts are already free
in the traced path: the tracer never sees tessellation, only analytic proxies.
The 1024 cubes and 678 spheres cost the tracer nothing per-vertex — they cost one
BVH node visit each. So the desire to keep polygon counts high is not in conflict
with performance here; the conflict is entirely in the ray work.


**5.11.4 Hardware multisampling — rejected, with reasons.**

| Method | Verdict |
|---|---|
| MSAA 2x/4x/8x | **Reject.** The only rasterised output is depth and velocity (pass 0); the image is produced by a compute ray tracer. There is nothing for MSAA to resolve. `VK_SAMPLE_COUNT_1_BIT` is correct as-is. |
| CSAA / EQAA | **Reject.** NVIDIA/AMD coverage-sample extensions. Vendor-specific, driver-dependent, and meaningless for a compute shader. |
| SGSSAA | **Reject.** Driver-forced supersampling. Not controllable from the engine, and it would fight the DRS scaler. |

**5.11.5 The classic temporal variant.**

46. TAA in 5.2 uses a *running average* (`max(0.02, 1/(FrameIndex+1))`). The
    original TAA formulation uses a **fixed alpha**. Expose both: fixed alpha is
    what the early papers used and is more predictable, running average converges
    faster on the first frames. This is a one-line change to `u_AccumulationAlpha`
    behind a flag.

**5.11.6 The `None` option matters.**

47. `ReconstructionFilter::None` — raw output at `RenderScale 1.0`, no upscale, no
    accumulation — is not just a degenerate case. It is the correct baseline for
    benchmarking (it isolates the tracer from the AA cost) and the correct choice
    for anyone comparing AA methods. Make it a first-class option rather than an
    unset checkbox.

**5.11.7 Verification cost scales with the method count.**

48. Every added method multiplies the twin-parity surface. Extend the 5.9
    method-parity test to iterate over the full `SamplePattern` x
    `ReconstructionFilter` cross-product and assert that each combination
    resolves to the same pass IDs in both twins. Without this, the likely failure
    is a method that works on DX12 and silently no-ops on OpenGL — which is
    precisely the fog-light bug this document started from.

---

## 6. Honest limits

- The traversal counts come from the shaders' loop bounds and the scene file,
  not from a profiler. The **ratio** (3.98x GL/DX) is what the code's own
  arithmetic predicts and it matches the observed ~5x; the absolute FPS numbers
  are estimates.
- I have not run the engine, so I cannot report the current FPS or confirm the
  overflow flush fires in your build. Phase 1 step 6 exists to settle that.
- The GLSL private-memory stack (2a) is real but I have not measured its share
  of the GL/DX gap. If Phase 0 brings GL and DX to parity, 2a was minor; if a
  gap remains, 2a is the next thing to attack.
- Phase 5 is a plan, not a design review. The AA inventory in 5.0 is read from
  the source; the cost estimates for FSR1/TSR (5.3, 5.4, 5.10) are reasoned, not
  measured. In particular, whether a spatial upscaler actually permits a lower
  `DynamicMinScale` than bilinear is an empirical question that can only be
  settled by measuring both at the same target FPS.
- The legacy set in 5.11 widens the surface faster than it widens the benefit.
  Six sample patterns and twelve filters is a cross-product of 72 combinations,
  most of which nobody will use. The value is in the *fallback* cases (FXAA for
  old GPUs, `None` for benchmarking, Halton for comparison) and in the pattern
  axis being a CPU-side choice that costs no shader work. Do not implement all
  72; implement the axes, then only the combinations that have a named use.
  Step 48's parity test is what makes that safe to leave incomplete.
