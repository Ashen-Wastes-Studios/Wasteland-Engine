# Million-Entity Engine Optimization — Implementation Plan

> **For Hermes:** Use subagent-driven-development to implement this plan task-by-task.

**Goal:** Make Wasteland hold and tick millions of *inert* entities at a stable 30–60 fps by preset, without shipping a second renderer.

**Architecture:** Nova traces analytically — there are no triangles in the hot path (`Renderer3D::EndScene` returns before the raster `DrawIndexed` calls when `RayTracingEnabled`). So "millions of entities" is not a geometry problem: it is (1) the *whole-registry scan* every frame, (2) the 288-byte per-instance upload that scales with what the camera sees, and (3) the 12000-light list that scales with what the scene holds. The plan fixes the first two exactly and makes the third bounded and tiered.

**Tech Stack:** C++20, entt **3.16.0** (vendored; `vendor/entt/single_include/entt/entt.hpp:29` — an earlier revision of this plan said 4.0, which is wrong), Box3D/Box2D, GL/DX11/DX12/Vulkan via NVRHI, Python (pybind11) headless suites, MSBuild Release x64.

---

## Phase 0 — MEASURED 2026-10-24 (this section supersedes the levers below)

Phase 0 was run with `tools/bench_entity_scale.py`: shipped editor binary, native OpenGL
path, **pinned camera**, `WL_DRS_OFF=1` (fixed render scale), 200 frames per run,
`Frame time:` line sampled every 100 frames. Window is 3840x2160.

### The clean ladder (render scale 1.00)

| entities | preset | app median | fps | instances | cpuSubmit |
|---|---|---|---|---|---|
| 100 | 0 Low | **66.7 ms** | 15.0 | 62 | 53.8 ms |
| 1,000 | 0 Low | 101.9 ms | 9.8 | 611 | 89.8 ms |
| 10,000 | 0 Low | 122.3 ms | 7.2 | 6,014 | 103.4 ms |
| 100 | 3 Ultra | **99.9 ms** | 10.0 | 62 | 88.1 ms |
| 1,000 | 3 Ultra | 145.8 ms | 6.9 | 611 | 135.5 ms |
| 10,000 | 3 Ultra | 166.9 ms | 5.3 | 6,014 | 144.0 ms |

> **⚠ EVERY RENDER NUMBER IN THIS DOCUMENT IS CONTENDED. Do not treat any of it as a
> clean baseline.** Discovered 2026-10-25 after the fact: an Ashen GPT worker
> (`C:\dev\Ashen AI Base\cuda\Scripts\python.exe ashen_gpt_worker.py`, running since
> 2026-10-23 22:22) holds **3.3–3.6 GB of the 8 GB VRAM at 100% GPU utilisation**, and it was
> running during *every* capture in this file. The later captures also had an interactive
> `DemonCore-Editor.exe` open (started 2026-10-25 01:44), adding a second GPU consumer.
>
> Consequences, in order of how much they should change your behaviour:
>
> 1. **Absolute frame times are inflated by an unknown amount.** "61 ms floor",
>    "11.27 µs/instance", "no row reaches 30 fps" are measurements of the engine *under
>    contention*, not of the engine. On a quiet GPU the real floor is lower and 30 fps may
>    well be reachable at 62 instances. Do not quote the absolute numbers as the engine's
>    cost.
> 2. **The two-term *shape* is probably real but unverified.** A fixed term plus a
>    per-instance term fit the data well, and two captures eight hours apart agreed within
>    8% — consistent with a *stable* contention offset rather than noise. But a stable
>    offset can also mask a different shape, so the decomposition needs re-deriving on a
>    quiet GPU before any lever is chosen from it.
> 3. **The 4K/5,591-instance TDR flakes are explained.** 3.6 GB of VRAM already committed
>    to another process leaves the renderer ~4.4 GB; that is a far more likely cause of the
>    intermittent `rc=-1` / `GPU TIMER DEGRADED` than an engine defect.
> 4. **The scale sweeps taken at 00:5x are unusable, not merely noisy.** They read
>    62 instances as *slower* than 5,591 at 0.25 scale — physically impossible — because the
>    editor and the LLM were competing for the same GPU mid-run.
>
> **The load measurements are NOT affected.** `tools/probe_load_cost.py` and
> `tools/bench_cpu_entity_scale.py` never initialise a renderer, so the 1.82x const fix and
> the 283x sidecar are sound regardless of GPU state. Anything CPU-only in this document can
> be trusted; anything that renders cannot.
>
> To get a trustworthy render baseline, the LLM worker must be paused and the editor closed
> first. The user has explicitly declined that for now, so **do not re-attempt render
> measurement without asking** — and never present a contended number as a baseline.

**Nothing here reaches 30 fps — including 62 instances at the Low preset.** The engine
cannot hit the user's floor on an almost-empty scene at 4K.

> **Reproduced 2026-10-25 on a later build** (after the tree gained ~1,283 lines of
> in-progress Nova/renderer changes and the renderer was repaired), with 400 frames per run
> and n=4–5 samples per row. The numbers match the table above within 8%:
>
> | entities | preset | app median | fps | instances |
> |---|---|---|---|---|
> | 100 | 0 Low | 61.6 ms | 16.2 | 62 |
> | 1,000 | 0 Low | 101.6 ms | 9.8 | 611 |
> | 10,000 | 0 Low | 123.9 ms | 8.1 | 5,591 |
> | 100 | 3 Ultra | 100.0 ms | 10.0 | 62 |
> | 1,000 | 3 Ultra | 148.1 ms | 6.8 | 611 |
> | 10,000 | 3 Ultra | 167.8 ms | 6.0 | 5,591 |
>
> Refitting that run gives **60.9 ms floor / 11.27 µs per instance (Low)** and
> **99.2 ms / 12.26 µs (Ultra)** — the same two-term structure. The conclusion is stable
> across builds, which is what matters for planning.
>
> A transient during that re-capture is worth knowing about: two runs at 10,000 entities
> with the scale pinned at 1.00 died with `rc=-1` after one frame (`GPU TIMER DEGRADED`,
> 4K, 5,591 instances). They did **not** reproduce on a re-run, and a manual run at the same
> settings completed at 147.6 ms. Treat it as a GPU-timeout flake under CPU contention
> (a build had just finished), not an engine defect — but it does mean a pinned-scale 4K
> capture at 5k+ instances is near the TDR margin on this machine, so re-run a `NO DATA`
> row before believing it.

### Two costs, and they are separable

Fitting the ladder gives a fixed term and a per-instance term:

| preset | fixed floor | per visible instance |
|---|---|---|
| 0 Low | **66.1 ms** | **9.34 µs** |
| 3 Ultra | **99.2 ms** | **11.26 µs** |

So at 1M *visible* instances the per-instance term alone is **9.3 s/frame (Low)** and
**11.3 s/frame (Ultra)**. That is the number that blocks "millions of entities", and it
is lever L2.

### The fixed floor is per-PIXEL, not per-entity — proven by a scale sweep

Same scene (1,000 entities), same preset (0), only render scale changed:

| render scale | pixels | app median | fps |
|---|---|---|---|
| 1.00 | 3840x2160 | 102.0 ms | 9.8 |
| 0.50 | 1920x1080 | 42.9 ms | 23.3 |
| 0.25 | 960x540 | 21.6 ms | 46.3 |

