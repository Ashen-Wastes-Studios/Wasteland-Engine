# Task 5 — WLAA: native-resolution anti-aliasing

**Status: DONE (2026-10-26).** Native-resolution AA at `ReconstructionFilter::Wlaa = 12`:
the temporal accumulation (pass 2) plus the RCAS unsharp mask, folded into the
full-resolution branch of `RunComposite` so the mode is one pass, not a composite
followed by a separate sharpen stage. Gated on `WL_AA_FILTER=12`, default off.

## Design

WLAA is DLAA-class: TAA (jitter + accumulate + temporal filter) reconstructs the
full-resolution image, and a mild unsharp mask restores the edge crispness the
temporal filter softens. It is NOT a new pass ID — it is a real branch inside
`RunComposite`'s full-resolution `else` arm, gated on `u_ReconstructionFilter == 12`.

The sharpen is the **same image-free core** pass 15 (WLSR RCAS) uses:

    vec3 RunWlsrSharpenCore(vec3 ctr, vec3 t00, vec3 t10, vec3 t01, vec3 t11,
                            float sharpness, float mask)

Both callers load their own five taps and hand the colours in. Pass 15 reads
`img_Output`; the WLAA branch reads `img_Accumulation` (the resolved temporal
result — exactly what a native-res AA mode reconstructs). The mask is read at the
trace coordinate from `img_Bloom_Temp`, same as pass 15.

Why the core takes no image: under NVRHI's bindless transform, GLSL rewrites
image-typed uniforms into handle types it will not implicitly convert to a
function parameter, so an image-typed parameter does not compile. The image-free
core is the portable form; the tap loads stay inline in each caller (five trivial
`imageLoad`/`.Load` lines).

- Default amount `WLAA_SHARPNESS = 0.5f`; overridable via `u_TSRSharpenStrength`
  (the WLSR sharpen knob); clamped to `[0,1]` either way.
- The gain ceiling `RCAS_MAX_SHARPEN = 0.75` and the anti-ringing guarantees are
  inherited unchanged from the RCAS core, so WLAA cannot ring harder than WLSR.
- HLSL: the `#define`s and a forward `RunWlsrSharpenCore` prototype sit before
  `RunComposite` (HLSL resolves `#define`s textually); the core uses raw
  `.Load(int3(coord,0))` because the `IMG_LOAD` macro token-pastes a fixed name.

## Wiring

- `Renderer3D.h`: `Wlaa = 12` added to the `ReconstructionFilter` enum.
- Both twins: `RunWlsrSharpenCore` core; the `== 12` branch in `RunComposite`'s
  full-res `else`; forward prototypes + defines before `RunComposite`.
- `Renderer3D.cpp`: `WL_AA_FILTER` override now names every value 0–12 (the old
  log labelled unknown values "Bilinear (default)", so `WL_AA_FILTER=8/12` were
  unidentifiable). RenderScale is forced to 1.0 for the A/B via the existing
  `WL_CAPTURE_SCALE` env (`EditorLayer` → `SetRenderScale`, clamped 0.25–1.0); no
  quality preset reaches 1.0 (Ultra caps at 0.75).

## Acceptance (plan Step 4)

TAA-style (jitter + accumulate + bilinear, `WL_AA_FILTER=2`) vs WLAA
(jitter + accumulate + sharpen, `WL_AA_FILTER=12`), `RenderScale = 1.0`, camera
pinned (`WASTELAND_PIN_CAMERA="0,0,0,9,-85,0"`), `WL_FREEZE_TIME=10.0`,
`WL_DRS_OFF=1`, Ultra preset. Converged at 800 frames.

| arm | p99 local contrast |
|---|---|
| TAA @800 (bilinear) | 15.142 |
| WLAA @800 (sharpen) | **25.773** |

**WLAA − TAA p99 = +10.631 → PASS.** WLAA more than holds the temporal filter's
softness budget and restores crisp edges.

`mean_abs_diff(WLAA@800, TAA@800) = 1.727 / 255` over all 3,376,170 px. Because
both arms read the **same deterministic accumulation** (the filter only changes
the composite, never pass 2), this difference is noise-free — it is the sharpen's
true effect, not convergence noise. Bin the difference by magnitude:

- 91.67% of pixels change by < 4 (untouched — flat areas are clean);
- 8.21% change by 4–12 (the geometry edges — edge enhancement);
- 0.117% change by ≥ 12 (only the sharpest corners/peaks).

A vision pass on the noise-free difference confirms the yellow (4–12) band stays
**on** the edges with no symmetric spread into flat areas, and the red (≥12) sits
only at sharp corners — **no parallel halo bands, i.e. no ringing.** The raw
WLAA image at low frame counts looks haloed only because a sharpen amplifies the
scene's heavy path-tracing noise; that confound disappears once the accumulation
converges or is differenced out.

## Verifiers

- `tools/verify_wlaa_contract.py` (228 lines): static — enum value present on CPU
  and uniform declared in both twins; `RunComposite` has a REAL `== 12` branch
  (not a fall-through) that invokes `RunWlsrSharpenCore` with a clamped amount;
  the `WL_AA_FILTER` override accepts `Wlaa`. `--self-test`: 4/4 mutations
  detected (enum removed, branch value changed, sharpen call removed, clamp
  removed).
- `tools/verify_rcas_sharpen.py` still green after the shared-core refactor: the
  tap loads are checked in `RunRCAS`, the gain/clamp/unsharp/mask math in
  `RunWlsrSharpenCore`. Static PASS, self-test 6/6.
- `tools/measure_wlaa_ab.py`: p99 contrast + mean-abs-diff for the A/B.
- Binding budget PASS, reactive-mask budget PASS.

## Findings / defects caught

1. **Image-typed shader parameter does not compile under bindless GLSL.** The
   first WLAA implementation parameterized `RunRCAS(sampler2D/imgSrc, ...)`. The
   GLSL twin failed with `C1115: unable to find overloaded RunRCAS(image2D4x32_
   bindless, ...)` — NVRHI rewrites image uniforms into handle types that will not
   convert to a function parameter. Fixed by extracting the image-free
   `RunWlsrSharpenCore` and having both callers load their own taps.
2. **Stale pass-15 entry still passed the image as the first arg** after the
   parameter was dropped — caught by the runtime GLSL compile error at the entry
   (`RunRCAS(img_Output, ...)`). Fixed in both twins.
3. **The engine reads shaders from `bin/…/assets/shaders/`, not the source
   tree.** Editing the source `.glsl`/`.hlsl` without copying to `bin/` leaves the
   running build on the old shader (the SPIR-V is deployed by
   `compile_nova_spirv.py --deploy`, but the GLSL source is read from disk at
   runtime). Synced both bin trees.

## Artifacts

`taa400.png`, `wlaa400.png`, `ref800.png` (TAA@800 converged reference),
`wlaa800.png`, `taa2000.png` (timed out — per-frame cost grows with frame count,
cpuSubmit ~271 ms/frame dominates), `wlaa_minus_taa_diff.png` (400-frame
noise-free diff), `wlaa_minus_taa_diff800.png` (800-frame noise-free diff, the
ringing test), `smoke_*.png`.

## Cleanup still owed

`WL_MASK_VIS`/`u_MaskVisualize`/`Loc_MaskVisualize` (GLSL + C++) and the
`WL_TEX_DEBUG` block at `Renderer3D.cpp:6604` are temporary diagnostics still in
tree. The user offered to keep `WL_MASK_VIS` in-tree; decide before declaring the
reconstruction stack final.
