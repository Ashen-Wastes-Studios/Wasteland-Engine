#!/usr/bin/env python3
"""
Compile the Nova path-tracer shader from Slang to all four backend targets.

Replaces tools/compile_nova_spirv.py (which used DXC to compile HLSL ->
SPIR-V only). Slang compiles from a single .slang source to:

  DXIL  (.cs.cso)   -- DX12/NVRHI DX12 pipeline
  DXBC  (.cs.cso)   -- DX11/NVRHI D3D11 pipeline  [note: requires cs_5_0;
                       the shader uses SM6 features, so DX11 may need a
                       separate cs_5_0 profile — see RISK in the plan]
  SPIR-V (.spv)     -- Vulkan/NVRHI Vulkan pipeline
  GLSL  (.glsl)     -- OpenGL/NVRHI OpenGL pipeline

REGISTER SPACES (same as the old compile_nova_spirv.py)
---------------
NVRHI derives a Vulkan descriptor binding as `offset + slot`
(vulkan-resource-bindings.cpp:100), where the offset comes from
nvrhi::BindingOffsets per ResourceType and the slot is the HLSL register
number. Slang derives SPIR-V bindings from the same register numbers when
no [[vk::binding]] is present.

  u-space  shift   0  -> images        bindings 0..7
  b-space  shift   7  -> NovaConstants binding 8   (b1 + 7)
  t-space  shift  16  -> SRVs + SSBOs  bindings 17..87
  s-space  shift 200  -> sampler       binding 200

Use --check to verify bindings and constant-buffer offsets against the
engine's C++ expectations. Use --deploy to copy artifacts to both bin trees.
"""

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VULKAN_BIN = r"C:\VulkanSDK\1.3.296.0\Bin"
SLANGC = os.path.join(VULKAN_BIN, "slangc.exe")
SPIRV_DIS = os.path.join(VULKAN_BIN, "spirv-dis.exe")
SPIRV_CROSS = os.path.join(VULKAN_BIN, "spirv-cross.exe")
GLSLANG = os.path.join(VULKAN_BIN, "glslangValidator.exe")

SHADER_DIR = os.path.join(REPO, "DemonCore-Editor", "assets", "shaders")
SRC = os.path.join(SHADER_DIR, "Renderer3D_NovaRenderer.slang")

OUT_DXIL = os.path.join(SHADER_DIR, "Renderer3D_NovaRenderer.cs.cso")  # DX12
OUT_SPV  = os.path.join(SHADER_DIR, "Renderer3D_NovaRenderer.spv")       # Vulkan
OUT_GLSL = os.path.join(SHADER_DIR, "Renderer3D_NovaRenderer.slang.glsl")  # OpenGL

# Visibility-buffer producer (Phase 1, Task 1.1). VS and FS are separate artifacts
# because slangc accepts one -stage per invocation. The GLSL pair needs a #type
# directive prepended, exactly as the compute GLSL does -- see
# compile_visibility_targets().
VIS_SRC     = os.path.join(SHADER_DIR, "Renderer3D_NovaVisibility.slang")
VIS_DXIL_VS = os.path.join(SHADER_DIR, "Renderer3D_NovaVisibility.vs.cso")
VIS_DXIL_PS = os.path.join(SHADER_DIR, "Renderer3D_NovaVisibility.ps.cso")
VIS_GLSL_VS = os.path.join(SHADER_DIR, "Renderer3D_NovaVisibility.vs.glsl")
VIS_GLSL_PS = os.path.join(SHADER_DIR, "Renderer3D_NovaVisibility.ps.glsl")

# Mesh-shader path (Phase 2, Task 2.2/2.3). Two sources, not one: Slang's DXIL
# emit for the mesh stage is broken (`out vertices inout ...`, rejected by dxc),
# so Vulkan comes from the .slang and DX12 from the .hlsl. See the header comment
# in Renderer3D_NovaMesh.hlsl.
MESH_SRC    = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.slang")
MESH_HLSL   = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.hlsl")
MESH_SPV    = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.spv")
MESH_DXIL_MS = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.ms.cso")
MESH_DXIL_PS = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.ps.cso")