Frame time falls roughly with pixel count (2.4x and 4.7x for 4x and 16x fewer pixels).
The floor is **full-screen per-pixel work at 4K** — volumetric fog/cloud raymarching,
SVGF denoise, bloom, god rays — not entities. That is why the presets barely help and why
the old adaptive scaler pinned at 0.40 without recovering.

### Load is a separate, harder wall (CPU, no renderer)

`tools/bench_cpu_entity_scale.py`: `Scene::LoadScene` is **327 µs/entity and linear**
(273 ms @1k, 3.27 s @10k, 16.34 s @50k). 1M entities ≈ **55 minutes** of load. Lever L4.

### Corrected lever ranking

```
L2  SUBMISSION     9.34 us/instance -> 9.3 s at 1M   [CONFIRMED, blocks "millions"]
L4  LOAD           327 us/entity -> 55 min at 1M      [CONFIRMED, blocks "millions"]
L0  FULL-SCREEN    66 ms fixed at 4K, per-pixel       [NEW, blocks "30 fps" at 4K]
L5  PRESETS        only 1.5x from Low->Ultra          [WEAK, needs L0 first]
L1  SIMULATION     flat: 0.001 ms at 50k inert        [DROPPED - already fine]
L3  LIGHTS         not yet measured                   [DEFERRED]
```

**L1 is dropped.** `StepPhysics` over 50,000 inert entities costs 0.001 ms/frame because
the sync passes are `view<RigidBody3DComponent>()` over an EMPTY pool, which iterates zero
times. They are not whole-registry scans.

### Two retractions

Both of these were in an earlier revision of this section and are wrong:

1. **"L2 is refuted; instance count barely predicts frame time."** That came from a
   *contaminated* log: 14,150 `DrawIndexed: Missing context` error lines alongside 74
   `Frame time:` lines, on a run where `NVRHIRendererAPI::m_Context` was null. `Log.cpp`
   does `spdlog::flush_on(level::warn)`, so each error was a synchronous flushed write,
   capped at 200/frame. That constant ~196 ms swamped the instance signal. On a clean log
   the per-instance term is a clear 9.34 µs.
2. **"There is a 196 ms fixed floor."** The clean floor is 66.1 ms (Low) / 99.2 ms (Ultra)
   and it is per-pixel, not per-entity.
3. **"The baseline is obsolete; the current build reads 21.6 ms."** A rebuild pulled in
   ~1,283 lines of uncommitted Nova/renderer work that rendered **black** (verified by
   inspecting a captured PNG, not by trusting the log — the scene loaded 1,012 entities and
   submitted 571 instances while drawing nothing). Frame time was 21.6 ms because nothing was
   drawn. Once that work was repaired the same six rows reproduced the original ladder within
   8% (see the reproduction table above). **A fast frame is not evidence of a fast renderer
   until the frame is looked at.**

### What this means for the user's goal

"Millions of entities at 30-60 fps by preset" decomposes into three independent problems,
and they must be solved in this order:

1. **Load** — 55 min for 1M entities is fatal before a frame is ever drawn. (L4)
2. **Submission** — 9.3 s/frame at 1M visible instances. (L2)
3. **Full-screen floor** — 66 ms at 4K even with 62 instances, so 30 fps needs the
   per-pixel passes to scale with preset far more aggressively than they do today. (L0/L5)

Solving only (1) and (2) yields a scene that loads and submits fast and still renders at
15 fps. Solving only (3) yields 30 fps on a scene that cannot hold the entities. All three
are required.

---

## Current context — measured from this tree

Read directly, not assumed. All paths relative to `C:/dev/Wasteland`.

| Fact | Evidence |
|---|---|
| Scene is already 31,354 entities | `grep -c "^  - Entity:" DemonCore-Editor/assets/scenes/DemoGame.wastescene` → 31354; 27979 `CubeRendererComponent`, 3153 `CylinderRendererComponent` |
| Scene file is 22.2 MB of YAML | `DemonCore-Editor/assets/scenes/DemoGame.wastescene` |
| entt **3.16.0** | `Wasteland/vendor/entt/single_include/entt/entt.hpp:29` (`ENTT_VERSION_MINOR 16`). An earlier revision of this table said 4.0.0, which is wrong. |
| 2035 engine/editor TUs, ~24k LOC of headers | `find ... wc -l` |
| **No threading anywhere** | only `std::thread` in `Wasteland/src` is the *declared-but-unused* `Renderer3D.cpp:1290 BVHWorkerThread` |
| **15 of 16 "large world" flags are dead** | each grep-counts **1** (declaration only): `AsyncBVHRebuildEnabled`, `MultiThreadedRenderingEnabled`, `GPUDrivenRenderingEnabled`, `GPUCullingEnabled`, `TextureAtlasEnabled`, `ShaderPermutationReductionEnabled`, `FrameGraphEnabled`, `BinarySceneFormatEnabled`, `AssetStreamingEnabled`, `StreamingRadius`, `TemporalReprojectionEnabled`, `GeometryLODEnabled`, `ComputeShaderOptimizationEnabled`, `InstancedRenderingEnabled`, `MaxBVHNodesPerWorkGroup` |
| `LODLevel` is uploaded but read by **no** shader | `CullInstanceForTrace` `Renderer3D.cpp:1721-1730` |
| Instance cap is a silent truncation | `MaxRayTracingInstances = 24576` (`Renderer3D.cpp:1274`), clamped + one-shot warn at `Renderer3D.cpp:4457-4466` |
| Per-frame rebuild of everything | `m_SceneInstances` cleared at `BeginScene`, every `Draw*` re-pushes all entities; `SceneInstancesChanged()` (`Renderer3D.cpp:2485`) memcmps 24576×288 B = **6.8 MB per frame** |
| BVH is a median-split, O(n log n), `BVH_LEAF_SIZE=4` | `BuildBVHRecursive` `Renderer3D.cpp:465`, `BuildAndFlattenBVH` `Renderer3D.cpp:572` |
| Light budget is 12000 | `Renderer3D.h:407 MaxAnalyticLights = 12000`; `SubmitSceneLights` `Scene.cpp:1865-1898` submits **unconditionally**; shader loops `min(u_LightCount, maxLightsCap)` — `u_MaxLights` is 4 at Ultra |
| Physics sync passes are **empty-pool views**, not registry scans | `SyncPendingPhysicsBodies` (`Scene.cpp:870`) + `SyncPendingPhysics2D` (`Scene.cpp:919`) are `m_Registry.view<RigidBody3DComponent>()`; with no bodies the pool is empty and the loop iterates zero times. Measured 0.001 ms/frame at 50k inert entities. |
| Physics read-back is O(bodies), not O(entities) | `RigidBody3DComponent` = 10 in DemoGame, `Rigidbody2DComponent` = 2 |
| Editor panel iterates every entity | `SceneHierarchyPanel.cpp:98 view<TransformComponent, TagComponent>().each(...)` |
| Python module is headless-drivable | `tests/headless/wlpath.py` + `Wasteland.pyd`; `Scene.CreateEntity`, `Renderer3D_*` all bound |
| Editor is scriptable headlessly | `DemonCoreEditorApp.cpp` `AutoSwitchLayer`, `WL_SCREENSHOT`, `WL_CAPTURE_PRESET`, `WASTELAND_PIN_CAMERA` |
| Perf log line exists | `Renderer3D.cpp:5394` `Frame time: app … fps … instances … cam (…)` |

