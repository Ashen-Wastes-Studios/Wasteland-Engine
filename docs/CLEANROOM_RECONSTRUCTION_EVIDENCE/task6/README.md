# Task 6 — WLFG prerequisites: frame pacing + the divisible sim clock

**Status: DONE (2026-10-26).** The frame-generation feature (Tasks 7–8) needs two
mechanisms that do not exist yet, and a way to measure them. This task adds all
three, all inert at their defaults so nothing about the running engine changes:

1. a synthetic frame-rate cap, independent of vsync;
2. a divisible simulation clock (`SimTimescale`);
3. a timing line reporting the WLFG accounting, plus the factor-ceiling function.

No shader, no pass ID, no render change.

## What was added

### 1. Synthetic frame cap — `Application::SetFrameInterval`
`Application::Run()` ends each iteration with a sleep-until-target when a cap is
set. The interval is read from `WL_FRAME_CAP` (fps; 0/absent = uncapped). It is
**independent of vsync**: vsync is a display synchronisation that cannot be
disabled at runtime without a window recreate, so it is not a pacing mechanism.
If the loop falls behind the schedule the target resets to now, so a stall never
leaves a backlog of catch-up sleeps.

### 2. Divisible sim clock — `Application::SetSimTimescale`
The timestep handed to every layer's `OnUpdate` is now
`rawDelta * m_SimTimescale` (was `rawDelta`). Default `1.0`, so with no call the
delta is unchanged. This is the mechanism interpolation leans on: at a 6x factor
the sim advances one sixth as fast, so the synthetic presents show the game at
the correct speed while the render loop keeps the real rate. A non-positive scale
is clamped back to `1.0` so a bad value cannot freeze the sim.

### 3. Timing line + factor ceiling
- `Application::SetWlfgStats(factor, warps, realFrames, driftMs)` and the
  `WLFG timing: factor N warps N realFrames N simDrift N.Nms` line (gated on
  `WL_WLFG_TIMING`, 1s cadence). `m_WlfgRealFrames` is incremented every real
  present each frame; warps/drift stay at their inert baseline (0) until Task 7
  populates them.
- `Application::WlfgMaxFactor(cameraMoved, accumAlpha, novaBackend)` — the factor
  is a **function of the accumulation state**, not a constant:

  | state | max factor |
  |---|---|
  | converged (alpha at the 0.02 floor) + static + Nova | 6 |
  | partially converged | 2 |
  | camera moved, or a non-Nova backend | 0 (off) |

  Wasteland's history IS its quality: re-projecting a converged frame is nearly
  free, re-projecting a 1-sample frame multiplies noise.

- `Bindings.cpp`: `SetSimTimescale`/`GetSimTimescale`, `SetFrameInterval`/
  `GetFrameInterval`, `SetWlfgStats`, `WlfgMaxFactor` exposed to script.
- `bench_nova_phase.py`: parses the WLFG line (`WLFG_RE`, `wlfg_last`) and prints
  a factor/warps/realFrames/simDrift summary when `WL_WLFG_TIMING` is set.

## Acceptance (plan Step 5)

`python3 tools/verify_wlfg_clock.py` — **PASS**. Two checks, plus a mutation
self-test (**PASS**):

- **Clock invariant** (pure arithmetic, no engine run): a real frame interval D
  presented F times, each present advancing the sim by `D * (1/F)`, sums to D —
  so over N presents the sum of the simulated deltas equals the real elapsed, for
  every factor 1–6. Measured drift 0.000us at all six factors.
- **Factor ceiling** (reads `Application.cpp`): `WlfgMaxFactor` returns 0 on
  `!novaBackend`, 0 on `cameraMoved`, 6 when `accumAlpha <= 0.02f`, else 2; the
  max is 6. The self-test mutates each branch (6→3, 2→0, camera-move→2) and the
  wrong-timescale case, and confirms every mutation is detected.

## Runtime verification

- **Build**: `DemonCore-Editor.exe` rebuilt clean (`MSBUILD_RC=0`, exe 0.6 min).
- **Timing line**: a pinned-camera run logged `WLFG timing: factor 1 warps 0
  realFrames 380 simDrift 0.0ms`, the counter climbing 380→396 — the inert
  baseline (factor 1, zero warps, zero drift) the task promises.
- **Frame cap**: uncapped rendered **21** frames in a 6s window; capped at 2fps
  (500ms/frame) rendered **11** in the same window — the cap throttles as
  designed, and 11 ≈ 6s / 0.5s confirms the interval is honoured.
- **No regressions**: WLAA, RCAS, and the Nova binding-budget verifiers all still
  pass.

## Finding — a corrupted include in the user's uncommitted EditorLayer.cpp

Building surfaced `EditorLayer.cpp:5` as `#include <cstdio> d/Renderer/
RenderCapabilities.h"` — a malformed line from an in-progress edit that merged
two includes and lost the `"Wasteland/` prefix, so `Wasteland::RenderCapabilities`
was undefined at line 1985. My `Application.h` change forced EditorLayer.cpp to
recompile and exposed it (incremental builds had been skipping the file). Fixed
surgically to two clean lines (`#include <cstdio>` + `#include "Wasteland/Renderer/
RenderCapabilities.h"`) — a baseline restore was refused deliberately, since the
file carries the user's own `RenderCapabilities` ImGui work.

## Cleanup still owed

`WL_MASK_VIS`/`u_MaskVisualize` and the `WL_TEX_DEBUG` block remain in tree from
earlier tasks (debug scaffolding). Decide keep-or-remove before declaring the
whole reconstruction + WLFG stack final.
