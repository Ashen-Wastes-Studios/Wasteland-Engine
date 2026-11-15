#!/usr/bin/env python3
"""
Smoke test: verify the .slang source compiles to ALL targets.

This replaces verify_nova_shader_parity.py. With a single Slang source,
parity is guaranteed by the compiler — if a pass diverges, slangc itself
fails. This script is a CI-friendly wrapper that runs slangc on all
targets and exits non-zero on any failure.

Run:  python3 tools/verify_nova_slang_compile.py
      python3 tools/verify_nova_slang_compile.py --check-only  (validate existing artifacts)
"""
import os, sys, subprocess, time, hashlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLANGC = r"C:\VulkanSDK\1.3.296.0\Bin\slangc.exe"
SRC = os.path.join(REPO, "DemonCore-Editor", "assets", "shaders", "Renderer3D_NovaRenderer.slang")

TARGETS = [
    ("dxil",  "Renderer3D_NovaRenderer.cs.cso"),
    ("spirv", "Renderer3D_NovaRenderer.spv"),
    ("glsl",  "Renderer3D_NovaRenderer.slang.glsl"),
]

SHIFTS = ["-fvk-u-shift", "0", "0", "-fvk-b-shift", "7", "0",
          "-fvk-t-shift", "16", "0", "-fvk-s-shift", "200", "0"]

COMMON = ["-stage", "compute", "-entry", "main", "-profile", "cs_6_0",
          "-D", "WASTELAND_NOVA_TERRAIN_GRID=1"]


def deployed_shader_dirs():
    """Every bin/ tree the engine could load shaders from.

    There are TWO trees and they are not independent:

      DemonCore-Editor/assets/shaders/            the SOURCE tree
      bin/<config>/DemonCore-Editor/assets/shaders/   the DEPLOYED tree

    `DemonCore-Editor.vcxproj`'s PostBuildEvent runs
    `robocopy /E "$(ProjectDir)assets" "$(TargetDir)assets"`, which copies the SOURCE
    tree OVER the deployed tree on EVERY build. And `compile_nova_slang.py --deploy`
    writes the deployed tree only, while a plain `compile_nova_slang.py` writes the
    source tree only.

    So a deploy that leaves the source tree stale is silently undone by the next
    build: the build copies the stale source artifact back over the good one. That is
    exactly how a DX12-breaking .cs.cso (351108 bytes, DX12 init fails with
    E_INVALIDARG and the later Vulkan switch loses the device) reappears "by itself"
    right after a build that changed no shader source.

    Keeping the two trees byte-identical is the invariant that prevents it.
    """
    import glob
    return sorted(glob.glob(os.path.join(REPO, "bin", "*", "DemonCore-Editor",
                                         "assets", "shaders")))


def check_trees_match():
    """Compare every deployed tree against the source tree. True if all match.

    Reports content, not mtime: a stale file can easily be NEWER than the .slang
    source (the build just copied it), so mtime checks pass while the content is
    wrong. This is the check the old mtime-only path was missing.
    """
    src_dir = os.path.dirname(SRC)
    dirs = deployed_shader_dirs()
    if not dirs:
        print("  SKIP deployed-tree comparison: no bin/*/.../assets/shaders found")
        return True
    ok = True
    for d in dirs:
        rel = os.path.relpath(d, REPO).replace(os.sep, "/")
        for _, name in TARGETS:
            a = os.path.join(src_dir, name)
            b = os.path.join(d, name)
            if not os.path.exists(b):
                print(f"  FAIL {rel}/{name}: missing from the deployed tree")
                ok = False
                continue
            ha = hashlib.sha256(open(a, "rb").read()).hexdigest()[:16]
            hb = hashlib.sha256(open(b, "rb").read()).hexdigest()[:16]
            if ha != hb:
                print(f"  FAIL {rel}/{name}: source and deployed DIFFER "
                      f"({ha} vs {hb}) -- the next build will overwrite the deployed one")
                ok = False
            else:
                print(f"  PASS {rel}/{name}: matches source ({ha})")
    return ok


