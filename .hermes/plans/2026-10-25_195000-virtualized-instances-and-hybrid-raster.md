# Virtualized Instances + Hybrid Primary Visibility — Implementation Plan

> ## ⚠️ SUPERSEDED — DO NOT IMPLEMENT THIS PLAN
>
> **Superseded 2026-10-26 by `.hermes/plans/2026-10-26_020000-triangle-mesh-geometry-pipeline.md`.**
>
> The product owner confirmed that **arbitrary triangle-mesh support is a requirement**. This
> plan was written to scale *analytic primitives*, and Nova has no triangles at all — its own
> §11 pre-committed to this outcome: *"Is arbitrary triangle-mesh support a product requirement?
> If yes, this plan is the wrong shape and a geometry pipeline should be planned instead."*
>
> Every phase below is dead, each for a measured reason:
>
> | phase | killed by |
> |---|---|
> | 1 — secondary LOD | secondary work is 0.5 ms; implemented and measured at zero |
> | 2 — two-level BVH | cost is pixel-bound; traversal ordering measured 0.0 ms |
> | 3 — GPU cull | the 24,576 cap does not bind — 16,041 instances in a gameplay view |
> | 4 — residency | same: nothing to page |
> | 5 — hybrid raster | was incoherent without meshes; now the *correct* direction, and is the basis of the replacement plan |
>
> **Keep this document for its measurements, not its plan.** Sections 1a onward record how the
> frame was actually attributed — the three broken GPU timers, the stale-exe measurement
> failure, the ablation results, the noise floor, and the per-preset DRS decision. Those are
> still the constraints the replacement plan must respect, and they are summarised in its §5.
> The phased sections (§2–§8) are historical and must not be built.

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Break Nova's 24,576-instance ceiling by giving it a two-level acceleration structure with GPU-driven LOD selection and residency, so instance count becomes a streaming budget instead of a hard truncation — and, separately, offer a compute-raster primary-visibility path that removes the per-pixel primary trace.

**Architecture:** Nova today traces analytic primitives against a *single-level* BVH over instance AABBs. This plan adds a second level (clusters of instances, each with its own AABB), moves culling and LOD selection onto the GPU in a compute pass that writes a compacted indirect instance list, and pages instances by screen-space importance. Phase 5 is a separate, optional raster path for primary visibility only.

**Tech Stack:** C++20, entt 3.16.0, OpenGL compute (existing `glDispatchCompute` path) + NVRHI parity, GLSL and HLSL shader twins, yaml-cpp scene format.

---

## 1. Current context (measured, not assumed)

These are the facts this plan is built on. Re-verify anything marked *verify* before relying on it.

| Fact | Value | Where |
|---|---|---|
| Instance cap | `MaxRayTracingInstances = 24576` | `Renderer3D.cpp:1279` |
| Behaviour at cap | list saturates; extra instances silently dropped | `Renderer3D.cpp:4479` |
| BVH type | single-level TLAS over **instance AABBs** | `BVHPrim` `Renderer3D.cpp:194` |
| BVH build | recursive median split on centroid, longest axis | `Renderer3D.cpp:461-610` |
| GPU node | `alignas(16)`, two `vec4`, 32 B; `w` = child or instance range | `BVHNodeGPU` `Renderer3D.cpp:165` |
| Instance record | `RayTracingInstance`, 288 B, matches shader ABI | `static_assert` `Renderer3D.cpp:159` |
| Geometry | **8 analytic shapes**, no triangles, no UVs | `PrimitiveShape` `ProceduralMeshes.h:136` |
| Intersection | `HitPrimitive` switches on shape ordinal, closed form | `.glsl:1726` |
| `LODLevel` | computed on CPU, **read by no shader** | set `Renderer3D.cpp:1730-1735`, declared `.glsl:160` |
| LOD bands | 500 m / 2000 m | `Renderer3D.cpp:674-675` |
| CPU cull | frustum + distance, applied at all 5 append sites | `Renderer3D.cpp:1712`, sites `6260,7462,7678,7969,8038` |
| Cull effectiveness | 10k entities → 250 instances (97.5% rejected) | `tools/probe_screen_cull.py` |
| Screen cull | 0 dropped at 2 px; 23% at 8 px (fidelity cost) | same |
| `screenCulled` logging | **was always 0** (logged before cull); fixed via `LastFrameScreenCulled` | `Renderer3D.cpp` end of `Flush` |

**Scope note that shapes everything below:** Nova has no triangle geometry, so Nanite-style *geometry* virtualization does not apply. The analogue here is *instance* virtualization. Do not build a triangle pipeline unless arbitrary meshes become a product requirement.

## 1a. Phase 0 result — the gate PASSED after three timer defects were fixed

The harness (`tools/bench_nova_phase.py`) now returns valid rows:

```
 count  preset   app ms    fps  gpu ms  cpu ms  instances  culled
 1000       3    113.7    8.8    17.3    91.4         31       0
10000       3    158.5    6.3    13.1   122.1        250       0
100000      3    178.2    5.6    14.7   106.0      24576       0
EXIT=0
```

### The three defects that made `gpu` read 0.0

1. **Begin starvation.** A failed `glBeginQuery(GL_TIME_ELAPSED)` leaves the *first* query
   active forever — GL rejects a second active query and does not close the previous one. The
   failure handler assumed the slot "never started" and closed nothing, so one orphaned query
   starved every later begin: 1 failure, then +100 per report interval forever. Fixed by
   force-closing the active query in the failure path. Failures dropped 301 → 2.

2. **Read index off by one.** The poll read `(DRSTimerIndex + 2) % 3`, which is *one* frame
   back, while the comment above it claimed two. The GPU queue on this renderer runs deeper
   than one frame, so the query was never complete when polled. Fixed to
   `(index + pool - 2) % pool`. The code contradicted its own comment — the comment was right,
   the arithmetic was wrong.

3. **The end query was not running.** Instrumented lifecycle counters showed
   `begins 295 ends 0` — the query was opened 295 times and never closed, so it could never
   complete. After the fix, `begins 295 ends 295`. (Pool depth was also raised 3 → 8; that
   alone changed nothing, which is how defect 3 was isolated.)

### Measurement-integrity failure, found the same day

**The harness launches `DemonCore-Editor.exe`, but `WastelandPython.vcxproj` only rebuilds
`Wasteland.pyd`.** The exe was stale from 12:58 while I reported `gpu 0.0` as an engine
limitation. Building `DemonCore-Editor/DemonCore-Editor.vcxproj` is required for any C++-side
change to be measured. Every render measurement taken between those two builds was against old
code. Two further harness fixes followed from this: `WL_DRS_DEBUG` was being stripped by the
`WL_`/`WASTELAND_` env filter (now allowlisted), and the frame log now carries
`begins`/`ends`/`endSite` counters.

### What the numbers say — the tracer is PIXEL-bound, not instance-bound

