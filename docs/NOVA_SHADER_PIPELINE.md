# Nova shader pipeline — twin maintenance, and one rejected approach

Status as of 2026-10-26. **Transitioned to Slang.** The two-twin arrangement
is replaced by a single `Renderer3D_NovaRenderer.slang` source compiled to
DXIL (DX12), DXBC (DX11), SPIR-V (Vulkan), and GLSL (OpenGL) via `slangc`
(Vulkan SDK 1.3.296.0). The dual-authoring problem — and the 5.33-luma
regression the twin model caused on 2026-10-05 — is eliminated by
construction. `tools/verify_nova_shader_parity.py` (the twin token-comparison
gate) is deleted; `compile_nova_spirv.py` is replaced by
`compile_nova_slang.py`. The GLSL/glslang verification path
(`verify_nova_glsl.py`) remains as the only GLSL *behavior* gate.

See `.hermes/plans/2026-10-29_180000-slang-transition.md` for the full
transition plan and task list.

## Mesh-shader path (Phase 2, added 2026-11-08)

The visibility buffer has two producers that write the identical G-buffer (meshId /
albedo / normal / depth), so the Nova compute schedule cannot tell them apart:

| producer | source | stages | status |
|---|---|---|---|
| classic raster | `Renderer3D_NovaVisibility.slang` | VS + FS | working, both backends |
| mesh pipeline | `Renderer3D_NovaMesh.slang` (Vulkan) + `.hlsl` (DX12) | MS + PS (+ AS on DX12) | compiles, not yet drawn |

### Why there are two mesh sources

Slang's DXIL emit for the mesh stage is broken. It lowers

    out vertices MeshVert verts[64]

to

    out vertices inout MeshVert verts_0[int(64)]

which dxc rejects with `duplicate HLSL parameter usages 'out'`. The SPIR-V emit is
fine. So Vulkan comes from the `.slang` and DX12 from the `.hlsl`; the two files must
stay semantically identical and nothing enforces that. The shared contract is the
G-buffer, which is what downstream reads.

### Amplification shader — resolved, not skipped (Task 2.4)

The open question was how to declare the AS payload. None of the four forms the plan
listed works:

| form | dxc result |
|---|---|
| `struct ASPayload { uint meshlet; };` as an `out` parameter | `Semantic must be defined for all outputs of an entry function` |
| `... : PAYLOAD;` | `Semantic PAYLOAD is invalid for shader model: as` |
| `... : MESHLET;` | `Semantic MESHLET is invalid for shader model: as` |
| `... : SV_DispatchMeshID;` | invalid for `as` |

The `out` parameter itself is the problem: dxc treats it as a shader output needing a
semantic, and no payload semantic is valid for `as`. **The working form is a local
struct passed by value to `DispatchMesh`**, exactly as dxc's own test suite does
(`DirectXShaderCompiler/tools/clang/test/CodeGenHLSL/mesh-val/multipleDispatchMesh.hlsl`).
`Renderer3D_NovaMeshAS.hlsl` compiles clean at `as_6_5`.

**`DispatchMesh` must be called exactly once.** A loop over the meshlet count fails
validation with `Non-Dominating DispatchMesh call`. So the CPU dispatches the AS once
per meshlet and each invocation dispatches one mesh group; `tid.x` is the meshlet index
and the dispatch count, not a loop, is what covers the mesh.

### Constants duplicated across three languages

The meshlet budget (64 vertices / 124 triangles) lives in `MeshletBuilder.h`, and is
restated in both shader sources because neither Slang nor dxc can see a C++ header.
**Nothing checks that the three copies agree.** A meshlet built with a different
budget than the shader's array sizes would read past the array.

## Current state

| File | Lines | Role |
|---|---|---|
| `Renderer3D_NovaRenderer.glsl` | 5984 | OpenGL path |
| `Renderer3D_NovaRenderer.hlsl` | 4344 | DX11 + DX12, and HLSL→SPIR-V for Vulkan |

Both compile; `tools/verify_nova_glsl.py` cross-checks that they agree on the
temporal/jitter/mip behaviours, and `tools/verify_nova_glsl_negative_control.py`
proves those guards fail when the defects are reintroduced. The neural stage has
its own, stronger check: `tools/verify_neural_stage_parity.py` asserts
statement-level parity for passes 17/18/19 specifically, because those three are
the ones whose constants are part of a wire ABI (see below).

The GLSL is larger because it carries the in-progress POM/parallax rewrite, which
the HLSL does not yet mirror. **That asymmetry is intentional and temporary.**

## Pass inventory

`u_PassID` selects one function per dispatch. Both twins carry the same cases.