**The baseline is now captured.** The measured ladder at the top of this file is the
reference every later phase is judged against; it was produced by
`tools/bench_entity_scale.py` against the shipped binary on the native OpenGL path with a
pinned camera. The earlier note here said no `Frame time:` line existed in the tree — that
was true before this harness, and is no longer.

---

## Assumptions and non-goals

**Assumptions**
1. "Millions of entities" means **massing/inert** entities: static or near-static transforms, no physics, no per-entity script, no animation. An entity that *simulates* or *animates* is a body or a skeleton instance and is not on this path.
2. The 30 fps floor is the DRS target (`TargetFPS = 30.0f`, `Renderer3D.cpp:834`); "60+ depending on preset" means Low/Medium at 1080p-class viewports reach 60 and Ultra reaches the 30 floor.
3. Nova stays the primary renderer. The raster fallback is not being extended for scale.
4. 1 unit = 1 m; the world is effectively unbounded (16 km `TraceMaxDistance` today).

**Non-goals (explicitly deferred)**
- A second/ECS-native renderer. There is no evidence the tracer is the bottleneck at scale; the tracer's cost is per-pixel, and cutting the BVH from ~862 to 203 instances moved the frame ~1% (measured, `wasteland-nova-performance`).
- Nanite-style cluster rasterization — inapplicable, there are no triangles on this path.
- Re-vendoring any NVIDIA RTX SDK. Open-source-only policy (`docs/RTX_KIT_LICENSES.md`).

---

## Proposed approach — the original five levers (SUPERSEDED, kept for audit)

> **The ranking below is superseded by "Phase 0 — MEASURED" above.** It is kept so the
> original reasoning stays auditable, but do not act on it. In summary:
> **L1 is dropped** (measured flat at 0.001 ms/frame — the sync passes are empty-pool views,
> not registry scans), **L4 and L2 are both confirmed and both block "millions"** (327 µs
> load per entity; 9.34 µs per visible instance), and a **new L0** the original plan missed
> is the fixed per-pixel floor at 4K (66 ms at Low with 62 instances), which is what blocks
> the 30 fps target rather than any entity count.

```
L1  SIMULATION     whole-registry scans per frame  →  component-tagged, dirty-driven
L2  SUBMISSION     288 B × visible instances       →  static chunk DAG + camera-differential BVH
L3  LIGHT BUDGET   12000 lights per pixel         →  distance-banded light grid
L4  MEMORY/LOAD    22 MB YAML per scene           →  chunked binary scene format
L5  THROTTLING     presets that only scale pixels →  sim/render decimation per preset
```

The original claim — "L1 and L2 are the ones that move the number; L4 and L5 are load-time
work the first two make safe to skip" — is **half wrong**. L2 does move the number, L1 does
not, and L4 cannot be skipped because it decides whether a 1M-entity scene ever opens.

---

## Phase 0 spec — DONE, see "Phase 0 — MEASURED" above

**Status: complete.** The harness described below was built and run; its results are the
measured ladder at the top of this file, which supersedes this spec. Two deviations from
the spec as written, both forced by what the code actually does:

- **The scene is generated as YAML, not via `Scene.CreateEntity`.** A bare `Scene()` has no
  component pools registered, so `AddWasteland::MaterialComponent` asserts
  `Slot not available` from `entt::storage`; only `Scene::LoadScene` registers them. The
  generator is `tools/gen_entity_block.py`.
- **`WL_AUTO_SWITCH_TO` is deliberately NOT set.** It switches to NVRHI mid-run, which on
  this machine leaves `NVRHIRendererAPI::m_Context` null (the "initialized" line is logged
  in the constructor, before `SetContext`), so every draw logs an error that
  `spdlog::flush_on(warn)` writes synchronously — 200 per frame. That spam is what
  produced the earlier, wrong 196 ms "floor".

The original spec, kept for the record:

**Objective:** produce the (backend × preset × entity-count) FPS split the plan is ranked on. Nothing else in this plan is actionable without it.

**Files:**
- Create: `tools/bench_entity_scale.py` (Python driver; writes a scene of N entities via the `Wasteland` module, pins the camera, reads `Frame time:` from the log)
- Create: `tools/bench_entity_scale_report.md` (the captured table; committed as evidence)
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:5394` — add `entities` (registry size) and `bodies` (physics body count) to the `Frame time:` line so entity count and instance count are both visible in one line.

**Step 1: write the driver**

`tools/bench_entity_scale.py` must:
- take `--count N` (default ladder: 1k, 10k, 100k, 1M) and `--preset 0..3`
- build the scene with `Scene.CreateEntity` + `TransformComponent` + `CubeRendererComponent` + `MaterialComponent` in a grid around the origin
- set `WASTELAND_PIN_CAMERA`, `WL_DRS_OFF=1`, `WL_SCREENSHOT=<path>`, `WL_SCREENSHOT_FRAME=<n>`
- run the editor exe, wait for exit, parse `Frame time:` lines from `Wasteland.log`
- report `entities`, `instances`, `app ms`, `fps`, `preset`, `scale` per row

**Step 2: capture the ladder at 1080p on OpenGL, presets 0–3**

Run, record, do not interpret. Expected shape if the diagnosis below is right: entity count ≤ ~10k is flat; the cliff is where `m_SceneInstances.size()` hits 24576 and the truncation warning fires.

**Step 3: commit the table**

`tools/bench_entity_scale_report.md` with the raw rows plus the *last* `Frame time:` line verbatim. This file is the baseline every later phase is measured against.

**Step 4: verify**

```bash
python tools/bench_entity_scale.py --count 1000 --preset 3
grep -c "Frame time:" Wasteland.log
grep "entities" Wasteland.log | tail -1     # the new field must be present
```

Expected: PASS, a non-zero `Frame time:` count, and the new fields present.

**Risk:** a 1M-entity Python scene build through pybind may be too slow to be a usable baseline. Mitigation: the driver falls back to a **generated `.wastescene`** (via a new `tools/gen_entity_block.py`) when N > 50k, and the Python path is only used for ≤ 50k. Both paths must produce byte-identical entity blocks (assert on a 100-entity prefix).

---

## Phase 1 — L1: make simulation scale with *dirty* entities, not with entities

> **STATUS: DROPPED — do not implement.** Measurement (see "Phase 0 — MEASURED" above)
> shows `StepPhysics` over 50,000 inert entities costs **0.001 ms/frame**, flat from 1k to
> 50k. The premise below — that `SyncPendingPhysicsBodies` and `SyncPendingPhysics2D` scan
> the whole registry — is **false**: both are `m_Registry.view<RigidBody3DComponent>()` /
> `view<Rigidbody2DComponent>()`, and with no bodies of that type the pool is empty, so the
> loop body never executes. There is no per-entity scan to remove.
>
> This phase is retained only so the reasoning is auditable. Implementing it would replace
> an empty loop with a dirty list and change the frame time by nothing measurable.
>
> The one genuinely useful thing it found is still worth keeping: the audit in Step 5
> (which `view<...>()` calls are per-frame and how narrow they are). Run that audit as
> documentation, not as an optimisation.

**Objective (superseded):** every per-frame loop over the registry becomes either a loop over a small dirty set or a loop over a component type that is genuinely per-entity (transforms).

**Files:**
- Modify: `Wasteland/src/Wasteland/Scene/Scene.h` (new members)
- Modify: `Wasteland/src/Wasteland/Scene/Scene.cpp:870` `SyncPendingPhysicsBodies`, `:919` `SyncPendingPhysics2D`, `:956` `StepPhysics`, `:1021` `StepPhysicsFixed`
- Modify: `Wasteland/src/Wasteland/Scene/Entity.h:96` `AddComponent` (mark the new component's set dirty)
- Test: `tests/headless/test_simulation_scaling.py`

**Step 1: write the failing test**

```python
# tests/headless/test_simulation_scaling.py
import wlpath; wlpath.setup()
from Wasteland import Scene, TransformComponent, RigidBody3DComponent

