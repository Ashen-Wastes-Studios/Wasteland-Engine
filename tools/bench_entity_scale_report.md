# Entity-scale frame-time baseline

> **⚠ CONTENDED — NOT A CLEAN BASELINE.** Every number below was captured while an Ashen
> GPT worker (`ashen_gpt_worker.py`, running since 2026-10-23 22:22) held 3.3–3.6 GB of the
> 8 GB VRAM at 100% GPU utilisation, and the later runs also had an interactive
> `DemonCore-Editor.exe` open. Absolute frame times are therefore inflated by an unknown
> amount and must not be quoted as the engine's cost. The two-term *shape* is consistent
> across captures eight hours apart (within 8%), which suggests a stable offset rather than
> noise — but it is unverified on a quiet GPU.
>
> A trustworthy baseline requires pausing that worker and closing the editor first. The user
> has declined that for now, so do not re-attempt these captures without asking.
>
> CPU-only measurements (`tools/probe_load_cost.py`, `tools/bench_cpu_entity_scale.py`) are
> unaffected — they never initialise a renderer. See §5 for the load numbers that ARE sound.

Captured 2026-10-24 on the shipped `DemonCore-Editor.exe`, **native OpenGL path**,
**pinned camera** (`WASTELAND_PIN_CAMERA`), **`WL_DRS_OFF=1`** (fixed render scale),
200 frames per run, window 3840x2160. `Renderer3D` emits its `Frame time:` line every
100 frames, so each row is the median of the post-startup samples (the `app 0.0ms`
startup sample is excluded — it has no previous timestamp to difference against).

Regenerate with:

```bash
python tools/bench_entity_scale.py --counts 100,1000,10000 --presets 0,3 --frames 400
```

Use `--frames 400` or more: the log line is emitted every 100 frames, so 200 frames yields
only one post-startup sample per row and the "median" is a single reading.

## 0. Reproduced on a later build (2026-10-25)

The table in §1 was re-captured on a build that had since gained ~1,283 lines of in-progress
Nova/renderer work, with 400 frames per run and n=4–5 samples per row:

| entities | preset | app median | fps | visible instances |
|---|---|---|---|---|
| 100 | 0 Low | 61.6 ms | 16.2 | 62 |
| 1,000 | 0 Low | 101.6 ms | 9.8 | 611 |
| 10,000 | 0 Low | 123.9 ms | 8.1 | 5,591 |
| 100 | 3 Ultra | 100.0 ms | 10.0 | 62 |
| 1,000 | 3 Ultra | 148.1 ms | 6.8 | 611 |
| 10,000 | 3 Ultra | 167.8 ms | 6.0 | 5,591 |

Agreement with §1 is within 8%, and the refit gives **60.9 ms floor / 11.27 µs per instance
(Low)** and **99.2 ms / 12.26 µs (Ultra)** — the same two-term structure. The conclusion is
stable across builds.

Two 10,000-entity rows in that re-capture died with `rc=-1` after a single frame
(`GPU TIMER DEGRADED`, 4K, 5,591 instances, scale pinned at 1.00) and did not reproduce on
re-run; a manual run at the same settings completed at 147.6 ms. Treat a `NO DATA` row at
5k+ instances as a GPU-timeout flake and re-run it before believing it.

## 1. The ladder (render scale 1.00)

| entities | preset | app median | fps | visible instances | cpuSubmit |
|---|---|---|---|---|---|
| 100 | 0 Low | **66.7 ms** | 15.0 | 62 | 53.8 ms |
| 1,000 | 0 Low | 101.9 ms | 9.8 | 611 | 89.8 ms |
| 10,000 | 0 Low | 122.3 ms | 7.2 | 6,014 | 103.4 ms |
| 100 | 3 Ultra | **99.9 ms** | 10.0 | 62 | 88.1 ms |
| 1,000 | 3 Ultra | 145.8 ms | 6.9 | 611 | 135.5 ms |
| 10,000 | 3 Ultra | 166.9 ms | 5.3 | 6,014 | 144.0 ms |

**No row reaches 30 fps — including 62 instances at the Low preset.**

## 2. The cost decomposes into a fixed term and a per-instance term

Fitting the ladder:

| preset | fixed floor | per visible instance |
|---|---|---|
| 0 Low | **66.1 ms** | **9.34 µs** |
| 3 Ultra | **99.2 ms** | **11.26 µs** |

Extrapolated to 1M *visible* instances, the per-instance term alone is **9.3 s/frame**
(Low) and **11.3 s/frame** (Ultra).

## 3. The fixed floor is per-PIXEL, not per-entity

Same scene (1,000 entities), same preset (0 Low), only render scale changed:

| render scale | internal pixels | app median | fps |
|---|---|---|---|
| 1.00 | 3840x2160 | 102.0 ms | 9.8 |
| 0.50 | 1920x1080 | 42.9 ms | 23.3 |
| 0.25 | 960x540 | 21.6 ms | 46.3 |

Frame time falls with pixel count (2.4x for 4x fewer pixels, 4.7x for 16x fewer). The
floor is full-screen per-pixel work at 4K — volumetric fog/cloud raymarching, SVGF
denoise, bloom, god rays — not entities.

## 4. Load is a separate wall (CPU only, no renderer)

`tools/bench_cpu_entity_scale.py`, which loads a scene through `Scene::LoadScene` and never
initialises a renderer:

| entities | LoadScene | per entity | StepPhysics (all inert) |
|---|---|---|---|
| 1,000 | 273 ms | 273 µs | 0.002 ms/frame |
| 10,000 | 3,274 ms | 327 µs | 0.002 ms/frame |
| 50,000 | 16,341 ms | 327 µs | 0.001 ms/frame |

Load is **linear at ~327 µs/entity** → 1M entities ≈ **55 minutes**.

`StepPhysics` is **flat**: 0.001 ms/frame at 50,000 inert entities. The physics sync passes
are `view<RigidBody3DComponent>()` over an empty pool, which iterates zero times. There is
no whole-registry scan, and "make simulation scale with dirty entities" optimises nothing.

## 5. What this rules out

An earlier baseline of "196 ms fixed floor" was an artifact, not a measurement. It came
from a run where `WL_AUTO_SWITCH_TO` switched the renderer to NVRHI and left
`NVRHIRendererAPI::m_Context` null — the "NVRHI Renderer API initialized" line is logged in
the **constructor**, before `SetContext()`. Every subsequent draw logged
`DrawIndexed: Missing context, shader, or vertex array`, and `Log.cpp` sets
`spdlog::flush_on(level::warn)`, so each was a synchronous flushed write, capped at 200 per
frame. That constant cost swamped the per-instance signal and made instance count look
uncorrelated with frame time (`corr = 0.154`). On a clean log the per-instance term is a
clear 9.34 µs.

The harness therefore deliberately leaves `WL_AUTO_SWITCH_TO` unset.