Two independent ladders, same build:

**Instance count, scale 1.00 — flat:**

| | 31 instances | 250 | 24,576 |
|---|---|---|---|
| app | 113.7 ms | 158.5 ms | 178.2 ms |
| gpu | 17.3 ms | 13.1 ms | 14.7 ms |
| cpuSubmit | 91.4 ms | 122.1 ms | 106.0 ms |

**Render scale, 250 instances — scales:**

| | scale 1.00 | scale 0.50 | ratio |
|---|---|---|---|
| app | 162.4 ms | 68.6 ms | 0.42 |
| gpu | 13.4 ms | 4.9 ms | 0.37 |
| cpuSubmit | 124.9 ms | 47.5 ms | 0.38 |
| fps | 6.2 | 14.6 | 2.35x |

`gpu` and `cpuSubmit` fall by the **same factor** (0.37 / 0.38). `cpuSubmit` is not independent
CPU work — it is the wall time of a block that is tracking GPU progress, so the flush is
submission/GPU-bound rather than doing 100 ms of its own computation. (The field's name is
misleading; treat it as "time the flush block held the thread", not "CPU cycles".)

**Conclusion:** at 24,576 instances the cost is the same as at 31, and halving the resolution
nearly doubles the frame rate. The tracer is **pixel-bound**. The `MaxRayTracingInstances =
24576` cap is real but it is not what the frame is paying for.

### Clean scale sweep — the fixed cost is ~33 ms, and the trace is not the frame

DRS off, same scene, 250 instances:

| scale | app | gpu | cpuSubmit |
|---|---|---|---|
| 1.00 | 157.5 ms | 25.4 ms | 0.1 ms |
| 0.80 | 115.5 ms | 9.2 ms | 86.6 ms |
| 0.66 | 91.8 ms | 7.0 ms | 66.7 ms |
| 0.50 | 64.5 ms | 9.0 ms | 5.4 ms |
| 0.40 | 52.8 ms | 6.9 ms | 0.1 ms |

A least-squares fit of `app` against `scale^2` gives

```
app ~= 32.9 ms  +  124.6 ms * scale^2      (residuals < 3 ms)
```

So the frame splits into **~33 ms that does not depend on resolution** and **~125 ms that
does**. Two consequences:

1. **The ~33 ms fixed cost caps the frame rate no matter how far the scale drops.** At the
   absolute clamp floor (0.25) the frame is still ~41 ms, i.e. ~24 fps. The 30 fps target is
   unreachable for this scene by scaling alone.
2. **`cpuSubmit` collapsed from ~120 ms to ~0.1 ms.** That is the epoch short-circuit
   (`SceneInstancesChanged`) working: on a static scene the flush now does almost nothing.
   The BVH is rebuilt 3 times in 300 frames (`bvhRebuilds 3`, `epochShort 298`), so it is not
   the cost. This is a real CPU win from the in-flight epoch work.

### The unexplained gap — where the next measurement must go

At scale 1.00 the frame is 157.5 ms but the GL query reports only 25.4 ms, and the CPU flush
is ~0.1 ms. **~130 ms of the frame is outside the query window.** The query opens at the trace
begin and closes before the composite, so it excludes the composite, bloom, god-ray and
neural passes, and everything after `Flush` — present, swap, and the editor UI.

That gap is the next thing to measure. It is not instance count, not the trace, and not the
CPU flush.

### The cost is entirely AFTER EditorLayer::OnUpdate

Instrumented split (10,000 entities, scale 1.00):

```
Frame split: onUpdate 0.0 ms  frame 145.5 ms  (difference is present/swap/UI-after)
Frame split: onUpdate 0.0 ms  frame 541.6 ms
Frame time:  app 163.4ms (6.1 fps)  gpu 13.6ms  cpuSubmit 125.0ms
```

`OnUpdate` returns in ~0 ms. **The whole frame cost is in the phase after it** — the ImGui
viewport render, the present, and the swap. The 3D trace is *submitted* during OnUpdate but
executed asynchronously, so the GPU work lands in that later phase and the present waits for
it.

This also explains why the GL query under-reports: it brackets only the trace dispatch, while
the composite, bloom and present sit outside it.

Note `ts` reads 541.6 ms while `app` reads 163.4 ms — `app` is a heavily smoothed EMA, so the
raw frame delta is far worse than the reported number suggests.

Next instrument: time the phases *after* OnUpdate (ImGui render, present, swap) to separate
"the editor UI is expensive at 4K" from "the present is waiting on the GPU".

### ANSWER: the present/swap is 94% of the frame

The engine already ships this instrumentation -- `WL_FRAME_TIMING=1` -- so no rebuild was
needed:

```
Frame sections (ms/frame): begin 0.0  update 1.0  imguiBegin 0.2  imguiDraw 4.7  imguiEnd 0.2  window 137.3  total 143.4
Frame sections (ms/frame): begin 0.0  update 4.6  imguiBegin 0.1  imguiDraw 3.4  imguiEnd 0.2  window 134.9  total 143.2
```

| section | ms/frame | share |
|---|---|---|
| begin | 0.0 | - |
| update (scene) | 1.0 - 4.6 | ~2% |
| imguiBegin | 0.1 - 0.2 | - |
| imguiDraw | 3.4 - 4.7 | ~3% |
| imguiEnd | 0.2 - 0.3 | - |
| **window (present/swap)** | **134.9 - 137.3** | **~94%** |

`m_Window->OnUpdate()` -- which ends in `SwapBuffers` -- is 135 ms of a 143 ms frame. The CPU
is parked in the swap waiting for the GPU.

### The contradiction this exposes

The GL query reports 24.9 ms of GPU work, but the swap waits 135 ms. **~110 ms of GPU work is
outside the query window.** The query brackets only the trace dispatch; the composite, bloom
and neural passes run after it, and all three dispatch on `RayTracingWidth/Height` --
**full window resolution, ignoring `RenderScale`** (composite at :5163, bloom at :5173,
neural at :5252).

That is the actionable finding: those passes do not get cheaper when the DRS scales the trace
down, so they put a floor under the frame that the scaler cannot remove. It is also why the
DRS settles at its 0.40 floor and still only reaches ~18 fps.

Next step: confirm by timing the composite/bloom group separately, then decide whether they
should dispatch on `ComputeWidth/Height` instead.

### The stall MOVES depending on where the CPU is asked to wait

Same scene, same scale, two builds:

```
before:  Frame sections: begin 0.0  update 1.0   imguiBegin 0.2  imguiDraw 4.7  imguiEnd 0.2  window 137.3  total 143.4
after:   Frame sections: begin 0.0  update 137.0 imguiBegin 0.1  imguiDraw 3.3  imguiEnd 0.2  window 0.1    total 140.6
```

Adding a second query read-back moved the ~137 ms stall from the swap to inside the flush.
The total did not change and the trace query still reads 13.0 ms.