| pass | function | GL dispatch |
|---|---|---|
| 0 | `RunVisibilityAndVelocity` | yes |
| 1 | `RunTraceAndDenoise` | yes |
| 2 | `RunTemporalAccumulation` | yes |
| 3 | `RunRestirGenerate` | yes |
| 4 | `RunBloomBlurHorizontal` | yes |
| 5 | `RunBloomBlurVertical` | yes |
| 6 | `RunComposite` | yes |
| 7 | `RunBilateralBlur` | yes |
| 8 | `RunUpscale` | **never dispatched** |
| 9 | `GenerateMaterialMaps` | yes, but **broken** — see below |
| 10 | `RunGodRays` | yes |
| 11 | `RunRestirCombine` | yes |
| 12 | `RunTemporalMoments` | yes |
| 13 | `RunATrousFilter` | yes |
| 14 | `RunRestirSpatial` | yes |
| 15 | `RunTSRResolve` | yes — **WLSR RCAS sharpen** (clean-room stack) |
| 16 | `RunTSRCopy` | yes — **WLAA** native-resolution AA sharpen |
| 17 | `RunNeuralReduce` | GL only, when the neural stage is enabled |
| 18 | `RunNeuralEval` | GL only, when the neural stage is enabled |
| 19 | `RunNeuralReduceFinal` | GL only, when the neural stage is enabled |
| 20 | `RunWlrrSnapshot` | **Nova schedule only** — WLRR pre-SVGF snapshot |
| 21 | `RunWlrrComposite` | **Nova schedule only** — WLRR residual correct |
| 22 | `RunWlfgWarp` | **Nova schedule only** — WLFG reprojection warp |
| 23 | `RunWlfgSynthesis` | **Nova schedule only** — WLFG neural synthesis head |
| 24 | `RunReactiveMask` | **Nova schedule only** — reactive mask for the upscaler |

Passes 20–23 are guarded `#if !defined(WASTELAND_DXIL) && !defined(WASTELAND_GLSL)`:
they compile to the SPIR-V target only, so the DXIL contains no WLRR symbol. Pass
17–19 are the opposite — they are dispatched by `glDispatchCompute` in
`Renderer3D::Flush()`, never through the Nova schedule, so they are GL-only.

### Written but never dispatched: 8

Pass 8 (`RunUpscale`) exists in the shader and is reachable only by setting
`u_PassID` directly. Nothing dispatches it on any backend. The clean-room stack
lands its upscale in pass 15's kernel family instead — see
`docs/CLEANROOM_RECONSTRUCTION.md` for why pass 8 was not given a body.

The risk is not the dead code itself; it is that a future reader can see `case 8`
in the shader, assume an upscale is live, and tune the wrong thing.

Passes 15 and 16 were previously listed here as well; they are now dispatched (as
WLSR and WLAA respectively) and their "never dispatched" status is historical.

### Pass 9 is dispatched and broken

`GenerateMaterialMaps` is the offline normal/roughness bake. It is reachable from
script (`Renderer3D_GenerateMaterialMaps`) and from the editor, so unlike 8/15/16
it *does* run — and it is wrong in two independent ways:

1. **`u_PassID` is never set.** The function binds the program, pushes
   `u_NormalStrength` and `u_RoughnessBias`, and dispatches. `u_PassID` still holds
   whatever the previous dispatch left in it, so the bake runs an arbitrary pass.
   There is no `glUniform1i(..., 9)` anywhere on the path.
2. **It binds image unit 8, which does not exist.** `GL_MAX_IMAGE_UNITS` is **8** on
   the target GPU, so valid units are 0..7 and the bind is silently a no-op. The
   GLSL declares `img_MaterialPacked` at `binding = 3`, not 8, so the shader writes
   to a unit that is not the one the CPU bound.

There is a third defect already documented in place: the dispatch uses truncating
`width / 8` instead of `(width + 7) / 8`, dropping up to 7 rows or columns.

None of this is fixed here; it is recorded so the pass table is not read as "pass 9
works".

### The neural stage is an ABI, not just a shader

Passes 17/18/19 carry the trained model's constants. The eight standardisation
values in `model.py` are duplicated into **both** twins, and the frame mean, the
weight blob layout, and the `NeuralGlobals` element offsets are shared by the GL
SSBO, the HLSL `RWStructuredBuffer<uint>` and the DX12 UAV descriptor.

That is why `tools/verify_neural_stage_parity.py` exists separately from
`verify_nova_glsl.py`: a mismatch in a temporal-jitter constant produces a slightly
different image, while a mismatch in a neural constant produces a **confident,
plausible, wrong** image. The parity tool is the guard; the constants are the thing
it guards.

## Why there was no shared source — and why Slang resolves it

**RESOLVED (2026-10-26):** Slang resolves both blockers that made shared
source fail as a `#include`-based probe a year ago.