sc = Scene()
for i in range(2000):
    e = sc.CreateEntity(f"bulk{i}")
    e.GetComponent[TransformComponent]  # inert entity: no physics

# 2000 inert entities must NOT make StepPhysics scan 2000 bodies.
# Measure by body count, not wall time: the assertion is structural.
sc.OnSimulationStart()
for _ in range(60):
    sc.StepPhysics(1.0 / 60.0)
# expected: pending-body scans return early because the dirty set is empty
assert sc.GetPendingPhysicsScanCount() == 0, "inert entities are being scanned"
```

**Step 2: run, expect FAIL** — `GetPendingPhysicsScanCount` does not exist yet.

**Step 3: implement**

a) Add to `Scene`:
```cpp
// Scene.h, private:
// The set of entities whose RigidBody3DComponent was added since the last
// sync. A full view scan per sub-step is O(entities); this is O(new bodies).
std::vector<entt::entity> m_PendingBodies3D;
std::vector<entt::entity> m_PendingBodies2D;
uint32_t m_PendingPhysicsScans = 0;   // diagnostic, read by the test
uint32_t GetPendingPhysicsScanCount() const { return m_PendingPhysicsScans; }
```
b) In `Entity::AddComponent` (and `AddOrReplaceComponent`), after the emplace, if `T` is `RigidBody3DComponent` push the handle onto `m_PendingBodies3D`; same for `Rigidbody2DComponent`. Keep it in `Scene::OnComponentAdded<T>` specializations rather than in `Entity` — the hook already exists and is the intended extension point.
c) `SyncPendingPhysicsBodies` becomes: iterate `m_PendingBodies3D`, clear it, done. Increment `m_PendingPhysicsScans` only when the vector is non-empty.
d) `StepPhysics`/`StepPhysicsFixed` keep calling both syncs — they are now no-ops on a static scene instead of 2 × O(entities) scans.

**Step 4: run, expect PASS**

```bash
python tests/headless/runner.py test_simulation_scaling
```

**Step 5: also fix the same class of bug elsewhere** — grep every `m_Registry.view<...>()` that is *not* per-frame and confirm it is per-frame. `Scene.cpp:631 HealthComponent`, `:670 NativeScriptComponent`, `:688 ScriptComponent`, `:709 BehaviorTreeComponent`, `:721/:748 CameraComponent`, `:1053/:1110/:1360/:1542 physics` are all already per-frame and already narrow views. Document the audit result in the plan report rather than silently leaving it.

**Risk:** a body created by a *script* after startup is the exact case the old scan covered. Mitigation: the dirty set is also appended from `CreateEntity` when the component is added by the serializer, and `m_PendingPhysicsScans` is asserted non-zero in the existing `test_kinematic_script_sync.py` run so the regression is caught by the suite, not by a user.

---

## Phase 2 — L2: make submission scale with *what changed*, not with the scene

**Objective:** the per-frame cost stops being `O(all entities)` and becomes `O(visible) + O(changed)`.

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:643` `Renderer3DData` (new chunk state)
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:572` `BuildAndFlattenBVH` → `BuildChunkBVH`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:2485` `SceneInstancesChanged` → version-counter gate
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:4457` the truncation clamp
- Create: `Wasteland/src/Wasteland/Renderer/SceneChunkIndex.h` (the chunk grid + DAG)
- Test: `tests/headless/test_instance_deltas.py`

**Step 1: write the failing test**

```python
# tests/headless/test_instance_deltas.py
# A static scene of 50k entities must submit the same instance count as a
# static scene of 50k entities with one moved building: the delta path, not
# the full rebuild.
sc = build_grid(sc, 50000)
sc.GetRendererStats()  # warm
a = sc.GetRendererStats().instances
move_one_building(sc)
b = sc.GetRendererStats().instances
assert abs(a - b) <= 1, "moving one entity rebuilt every instance"
assert sc.GetRendererStats().bvhRebuilds <= 1, "BVH rebuilt more than once"
```

**Step 2: run, expect FAIL** — no `GetRendererStats()` binding, no `bvhRebuilds` counter.

**Step 3: implement the transform version counter**

a) Add `uint32_t Version` to `TransformComponent` (default 0).
b) `Scene::RenderScene` and the runtime path stamp a global `m_TransformEpoch` only when a transform is actually written (script write, physics read-back, animator). The read-back is already a narrow `RigidBody3DComponent` view — O(bodies), not O(entities).
c) `SceneInstancesChanged()` gains a third, cheap gate: if `m_TransformEpoch == m_LastTransformEpoch` and the instance count is unchanged, return false *before* the memcmp. The 6.8 MB memcmp disappears from the static case entirely.
d) Keep the memcmp as the second gate (bit-exactness for the case where a transform is written to an identical value) and the size check first.

**Step 4: implement the chunk index**

`SceneChunkIndex.h`:
```cpp
struct SceneChunk {
    glm::vec3 Min, Max;          // world AABB of every instance in the chunk
    uint32_t FirstInstance;      // into the per-chunk instance array
    uint32_t InstanceCount;
    uint32_t Version;            // bumped when any instance in the chunk moves
};
// 64 m cells by default; a chunk is a cell, or a merged super-cell for
// dense districts. 1M entities / 64 m cells in a 20 km world = ~2500 cells.
```
- Build once per scene load (not per frame): O(n) single pass, ~2 MB at 1M instances.
- Per frame: frustum-test **chunks** (2500 AABB tests), then emit only instances in visible chunks. This is the existing `CullInstanceForTrace` per-instance test promoted one level — same math, applied to a box that contains thousands of instances.
- The 24576 cap becomes **per-frame visible**, not per-scene: `MaxRayTracingInstances` stays 24576 as the *visible budget* and the truncation warning is replaced by a *reported* `chunksVisible / chunksTotal` ratio, not a silent resize.

**Step 5: implement the delta BVH**

- The BVH is rebuilt only for chunks whose `Version` changed. A static scene at 1M entities rebuilds **zero** nodes per frame.
- `m_ReorderedInstances` becomes a chunk-ordered array; `m_PrevSceneInstances` memcmp is replaced by the epoch gate in Step 3.
- The light index list (`m_LightIndices`) is rebuilt only when the visible chunk set changes.

**Step 6: verify**

```bash
python tests/headless/runner.py test_instance_deltas
```

Expected: PASS. Then re-run Phase 0's ladder — the `app` column must be flat from 10k to 100k.

**Risk (the big one):** chunk culling is conservative only if chunk AABBs are tight. A single 1 km building spanning 16 cells puts 16 cells in the frustum and re-introduces the full cost. Mitigation: chunk AABB is the *union of instance AABBs*, and oversized chunks (footprint > 4 cells) are split on load by a second pass. Add a verifier that asserts no chunk's AABB exceeds 2× its instance-count-weighted mean — `tools/verify_chunk_bounds.py`.

**Risk:** the 288-byte `RayTracingInstance` SSBO at 24576 is ~7 MB; at 1M visible it would be 288 MB. Mitigation: this plan never makes 1M *visible* — the chunk cull keeps visible ≤ the 24576 budget, and Phase 5's presets lower it. If a preset needs more, `MaxRayTracingInstances` is a named constant both the allocation and the upload clamp reference (already true today at `Renderer3D.cpp:2954`); raise it together with the SSBO size, never one without the other.

---

## Phase 3 — L3: make the light budget bounded and distance-banded

**Objective:** the per-pixel light loop stops being `min(12000, cap)` and becomes a spatial query.

**Files:**
- Modify: `Wasteland/src/Wasteland/Scene/Scene.cpp:1865` `SubmitSceneLights`
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:6630` `PushAnalyticLight`
- Modify: `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` and `.hlsl` (both twins — numeric constants must stay identical)
- Test: `tests/headless/test_light_budget.py`