# Amplification shader (Task 2.4). DX12 only -- Vulkan has no AS stage, and NVRHI's
# MeshletPipelineDesc allows AS to be null so the mesh path works without it there.
# Compiled by the same SDK dxc.
MESH_AS_HLSL  = os.path.join(SHADER_DIR, "Renderer3D_NovaMeshAS.hlsl")
MESH_DXIL_AS  = os.path.join(SHADER_DIR, "Renderer3D_NovaMesh.as.cso")

# Windows SDK dxc, used ONLY for the mesh HLSL. Slang's own dxc cannot be used
# here because Slang rewrites the mesh entry's parameter qualifiers before dxc
# sees them. Located dynamically: the SDK version directory changes on update.
def _find_sdk_dxc():
    base = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                        "Windows Kits", "10", "bin")
    if not os.path.isdir(base):
        return None
    candidates = []
    for version in os.listdir(base):
        p = os.path.join(base, version, "x64", "dxc.exe")
        if os.path.isfile(p):
            candidates.append(p)
    return sorted(candidates)[-1] if candidates else None

# shift <space>; see module docstring. DO NOT change without updating
# BINDING_OFFSETS below and VulkanNova.cpp.
SHIFTS = [("-fvk-u-shift", "0", "0"),   # UAV images
          ("-fvk-b-shift", "7", "0"),   # constant buffer
          ("-fvk-t-shift", "16", "0"),  # sampled textures + structured buffers
          ("-fvk-s-shift", "200", "0")] # sampler

# nvrhi::BindingOffsets values this produces, for reference in VulkanNova.cpp.
BINDING_OFFSETS = {
    "unorderedAccess": 0,    # Texture_UAV
    "constantBuffer": 7,     # ConstantBuffer
    "shaderResource": 16,    # Texture_SRV, StructuredBuffer_SRV
    "sampler": 200,          # Sampler
}

HLSL_TYPE_TO_NVRHI = {
    "RWTexture2D": "Texture_UAV",
    "Texture2D": "Texture_SRV",
    "RWStructuredBuffer": "StructuredBuffer_UAV",
    "StructuredBuffer": "StructuredBuffer_SRV",
    "cbuffer": "ConstantBuffer",
    "SamplerState": "Sampler",
    "SamplerComparisonState": "Sampler",
}


def run_slangc(args, label):
    """Run slangc with the given args; exit on failure."""
    cmd = [SLANGC, SRC] + args + ["-o", OUT_DXIL if "-target dxil" in args else
          OUT_SPV if "-target spirv" in args else
          OUT_GLSL if "-target glsl" in args else
          "/c/dev/Wasteland/bin/test_output"]
    # Actually, let's handle output files properly per-target
    # This is set up below in compile_all_targets
    pass