**Interpretation:** the ~127 ms is GPU work, and it surfaces as a stall wherever the driver
happens to need the CPU to wait -- at `SwapBuffers`, or at a query poll inside the flush. The
location is not stable, so "window is 94% of the frame" from the previous section is a real
observation about *where the wait landed that run*, not a stable attribution.

### What is NOT yet proven

The post-query timer I added to bracket composite -> bloom -> god-ray -> neural reports
`post 0.0ms` -- it never produced a sample, so **the ~127 ms cannot yet be attributed to those
passes.** The hypothesis (they dispatch on `RayTracingWidth/Height`, ignoring `RenderScale`)
is still a hypothesis. Confirming it needs the post-query read-back to actually complete, or a
different attribution method.

### What is actually running outside the query window

Checked the defaults of every pass that runs after the main query closes:

| pass | default | outside query window? |
|---|---|---|
| SVGF denoiser | `SvfgEnabled = false` | yes, but OFF |
| composite (pass 6) | **always runs** | yes, **ON** |
| bloom (passes 4,5) | `BloomEnabled = false` | yes, but OFF |
| god rays (pass 10) | `GodRaysEnabled = false` | yes, but OFF |
| neural (passes 17-19) | `NeuralStageEnabled = false` | yes, but OFF |

**The composite is the only active pass outside the window.** It is unconditional and
dispatches on `RayTracingWidth/Height` -- full window resolution, deliberately, because it is
what upscales the reduced-res accumulation buffer to the output (`:5193-5205`).

### The contradiction this leaves

The composite is a full-resolution pass, so it should be **constant** as `RenderScale` changes.
But the measured frame scales strongly with `scale^2` (124.6 ms * scale^2 from the sweep).
A constant pass cannot produce a scaling curve.

So the dominant cost is in the **compute-resolution** passes -- i.e. the trace -- yet the trace
query reads 13.0 ms against a 140 ms frame. Either the query is not measuring what it appears
to, or the compute passes cost far more than the query reports.

That is the open question. The next measurement should validate the GL timer against a known
workload before trusting it to attribute anything: run the same scene at two render scales and
check that the query tracks the known pixel-count ratio. If it does not, the timer is the
problem and every GPU attribution in this document is suspect.

### RESOLVED: the trace is 88% of the frame, and the earlier readings were a mismeasurement

A `glFinish`-bounded probe settles it. `glFinish` cannot lie -- it returns only when the driver
has drained every queued command -- and forcing the drain also makes the GL queries complete
properly, which they never did on their own:

```
gpu 131.3ms   post 8.2ms   finish 139.7-146.1ms   app 149.4ms
Frame sections: update 141.5  imguiBegin 0.1  imguiDraw 3.4  imguiEnd 0.1  window 0.1  total 145.2
```

| component | ms | share |
|---|---|---|
| trace (main query) | 131.3 | 88% |
| composite / post query | 8.2 | 5% |
| ImGui | 3.4 | 2% |
| **glFinish total** | **139.7 - 146.1** | **100%** |

**The trace is 88% of the frame. The GPU is the bottleneck. It is pixel-bound.**

### Why every earlier number was wrong

The queries were never completing. `GL_QUERY_RESULT_AVAILABLE` returned false, the poll skipped
the sample, and the EMA stayed at whatever it last held -- which is why `gpu` read 0.0 for most
of a run, and 13 ms when it read anything. `glFinish` drains the queue, so the queries complete
and report the real 131 ms. The "127 ms outside the query window" was an artefact of the query
under-reporting, not a real gap. Likewise "the composite is the problem" was wrong: it is 8.2 ms.

The engine's own four-segment Nova profiler has the same defect -- it reported
`visibility 1.0 trace 0.0 accum 0.0 composite 0.0` against a 146 ms frame. Any GPU attribution
made on this path without a `glFinish` (or an external profiler) is unreliable.

### What is now established

- Frame ~149 ms at 4K, scale 1.00, 250 instances.
- **Trace: 131 ms (88%).** GPU-bound, pixel-proportional.
- Instance count is not in the cost curve (31 -> 24,576 instances is flat).
- Composite 8.2 ms, ImGui 3.4 ms, scene update ~1-5 ms, BVH rebuilt 3x per 300 frames.
- The DRS is the correct lever and already works; it floors at `DynamicMinScale = 0.4` and
  reaches ~18 fps against a 30 fps target.

### Consequence for the plan

The lever is **per-pixel trace cost**, exactly as Phase 5 assumed. The options, in order of
effort:

1. **Lower the DRS floor** below 0.4 (currently `DynamicMinScale = 0.4f`). Cheapest possible
   change; the fit says the frame at scale 0.25 is still ~41 ms, so it will not reach 30 fps
   alone, but it is one constant.