**Step 1: write the failing test**

```python
# A scene with 5000 point lights must not upload 5000 lights.
sc = Scene()
for i in range(5000):
    add_point_light(sc, pos=(i * 10.0, 2.0, 0.0))
stats = sc.GetRendererStats()
assert stats.analyticLightsSubmitted <= stats.analyticLightBudget, \
    "light upload is unbounded"
assert stats.analyticLightsSubmitted > 0, "no lights submitted"
```

**Step 2: run, expect FAIL.**

**Step 3: implement**

a) `PushAnalyticLight` sorts by camera distance and keeps the nearest `MaxAnalyticLights` — but `MaxAnalyticLights` is a *scene* cap, not a *pixel* cap. Introduce a two-tier budget:
   - `MaxAnalyticLights` (scene, 12000) — unchanged, the submission ceiling.
   - `u_MaxLights` (pixel, 1/2/4/u_MaxLights by preset) — already exists in both twins.
b) The submission ceiling becomes distance-sorted: `SubmitSceneLights` collects into a vector, `partial_sort`s by camera distance, truncates to the budget, then submits. `O(n + k log k)` instead of `O(n)` — acceptable at 12000.
c) The shader's `lightStart`/`lightStride` rotation already exists; keep it and add a *chunk-banded* selection: lights beyond `LODFarDistance` are submitted at 1/4 the rate, beyond `MaxDrawDistance` not at all. Both twins, same constants, verified by `tools/verify_nova_shader_parity.py`.

**Step 4: verify**

```bash
python tests/headless/runner.py test_light_budget
python tools/verify_nova_shader_parity.py     # must stay green
```

**Risk:** the GLSL/HLSL twin divergence is the single most likely regression here — the existing verifier already flags `ComputeDirectLighting` (`u_MaxLights` vs hardcoded 32). Fix the HLSL twin, never delete the GLSL's read.

---

## Phase 4 — L4: load a 1M-entity scene in seconds, not minutes

> **RE-SCOPED after measurement (2026-10-24).** The premise below — that load cost is
> "YAML node allocations", so a binary sidecar is the fix — is only **half** right.
>
> `tools/probe_load_cost.py` varied only how many components each entity carries:
>
> | variant | bytes | load ms (20k) | µs/entity | vs tag |
> |---|---|---|---|---|
> | tag only | 1.41 MB | 3,123 | 156.2 | 1.00x |
> | tag+transform | 3.53 MB | 3,975 | 198.7 | 1.27x |
> | tag+xform+cube | 5.67 MB | 4,614 | 230.7 | 1.48x |
> | full (4 comps) | 9.27 MB | 5,924 | 296.2 | 1.90x |
>
> Fitting those gives **~109 µs/entity of per-ENTITY overhead** that is independent of
> component count, plus **~47 µs per component**. A binary format removes only the second
> term: at 1M entities it would take ~55 min down to ~3 min, not to "seconds".
>
> **The per-entity 109 µs is a bug, and it is one word.** `SceneSerializer::Deserialize`
> iterated entities with `for (auto entity : entities)` — a *mutable* `YAML::Node`. The
> reader then probes 25 known component names per entity via `entity["X"]`, ~21 of which
> are absent. yaml-cpp's **non-const** `Node::operator[]` routes to the non-const
> `node_data::get()`, which on a missing key **creates a node and inserts it into the map**
> (`vendor/yaml-cpp/src/node_data.cpp:242-244`); the const overload
> (`include/yaml-cpp/node/impl.h:325`) returns a ZombieNode and inserts nothing. And
> `node_data::get()` is a **linear scan over `m_map`**, so every phantom insertion made
> every later probe scan a longer map — the entity's map grew from ~5 entries to ~26 while
> being read. ~21 allocations + ~21 insertions per entity, and a quadratically-lengthening
> scan, is the 109 µs. At 1M entities that is ~21M wasted nodes.
>
> **Fix applied:** `const auto entities` and `for (const auto& entity : entities)`.
>
> **Second pass (measured, no gain):** the same bug class existed one level down — 26 nested
> component locals were declared `auto x = entity["X"]`, which copies a const Node into a
> *mutable* one, so every field probe (`x["Translation"]`) used the non-const `operator[]`.
> All 26 were made `const auto`. Measured effect on load: **none** (172.7 vs 175.3 µs/entity,
> within noise), because those field keys are *present* in a complete scene and insertion only
> happens on a miss. Kept as hygiene — it stops DOM corruption for scenes with partial
> component blocks — but it is not a performance change and should not be reported as one.
>
> **What is left is genuine parse work.** The other half of the per-component cost is that
> ~120 of 124 field reads are **double lookups**: `if (n["F"]) target = n["F"].as<T>();` scans
> the map twice per field. Collapsing those to one lookup per field is a mechanical but
> 120-site edit; the binary sidecar below removes all of it at once.
>
> So Phase 4 is now two ordered steps, cheapest first:
>
> 1. **The const fix** (done) — removes the per-entity term. No format change, no
>    migration, benefits every existing scene immediately.
> 2. **The binary sidecar** (done) — removes the per-component term AND the YAML parse.
>
> ### Step 2 result — measured 2026-10-25
>
> `SceneChunkStore` (new file) writes `<scene>.wlbin` beside the `.wastescene` on save and
> reads it on open, skipping yaml-cpp entirely. On a 50,000-entity, 23.4 MB scene:
>
> | path | load time | per entity |
> |---|---|---|
> | YAML (no sidecar) | **10,928 ms** | 218.6 µs |
> | binary sidecar | **39 ms** | **0.8 µs** |
>
> **283x**, and 1M entities now projects to ~0.8 s rather than ~3 minutes. The sidecar for
> that scene is 8.6 MB (172 B/entity) vs 23.4 MB of YAML.
>
> It is a cache, not a format, and is gated so it can never be lossy:
> - written **only** when every entity is representable — components ⊆ {ID, Tag, Transform,
>   CubeRenderer, Material}, no texture references, tags ≤ 63 chars. Otherwise no sidecar
>   is written and loading stays on YAML. DemoGame (31,354 entities, lights/textures/
>   scripts) therefore gets no sidecar, by design, and loads exactly as before.
> - read **only** when the header's recorded YAML byte size and write time still match, and
>   the version/layout match. A stale or truncated sidecar is ignored and the YAML path
>   runs; the scene is cleared before reading so a partial load cannot pass as a success.
> - `*.wlbin` is gitignored — it is derived and deletable.
>
> Guarded by `tests/headless/test_scene_sidecar.py` (4 checks): exact field-level equality
> between a YAML load and a sidecar load, stale-sidecar rejection, truncated-sidecar
> rejection, and no-sidecar-for-unsupported-scenes.
>
> Re-measure with `tools/probe_load_cost.py` after step 1 before writing any of step 2.

