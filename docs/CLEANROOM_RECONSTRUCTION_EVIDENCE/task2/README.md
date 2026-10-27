# Task 2 evidence — the reactive mask pass (pass 24)

**Date:** 2026-10-26
**Commit:** pending
**Plan:** `.hermes/plans/2026-10-26_001250-clean-room-dlss-class-stack.md` task 2

## What was added

Pass 24 (`RunReactiveMask`), writing a per-pixel reactive/trust mask into the
**alpha** channel of `img_Bloom_Temp`, in both shader twins, dispatched from
`RunNovaPassSchedule` after pass 2 and gated on `s_Data.ReconstructionEnabled`
(with a `WL_RECONSTRUCTION` env override for headless verification).

## Acceptance: the image must be unchanged

The mask is stored in a spare channel that nothing reads. If that claim is true,
a run with the pass on must be **bit-identical** to a run with it off. That is
the test, and it passed.

```
WL_FREEZE_TIME=10.0 WL_DRS_OFF=1 WASTELAND_PIN_CAMERA="0,0,0,9,-85,0"
WL_SCREENSHOT_FRAME=120, both arms, Release x64, OpenGL
```

```
2332x1451  3383732 pixels
tolerance      : 0 (8-bit units per channel)
differing      : 0 (0.000000%)
max delta      : 0
mean delta     : 0.0000

RESULT: IDENTICAL
```

**Control — determinism.** Two runs with *identical* settings (both
`WL_RECONSTRUCTION` unset, time frozen) also match bit-for-bit:

```
2332x1451  3383732 pixels
differing      : 0 (0.000000%)
RESULT: IDENTICAL
```

Without this control the acceptance would prove nothing: an A/B that always
reports "identical" would pass just as readily. The control shows the harness
*can* detect a difference, and that the off/on match is a real result.

**Why the camera had to be pinned, and why pinning alone was not enough.** The
first attempt produced a 99.77% difference. The editor camera was pinned with
`WASTELAND_PIN_CAMERA`, but the two runs reported different cameras:

```
off: cam (0.0,1.9,-10.4)
on : cam (0.0,1.6,-8.9)
```

The pin fixes the editor camera's *initial* pose, not the simulation clock, so
the scene advanced differently in each run. `WL_FREEZE_TIME` (which
`Renderer3D.cpp:336` already implements for exactly this purpose) is what makes
the pair comparable: with it set, both runs report `cam (0.0,1.6,-8.9)`.

**This is a general hazard for every A/B in this plan.** Any comparison that
does not freeze time is measuring two different scenes. Task 3 onward must use
`WL_FREEZE_TIME` and `WL_DRS_OFF=1` in both arms.

## Captures

| File | Meaning |
|---|---|
| `frozen_off.png` | `WL_RECONSTRUCTION` unset — pass 24 not dispatched |
| `frozen_on.png` | `WL_RECONSTRUCTION=1` — pass 24 dispatched |
| `frozen_off2.png` | determinism control, same settings as `frozen_off.png` |
| `acceptance_report.txt` | off vs on |
| `control_report.txt` | determinism control |
| `ab_report.txt` | the INVALID first attempt, kept as evidence of the hazard above |

`recon_off.png` / `recon_on.png` / `control_off.png` are the pre-fix attempt and
are **not** a valid comparison.

## Verifier output

```
$ python3 tools/verify_reactive_mask_budget.py
Reactive-mask channel budget
  [GLSL] pass arms / dispatch / channel liveness
    pass 24 (RunReactiveMask): declared and dispatched
  [HLSL] pass arms / dispatch / channel liveness
    pass 24 (RunReactiveMask): declared and dispatched
    consumer pass 8 (RunUpscale): declared, not yet dispatched
    consumer pass 15 (RunTSRResolve): declared, not yet dispatched
    consumer pass 16 (RunTSRCopy): declared, not yet dispatched
  [parity] both twins declare 20 pass arms, identical sets

RESULT: PASS -- the mask's channel is written by its pass, read only by its consumers, and declared in both twins

$ python3 tools/verify_reactive_mask_budget.py --self-test
  ok   stray read of the mask channel
  ok   mask writer not dispatched
  ok   pass arm missing from one twin
  ok   mask writer writes the wrong image
  ok   dispatched consumer arm missing from one twin
RESULT: PASS -- all five reactive-mask mutations were detected
```

The three "not yet dispatched" consumers are **expected**, not hazards: passes
8/15/16 are the dead cases tasks 3-5 claim. The verifier deliberately does not
fail on a declared-but-undispatched consumer, because doing so would leave it
red for the entire duration of the plan.

## Full suite

```
verify_nova_glsl.py                     PASS
verify_nova_shader_parity.py            PASS
verify_nova_binding_budget.py           PASS
verify_nova_binding_budget.py --self-test-wlsr   PASS
verify_reactive_mask_budget.py          PASS
verify_reactive_mask_budget.py --self-test       PASS
verify_nova_constant_names.py           PASS
verify_vulkan_nova_contract.py          PASS
verify_texture_sampler_indexing.py      PASS
```

Build: `MSBUILD_RC=0` (Release x64, `DemonCore-Editor.vcxproj`).

## Two defects found and fixed while writing it

**1. The G-buffer's normal is in `.yzw`, not `.gba`.** The store at
`.glsl:3978` is `vec4(clamp(tndc*0.5+0.5), primary.normal * 0.5 + 0.5)` — NDC
depth in `.r`, packed normal in `.yzw`. The first version of pass 24 read
`.gba`, which fetches `(ny, nz, nx)`: a rotated normal with a plausible length,
so it would have produced a plausible-looking but wrong mask with no error.
Fixed in both twins.

**2. The verifier must not fail on pending work.** The first version flagged any
declared-but-undispatched consumer as a hazard. That is true of passes 8/15/16
today and would have made the verifier red until task 5 — a guard that is red
for four tasks is a guard that gets ignored. It now only flags a *dispatched*
consumer.

## Not verified

- **The mask's values.** This task proves the pass runs and is invisible. That
  the mask contains sensible numbers is proven by tasks 3-5, when passes 8/15/16
  read it and its effect becomes visible.
- **DX11/DX12/Vulkan.** Contract-verified only. The HLSL twin is written because
  it is what those backends consume, but nothing here executes on them.
