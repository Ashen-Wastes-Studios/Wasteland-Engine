# Task 3 — WLSR EASU + reactive mask: evidence

**Date:** 2026-10-26
**Status:** DONE — the reactive mask now changes the EASU output measurably.

## What task 3 actually was

The plan assumed WLSR needed an EASU written from scratch. **That premise was
wrong.** A complete, documented FSR1 EASU already existed in both twins
(`RunFSR1EASU`, ~12 tap edge-adaptive anisotropic lanczos-2), reachable via
`ReconstructionFilter::FSR1` (value 8) inside `RunComposite`. Its own comment
records that RCAS was deliberately omitted for want of a scratch image — and
task 2 had just created exactly that spare capacity (the mask channel).

So the real gap was narrow: **the EASU read no reactive mask.** Task 3 wires the
mask into the EASU's branch, blending toward bilinear where the mask says the
current frame cannot be trusted.

## Acceptance

Same scene, same frozen clock, same pinned camera, `WL_AA_FILTER=8`:

| arm | env |
|-----|-----|
| unmasked | `WL_RECONSTRUCTION` unset |
| masked | `WL_RECONSTRUCTION=1` |

```
2332x1451  3383732 pixels
tolerance      : 0 (8-bit units per channel)
differing      : 1535911 (45.391036%)
max delta      : 8
mean delta     : 1.0429
bbox           : x 0..2331, y 0..1450
RESULT: DIFFERENT (1535911 pixels)
```

45.4% of pixels change when the mask is enabled. The mask is a genuine partial
blend, not a saturation: it is neither 0 everywhere (which would leave the EASU
untouched) nor 1 everywhere (which would replace it with bilinear).

Captures: `FINAL_unmasked.png`, `FINAL_masked.png`, report `FINAL.txt`.

## The bug that cost the most time: a stale exe

Every "test" between 02:58 and 04:14 ran a **50-minute-old executable**. The
build printed `MSBUILD_RC=0` and copied post-build Python files, but compiled
and linked nothing.

Cause: I had been passing `-p:BuildProjectReferences=false` to `tools/msb.sh`.
That flag skips the referenced `Wasteland` project entirely, so C++ changes
never compile — and MSBuild still reports success.

The giveaway was a diagnostic `WL_CORE_INFO` log that never appeared. The code
was correct; the binary was old.

**Check after every build here:**

```bash
python3 -c "import os,time;p='bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe';print(f'exe age: {(time.time()-os.path.getmtime(p))/60:.1f} min')"
```

An age over ~1 minute after a "successful" build means it did not relink.

## Two real defects found while chasing it

### 1. The OpenGL path tracer is a separate schedule

`Renderer3D::Flush()` tries DX11 Nova, DX12 Nova, Vulkan Nova, the NVRHI raster
fallback, then a **hand-written OpenGL path tracer inline in `Flush()`**.
`RunNovaPassSchedule` is used only by the three Nova backends.

Pass 24 existed only in `RunNovaPassSchedule`, so it never ran on OpenGL. It now
has its own dispatch in the GL block, positioned after pass 7 (the last writer of
`img_Bloom_Temp`) and before pass 6 (the consumer).

### 2. The `WL_RECONSTRUCTION` override was backend-specific

It lived inside `RunNovaPassSchedule`, so on OpenGL the flag was never read from
the environment and stayed at its default `false`. It now sits in
`SceneInstancesChanged()` beside `WL_AA_FILTER` and `WL_DRS_OFF`, which every
backend reaches.

## Verifier

`tools/verify_reactive_mask_budget.py` now enforces the mask's dispatch position
as a **window** (after every writer of the image, before every declared
consumer) rather than a simple ordering, and registers **pass 6** as a consumer —
that omission is what left the stale-read bug invisible.

`--self-test`: 7 mutations, all detected.

```
  ok   stray read of the mask channel
  ok   mask writer not dispatched
  ok   pass arm missing from one twin
  ok   mask writer writes the wrong image
  ok   dispatched consumer arm missing from one twin
  ok   mask writer dispatched before a later writer of the same image
  ok   consumer dispatched before the mask writer (stale mask read)
RESULT: PASS -- all six reactive-mask mutations were detected
```

Full verifier suite: green.

## Also fixed en route

- **HLSL `float2(u_RenderScale)` is invalid.** GLSL accepts `vec2(scalar)`
  broadcast; DXC rejects it with *"too few elements in vector initialization
  (expected 2 elements, have 1)"*. This broke the Vulkan build and left the
  `.spv` stale, so Vulkan silently ran an older shader. Fixed to
  `float2(u_RenderScale, u_RenderScale)`.
- **`tools/compile_nova_spirv.py --deploy` is a manual step.** The build does not
  regenerate the `.spv`. Re-run it after every HLSL edit.

## Methodology lessons

- **Never compare two different filters to prove a branch runs.** Comparing
  filter 2 against filter 8 always differs; that proves nothing. Use a forced
  constant inside the branch, or read the value back.
- **A guard must not fail on pending work** — the verifier flags only
  *dispatched* consumers, so it stays green while passes 8/15/16 are
  declared-but-undispatched.
- **`WL_FREEZE_TIME` is mandatory in every A/B.** `WASTELAND_PIN_CAMERA` pins the
  initial pose only; without freezing the clock the two arms render different
  scenes.