**Objective (superseded):** the 22 MB YAML deserialize (376k entities logged, frames not yet rendered) stops being the load-time cliff.

**Files:**
- Create: `Wasteland/src/Wasteland/Scene/SceneChunkStore.h` / `.cpp`
- Modify: `Wasteland/src/Wasteland/Scene/SceneSerializer.cpp` (dual-format read)
- Create: `tools/gen_entity_block.py` (chunk writer; also the Phase 0 fallback)
- Test: `tests/headless/test_scene_load_scale.py`

**Step 1: write the failing test**

```python
# 100k entities must load in < 10 s and round-trip byte-identically.
t0 = time.time()
sc = SceneSerializer(path).Deserialize()
dt = time.time() - t0
assert sc.GetEntityCount() == 100000
assert dt < 10.0, f"load took {dt:.1f}s"
```

**Step 2: run, expect FAIL** (YAML path is O(n) YAML-node allocations).

**Step 3: implement**

A chunked binary sidecar: `Scene: <name>\nEntities: <n>\nChunks: <path>.wlchunk`. Each chunk is a flat struct array (`TransformComponent`, `TagComponent`, `MaterialComponent`, renderer component) written with one `fwrite` per chunk. The YAML file is kept as the authoring format; the serializer writes the binary sidecar on save and reads it when present, falling back to YAML. This is the existing `BinarySceneFormatEnabled` flag finally being wired to something real.

**Step 4: verify**

```bash
python tests/headless/runner.py test_scene_load_scale
```

**Risk:** a binary sidecar is a second source of truth. Mitigation: the sidecar carries the entity count and a per-chunk CRC; a mismatch falls back to YAML with a warning, never to a partial load.

---

## Phase 5 — L5: make presets scale *simulation and render*, not just pixels

**Objective:** Low/Medium reach 60 fps not by dropping resolution alone (which cannot cut a per-pixel cost) but by decimating simulation and submission.

**Files:**
- Modify: `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:5276` `ApplyQualityPreset`
- Modify: `Wasteland/src/Wasteland/Scene/Scene.cpp:1172` `OnUpdateSimulation`, `:1192` `OnUpdateEditor`
- Modify: `DemonCore-Editor/src/EditorLayer.cpp` (preset combo → sim rate)
- Test: `tests/headless/test_preset_sim_rates.py`

**Step 1: write the failing test**

```python
# Each preset must declare a sim rate; Low must be slower than Ultra.
rates = [preset_sim_rate(p) for p in range(4)]
assert rates == sorted(rates), "presets do not order by sim cost"
assert rates[0] < rates[3], "Low does not decimate"
```

**Step 2: run, expect FAIL.**

**Step 3: implement**

Add to `Renderer3DData`:
```cpp
// Simulation decimation: how many frames between physics steps per preset.
// Low = 4 (15 Hz sim), Medium = 2, High = 1, Ultra = 1.
static constexpr uint32_t SimDecimation[4] = {4, 2, 1, 1};
```
`StepPhysics` already banks an accumulator (`Scene.cpp:1002`), so a decimated rate is a *larger* `kFixedStep`, not a skipped call. The DRS target stays 30; the preset additionally lowers sim cost, which is the part the scaler cannot touch.

**Step 4: verify**

```bash
python tests/headless/runner.py test_preset_sim_rates
```

---

## Phase 6 — Verify the whole thing

**Objective:** rendered proof at the target, not a clean build.

**Step 1: build**

```bash
cd C:/dev/Wasteland && "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" \
  Wasteland.sln /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal \
  > "$TMPDIR/wl_build.log" 2>&1; echo "MSBUILD_EXIT=$?" >> "$TMPDIR/wl_build.log"
grep MSBUILD_EXIT "$TMPDIR/wl_build.log"
```

`/m:1` is mandatory. Never build while the editor exe is running — `LNK1104` on the bin-int PCH is a collision, not a code error.

**Step 2: shader verifiers** (MSBuild compiles none of these)

```bash
python tools/verify_nova_shader_parity.py
python tools/verify_nova_glsl.py
python tools/verify_analytic_light_admission.py
python tools/verify_nova_binding_budget.py --all
python tools/verify_chunk_bounds.py          # new, Phase 2
```

**Step 3: capture**

```bash
WL_DRS_OFF=1 WASTELAND_PIN_CAMERA=...,...,...,50.0,-10.0,45.0 \
WL_SCREENSHOT="$TMPDIR/scale_100k.png" WL_SCREENSHOT_FRAME=40 \
WL_CAPTURE_PRESET=3 WL_CAPTURE_SCENE=Scale100k.wastescene \
DemonCore-Editor.exe --project=DemonCore-Editor
```

Verify visually: geometry present (local contrast, not mean luma — see `wasteland-large-world-rendering` §1a), no pop, and the `Frame time:` line reporting `app` at the target.

**Step 4: re-run the Phase 0 ladder** and diff the table against the baseline. The deliverable is the *delta*, not the absolute numbers.

---

## Files likely to change

| Path | Change |
|---|---|
| `Wasteland/src/Wasteland/Scene/Scene.h` | dirty-set members, `GetPendingPhysicsScanCount`, `GetEntityCount` |
| `Wasteland/src/Wasteland/Scene/Scene.cpp` | `SyncPendingPhysics*`, `StepPhysics*`, `SubmitSceneLights`, preset sim rates |
| `Wasteland/src/Wasteland/Scene/Entity.h` | dirty-set append in `AddComponent` |
| `Wasteland/src/Wasteland/Scene/SceneSerializer.cpp` | dual-format read |
| `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` | chunk index, epoch gate, delta BVH, light sort, `Frame time:` fields, `SimDecimation` |
| `Wasteland/src/Wasteland/Renderer/SceneChunkIndex.h` | **new** |
| `Wasteland/src/Wasteland/Scene/SceneChunkStore.h/.cpp` | **new** |
| `Wasteland/src/Wasteland/Scene/Components.h` | `TransformComponent::Version` |
| `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.{glsl,hlsl}` | banded light selection (both twins) |
| `DemonCore-Editor/src/Panels/SceneHierarchyPanel.cpp` | paginate the entity walk |
| `Wasteland/src/Wasteland/Scripting/Bindings.cpp` | `GetRendererStats`, `GetEntityCount`, `GetPendingPhysicsScanCount` |
| `tools/bench_entity_scale.py`, `tools/gen_entity_block.py`, `tools/verify_chunk_bounds.py`, `tools/bench_entity_scale_report.md` | **new** |
| `tests/headless/test_{simulation_scaling,instance_deltas,light_budget,scene_load_scale,preset_sim_rates}.py` | **new** |

---

## Tests / validation summary

| Phase | Test | Command |
|---|---|---|
| 0 | ladder driver | `python tools/bench_entity_scale.py --count 100000 --preset 3` |
| 1 | `test_simulation_scaling` | `python tests/headless/runner.py test_simulation_scaling` |
| 2 | `test_instance_deltas` | `python tests/headless/runner.py test_instance_deltas` |
| 3 | `test_light_budget` + parity | `python tests/headless/runner.py test_light_budget` |
| 4 | `test_scene_load_scale` | `python tests/headless/runner.py test_scene_load_scale` |
| 5 | `test_preset_sim_rates` | `python tests/headless/runner.py test_preset_sim_rates` |
| 6 | build + capture | see Phase 6 |
| — | regression | `python tools/run_all_suites.py` |