def compile_all_targets():
    """Compile the .slang source to all four backend targets."""
    if not os.path.exists(SLANGC):
        sys.exit("slangc.exe not found at %s -- install the Vulkan SDK." % SLANGC)
    if not os.path.exists(SRC):
        sys.exit("Slang source not found at %s" % SRC)

    common = ["-stage", "compute", "-entry", "main", "-profile", "cs_6_0",
              "-D", "WASTELAND_NOVA_TERRAIN_GRID=1"]

    results = {}

    # --- DXIL (DX12) ---
    # NO -fvk-*-shift flags here. Those flags renumber the *shader's* registers
    # (they are not SPIR-V-only): with them, the cbuffer landed at cb8, the
    # reservoir SRV at t25 and the snapshots at t65..t89, while the D3D12 root
    # signature in DX12Nova.cpp declares cb1, t9 and the t48/t64 blocks. A root
    # signature that does not cover the registers a shader actually uses fails
    # CreateComputePipelineState with E_INVALIDARG (0x80070057), which is exactly
    # the silent raster fallback DX12 Nova showed. Vulkan keeps the shifts
    # because NVRHI derives its descriptor set bindings from them.
    # -D WASTELAND_DXIL is what lets the source omit resources the DX12 backend
    # genuinely cannot provide. WLRR (t76/t77, u15) is Vulkan-only: DX12Nova has
    # an 8-image array with no img_WlrrInput and no payload buffer, so declaring
    # those registers makes DXC emit them and CreateComputePipelineState then
    # fails with E_INVALIDARG because ranges[2] does not cover t76/t77 -- the
    # same trap that fired when t75 was added. Excluding them keeps DX12's DXIL
    # byte-compatible with the root signature it already has.
    # DX11 loads this same .cso, so one define covers both D3D backends.
    r = subprocess.run([SLANGC, SRC] + common + ["-D", "WASTELAND_DXIL"] +
                       ["-target", "dxil", "-o", OUT_DXIL],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(OUT_DXIL):
        sys.exit("DXIL compilation FAILED:\n%s\n%s" % (r.stdout, r.stderr))
    results["dxil"] = os.path.getsize(OUT_DXIL)
    print("compiled %s -> %s (%d bytes) [DXIL/CS_6_0]" % (os.path.basename(SRC), os.path.basename(OUT_DXIL), results["dxil"]))

    # --- SPIR-V (Vulkan) ---
    r = subprocess.run([SLANGC, SRC] + common +
                       ["-target", "spirv", "-o", OUT_SPV] +
                       [flag for pair in SHIFTS for flag in pair],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(OUT_SPV):
        sys.exit("SPIR-V compilation FAILED:\n%s\n%s" % (r.stdout, r.stderr))
    results["spirv"] = os.path.getsize(OUT_SPV)
    print("compiled %s -> %s (%d bytes) [SPIR-V]" % (os.path.basename(SRC), os.path.basename(OUT_SPV), results["spirv"]))

    # --- GLSL (OpenGL) ---
    # -fvk-use-dx-layout makes the GLSL use D3D-style cbuffer packing instead
    # of std140, matching what the OpenGL backend's glGetUniformLocation
    # expects (same as the old GLSL twin).
    # -D WASTELAND_GLSL is passed ONLY for the GLSL target so the .slang source
    # can exclude extra image declarations that exceed GL_MAX_IMAGE_UNITS=8 on
    # consumer GPUs (img_Mask, img_Moments — borrow img_Bloom_Temp.a / img_TraceGBuffer
    # instead, matching the old hand-authored GL SL twin's adaptations).
    # GLSL uses ZERO Vulkan shifts so binding numbers match the HLSL register
    # numbers directly (u0→0, t1→1, t48→48, s0→0), which matches the old GLSL
    # twin's SSBO bindings (1/2/3) and the renderer's scene-texture array at unit 15.
    # The Vulkan SPIR-V target keeps the -fvk-* shifts (see SHIFTS above).
    r = subprocess.run([SLANGC, SRC] + common
                        + ["-D", "WASTELAND_GLSL"]
                        + ["-target", "glsl", "-profile", "glsl_430"]
                        + ["-fvk-use-dx-layout", "-o", OUT_GLSL]
                        + ["-fvk-u-shift", "0", "0"]
                        + ["-fvk-b-shift", "0", "0"]
                        + ["-fvk-t-shift", "0", "0"]
                        + ["-fvk-s-shift", "0", "0"],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(OUT_GLSL):
        sys.exit("GLSL compilation FAILED:\n%s\n%s" % (r.stdout, r.stderr))
    results["glsl"] = os.path.getsize(OUT_GLSL)
    print("compiled %s -> %s (%d bytes) [GLSL_430]" % (os.path.basename(SRC), os.path.basename(OUT_GLSL), results["glsl"]))

    # Inject the #type compute loader directive that OpenGLShader's file-backed
    # ctor expects at the first column of its own line. Without it the loader
    # finds zero stages — the program links but every glGetUniformLocation
    # returns -1 and glDispatchCompute runs pass 0 with no SSBOs bound, leaving
    # the viewport black. slangc does not emit engine-specific #type directives,
    # so we prepend one to the GLSL output (stripping any existing shebang
    # line if present, though slangc never emits one).
    with open(OUT_GLSL, "r", errors="replace") as f:
        glsl_src = f.read()

    # Post-process GLSL output to fix slangc's broken GLSL codegen for this
    # shader. Three categories of defect:
    #   1. Vulkan-only barrier: controlBarrier(gl_ScopeWorkgroup, ...) requires
    #      GL_KHR_memory_scope_semantics (Vulkan extension, not in desktop GL).
    #      Replace with GLSL's memoryBarrier() + barrier().
    #   2. Mangled array type names: _Array_default_vectorx3Cfloatx2CNx3EM_0
    #      → vecN[M] (Slang emits C++ template syntax as opaque type names).
    #   3. Unsupported extension: GL_KHR_memory_scope_semantics is Vulkan-only;
    #      remove it since we no longer need it after fixing the barriers.
    import re
    # Fix 1: Replace Vulkan controlBarrier with GLSL memoryBarrier+barrier
    glsl_src = re.sub(
        r"controlBarrier\(gl_ScopeWorkgroup,\s*gl_ScopeWorkgroup,\s*gl_StorageSemanticsShared,\s*gl_SemanticsAcquireRelease\)",
        "memoryBarrier(); barrier()", glsl_src)
    # Fix 2: SKIPPED — Slang emits mangled struct types like
    # _Array_default_vectorx3Cfloatx2C4x3E4_0 as a DEFINED GLSL struct
    # (with member vec4 data_2[4]), plus uses it at declaration sites.
    # Previously Fix 2 rewrote the declaration to `vec4 varname[4]` but
    # left the struct-member access `varname.data_2[i]` intact, causing
    # "'cannot apply '.' to an array" errors. The mangled name IS a valid
    # GLSL identifier (letters/digits/underscores only), the struct IS
    # defined before use, and member access IS valid. No transformation
    # needed — keep the struct type and its .data_N[i] access pattern.
    # Fix 3: Remove Vulkan-only extensions not supported by desktop GL 4.5.
    # GL_KHR_memory_scope_semantics — Vulkan-only (provided gl_ScopeWorkgroup etc.)
    # GL_EXT_samplerless_texture_functions — core in GLSL 4.2+, the #extension
    # directive causes the driver to reject the entire shader. texelFetch on
    # texture2D is available without it on GL 4.3+ (which this #version 450 targets).
    glsl_src = glsl_src.replace(
        "#extension GL_KHR_memory_scope_semantics : require\n", "")
    glsl_src = glsl_src.replace(
        "#extension GL_EXT_samplerless_texture_functions : require\n", "")

    # Fix 4: Convert texture2D (texture-only) → sampler2D (combined type).
    #
    # The NVIDIA driver does NOT support texture2D as a GLSL type — it is only
    # available via GL_EXT_samplerless_texture_functions, which driver 591.59
    # rejects ("extension not supported"). The old hand-authored twin works by
    # using sampler2D (combined texture+sampler) exclusively: no separate
    # sampler object, no sampler2D(tex, samp) constructors.
    #
    # Slang already emits `layout(binding = N)` for each resource (via
    # -fvk-use-dx-layout + zero shifts). We do NOT re-add it (that created
    # duplicate layout qualifiers that broke parsing). Instead we only:
    #   a) Change type: uniform texture2D → uniform sampler2D
    #   b) Simplify: sampler2D(var, s_Sampler_0) → var
    #      (combined sampler2D already embeds the sampler state)
    #   c) Remove: uniform sampler s_Sampler_0; (no longer needed)
    #
    # The old twin has 8 sampler2D declarations (7 individual + 1 [32] array)
    # and 0 separate samplers. Our conversion: 16 individual + 1 array = 17
    # declarations — if the compiler counts array elements individually, 48
    # may exceed the 32-sampler limit; in that case the scene-texture array
    # must be promoted to sampler2DArray. For now, match the twin's pattern.
    # Step (a): type change only — preserves Slang's layout(binding = N).
    glsl_src = re.sub(r"uniform\s+texture2D\s+", "uniform sampler2D ", glsl_src)
    # Step (b): simplify constructor calls. Handles both simple vars
    # (s_DepthBuffer_0) and array-indexed (u_SceneTextures_0[0]).
    glsl_src = re.sub(
        r"sampler2D\(([\w_]+(?:\[\d+\])?),\s*s_Sampler_0\)",
        r"\1", glsl_src)
    # Step (c): remove the now-unused separate sampler declaration.
    # Slang emits it as two lines (layout on one, declaration on the next)
    # or as a single line. Handle both forms, including duplicate layout
    # lines that the OLD Fix 4 may have left in a stale artifact.
    glsl_src = re.sub(
        r"(?:layout\(binding\s*=\s*0\)\s*\n\s*)+"
        r"(?:layout\(binding\s*=\s*0\)\s*)?"
        r"uniform\s+sampler\s+s_Sampler_0;\s*\n",
        "", glsl_src)

    # Fix 5: Promote the scene-texture array to sampler2DArray to avoid exceeding
    # the 32-sampler compile limit (GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS) on consumer
    # GPUs. A sampler2D[32] counts as 32 samplers; a sampler2DArray counts as 1.
    # Total after Fix 5: 15 individual + 1 array = 16 ≤ 32 (was 15 + 32 = 47).
    glsl_src = re.sub(
        r'uniform\s+sampler2D\s+u_SceneTextures_0\[32\]',
        'uniform sampler2DArray u_SceneTextures_0', glsl_src)
    # Rewrite each textureLod(u_SceneTextures_0[N], (uv), (lod)) →
    # textureLod(u_SceneTextures_0, vec3((uv), float(N)), (lod))
    glsl_src = re.sub(
        r'textureLod\(\s*u_SceneTextures_0\[(\d+)\],\s*\(([^)]+)\),\s*\(([^)]+)\)\s*\)',
        lambda m: 'textureLod(u_SceneTextures_0, vec3((%s), float(%s)), (%s))'
                  % (m.group(2), m.group(1), m.group(3)),
        glsl_src)

    # Fix 7: Convert the cbuffer uniform block (_S1) to a struct + uniform
    # variable. Slang emits HLSL cbuffer as a GLSL uniform block:
    #   layout(binding = 1) layout(std140) uniform _S1 { ... } NovaConstants_0;
    # Uniform block members are NOT queryable via glGetUniformLocation —
    # they require the UBO API (glGetBlockIndex + GL_UNIFORM_OFFSET), which
    # returns -1 for every member. The C++ backend uses glUniform* calls
    # keyed on glGetUniformLocation, so EVERY uniform lookup was silently
    # failing — sky colors defaulted to (0,0,0)=black, camera matrices were
    # zero, instance count was 0, etc. Converting to:
    #   struct _S1 { ... }; uniform _S1 NovaConstants_0;
    # makes glGetUniformLocation("NovaConstants_0.u_member_0") return valid
    # locations, restoring all per-frame uniform uploads.
    glsl_src = re.sub(
        r"layout\(binding\s*=\s*1\)\s*\n\s*layout\(std140\)\s+uniform\s+_S1\s*\n\s*\{",
        "struct _S1\n{",
        glsl_src,
    )
    glsl_src = glsl_src.replace("}NovaConstants_0;", "};\nuniform _S1 NovaConstants_0;")

    if not glsl_src.startswith("#type"):
        header = "// Auto-generated by slangc, post-processed by the Nova shader toolchain.\n"
        header += "#type compute\n"
        glsl_src = header + glsl_src
    with open(OUT_GLSL, "w") as f:
        f.write(glsl_src)

    # --- DXBC (DX11) ---
    # Slang uses FXC for DXBC, which only supports up to cs_5_0. The shader
    # uses SM6 features. Try anyway; the caller should check the result.
    # If this fails, the DX11 path falls back to loading the DXIL .cso via
    # the D3D11 Agility SDK (same blob, different runtime path).
    out_dxbc = os.path.join(SHADER_DIR, "Renderer3D_NovaRenderer.cs_dxbc.cso")
    r = subprocess.run([SLANGC, SRC] +
                       ["-target", "dxbc", "-profile", "cs_5_0", "-entry", "main",
                        "-D", "WASTELAND_NOVA_TERRAIN_GRID=1", "-o", out_dxbc] +
                       [flag for pair in SHIFTS for flag in pair],
                       capture_output=True, text=True, errors="replace")
    if r.returncode == 0 and os.path.exists(out_dxbc):
        results["dxbc"] = os.path.getsize(out_dxbc)
        print("compiled %s -> %s (%d bytes) [DXBC/CS_5_0]" % (os.path.basename(SRC), os.path.basename(out_dxbc), results["dxbc"]))
    else:
        # Expected: SM6 features not available in cs_5_0
        results["dxbc"] = None
        print("DXBC (cs_5_0) skipped — shader uses SM6 features (DX11 loads the DXIL .cso via Agility SDK)")

    # Touch output files so --check-only staleness detection passes after every
    # compile. slangc may skip rewriting an output if the content is byte-identical,
    # which leaves the old timestamp and triggers a false STALE in --check-only.
    for out in (OUT_DXIL, OUT_SPV, OUT_GLSL):
        if os.path.exists(out):
            os.utime(out, None)

    return results


# --- Binding verification (same approach as compile_nova_spirv.py) ---

def compile_visibility_targets():
    """Compile the Nova visibility VS/FS to the four backend artifacts.

    Separate from compile_all_targets(): that compiles ONE entry point to three targets,
    while this compiles TWO entry points to two targets each. slangc accepts a single
    -stage per invocation, so a combined compile is not possible.

    The GLSL artifacts get a `#type` directive prepended, exactly as the compute GLSL
    does. OpenGLShader's file-backed ctor splits a multi-stage .glsl on those markers;
    with no marker it finds zero stages, links an empty program, and every
    glGetUniformLocation returns -1 -- the failure the compute path documents.
    """
    if not os.path.exists(VIS_SRC):
        sys.exit("Slang source not found at %s" % VIS_SRC)

    # (entry, stage, target, profile, define, output)
    targets = [
        ("NovaVisVS", "vertex",   "glsl", "glsl_430", "WASTELAND_GLSL", VIS_GLSL_VS),
        ("NovaVisFS", "fragment", "glsl", "glsl_430", "WASTELAND_GLSL", VIS_GLSL_PS),
        ("NovaVisVS", "vertex",   "dxil", "vs_6_0",   "WASTELAND_DXIL", VIS_DXIL_VS),
        ("NovaVisFS", "fragment", "dxil", "ps_6_0",   "WASTELAND_DXIL", VIS_DXIL_PS),
    ]

    for entry, stage, target, profile, define, out in targets:
        r = subprocess.run([SLANGC, VIS_SRC, "-entry", entry, "-stage", stage,
                            "-target", target, "-profile", profile,
                            "-D", define, "-o", out],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit("visibility %s/%s -> %s FAILED:\n%s\n%s"
                     % (entry, stage, target, r.stdout, r.stderr))
        print("compiled %s -> %s (%d bytes) [%s/%s]"
              % (os.path.basename(VIS_SRC), os.path.basename(out),
                 os.path.getsize(out), target, stage))

    # Prepend the loader directive to the GLSL pair. Each file holds exactly one stage,
    # so the token is the whole directive line. Text-mode write translates \n to \r\n,
    # matching every other artifact this script produces.
    for path, token in ((VIS_GLSL_VS, "#type vertex"),
                        (VIS_GLSL_PS, "#type fragment")):
        with open(path, "r", errors="replace") as f:
            src = f.read()
        if src.startswith(token):
            continue
        with open(path, "w") as f:
            f.write("// Auto-generated by slangc, post-processed by the Nova shader toolchain.\n")
            f.write(token + "\n")
            f.write(src)


def compile_mesh_targets():
    """Compile the mesh path: Slang -> SPIR-V, SDK dxc -> DXIL (mesh + pixel).

    Two sources because Slang's DXIL mesh emit is broken. The Vulkan artifact is
    the .slang compiled with the GL_EXT_mesh_shader capability; the DX12 artifacts
    are the .hlsl compiled by the Windows SDK dxc at ms_6_5 and ps_6_0.
    """
    if not os.path.exists(MESH_SRC):
        sys.exit("Slang source not found at %s" % MESH_SRC)
    if not os.path.exists(MESH_HLSL):
        sys.exit("HLSL source not found at %s" % MESH_HLSL)

    r = subprocess.run([SLANGC, MESH_SRC, "-entry", "NovaMeshMS", "-stage", "mesh",
                        "-target", "spirv", "-profile", "glsl_460",
                        "-capability", "GL_EXT_mesh_shader", "-o", MESH_SPV],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(MESH_SPV):
        sys.exit("mesh -> spirv FAILED:\n%s\n%s" % (r.stdout, r.stderr))
    print("compiled %s -> %s (%d bytes) [spirv/mesh]"
          % (os.path.basename(MESH_SRC), os.path.basename(MESH_SPV),
             os.path.getsize(MESH_SPV)))

    dxc = _find_sdk_dxc()
    if not dxc:
        sys.exit("Windows SDK dxc.exe not found -- cannot compile %s" % MESH_HLSL)
    for entry, profile, out in (("NovaMeshMS", "ms_6_5", MESH_DXIL_MS),
                                ("NovaMeshFS", "ps_6_0", MESH_DXIL_PS)):
        r = subprocess.run([dxc, "-T", profile, "-E", entry, "-Fo", out, MESH_HLSL],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit("mesh %s -> %s FAILED:\n%s\n%s" % (entry, profile, r.stdout, r.stderr))
        print("compiled %s -> %s (%d bytes) [dxil/%s]"
              % (os.path.basename(MESH_HLSL), os.path.basename(out),
                 os.path.getsize(out), entry))

    if os.path.exists(MESH_AS_HLSL):
        r = subprocess.run([dxc, "-T", "as_6_5", "-E", "NovaMeshAS",
                            "-Fo", MESH_DXIL_AS, MESH_AS_HLSL],
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0 or not os.path.exists(MESH_DXIL_AS):
            sys.exit("mesh AS -> %s FAILED:\n%s\n%s" % (MESH_DXIL_AS, r.stdout, r.stderr))
        print("compiled %s -> %s (%d bytes) [dxil/NovaMeshAS]"
              % (os.path.basename(MESH_AS_HLSL), os.path.basename(MESH_DXIL_AS),
                 os.path.getsize(MESH_DXIL_AS)))


def read_spirv_bindings(spv_path):
    """Read SPIR-V OpName + OpDecorate(Binding) pairs."""
    if not os.path.exists(SPIRV_DIS):
        print("  (spirv-dis.exe missing -- skipping SPIR-V binding check)")
        return {}, {}
    out = subprocess.run([SPIRV_DIS, spv_path], capture_output=True,
                          text=True, errors="replace").stdout
    names, binds = {}, {}
    for line in out.split("\n"):
        m = re.search(r'OpName (%[\w$]+)\s+"([^"]+)"', line)
        if m:
            names[m.group(1)] = m.group(2)
        m = re.search(r"OpDecorate (%[\w$]+) Binding (\d+)", line)
        if m:
            binds[m.group(1)] = int(m.group(2))
    return names, binds


def read_slang_slots():
    """HLSL register number per resource name, from the .slang source."""
    decl = re.compile(
        r"^\s*(RWStructuredBuffer|StructuredBuffer|RWTexture2D|Texture2D|RWBuffer|"
        r"cbuffer|SamplerState|SamplerComparisonState)\s*(?:<[^>]*>)?"
        r"\s+(\w+)\s*(\[[^\]]*\])?\s*:\s*register\(([ustb])(\d+)\)")
    slots, kinds = {}, {}
    with open(SRC, encoding="utf-8") as fh:
        for line in fh:
            m = decl.match(line)
            if m:
                kind, name, _arr, _space, num = m.groups()
                slots[name] = int(num)
                kinds[name] = HLSL_TYPE_TO_NVRHI[kind]
    return slots, kinds


def check_bindings(spv_path):
    """Verify SPIR-V bindings match the offset+slot contract and have no collisions."""
    names, binds = read_spirv_bindings(spv_path)
    slots, kinds = read_slang_slots()

    by_binding = collections.defaultdict(list)
    for var, binding in binds.items():
        by_binding[binding].append(names.get(var, var))

    collisions = {b: v for b, v in by_binding.items() if len(v) > 1}
    if collisions:
        print("BINDING COLLISIONS -- NVRHI would build an invalid descriptor set:")
        for b, v in sorted(collisions.items()):
            print("   binding %d: %s" % (b, ", ".join(v)))
        return False

    offsets = collections.defaultdict(set)
    unmatched = []
    for var, binding in binds.items():
        name = names.get(var, var)
        if name not in slots:
            unmatched.append(name)
            continue
        offsets[kinds[name]].add(binding - slots[name])

    print("bindings: %d resources, %d distinct, no collisions"
          % (len(binds), len(by_binding)))
    if unmatched:
        print("  (no HLSL register found for: %s -- Slang may have added these)"
              % ", ".join(sorted(unmatched)))
    for rtype, vals in sorted(offsets.items()):
        if len(vals) != 1:
            print("  INCONSISTENT offset for %s: %s" % (rtype, sorted(vals)))
            return False
        got = vals.pop()
        want = BINDING_OFFSETS.get(rtype)
        flag = "ok" if want is None or want == got else "MISMATCH (script says %s)" % want
        print("   %-22s offset = %-5d %s" % (rtype, got, flag))
    return True


def check_staleness():
    """--check must not trust a .spv/.cso older than its .slang source."""
    for src in (SRC, VIS_SRC, MESH_SRC, MESH_HLSL, MESH_AS_HLSL):
        if not os.path.exists(src):
            sys.exit("Slang source not found at %s" % src)
    for src, artifacts in (
            (SRC, (OUT_DXIL, OUT_SPV, OUT_GLSL)),
            (VIS_SRC, (VIS_DXIL_VS, VIS_DXIL_PS, VIS_GLSL_VS, VIS_GLSL_PS)),
            (MESH_SRC, (MESH_SPV,)),
            (MESH_HLSL, (MESH_DXIL_MS, MESH_DXIL_PS)),
            (MESH_AS_HLSL, (MESH_DXIL_AS,))):
        for artifact in artifacts:
            if not os.path.exists(artifact):
                sys.exit("no artifact to check at %s -- compile first" % artifact)
            if os.path.getmtime(artifact) < os.path.getmtime(src):
                sys.exit("STALE ARTIFACT: %s is older than %s.\nRecompile first."
                         % (os.path.basename(artifact), os.path.basename(src)))


def deploy():
    """Copy compiled artifacts into both bin trees."""
    for cfg in ("Release", "Debug"):
        dst = os.path.join(REPO, "bin", "%s-windows-x86_64" % cfg,
                           "DemonCore-Editor", "assets", "shaders")
        if not os.path.isdir(dst):
            print("  skip %s (no %s)" % (cfg, dst))
            continue
        for artifact in (OUT_DXIL, OUT_SPV, OUT_GLSL,
                         VIS_DXIL_VS, VIS_DXIL_PS, VIS_GLSL_VS, VIS_GLSL_PS,
                         MESH_SPV, MESH_DXIL_MS, MESH_DXIL_PS, MESH_DXIL_AS):
            shutil.copyfile(artifact, os.path.join(dst, os.path.basename(artifact)))
        print("  deployed -> %s" % dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--check-only", action="store_true",
                    help="verify existing artifacts without recompiling")
    ap.add_argument("--deploy", action="store_true",
                    help="copy artifacts into the Debug and Release bin trees")
    args = ap.parse_args()

    if not args.check_only:
        compile_all_targets()
        compile_visibility_targets()
        compile_mesh_targets()
    else:
        check_staleness()
        print("staleness: OK (all artifacts newer than source)")

    ok = True
    if os.path.exists(OUT_SPV):
        ok = check_bindings(OUT_SPV) and ok

    if not ok:
        sys.exit(1)
    if args.deploy:
        deploy()
    print("OK")


if __name__ == "__main__":
    main()
