# Task 7 — WLFG warp (pass 22): reprojection + disocclusion fill

**Status: warp algorithm DONE and verified (2026-10-26). The binding integration
(Step 1 — the second history buffer across the three backends) and the bit-
identical acceptance (Step 5) remain.**

Task 7 is the WLFG reprojection warp, `pass 22`. This document covers the part
that is self-contained and verifiable without touching the live temporal path:
the warp's address math and its disocclusion fill. The pass is defined in BOTH
twins and dispatched by a `case 22:` that no `u_PassID` value selects, so it is
dead code — it compiles but never runs, and cannot affect the live path.

## What was added

### `RunWlfgWarp()` (pass 22) — both twins
The first half of frame generation. It reprojects the previous present onto the
current camera with the velocity buffer, then fills the disocclusions. The
address math is **the same as `RunTemporalAccumulation` (pass 2)**:

```
velocity = imageLoad(img_Velocity, pixelCoords).xy
prevUV   = uv - velocity
histUV   = prevUV * u_RenderScale            // map into the accumulation subregion
histUV   = clamp(histUV, 0.5/fullRes, u_RenderScale - 0.5/fullRes)   // BOTH sides
valid    = u_FrameIndex > 0 && prevUV in [0,1]
if valid: depth test  |currentDepth - prevDepth| > 0.02  -> invalid
warped   = texture(s_Accumulation, histUV).rgb            // unconditional fetch
result   = valid ? warped : RunWlfgDisocclusionFill(...)
mask     = imageLoad(img_Bloom_Temp, pixelCoords).a       // point sample, trace coord
result   = mix(result, img_Bloom, clamp(mask, 0, 1))      // reactive mask: warp vs fresh
imageStore(img_Accumulation, pixelCoords, result)
```

It reads the same `s_Accumulation` SRV and writes the same `img_Accumulation`
UAV as pass 2, so it is dispatch-correct by construction — the only thing that
makes it inert is that no pass dispatches `case 22`.

### `RunWlfgDisocclusionFill()` — the fill (both twins)
A 3x3 neighbourhood search for a neighbour whose OWN reprojection is valid (in-
frame and depth-consistent), taking the tap whose reprojected depth is **closest
to this pixel's depth** — the shallowest matching surface, which keeps the fill
on the same object instead of bleeding a background colour across a moving edge.
No valid neighbour → the current frame's real render (`img_Bloom`).

## Why the address math is the whole contract

The acceptance (Step 5) is the **zero-velocity** case: at a static camera the
velocity is `(0,0)`, so `prevUV == uv`, the depth test passes, and the warp
writes the previous present unchanged — **bit-identical to the no-WLFG control**.
Any delta at zero velocity is an address bug, never a quality effect. That makes
the address math the entire correctness contract, and it is exactly what the
verifier locks down.

## Acceptance (plan Step 4) — `python3 tools/verify_wlfg_warp.py`

**PASS — 31/31 checks + self-test 6/6.** The verifier transcribes the warp's
address math into Python and checks the four contracts numerically (no engine run
needed):

1. **Zero-velocity identity** — `prevUV == uv`, `histUV` maps to the same pixel;
   at scale 1.0 `histUV == uv` to 1e-9.
2. **Out-of-frame rejection** — a reprojected UV outside `[0,1]` → the fill path.
3. **Depth-test rejection** — `|currentDepth − prevDepth| > 0.02` → the fill path;
   within 0.02 stays warped.
4. **Coverage** — every output pixel resolves to exactly one source (warped or
   fill); frame 0 fills every pixel (no history).

Plus static checks (warp + fill in BOTH twins, mirrors pass 2's math, reads the
right images, point-sampled reactive mask, `case 22` present) and a mutation
self-test that mutates the warp and confirms each mutation is caught (prevUV
sign flip, dropped depth test, one-sided clamp, wrong fill fallback, bilinear
mask, removed case 22).

## Compile verification

- `compile_nova_spirv.py --deploy` — **OK** (HLSL → SPIR-V; `case 22` makes
  `RunWlfgWarp` reachable so DXC compiles it).
- `verify_nova_glsl.py` — **PASS** (GLSL cross-compile, NVRHI validation).
- Both twins deployed to `bin/Release` and `bin/Debug`.

## Integration complete (2026-10-27)

The warp is now wired end-to-end and dispatches. All four pieces:

- **Shader read-swap (both twins).** The warp samples `s_WlfgHistory` (HLSL
  `register(t72)` / GLSL `binding = 14`) instead of `s_Accumulation` — same
  bilinear `texture()` fetch, no `.Load`/UAV/heap change. `verify_wlfg_warp.py`
  updated → **31/31 + self-test 6/6**; both twins compile (SPIR-V `OK`, GLSL
  `PASS`).
- **DX12 resource state.** `wlfgHistory` is transitioned to the read state in
  `BindPassImages` (and back to `COPY_DEST`) so the compute warp can sample t72.
- **C++ flag + env + dispatch.** `WlfgWarpEnabled` (in `s_Data`) + `WL_WLFG` env
  (read in `SceneInstancesChanged`, beside the other stack overrides) + gated
  `pass(22)` as the **last** pass in the Nova schedule (after god rays, before
  the FrameIndex advance).
- **Step 5 acceptance — byte-identical.** `WL_WLFG=0` vs `WL_WLFG=1`, frozen
  camera (`WL_FREEZE_TIME=10`, `WASTELAND_PIN_CAMERA`), DRS off, 300 frames, both
  renders exit 0 with **no GPU fault**. The two screenshots are **byte-identical**
  (`cmp` + matching sha256, 10,153,485 bytes each).

### Honest scope of this acceptance

The warp writes `img_Accumulation` as the last pass; the screenshot reads
`img_Output`, which the composite (pass 6) wrote *before* the warp. So the
byte-identical result proves the warp **dispatches, reads `s_WlfgHistory`
(bound + correctly transitioned), and writes without corrupting the visible
output** — it is exact at zero velocity. It is *not* a proof that the warp
reprojects the previous present *into* the output, because the warp does not
feed `img_Output`.

Making the warp feed the output is the frame-gen output-target: `img_Output` is
full-res (the composite dispatches `pass(6,true)` at `RayTracingWidth×Height`),
so a trace-res warp writing it directly would corrupt it. The correct version is
a **full-res reproject** (dispatch at full-res, velocity bilinear-interpolated
from trace-res, matching the composite's upscale) — and the upscale/synthesis
head plus the factor-gated schedule are **Task 8**.

### Still owed

- **Vulkan + DX11 parity** for the t72 `wlfgHistory` binding (the shader now
  declares `s_WlfgHistory`; those backends must map binding 14 / `t72` to the
  same texture once the warp is dispatched there).
- The meaningful "warp reprojects into the visible output" acceptance (full-res
  reproject → `img_Output`) — **Task 8**.