---

## Risks, tradeoffs, open questions

**Risks**
1. **Chunk culling tightness** — a single huge building re-introduces full cost. Mitigated by the 2×-mean chunk-bound verifier.
2. **The 288-byte ABI** — `RayTracingInstance` is `static_assert`ed to 288 and mirrored in both shader twins. Any field added to it is a four-language change (C++, GLSL, HLSL, and the SSBO layout). Do not add fields to reach scale; reach scale by *fewer* instances uploaded.
3. **The silent truncation** — `MaxRayTracingInstances` is a clamp with a one-shot warning. At 1M entities the clamp is hit every frame and the warning fires once. Phase 2 replaces it with a reported ratio, but any code path that still resizes the list must be found and removed (grep `m_SceneInstances.resize`).
4. **The 15 dead flags** — implementing them is not the plan. They are a trap: a future reader greps `GPUDrivenRenderingEnabled`, finds it, and assumes GPU-driven rendering exists. Recommend a follow-up task to delete them or wire them, not to build on top of them.
5. **Baseline may contradict the plan.** If Phase 0 shows the cliff is at 10k and is *not* the registry scan, the plan is wrong and must be rewritten from the measurement. That is the intended outcome of Phase 0, not a failure.

**Tradeoffs**
- Chunk culling costs one extra AABB test per chunk per frame (~2500) to save thousands of per-instance tests. Net win at 1M, neutral at 1k — so the chunk path must be *disabled below a threshold* (e.g. < 5000 entities) or it is pure overhead on small scenes.
- The binary sidecar trades a single source of truth for load speed. Mitigated by the CRC fallback.
- Sim decimation trades simulation fidelity for frame rate at Low. That is what the preset is for; it must be *declared* in the preset, not hidden.

**Open questions for the user before execution**
1. What is the actual target viewport? The plan assumes 1080p-class for the 60 fps claim and 4K for the 30 fps floor. The existing measurements are 4K.
2. Is "millions of entities" a *scene* target (a world that big) or a *runtime* target (spawned NPCs)? The plan covers the former; the latter needs an AI/LOD tier that is a separate plan.
3. Should the 15 dead flags be deleted in the same pass, or left alone? Deleting them is safe (nothing reads them) but touches a file with 25 uncommitted modifications.
4. Is the raster fallback in scope? The plan assumes no. If it is, the chunk index must be duplicated there and the plan doubles.

---

## Execution handoff

Plan complete and saved. Ready to execute using subagent-driven-development — a fresh subagent per task with two-stage review (spec compliance, then code quality). Phase 0 is blocking: do not start Phase 1 until `tools/bench_entity_scale_report.md` exists and the user has seen the table.

---

## Phase 2 — BLOCKED (2026-10-25): Step 3 is unsound as written, and Step 1's test cannot pass

Investigated before implementing. Two independent blockers, both verified against the tree.

### Blocker 1 — the plan's test cannot go green, because there is no headless render

`tests/headless/test_instance_deltas.py` was written (Steps 1-2) and run. It fails RED, but
not on the assertion the plan expects:

- `Scene` exposes **no** render entry point. `Wasteland.Renderer3D` is not even a module-level
  binding — `hasattr(Wasteland, "Renderer3D")` is `False`. `dir(Scene())` contains zero members
  matching `Render`/`Stats`.
- `GetRendererStats()` has **0 hits** in the entire tree, and `bvhRebuilds` has **0 hits**. The
  plan's Step 1 test reads both, and the plan's **Files** list never adds either one.
- `Scene::OnUpdateRuntime` advances water, health, scripts, behaviour trees and physics — it
  never renders. So no headless test can observe the instance list at all.

The plan's Step 2 says "run, expect FAIL — no `GetRendererStats()` binding, no `bvhRebuilds`
counter", which reads as *the test failing is the point*. It is not: a test that can never pass
is not a spec, it is a permanent red. Phase 2 needs a headless submission path (or the counters
bound against the editor binary) before Step 1 can be written.

### Blocker 2 — the transform version counter cannot observe script writes

Step 3b says the epoch is stamped "only when a transform is actually written (script write,
physics read-back, animator)". There is **no write-notification mechanism in the tree**:

- `grep` for `OnTransformChanged`, `MarkTransformDirty`, `NotifyTransformWrite`,
  `TransformWritten`, `SetTranslation` across `Wasteland/src/**/*.cpp` → **0 files each**.
- `Bindings.cpp:3765-3790` binds `Translation`/`Rotation`/`Scale` with
  `py::return_value_policy::reference_internal`. `t.Translation.x += 1` mutates the live member
  **directly** — no setter runs, so nothing can observe the write. This is deliberate and
  documented in the same file (`:3752-3761`): returning by value would silently discard the write.
- `TransformComponent::GetTransform()` (`Components.h`) *is* a self-validating cache — it
  rebuilds only when a field differs from the cached key. But it is `const`, it **discards the
  was-dirty bit**, and the signal is *consumed by the read*. It cannot serve as a frame-level
  epoch, and OR-ing it across 50k components is O(n) — the exact cost the gate exists to remove.

**Why this matters more than a missing binding.** The gate is a *fast path*, so its correctness
requirement is one-directional: it must never report "unchanged" on a frame that changed. A
missed epoch bump makes the renderer keep a stale BVH and a stale instance SSBO, which is
precisely the pop-in Phase 2 exists to fix. A gate that silently under-reports is worse than no
gate. The only sound detector available today is the memcmp the epoch is meant to replace.

### Salvage path (in order of soundness)

1. **Stamp the epoch from the write sites that have them** (physics read-back at
   `Scene.cpp:1057`/`:1114`, the two direct writes at `:1401`/`:1579`), and **keep the memcmp as
   the second gate** for everything else. Sound, but the win is limited to physics-driven scenes.
2. **Route script writes through a setter** (`t.SetTranslation(v)` / a `py::def_property` setter
   that bumps a counter) so the epoch sees them. Sound and complete, but it changes the script
   API and every authored mover tree.