def compile_target(target, output_name):
    """Compile SRC to the given target. Returns (ok, output, stderr)."""
    out_path = os.path.join(os.path.dirname(SRC), output_name)
    # Vulkan binding shifts belong to SPIR-V ONLY.
    #
    # Applying them to dxil produces a .cs.cso that DX12 REJECTS: Nova init fails at
    # CreateComputePipelineState with E_INVALIDARG (0x80070057), falls back to
    # raster, and the later switch to Vulkan then loses the device
    # ("NVRHI: Device Removed!"). The artifact is also a different size (351108 vs
    # the correct 350976), so the two compile tools disagreed on bytes for the same
    # source -- which is how this went unnoticed: the failure looks like
    # non-deterministic codegen, but it is a flag difference between two scripts.
    #
    # This script writes the SOURCE tree, and DemonCore-Editor.vcxproj's
    # PostBuildEvent robocopies the source tree over the deployed tree on every
    # build -- so a wrong artifact produced here is silently promoted to the tree the
    # engine actually loads. Keep this identical to compile_nova_slang.py.
    shifts = SHIFTS if target == "spirv" else []
    cmd = [SLANGC, SRC] + COMMON + ["-target", target] + shifts + ["-o", out_path]
    if target == "glsl":
        # GLSL uses zero Vulkan shifts so binding numbers match raw HLSL
        # register numbers (matching the renderer's existing layout).
        cmd = [SLANGC, SRC] + COMMON \
              + ["-D", "WASTELAND_GLSL"] \
              + ["-target", "glsl", "-profile", "glsl_430", "-fvk-use-dx-layout"] \
              + ["-o", out_path] \
              + ["-fvk-u-shift", "0", "0", "-fvk-b-shift", "0", "0",
                 "-fvk-t-shift", "0", "0", "-fvk-s-shift", "0", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    ok = r.returncode == 0 and os.path.exists(out_path)
    if not ok:
        return False, r.stdout, r.stderr

    # Inject #type compute directive for the OpenGLShader file-backed ctor,
    # which expects it at the first column. Without it the loader finds zero
    # stages and glDispatchCompute runs pass 0 with no SSBOs bound (black VP).
    # Also post-process GLSL to fix slangc's broken GLSL codegen (same fixes as
    # compile_nova_slang.py): Vulkan barriers, mangled array types, extension.
    if target == "glsl":
        import re as _re
        with open(out_path, "r", errors="replace") as f:
            glsl_src = f.read()
        # Fix Vulkan-only controlBarrier → GLSL memoryBarrier+barrier
        glsl_src = _re.sub(
            r"controlBarrier\(gl_ScopeWorkgroup,\s*gl_ScopeWorkgroup,\s*gl_StorageSemanticsShared,\s*gl_SemanticsAcquireRelease\)",
            "memoryBarrier(); barrier()", glsl_src)
        # Fix 2: SKIPPED — Slang emits mangled struct types (e.g.
        # _Array_default_vectorx3Cfloatx2C4x3E4_0) as DEFINED GLSL structs
        # with member vec4 data_2[4], plus uses the struct at declaration
        # sites. The mangled name is a valid GLSL identifier; the struct is
        # defined before use; member access data_2[i] is valid. No transform
        # needed — rewriting the declaration type broke struct member access.
        # (Same rationale as compile_nova_slang.py.)
        # Remove Vulkan-only extensions (same fixes as compile_nova_slang.py)
        glsl_src = glsl_src.replace(
            "#extension GL_KHR_memory_scope_semantics : require\n", "")
        glsl_src = glsl_src.replace(
            "#extension GL_EXT_samplerless_texture_functions : require\n", "")

        # Fix 4: Convert texture2D → sampler2D (combined). Same rationale as
        # compile_nova_slang.py: NVIDIA driver rejects texture2D as a type.
        # Slang already emits layout(binding = N); we only change the type,
        # simplify constructor calls, and drop the separate sampler.
        glsl_src = _re.sub(r"uniform\s+texture2D\s+", "uniform sampler2D ", glsl_src)
        glsl_src = _re.sub(
            r"sampler2D\(([\w_]+(?:\[\d+\])?),\s*s_Sampler_0\)",
            r"\1", glsl_src)
        glsl_src = _re.sub(
            r"(?:layout\(binding\s*=\s*0\)\s*\n\s*)+"
            r"(?:layout\(binding\s*=\s*0\)\s*)?"
            r"uniform\s+sampler\s+s_Sampler_0;\s*\n",
            "", glsl_src)

        # Fix 5: sampler2DArray for scene-texture array (same as compile_nova_slang.py)
        glsl_src = _re.sub(
            r'uniform\s+sampler2D\s+u_SceneTextures_0\[32\]',
            'uniform sampler2DArray u_SceneTextures_0', glsl_src)
        glsl_src = _re.sub(
            r'textureLod\(\s*u_SceneTextures_0\[(\d+)\],\s*\(([^)]+)\),\s*\(([^)]+)\)\s*\)',
            lambda m: 'textureLod(u_SceneTextures_0, vec3((%s), float(%s)), (%s))'
                      % (m.group(2), m.group(1), m.group(3)),
            glsl_src)

        # Fix 7: Convert cbuffer uniform block (_S1) to struct + uniform
        # variable so glGetUniformLocation works (uniform blocks return -1
        # for all members). Same rationale as compile_nova_slang.py.
        glsl_src = _re.sub(
            r"layout\(binding\s*=\s*1\)\s*\n\s*layout\(std140\)\s+uniform\s+_S1\s*\n\s*\{",
            "struct _S1\n{",
            glsl_src,
        )
        glsl_src = glsl_src.replace("}NovaConstants_0;", "};\nuniform _S1 NovaConstants_0;")

        if not glsl_src.startswith("#type"):
            header = "// Auto-generated by slangc, post-processed by the Nova shader toolchain.\n"
            header += "#type compute\n"
            glsl_src = header + glsl_src
        with open(out_path, "w") as f:
            f.write(glsl_src)

    size = os.path.getsize(out_path)
    print(f"  {target:5s} -> {output_name} ({size:,} bytes)  OK")
    return True, r.stdout, r.stderr


def main():
    if not os.path.exists(SLANGC):
        print(f"FAIL slangc.exe not found at {SLANGC}")
        return 1
    if not os.path.exists(SRC):
        print(f"FAIL Slang source not found at {SRC}")
        return 1

    check_only = "--check-only" in sys.argv

    if check_only:
        print("Check-only: validating existing artifacts are newer than source...")
        src_mtime = os.path.getmtime(SRC)
        all_ok = True
        for _, name in TARGETS:
            path = os.path.join(os.path.dirname(SRC), name)
            if not os.path.exists(path):
                print(f"  FAIL {name}: artifact missing")
                all_ok = False
            elif os.path.getmtime(path) < src_mtime:
                print(f"  FAIL {name}: stale (older than .slang source)")
                all_ok = False
            else:
                print(f"  PASS {name}: fresh")
        # mtime is not enough on its own: a stale artifact copied by a build is NEWER
        # than the source, so the loop above passes while the deployed content is
        # wrong. Compare the two trees' bytes as well.
        print("Check-only: comparing source tree against deployed tree...")
        if not check_trees_match():
            all_ok = False
        return 0 if all_ok else 1

    print("Compiling Renderer3D_NovaRenderer.slang to all targets:")
    all_ok = True
    for target, out_name in TARGETS:
        ok, stdout, stderr = compile_target(target, out_name)
        if not ok:
            print(f"  FAIL {target}: {stderr[:500]}")
            # Print warnings (non-fatal) from stdout for context
            if stdout.strip():
                for line in stdout.strip().split("\n")[:3]:
                    print(f"    | {line}")
            all_ok = False

    if all_ok:
        print("\nPASS: all targets compiled successfully")
        return 0
    else:
        print("\nFAIL: one or more targets failed to compile")
        return 1


if __name__ == "__main__":
    sys.exit(main())
