# Nova shader pipeline — twin maintenance, and one rejected approach

Status as of 2026-10-05. **The two-twin arrangement is retained.** A shared-source
experiment was tried and rejected on evidence; the details are here because the
failure mode is instructive, not because the idea is wrong in principle.

## Current state

| File | Lines | Role |
|---|---|---|
| `Renderer3D_NovaRenderer.glsl` | 4443 | OpenGL path |
| `Renderer3D_NovaRenderer.hlsl` | 2760 | DX11 + DX12, and HLSL→SPIR-V for Vulkan |

Both compile; `tools/verify_nova_glsl.py` cross-checks that they agree on the
temporal/jitter/mip behaviours, and `tools/verify_nova_glsl_negative_control.py`
proves those guards fail when the defects are reintroduced.

The GLSL is larger because it carries the in-progress POM/parallax rewrite, which
the HLSL does not yet mirror. **That asymmetry is intentional and temporary.**

## Why there is no shared source today

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
becomes legitimate and the win is real for macro-expressible passes.

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
  **zero POM references survived in it** — the POM rewrite was never in that file.
- `git checkout` on the HLSL was therefore safe *and* correct: HEAD already contains
  a working `ApplyPOM` implementation (3 hits, plus `marchLOD`/`marchK`). The
  GLSL-side rewrite is a refinement of it, not an invention. **The HLSL POM work was
  lost and must be re-applied by hand; it was not in HEAD and not in the backup.**
- The GLSL was repaired in place — three targeted fixes — and kept **all** of its
  content, including the full POM rewrite.
- All four temporal/jitter/mip fixes were re-applied to the restored HLSL and are
  verified present in both twins.

### The lesson worth keeping

**`BUILD_EXIT=0` says nothing about the shaders.** MSBuild has zero references to
them (`grep -ci "NovaRenderer\|\.hlsl\|\.glsl" Wasteland/Wasteland.vcxproj` → 0), so
the engine built green the entire time both twins were broken. The shader verifiers
are the only gate, which is why they must be run after *every* shader edit — and why
"the build passed" must never be reported as evidence that a shader change is sound.

Second lesson: **back up before you refactor a file whose contents are uncommitted.**
The GLSL POM rewrite exists only in the working tree. A `git checkout` on that file
would have destroyed it silently, and nothing in git would have said so.
