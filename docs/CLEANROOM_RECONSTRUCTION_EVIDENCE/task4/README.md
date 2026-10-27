# Task 4 — WLSR RCAS sharpen stage (pass 15)

Clean-room contrast-adaptive sharpen for the WLSR reconstruction stack. Repurposed
the dead pass 15 (a complete but undispatched `RunTSRResolve`) into `RunWlsrRcas`,
dispatched from both the Nova schedule (DX11/DX12/Vulkan) and the hand-maintained
OpenGL path-tracer schedule, gated default-off.

Date: 2026-10-26.

## What was built

A 5-tap unsharp-mask sharpen (centre + four diagonals) in both shader twins:

    vec3 RunWlsrRcas(int2 pos, int2 size, float sharpness) {
        // centre + four diagonal taps, clamped to the image bounds
        ...
        vec3 contrast = mx - mn;                                  // local range
        vec3 gain = contrast * clamp(sharpness,0,1)
                          * (1 - clamp(mask,0,1));                // trust term
        gain = clamp(gain, 0, RCAS_MAX_SHARPEN);                  // anti-ring bound
        vec3 blur = (t00+t10+t01+t11) * 0.25;
        return clamp(ctr + gain * (ctr - blur), 0, 1);            // unsharp push
    }

Design points, each of which is a failure mode the verifier pins:

- **Five taps, diagonals.** The axis neighbours are already exercised by the
  upscale's own kernel, so the sharpen lives on the diagonals.
- **Gain = local contrast, not a fixed slider.** A flat region has `mx - mn` near
  zero and gains nothing; an edge gains a lot. A fixed gain would sharpen flat
  areas into noise.
- **`1 - mask` trust term.** Where the reactive mask (pass 24) says the frame is
  a single frame's worth of data, sharpening is *reduced*, never increased.
  Backwards (`× mask`) would sharpen exactly the disocclusion pixels that are
  least reliable.
- **Gain clamp + result clamp.** Two bounds: the gain clamp stops a bright texel
  dragging its neighbours past the local range (the halo on a hard edge); the
  result clamp catches the residual push on a strong edge.
- **Gated, default-off.** `u_RCSEnabled` in the shader, `s_Data.RCASEnabled` on
  the CPU, env `WL_RCAS` / `WL_RCAS_STRENGTH` — belt-and-braces, inert unless set.

## Acceptance

The plan's bar: *a scale-0.5 render with EASU+RCAS must have a higher p99 local
contrast than the same render with EASU alone, and no visible ringing on a
high-contrast edge.*

Captured at 2332×1451, scale 0.5, `WL_AA_FILTER=8` (EASU), `WL_RECONSTRUCTION=1`,
`WL_FREEZE_TIME=10.0`, `WL_DRS_OFF=1`, `WASTELAND_PIN_CAMERA=0,0,0,9,-85,0`,
frame 120:

| render | p99 local contrast | differing px vs no-RCAS |
|--------|--------------------|-------------------------|
| EASU only (no RCAS) | 7.571 | — |
| EASU + RCAS (strength 0.6) | 7.639 | 47,520 / 3,383,732 (1.40%) |

p99 contrast **rises** (7.571 → 7.639) — the sharpen raises local contrast rather
than blurring. RCAS changes 1.40% of pixels, max delta 8, mean delta 1.04 — a
measurable, bounded effect, not a no-op and not a runaway.

## The reorder this task earned

The reactive-mask budget verifier flagged a real HAZARD: pass 15 (RCAS) read
`img_Bloom_Temp.a` — the mask — but the bloom blur (pass 4/5) *also* writes
`img_Bloom_Temp`, and it was dispatched **between** the mask writer (pass 24) and
the RCAS consumer. With bloom on, RCAS would read the bloom's alpha, not the
mask, and the trust term would be a silent no-op.

Fixed by dispatching RCAS **before** the bloom chain in both schedules. This is
also the artistically correct order: bloom is additive light, and sharpening it
would halo around bright sources. The bloom is now computed from the sharpened
scene and added on top.

The A/B captures above were re-run after the reorder (no_rcas4 / with_rcas4); the
output is identical to the pre-reorder captures because bloom is off in this
config, so the reorder is a no-op here but correct when bloom is on. Both budget
verifiers now PASS.

## Verification

    tools/verify_rcas_sharpen.py            # 23/23 static, both twins
    tools/verify_rcas_sharpen.py --pngs A B # 25/25 incl. p99-contrast acceptance
    tools/verify_rcas_sharpen.py --self-test# 6/6 mutations detected
    tools/verify_nova_binding_budget.py     # PASS (pass 15 removed from DEAD_PASSES)
    tools/verify_reactive_mask_budget.py    # PASS (mask not clobbered before read)

### Self-test mutations (each detected)

1. gain clamp removed — unbounded gain rings.
2. mask direction inverted — sharpens disocclusions.
3. contrast term dropped — fixed gain sharpens flat areas into noise.
4. pass 15 arm removed — stage never dispatched.
5. result clamp removed — edge push escapes the range.
6. unsharp form broken (`ctr + gain*blur`) — no longer raises contrast.

## Files

- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl` — `RunWlsrRcas`,
  `case 15`, `u_RCSEnabled`, entry point.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl` — twin.
- `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.spv` — regenerated.
- `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp` — `Loc_RCSEnabled`, upload,
  `WL_RCAS` / `WL_RCAS_STRENGTH` overrides, Nova + GL dispatch (before bloom).
- `Wasteland/src/Wasteland/Renderer/Renderer3D.h` — accessors + struct fields.
- `tools/verify_rcas_sharpen.py` — static + measured + self-test verifier.

## Build

Release x64, `MSBUILD_RC=0`, exe fresh (0.0 min). Active backend DirectX 12.