2. **Reduce per-pixel secondary work** (Phase 1, `TestInstanceHit`'s `isPrimaryRay` hook) --
   small, already-hooked.
3. **Hybrid primary visibility** (Phase 5) -- the real fix for a pixel-bound tracer, and the
   only one that changes the coefficient rather than the scale.

Phases 3-4 (GPU culling / streaming) remain irrelevant to frame cost.

### Lever 1 measured: DRS floor 0.4 -> 0.25 gives 18 -> 24-28 fps

`DynamicMinScale` changed from `0.4f` to `0.25f` (the clamp floor the resize path already
enforces). Same scene, same preset, DRS on:

```
scale 1.00   app 139.9ms    7.1 fps   (before the scaler ramps)
scale 0.25   app  35.5ms   28.2 fps
scale 0.25   app  41.3ms   24.2 fps   <- steady state
```

| floor | settled scale | fps |
|---|---|---|
| 0.40 (old) | 0.40 | ~18 |
| 0.25 (new) | 0.25 | 24 - 28 |

**+33% to +55% frame rate for one constant.** It confirms the fit (~41 ms at scale 0.25) and
confirms the trace is pixel-bound.

It also confirms the limit: the scaler settles **at** the new floor, so it is still
floor-limited, and 24-28 fps is short of the 30 fps target. 0.25 is the clamp minimum, so no
further floor reduction is available -- **scaling alone cannot reach 30 fps on this scene.**

Getting the rest requires reducing per-pixel cost: Phase 1 (secondary-ray LOD) or Phase 5
(hybrid primary visibility).

### Phase 1 implemented and MEASURED: no win. Leaf-level LOD skipping is insufficient.

Implemented the revised Phase 1 in both twins -- `IsOccluded`'s leaf loop now skips
`Instances[i].LODLevel >= 2` for shadow rays only. The closest-hit path (`TestInstanceHit`)
was deliberately left untouched, so primary visibility is bit-identical and nothing can pop.

Verification: `verify_nova_glsl.py` PASS, negative control PASS, `verify_nova_binding_budget.py`
PASS. No new uniforms or bindings were added.

Measurement (10,000 entities, 250 instances, scale 1.00, DRS off):

```
before:  app 149.4 - 163.4 ms
after:   app 142.7 - 162.6 ms
```

**No measurable change.**

**Why:** skipping an instance inside a leaf does not skip the NODE that contains it. The BVH
traversal still visits every node whose AABB the shadow ray intersects; only the per-instance
`TestInstanceOcclusion` call is avoided, and that is the cheap part. The expensive part is the
node walk.

Getting a real win needs **subtree pruning** -- rejecting a whole node when its AABB lies
entirely beyond the LOD far distance. That requires exposing `LODFarDistance` (currently a C++
`static constexpr` at `Renderer3D.cpp:687`, not a uniform) to the shader, plus the same in the
HLSL twin, plus a binding-budget re-check.

**Judgement:** even with subtree pruning the ceiling is low, because the frame is dominated by
PRIMARY ray cost (88% of a pixel-bound trace). Shadow rays are a secondary term. Phase 1 is a
small lever by construction, and this measurement is consistent with that.

### SPIKE: secondary work is ~0.5 ms. The whole trace is primary visibility.

Disabled shadow traversal entirely (`IsOccluded` -> `return false`) and measured:

```
baseline:      gpu 131.3 ms   app 149.4 ms
no shadows:    gpu 130.8 ms   app 148.5 ms
```

**Difference: 0.5 ms.** Every shadow ray in the scene, removed, buys nothing.

Two things follow:

1. **Phase 1 was doomed from the start, and now we know why.** It optimised a 0.5 ms term.
   The leaf-level skip showed no win because there was no win available -- not because the
   implementation was wrong. No amount of subtree pruning changes a 0.5 ms budget.
2. **Phase 5 has a large ceiling and is the correct target.** Hybrid primary visibility
   attacks the 130.8 ms that remains, which is ~88% of the frame. It is the only lever that
   touches the actual cost.

The spike was reverted (`git diff` clean against the staged tree, `verify_nova_glsl.py` PASS).

### Revised priority

| lever | measured ceiling | verdict |
|---|---|---|
| DRS floor 0.4 -> 0.25 | 18 -> 24-28 fps | done, quality cost |
| Phase 1 secondary LOD | ~0.5 ms | **dead** -- nothing to win |
| Phase 3/4 culling / streaming | 0 (instance count flat) | irrelevant to cost |
| **Phase 5 hybrid primary visibility** | **~130 ms (88%)** | **the only real lever** |

### SPIKE 2: POM is ~0 ms too. What is left is traversal + intersection + shading.

```
baseline:      gpu 131.3 ms
no shadows:    gpu 130.8 ms   (-0.5)
no POM:        gpu 131.8-135.1 ms   (-0.0)
```

Two ablation spikes, both negative. The 130 ms primary term is **not** shadow rays and **not**
parallax occlusion mapping. It is the BVH traversal, primitive intersection and surface shading
that every primary pixel pays.

### Phase 5 as written is incoherent for this renderer

This is the same conclusion reached earlier about a clean-room Nanite, and it now applies to
the plan's own Phase 5:

- Hybrid raster means rasterizing primary visibility. Rasterizing requires triangles. **Nova has
  no meshes** -- 8 analytic primitives, one AABB per instance, `HitPrimitive` switching on a
  shape ordinal, no UVs, no vertex buffers.
- Even if it could rasterize, the plan said "trace only secondary rays" -- and secondary is
  0.5 ms. There would be nothing left to trace.

So Phase 5 cannot be executed as specified. It needs to be replaced, not refined.

### What the ablation leaves

The remaining lever is the cost of the primary traversal+shading term itself, at a fixed
resolution. Ablation has ruled out:

| component | measured cost |
|---|---|
| shadow rays | 0.5 ms |
| POM | ~0.0 ms |
| composite / post | 8.2 ms |
| ImGui | 3.4 ms |
| **primary traversal + intersection + shading** | **~130 ms** |

Next measurement should ablate *within* that term: traversal only (return a constant colour on
hit, no shading) versus shading only. That splits BVH cost from material cost, and those have
completely different fixes.

### SPIKE 3: shading is ~0 ms. The ENTIRE 130 ms is traversal + intersection.

Stored the hit normal and returned before any material evaluation, lighting, fog or
volumetrics:

```
baseline:      gpu 131.3 ms
no shadows:    gpu 130.8 ms   (-0.5)
no POM:        gpu 131.8 ms   (-0.0)
no shading:    gpu 134.1-136.0 ms   (-0.0, within noise)
```

All three ablations negative. **The renderer is a pure primary-visibility ray caster, and its
entire cost is `TraceScene`: BVH traversal plus analytic primitive intersection.**

### The number is anomalously high

130 ms for 8.3M pixels is ~15.7 ns/pixel. With only 250 visible instances the BVH is shallow
(~8 levels), so a well-ordered traversal should not cost anywhere near that. Two suspects:

1. **No front-to-back ordering.** `TraceScene` pushes right-then-left and pops, so it is plain
   depth-first with no distance ordering (`:3652-3656`). The only pruning is `tNear > info.t`
   (`:3643`), which cannot prune a subtree visited before the nearest hit is found. Ordering
   children by `tNear` would prune most of the tree on the first hit.
2. **Divergent SSBO reads.** `BVHNodes[nodeIdx]` and `Instances[instIdx]` are 288-byte struct
   reads at per-thread-divergent indices. The `RayTracingInstance` load at `:3252` is paid for
   every candidate instance even when the AABB test then rejects it.

Either is a plausible 10x. This is the only remaining lever, and it is a traversal-optimisation
problem -- not a LOD, streaming, or hybrid-raster problem.

### Final attribution

| component | cost |
|---|---|
| shadow rays | 0.5 ms |
| POM | ~0.0 ms |
| surface shading, lights, fog | ~0.0 ms |
| composite / post | 8.2 ms |
| ImGui | 3.4 ms |
| **BVH traversal + analytic intersection** | **~130 ms (88%)** |

### SPIKE 4: front-to-back child ordering is also ~0. In-engine ablation is exhausted.

Implemented near-distance ordering of the two child pushes in `TraceScene` (push the farther
child first so the nearer is popped first, enabling the `tNear > info.t` prune to fire):

```
baseline:            gpu 131.3 ms
front-to-back order: gpu 134.1-134.7 ms
```

No win. Reverted -- it costs two extra AABB reads per inner node and buys nothing.

### Four negative results, and what they mean together

| ablation / optimisation | delta |
|---|---|
| shadow rays removed | -0.5 ms |
| POM removed | ~0.0 ms |
| surface shading removed | ~0.0 ms |
| front-to-back traversal ordering | ~0.0 ms |

The 130 ms primary term is not shadowing, not POM, not materials/lighting/fog, and not
traversal ORDER. It is the raw cost of 8.3M primary rays doing BVH node walks plus analytic
intersection, and none of the obvious structural fixes move it.

**In-engine ablation has reached its limit.** Every remaining hypothesis (BVH leaf occupancy,
SSBO struct-load bandwidth, wave divergence, instruction mix inside `HitPrimitive`) needs
per-instruction or per-shader-cycle attribution that this engine's instrumentation cannot
provide. That means an external profiler -- Nsight Graphics or RenderDoc -- not another spike.

Neither is installed on this machine (checked the standard NVIDIA and RenderDoc paths), so
that is a prerequisite, not a next step.

### Where that leaves the goal

The only lever demonstrated to work in this entire investigation is resolution:

| change | result |
|---|---|
| DRS floor 0.4 -> 0.25 | 18 -> 24-28 fps, at a real image-quality cost |

Everything else -- LOD, culling, streaming, hybrid raster, traversal ordering -- measured zero.
**The million-entity goal is not blocked by entity count and never was; it is blocked by
per-pixel primary-ray cost, which needs a profiler to attack.**

### CORRECTION: instance count DOES matter. The earlier "flat" result was the broken timer.

**Noise floor** (5 identical runs, 10,000 entities):

```
app 154.7-160.8 ms (+-2%)   gpu 127.6-136.2 ms (+-3.3%)   finish 148.4-152.0 ms (+-1.2%)
```

The harness IS reproducible. The four ablation negatives therefore stand -- they all sat inside
this band. (An isolated 36.5 ms ladder reading was an outlier, not contention; the earlier claim
that "all measurements are invalid" was itself wrong.)

**Interleaved A/B** (alternating, to cancel drift), 3 runs each:

| entities | instances | gpu | finish |
|---|---|---|---|
| 1,000 | 31 | 93.5 / 93.7 / 93.9 | 102.2 / 102.6 / 102.6 |
| 100,000 | 24,576 | 151.6 / 149.7 / 147.8 | 161.4 / 158.5 / 156.4 |

Spread +-0.2% and +-1.3%. **Instance count costs 1.6x**, from 31 to 24,576 instances.

The earlier "flat 31 -> 24,576" conclusion came from the broken GPU timer and is withdrawn.

### Revised cost model

At 4K (8.3M pixels), scale 1.00:

| component | cost | scales with |
|---|---|---|
| fixed per-pixel trace cost | ~94 ms | resolution only |
| per-instance cost | ~56 ms at 24,576 | instance count (logarithmic) |

So the frame is **both** resolution-bound and instance-count-bound, roughly 63/37 at the
24,576 cap.

### Revised lever table

| lever | ceiling | verdict |
|---|---|---|
| DRS / resolution | large -- attacks the 94 ms | **works**, 18 -> 24-28 fps at floor 0.25 |
| instance-count reduction (Phase 3/4 culling, streaming) | **1.6x** | **now justified** -- was wrongly dismissed |
| Phase 1 secondary LOD | 0.5 ms | dead |
| Phase 5 hybrid raster | incoherent (no meshes) | dead |
| traversal ordering | 0 | dead |

Phases 3-4 are back on the table with a measured 1.6x ceiling. They do not fix the 94 ms
fixed term, which remains resolution-bound and is the larger share.

### The 100k scene renders 44% of its geometry. Measured cost of fixing it.

`MaxRayTracingInstances` was made env-overridable (`WL_MAX_INSTANCES`, default unchanged at
24576) so the truncation curve could be measured in one binary. 100,000 entities, 4K, DRS off:

| cap | instances traced | gpu | finish |
|---|---|---|---|
| 24,576 (current) | 24,576 | 150.7 ms | 159.4 ms |
| 49,152 | 49,152 | 259.4 ms | 268.1 ms |
| 98,304 | **56,019** | 275.9 ms | 284.4 ms |
| 147,456 | **56,019** | 278.3 ms | 286.6 ms |

Two facts fall out:

1. **The scene needs 56,019 instances.** Raising the cap past that changes nothing -- the count
   saturates and the time is flat (275.9 / 278.3, within noise). So 56,019 is the complete
   visible set.
2. **At the current cap the scene draws 24,576 of 56,019 -- 44%.** It is missing 31,443
   instances, more than half its geometry, silently.

**Cost of rendering it completely: 150.7 -> 275.9 ms (+83%).** Per-instance cost is roughly
linear at ~3-4 us per 1000 instances above a ~94 ms fixed, resolution-bound term.

Memory: 56,019 x 288 B = 16.1 MB instance SSBO, plus 2 nodes/instance of BVH -- well within
budget on an 8 GB card.

### This is the decision the cap was hiding

The 24,576 constant was not a performance optimisation, it was a silent correctness ceiling. The
real choice is:

- **keep 24,576**: 44% of a 100k scene renders, ~6 fps at 4K
- **raise to 56,019**: the scene renders completely, ~3.6 fps at 4K
- **raise to 56,019 AND rely on the DRS**: complete geometry at reduced resolution -- this is the
  only combination that is both correct and interactive

The DRS lever (0.4 -> 0.25, measured 18 -> 24-28 fps at 250 instances) is what makes the complete
geometry affordable, which makes raising the cap the right call rather than a trade.

### Truncation is no longer silent, and the peak is higher than measured

The truncation warning used to sit behind a function-static `bool`, so it fired once per
PROCESS. A scene losing half its geometry every frame said so exactly once, at startup. It now
reports on CHANGE of the truncated count and publishes `Stats.InstancesTruncated`, which resets
to 0 on any frame that fits.

Running it produced the number the fixed-frame ladder could not:

```
61372 instances wanted, cap 24576 -> dropping 36796 (60% NOT RENDERED)   <- peak
56019 instances wanted, cap 24576 -> dropping 31443 (56% NOT RENDERED)   <- steady
```

**The scene peaks at 61,372 instances.** The earlier saturation measurement said 56,019, but that
was a late-frame sample; the peak occurs earlier. So raising the cap to 56,019 would still drop
5,353 instances at peak -- the cap needs to be **65,536** to be safe, not 56,019.

### Judgement call: the tessellation overflow is NOT worth fixing

`Renderer3D` also logs `CPU tessellation batch overflowed mid-frame 2 time(s) this frame`,
against `MaxVertices = 500000`. I did not raise it:

- A mid-frame flush SUBMITS the accumulated batch and resets, so geometry still renders. It costs
  two extra draw submissions per frame, not correctness.
- `Vertex3D` is ~72 B, so tripling to 1.5M vertices costs ~126 MB of host memory plus 18 MB of
  indices to save two draw calls.

That trade is bad. The overflow is documented here as benign rather than "fixed" by throwing
memory at it. If it ever becomes a real cost, the fix is to find out why a ray-traced renderer
tessellates 1.5M vertices per frame at all, not to enlarge the buffer.

### The truncation is a BENCHMARK ARTEFACT, not a gameplay constraint

Every large-N number in this document was measured with the camera pulled back to frame the
WHOLE grid (`_dist_for` = 2.2x half-extent). At 100k entities that is ~3.5 km away, so each cube
is 1-2 px and the frame renders as a near-black field of sub-pixel geometry. The frame rate was
real; the view was not.

Visible instance count depends almost entirely on camera placement:

| view | entities | instances |
|---|---|---|
| far framing (whole grid) | 10,000 | 250 |
| far framing (whole grid) | 100,000 | **56,019** (truncated from 61,372) |
| horizon, eye height | 10,000 | 1,573 |
| horizon, eye height | 100,000 | 16,041 |
| close, looking up | 10,000 | 9 |

**At eye level, 100k entities produce 16,041 visible instances -- under the 24,576 cap.** The
cap only bites when framing the entire grid from kilometres away, which is not a view a player
has. So:

- The 24,576 cap is **not** a gameplay constraint and does not need raising for play.
- The "56-60% of the scene is not rendered" finding is true of that specific far-framed
  benchmark, not of gameplay.
- The instance-truncation reporting fix is still correct and worth keeping -- it is what made
  this visible in the first place.

### Verified visually (not just by log)

- 10k, far framing: renders correctly -- lit grid of cubes with directional light and cast
  shadows.
- 10k, eye level at 40 units: renders correctly -- worm's-eye city view, 6 cuboid buildings with
  correct perspective and faces.
- 100k, far framing: near-black field of sub-pixel cubes. Renders, but not a meaningful view.
- 100k, eye level at 40 units: only 56 instances; the camera is too close to the grid to be
  representative.

The eye-level preset is now in the harness (`--eye-level`, focal y=6, pitch -8, distance 40).
**It still needs a distance that shows a representative instance count** -- 40 units is too
close (9-56 instances), and the first attempt at pitch 0 / y=0 put the camera at ground level
and produced a flat sky-over-ground horizon. Both failures are recorded here so the next person
does not repeat them.

### DECISION TAKEN: the DRS is the answer. Floor is now per-preset.

Option 1 of the closing options: stop optimising the trace and let the dynamic resolution scaler
carry it. `DynamicMinScale` is no longer one global constant -- it is set per quality preset in
`ApplyQualityPreset`, so a preset expresses how much resolution it is willing to surrender.

| preset | DRS floor | settled scale | app | fps |
|---|---|---|---|---|
| 0 Low | 0.25 | 0.25 | 25.9 ms | **38.6** |
| 1 Medium | 0.25 | 0.25 | 26.9 ms | **37.1** |
| 2 High | 0.40 | 0.40 | 48.2 ms | 20.7 |
| 3 Ultra | 0.50 | 0.50 | 66.4 ms | 15.1 |

Measured on a representative dense view (10k entities, eye level at 900 units, ~1,042
instances, ~95% of the frame covered by geometry), 4K window.

Low and Medium now **exceed** the 30 fps target and the scaler stops there rather than
overshooting. High and Ultra deliberately miss it: they refuse to go below 0.40 / 0.50 and
accept a softer target instead of a softer image.

**Visual check at 0.25 (Low):** upscaling is smooth -- no blockiness or pixelation -- but edges
are visibly fuzzy and fine detail is lost. It is a soft image, not a broken one. That is the
honest cost of the 38 fps presets, and it is confined to the presets named "Low" and "Medium".

**Visual check at 0.50 (Ultra):** the same scene at 1080p-equivalent, materially sharper.

### Final state of the investigation

The trace costs ~160 ms at 4K on this hardware regardless of what is in front of it, and scales
with pixels rather than scene content. Not shadows (0.5 ms), not POM (0.0), not shading (0.0),
not traversal order (0.0), not instance count in the gameplay range. Resolution is the only lever
that produced a measured win, so the DRS carries the frame and the presets express the trade.

### What this means for the plan

- **Phases 3–4 (GPU-side culling and streaming) would improve correctness, not performance.**
  They would let more instances be *visible*; they would not make the frame cheaper, because
  instance count is not in the cost curve.
- **The performance lever is pixel count** — which is what the DRS already adjusts. With the
  DRS on (not `WL_DRS_OFF=1`) the engine should already be buying this; measuring whether it
  is, and how aggressively, is a much smaller question than the plan's original scope.
- **Phase 5 (hybrid primary visibility) is now the most promising phase**, because it attacks
  exactly the cost that exists: primary-ray traversal per pixel. Rasterizing primary visibility
  and tracing only secondary rays is a direct attack on a pixel-bound tracer.
- Phase 1 (secondary-ray LOD) stays valid and is cheap; it reduces per-pixel secondary work.

Caveat: `gpu` is built from few samples (it reads 0.0 until late in a run), so the absolute GPU
numbers are indicative. The *ratio* between the two scale points is the reliable part, and it
is consistent across repeated runs.

### Measurement discipline (read before Phase 0)

### Measurement discipline (read before Phase 0)

This machine usually has an LLM worker holding 3.3–3.6 GB VRAM at 100% GPU, and the editor is often open. Absolute frame times are then contention, not the engine. **Every phase gate below requires a quiet GPU** (`nvidia-smi` util low, no editor). If that is not available, the gate is *not passed* — do not average contended numbers or treat an A/B under load as a result. Instance/instance-count measurements are CPU-side and stay valid regardless.

---

## 2. Phased approach

| Phase | Deliverable | Risk | Gate to proceed |
|---|---|---|---|
| 0 | Clean baseline + harness | none | baseline captured on a quiet GPU |
| 1 | `LODLevel` actually read by the shader | low | image diff vs baseline explained; fps ≥ baseline |
| 2 | Two-level BVH (TLAS over clusters + per-cluster BLAS) | medium | traversal correct; build time acceptable |
| 3 | GPU-side cull + LOD select → indirect instance list | high | matches CPU cull within tolerance |
| 4 | Residency/streaming past 24,576 | high | >24,576 visible instances render |
| 5 | Compute-raster primary visibility (optional) | high | visual parity with traced primary |

Phases 3–5 are research-grade and each needs its own detailed breakdown after the preceding phase is measured. This document gives task-level detail for 0–2 and design-level detail for 3–5, deliberately: writing 2-minute tasks for unmeasured work would be fiction.

---

## 3. Phase 0 — Baseline and harness

**Objective:** A reproducible number to compare every later phase against, on a quiet GPU.

**Files:**
- Create: `tools/bench_nova_phase.py` (modelled on `tools/probe_screen_cull.py`)
- Reuse: `tools/probe_screen_cull.py` (instance-count probe, CPU-side)

**Task 0.1 — Confirm the GPU is quiet before measuring**

```bash
nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv
powershell -NoProfile -Command "Get-Process | Where-Object {$_.ProcessName -match 'DemonCore|ashen_gpt'} | Select-Object Id,ProcessName,StartTime"
```

If utilisation is high or `ashen_gpt_worker` is running: **stop and ask the user**. Do not proceed to a timing gate.

**Task 0.2 — Capture the baseline ladder**

Run at presets 0 and 3, render scale 1.00, `WL_DRS_OFF=1`, pinned camera, entity counts 1k / 10k / 100k, 300 frames each. Record `instances`, `screenCulled`, `app`, `gpu`, `cpuSubmit` from the last `Frame time:` line.

**Verification:** every row has `gpu > 0.0` (if `gpu` is 0 the GL timer is not working and the row is invalid) and `ZeroMaxDistanceInstances == 0`.

**Task 0.3 — Commit the baseline**

```bash
git add tools/bench_nova_phase.py
git commit -m "tools: Nova phase baseline harness"
```

---

## 4. Phase 1 — Make `LODLevel` mean something

**Objective:** Give `LODLevel` a real effect. The shader receives it in every instance and ignores it.

> **CORRECTION (2026-10-25, after Phase 0).** This phase was originally written as
> "LODLevel gates far-instance tracing", i.e. drop instances past the far band. **That is
> wrong and would reintroduce a fixed bug.** Three facts, all verified:
>
> 1. Analytic primitives have **constant intersection cost**. A cube costs the same
>    `HitPrimitive` work at 10 m and at 10 km. There is no simplified proxy to swap in,
>    because a cube's proxy *is* a cube. So there is no geometric LOD to apply.
> 2. Therefore "apply LOD" can only mean **drop the instance**, which is a distance cull.
> 3. `LODFarDistance` is 2000 m, but `TraceMaxDistance` was deliberately raised 10 km →
>    16 km because the Ash District is 7.2 km across and a 10 km cap silently culled far
>    districts — *"buildings popping in and out after the scale-up"*
>    (`Renderer3D.cpp:109-115`). Dropping at 2 km would be far worse than the bug that
>    was already fixed.
>
> **Revised Phase 1: `LODLevel` reduces SECONDARY-ray cost only, never primary
> visibility.** Far instances stay visible and keep their silhouette; what changes is that
> shadow/GI rays may skip them. `LODLevel` becomes a shading budget, not a visibility
> switch.
>
> **The hook already exists.** `TestInstanceHit(ray, instIdx, isPrimaryRay, info)`
> (`.glsl:3251`) already takes a primary/secondary discriminator, currently used only to
> gate POM — *"Secondary (bounce/shadow) rays skip the [POM]"* (`.glsl:3248`). `isPrimaryRay`
> is derived at `.glsl:3613` from whether the ray origin sits at the camera. Phase 1 adds
> the LOD test to the secondary branch of that existing gate; the primary branch is not
> touched, which is what guarantees no popping.
>
> **Second correction, found while implementing:** there is no bounce loop to reduce.
> `u_MaxBounces` is `1` at every preset and documented as unused (`.glsl:359`), so this is
> a single-bounce tracer — primary hit plus shadow rays. The only secondary work is
> shadow-ray traversal (`IsOccluded`, `.glsl:1798`). An earlier draft of this correction
> said "cap secondary bounces by LOD", which describes a loop that does not exist.
>
> **Consequence for scheduling:** this phase's benefit is GPU-side and therefore
> **unmeasurable until Phase 0.5 lands** (the GPU timer is dead — §1a). Implement it, but
> do not claim a win from it. That is why Phase 0.5 is ordered first.

**Files:**
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` (`TestInstanceHit` secondary branch, `:3289`)
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` (same — **twins must match**)
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` (`:1730-1735` band assignment)
- Test: `tests/headless/test_nova_lod.py` (new)

**Task 1.1 — Write the failing test**

The invariant is "far geometry is never removed". Assert silhouette preservation, not colour:

```python
# tests/headless/test_nova_lod.py
def test_far_instance_keeps_its_silhouette_at_lod2():
    """An instance in the far LOD band must still be hit by primary rays.

    Guards the specific regression this phase must not cause: LODLevel is a
    shading budget, not a visibility switch. A distance-dropped instance here
    would reproduce the 2 km popping bug that TraceMaxDistance was raised to fix.
    """
```

**Task 1.2 — Run it, confirm it fails**

```bash
cd tests/headless && python runner.py test_nova_lod
```

**Task 1.3 — Implement in GLSL**

In the bounce loop, cap secondary bounces and shadow-ray attempts by `inst.LODLevel`. Do **not** touch the primary hit path — primary visibility must be identical at every LOD level.

**Task 1.4 — Mirror in HLSL, verify twins**

```bash
python tools/verify_nova_glsl.py && python tools/verify_nova_glsl_negative_control.py
python tools/verify_nova_binding_budget.py
```

**Task 1.5 — Measurement gate (see Phase 0 caveat)**

Primary-visibility pixel diff must be **zero**. Shading diff must be confined to distant, indirectly-lit regions. `cpuSubmit` and `app` should fall; if `gpu` reads 0.0 the GPU timer is unavailable and no timing claim can be made (this is the current state — see §1a).

**Task 1.6 — Commit**

---

## 5. Phase 2 — Two-level acceleration structure

**Objective:** Replace the single flat TLAS with a cluster level, so the GPU can reject whole groups of instances with one box test.

**Design:** A cluster is a spatial group of up to N instances (start with N=64, chosen so a cluster AABB stays tight for a city block). Structure becomes:

- Level 1 (`ClusterBuffer`): one `BVHNodeGPU`-shaped AABB per cluster, plus the instance range it covers.
- Level 2 (`InstanceBuffer`): instances grouped by cluster, so a cluster's members are contiguous.
- Traversal: ray → cluster AABB → instance AABB → `HitPrimitive`.

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` (`BVHBuildNode` `:178`, build `:461-610`, flatten `:573`, upload `:4479`)
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.h` (`RayTracingInstance`, `Stats`)
- Modify: `.glsl:201` (`BVHNode`) + traversal, and `.hlsl` twin
- Create: `tests/headless/test_bvh_clusters.py`
- Reuse: `tools/verify_nova_binding_budget.py`

**Task 2.1 — Write the failing test (CPU-side only)**

The cluster build is pure CPU geometry, so it is testable without a GPU — which is exactly why it goes first.

```python
# tests/headless/test_bvh_clusters.py
def test_every_instance_lands_in_exactly_one_cluster():
    """Clusters must partition the instance set: no duplicates, no omissions."""

def test_cluster_aabb_contains_all_member_instances():
    """A cluster whose AABB misses a member silently drops geometry at trace time."""
```

**Task 2.2 — Run, confirm failure** (`python runner.py test_bvh_clusters`)

**Task 2.3 — Implement `BuildClusters()` on the CPU**

Spatial grouping by cluster-grid cell (not median split — grid cells keep clusters spatially tight, which is what makes the level-1 test effective). Cap at N instances; overflow spills to the next cell.

**Task 2.4 — Reorder instances into cluster order**

Reuse the existing BVH-prim reorder machinery; the instance buffer must be permuted so each cluster's members are contiguous.

**Task 2.5 — Extend the GPU buffers**

Add `ClusterBuffer` SSBO. **Check the binding budget before choosing a slot** — `u12`/`t74` are already taken and taking `u12` once moved the DX12 UAV heap block from 52 to 53. Run `python tools/verify_nova_binding_budget.py` immediately after adding the binding, not at the end.

**Task 2.6 — Traverse two levels in GLSL, mirror in HLSL**

**Task 2.7 — Verify parity + visual equality**

```bash
python tools/verify_nova_glsl.py && python tools/verify_nova_glsl_negative_control.py
python tools/verify_nova_binding_budget.py
```
Then render the same pinned-camera frame as Phase 0 and diff: **the two-level structure must be visually identical** — it is an acceleration structure, not a quality change. Any pixel difference is a bug.

**Task 2.8 — Measurement gate**

Build time must not exceed the frame budget on scene change (it currently rebuilds when `SceneInstancesChanged`). Record build ms at 100k entities; if it regresses badly, the cluster grid needs tuning.

**Task 2.9 — Commit**

---

## 6. Phase 3 — GPU-side cull and LOD selection (design level)

**Objective:** Move the CPU cull in `CullInstanceForTrace` onto the GPU as a compute pass that writes a compacted, indirect instance list — the enabling step for both higher instance counts and per-cluster LOD.

**Shape of the work:**
1. New compute shader `Renderer3D_NovaCull.comp` (+ HLSL twin) reading clusters + instances, writing a compacted index list via an atomic counter.
2. Frustum planes uploaded as a uniform block; Hi-Z occlusion needs a depth pyramid (does not exist — build it from the previous frame's depth).
3. `m_SceneInstances` becomes a *candidate* buffer; the traced set becomes the indirect list.
4. Keep the CPU cull as the reference implementation and A/B the two — the GPU result must match it within an explicit tolerance before the CPU path is retired.

**Why this is not detailed further:** the correct design depends on Phase 2's measured cluster count and on whether Hi-Z pays for itself at the target resolution. Deciding that now would be guessing.

---

## 7. Phase 4 — Residency past 24,576 (design level)

**Objective:** Turn the cap into a budget. Instances beyond the resident set are paged in by screen-space importance.

**Shape of the work:**
1. Partition the instance set into pages (cluster-aligned).
2. Per-frame residency scoring (projected size × visibility), then an async upload of the top-N pages.
3. A stable indirect index so a page swap does not invalidate in-flight work.
4. Deterministic eviction so a pinned camera produces a stable frame (essential for screenshot testing).

**Hard prerequisite:** Phase 3. Streaming without GPU-side culling means the CPU decides residency, which reintroduces the per-frame CPU cost this plan exists to remove.

---

## 8. Phase 5 — Hybrid primary visibility (optional, design level)

**Objective:** Rasterize the 8 analytic primitives into a visibility buffer in compute, then trace only secondary rays (shadows, GI, reflections).

**Why separate:** this is a different renderer mode, not an optimisation of the tracer. It must be opt-in and A/B-able, and it does not depend on Phases 2–4.

**Shape of the work:**
1. Tessellate each shape to a bounded triangle count in compute (a cube is 12 tris; a sphere needs a fixed LOD).
2. Software rasterizer → 64-bit visibility buffer (instance id + primitive id).
3. Material/shading resolve from the visibility buffer.
4. Feed secondary rays from the resolved G-buffer instead of from primary traversal.
5. Visual parity gate: rasterized primary must match traced primary within a stated tolerance, or it ships as a separate mode.

**Open question that decides viability:** at 4K the primary trace is ~8.3M rays. Whether rasterizing 8 analytic shapes beats tracing them depends on tessellation cost and overdraw, which is measurable in a spike — do that before committing.

---

## 9. Files likely to change

| File | Phases | Nature |
|---|---|---|
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | 1,2,3,4 | BVH build/flatten/upload, cull, Flush append sites |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.h` | 1,2,3 | `RayTracingInstance`, `Stats`, new buffers |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` | 1,2,3 | traversal, `HitPrimitive`, `BVHNode` |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` | 1,2,3 | **must stay in lockstep with GLSL** |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaCull.comp` | 3 | new |
| `Wasteland/src/Wasteland/Renderer/ProceduralMeshes.h` | 5 | shape tessellation only |
| `tests/headless/test_nova_lod.py` | 1 | new |
| `tests/headless/test_bvh_clusters.py` | 2 | new |
| `tools/bench_nova_phase.py` | 0 | new |

---

## 10. Validation summary

Every phase must run, at minimum:

```bash
# CPU-side correctness (works regardless of GPU contention)
cd tests/headless && python runner.py test_bvh_clusters test_nova_lod test_avb_demogame_scene

# Shader twins + binding budget
python tools/verify_nova_glsl.py
python tools/verify_nova_glsl_negative_control.py
python tools/verify_nova_binding_budget.py

# Build (Release; /m:1, kill stray MSBuild by PID if the PCH locks)
msbuild WastelandPython.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
```

**Screenshot verification is mandatory for Phases 1, 2 and 5.** A clean build and passing tests are not evidence that rendering is correct — every phase here changes what is drawn.

---

## 11. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| **Binding budget exhaustion** | shader silently fails to bind; black or wrong frame | check budget immediately after each new SSBO; `u12`/`t74` already taken |
| **GLSL/HLSL drift** | DX11/DX12/Vulkan render differently from GL | twin check in every phase gate; edit both in the same commit |
| **Contended GPU** | every timing number is fiction | Phase 0 gate; instance counts only when contended |
| **Two-level BVH slows traversal** | clusters add a level of indirection for scenes that fit in one | measure at 1k before assuming it helps; keep single-level for small scenes |
| **Cluster build cost** | scene-change hitch | measure build ms at 100k; consider async build |
| **Streaming nondeterminism** | screenshot tests become flaky | deterministic eviction (Phase 4 task) |
| **Patent exposure** | virtualized-geometry patents exist | counsel review before Phases 3–4 commit engineering time; clean-room avoids *code* copying, not patents |

**Open questions for the user:**
1. Is arbitrary triangle-mesh support a product requirement? If yes, this plan is the wrong shape and a geometry pipeline should be planned instead.
2. What is the target: a specific fps at 4K, or "millions of entities visible"? The former points at Phase 5, the latter at Phases 3–4.
3. Is a quiet GPU available for Phase 0? Without it the plan cannot pass its own gates.

---

## 12. Recommended first increment

Phase 0 then Phase 1. They are small, independently valuable, need no new bindings, and Phase 1 gives a real measurement of whether LOD enforcement moves the frame at all — which is the cheapest possible test of whether the rest of this plan is worth building.