3. **Derive the epoch from the registry** (entt's per-component version) if a narrow view can
   enumerate only-changed components. Not verified; entt 3.16.0 has no such enumeration.

Option 1 is the only one that is sound today. It is a smaller claim than the plan makes, and it
should be made as such.

### Not done

Phase 2 Steps 3-5 (the chunk grid + DAG, the delta BVH, the per-chunk versioning) are
**unimplemented**. `m_TransformEpoch` has 0 hits; `SceneChunkIndex.h` does not exist.

---

## Phase 2 — Step 3 IMPLEMENTED (2026-10-25): the epoch gate, in its sound subset

Blocker 1 is resolved differently than the plan proposed (see the BLOCKED section above for
why the original Step 1 test can never pass): instead of inventing a headless render, the
counters were **bound** so a real headless test can read them, and the test was rewritten
to assert what actually exists.

Blocker 2 was resolved by choosing salvage option 1 and recording the claim honestly.

### What shipped

| Change | File |
|---|---|
| `Scene::BumpTransformEpoch()` / `GetTransformEpoch()` + `m_TransformEpoch` | `Scene/Scene.h` |
| Bump at the 3D read-back, the 2D read-back, and for the whole script/tree pass | `Scene/Scene.cpp` |
| `SetSceneTransformEpoch(m_TransformEpoch)` published before **every** `Renderer3D::BeginScene` | `Scene/Scene.cpp` (2 sites) |
| `m_SceneTransformEpoch` / `m_LastSceneTransformEpoch` + `kEpochUninitialised = 0xFFFFFFFF` | `Renderer/Renderer3D.cpp` |
| `SceneInstancesChanged()` gate: size → empty → **epoch** → memcmp | `Renderer/Renderer3D.cpp` |
| `Statistics` counters: `InstancesSubmitted`, `BvhRebuilds`, `InstanceInvalidations`, `EpochShortCircuits`, `SceneTransformEpoch` | `Renderer/Renderer3D.h` |
| `BvhRebuilds++` inside `BuildAndFlattenBVH` (the one function every rebuild funnels through) | `Renderer/Renderer3D.cpp` |
| Bound: `Scene.GetTransformEpoch`, `Renderer3D_SetSceneTransformEpoch`, the 5 counters | `Scripting/Bindings.cpp` |
| `tools/verify_instance_delta_gate.py` — 37 checks + 7 mutation self-tests | `tools/` |
| `tests/headless/test_instance_deltas.py` — rewritten from a permanent red to a real guard | `tests/headless/` |

### The four decisions that carry the correctness argument

1. **The epoch compare runs AFTER the empty check, not before.** The first cut put it
   before, and `tools/verify_instance_delta_gate.py` caught it: the cheapest degenerate-case
   test must still run first. Reordered to size → empty → epoch → memcmp.
2. **The memcmp stays.** The epoch is the fast path, the memcmp is the second gate. Correctness
   does not depend on the stamp being precise, which is the only reason a conservative stamp is
   allowed.
3. **`m_LastSceneTransformEpoch` starts at `0xFFFFFFFF`, not 0.** Otherwise the first frame
   compares equal to an unstamped 0 and skips a build the scene needs. 0 is a legitimate epoch
   (a fresh `Scene` is at 0), so the sentinel cannot be 0.
4. **`BvhRebuilds` is incremented inside `BuildAndFlattenBVH`, not at its four call sites**, so
   the counter cannot drift from a path that forgot to bump it.

### What the stamp deliberately does NOT claim

`Scene::BumpTransformEpoch` fires for the **whole script/tree pass**, not for an observed
transform write. That is not a shortcut — it is the only sound option: a Python write goes
through pybind11's `reference_internal` view of the member (`t.Translation.x += 1`), no C++
setter runs, and there is no write-notification mechanism in the tree. Over-bumping costs the
fast path on scripted scenes; under-bumping would be a stale BVH. The plan's Step 3b wording
("only when a transform is actually written") is **not** implementable against this binding
layer, and is marked as such rather than silently approximated.

### What is still not done

- **The instance counts themselves are still not observable headless.** `Scene` has no render
  entry point, so `m_SceneInstances` cannot be driven from Python. The plan's Step 1
  assertions ("moving one entity does not change the submitted instance count", "bvhRebuilds
  <= 1") remain unwritable. `test_instance_deltas.py` reports this as a confirmed gap rather
  than a passing test.
- Steps 4-5 (chunk grid + DAG, delta BVH, per-chunk versioning) are **unimplemented**.
  `SceneChunkIndex.h` does not exist.
- The epoch has not been measured against a large scene yet. Phase 0's wall-clock ladder is
  where that number comes from.


### Correction (same day): the stamp must be CONDITIONAL on the value actually changing

The first cut bumped unconditionally at both physics read-backs. `test_instance_deltas.py`
caught it on the very first run: a body resting on the floor re-bumped every step forever
(epoch 482 -> 722 over 120 steps, ~2 bumps/step for the 2D and 3D read-backs), so the gate
**never fired**. Nothing crashes, nothing goes stale, no error is logged — the BVH is simply
rebuilt every frame and the frame time stays where it was. That is the failure mode hardest
to notice, and it is exactly the one a fast path invites.

Both read-backs now compare old vs new before assigning and bumping:

```cpp
const glm::vec3 newPosition((float)pos.x, (float)pos.y, (float)pos.z);
const glm::vec3 newRotation = B3QuatToEuler(rot);
if (transform.Translation != newPosition || transform.Rotation != newRotation)
{
    transform.Translation = newPosition;
    transform.Rotation = newRotation;
    BumpTransformEpoch();
}
```

The compare is EXACT because the memcmp it fronts is exact too — both read the same floats.
A body that jitters by 1e-7 bumps the epoch AND trips the memcmp, so the two stay consistent
instead of the epoch claiming "unchanged" on a frame the memcmp would have rejected. The
script/tree pass stays unconditional (conservative), because a Python write is unobservable.

`tools/verify_instance_delta_gate.py` grew CHECK 11 to require that guard in both read-backs,
with a self-test that deletes it and expects the check to fail.

### Refinements picked up from the tree while landing this

The tree moved under the change (concurrent edits by the user's IDE). Adapted to, not fought:

- `kEpochUninitialised` is now a `Renderer3DData` member, so the gate reads
  `Renderer3DData::kEpochUninitialised`. The verifier accepts either spelling rather than
  pinning one, because the sentinel's MEANING is what matters, not its scope.
- The BVH counter moved out of the inline increment into a `NoteBvhRebuild()` helper, and it
  is now **skipped for an empty submission** — an empty frame is not a rebuild, and counting
  one would inflate the metric in exactly the state the gate exists to make common. The
  verifier now requires exactly ONE `Stats.BvhRebuilds++` in the whole file.
- `SceneInstancesChanged()` gained a **second memcmp for `m_TerrainVertices`**. The epoch
  gate fronts both, which is fine: an unchanged epoch short-circuits whichever memcmp would
  have run, and the memcmps remain the second gate for anything the stamp misses.


### Measured result (2026-10-25, `tests/headless/test_instance_deltas.py`)

Built `WastelandPython.vcxproj` Release x64, `exit=0`, `Wasteland.pyd` 8,424,448 bytes.
`DemonCore-Editor.exe` relinked from the same lib at 21:29:41.

A dynamic body dropped 4 m onto a static floor, stepped at 1/60 s, epoch sampled each step:

| step | epoch | body y |
|---|---|---|
| 0 | 0 | +4.000000 |
| 1 | 1 | +3.998297 |
| 30 | 30 | +2.763532 |
| 60 | 60 | +0.499423 |
| 87 | **87** | settled |
| 120 | **87** | +0.499931 |
| 240 | **87** | +0.499931 |
| 360 | **87** | +0.499931 |

The epoch tracks the fall 1:1 and then **freezes at 87 for the remaining 240 steps** — the
state the gate exists for. A scene with no physics and no scripts stays at epoch 0 across
five `OnUpdateRuntime` ticks (the negative control that makes the positive one mean something).

`tools/verify_instance_delta_gate.py` — 43 checks, 9 mutation self-tests, all caught.
All 22 `tools/verify_*.py` / `tools/compile_*.py` pass. All 30 `tests/headless/test_*.py` pass.

**Not done:** the epoch has not been measured against a real large scene — no headless render
exists, so `InstancesSubmitted` / `EpochShortCircuits` read zero headless and the plan's
instance-count assertions remain unwritable. Phase 0's wall-clock ladder is where that number
comes from, and `.hermes/plans/2026-10-25_195000-virtualized-instances-and-hybrid-raster.md`
is now the successor plan (its Phase 0 gate failed: the `GL_TIME_ELAPSED` query returns 0.0,
so no GPU timing claim is possible on this path).