A probe moved `RunBilateralBlur` into a shared `NovaShared_Bilateral.h` with thin
per-language macro shims. It worked mechanically — 27 non-comment lines per wrapper,
inside the ~40-line budget. It was **rejected** for two independent reasons:

1. **The GLSL half needed `GL_GOOGLE_include_directive`, which the engine cannot
   supply.** `OpenGLShader::PreProcess` (`Platform/OpenGL/OpenGLShader.cpp:155`)
   only splits the source on its `#type` markers and hands the text to
   `glShaderSource`; it expands no includes and resolves no include paths. The
   shader would have compiled only where the driver happens to implement the
   extension — ANGLE does, many desktop drivers do not. Meanwhile
   `tools/verify_nova_glsl.py` copies the include into its temp dir and **still
   passes**. That is the worst failure mode available: green verification, broken
   OpenGL backend.

2. **The verification gain was illusory where it mattered.** The twin guard keys on
   `validHistory`, `histUV`, `mipLevel` and `GetJitter`. A *syntax* error in a shared
   file is caught; a **silent semantic change** (`exp(-lumaDiff * 4.0f)` → `400.0f`)
   passes with rc=0. One source of truth is not a guarantee if nothing guards it.

Marginal cost either way: ~150–250 duplicated lines per new pass, versus ~56 for a
macro-expressible one — and *only* for self-contained passes. Anything using SSBOs,
dynamic sampler indexing, or shared memory is not convertible at all; the twins
already diverge there.

**To revisit this, one change is needed first:** make `OpenGLShader::PreProcess`
expand `#include` relative to the shader's own directory. Then the GLSL half

→ **Resolved by Slang.** `slangc` resolves all `#include`s at compile time
and produces GLSL text that `OpenGLShader::PreProcess` consumes unchanged.
The `EXPENSIVE` semantic-change failure mode (§2) is still a concern — but
Slang's `-warnings-as-errors` plus `-entry` per-entry-point catches divergences
that the old twin guard could not. See `docs/NOVA_SHADER_PIPELINE.md` top for
the resolved status.

The old "to revisit" advice — `OpenGLShader::PreProcess` expanding `#include` — is moot:
Slang produces fully-flattened GLSL at build time, so the runtime path needs no changes.

## Incident record — 2026-10-05 shader damage and recovery

A botched in-place revert of that probe **destroyed the HLSL twin** (2724 → 344
lines; six of ten pass bodies lost) and **corrupted the GLSL** (a missing brace, a
160-line duplicated tail, and a second missing brace). Cause: brace-matching
surgery driven by a Python script whose boundary logic was wrong, applied in
several passes without re-verifying between them. Each fix moved the problem rather
than solving it.

Recovery, and what it cost:

- The damaged HLSL was **backed up before any further edit** (three locations under
  the scratch/temp/Documents dirs, sha256-verified). Inspecting it then showed
  **zero POM references survived in it**.
- `git checkout` on the HLSL restored HEAD, which **already contained the complete
  POM rewrite** — `strataW`, `neuralK`, `mesoAmp`, `wl_pom_full`, the shadow march
  with `rayRise`/`selfOcclusion`. **No POM work was lost.** An earlier version of this
  document claimed it was, on the strength of `grep -c ApplyPOM` returning 5; that was
  a misreading of a partial match count, not evidence. The construct-level comparison
  (identical counts on both twins) is the check that should have been run first.
- The GLSL was repaired in place — three targeted fixes — and kept **all** of its
  content, including the full POM rewrite.
- All four temporal/jitter/mip fixes were re-applied to the restored HLSL and are
  verified present in both twins.

**The real remaining asymmetry is small and specific.** The GLSL's `ApplyPOM` is 164
code lines against the HLSL's 102. The HLSL lacks the `uvDelta` UV-offset blend and its
`0.15` hard clamp (`marchK`-scaled, with a grazing blow-up rail) — present in the GLSL
at its `wl_pom_full`/`ApplyPOM` boundary and absent entirely from the HLSL (`grep -c
uvDelta` → 4 vs 0). That is a behavioural difference in grazing-angle parallax, not a
missing subsystem, and it is the concrete item to port.

### The lesson worth keeping

**`BUILD_EXIT=0` says nothing about the shaders.** MSBuild has zero references to
them (`grep -ci "NovaRenderer\|\.hlsl\|\.glsl" Wasteland/Wasteland.vcxproj` → 0), so
the engine built green the entire time both twins were broken. The shader verifiers
are the only gate, which is why they must be run after *every* shader edit — and why
"the build passed" must never be reported as evidence that a shader change is sound.

Second lesson: **back up before you refactor a file whose contents are uncommitted.**
The GLSL POM rewrite exists only in the working tree. A `git checkout` on that file
would have destroyed it silently, and nothing in git would have said so.
