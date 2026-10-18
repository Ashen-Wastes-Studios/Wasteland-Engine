#!/usr/bin/env python3
"""Validate Renderer3D_NovaRenderer.glsl after an edit, the way the engine uses it.

WHY THIS EXISTS
The engine does not currently link (Scene.cpp fails on an in-flight
Navigation:: refactor, plus 41 Renderer3D symbols declared in Renderer3D.h have
no implementation), so a shader edit cannot be verified through the build. This
compiles the GLSL out-of-process instead.

THE OBVIOUS APPROACH IS WRONG -- DO NOT "FIX" IT BACK
The first version of this script injected layout(location=) onto the file's 85
loose uniforms and reported that the result compiled to SPIR-V. It does not.
MEASURED, minimal repro against glslangValidator 1.3.296:

    A: layout(location=0) uniform vec3 u;                    -> FAIL
       'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan
    B: layout(std140, binding=1) uniform CB { vec3 u; } cb;   -> PASS
    C: layout(push_constant) uniform PC { vec3 u; } pc;       -> PASS

A location qualifier alone does NOT make a uniform opaque to Vulkan GLSL; the
uniform must live inside a block. So "inject 182 locations" is not a shortcut to
a Vulkan-ready shader, it is a different, still-invalid shader. This also
corrects an earlier claim in this session that the GLSL compiled to SPIR-V after
location injection -- it compiled under OpenGL SPIR-V semantics, a different mode
that does permit loose uniforms.

WHAT THIS SCRIPT ACTUALLY CHECKS
The engine's OpenGL backend compiles this GLSL with loose uniforms and no
blocks, which is legal desktop GL. So the faithful check is glslang's -G (OpenGL
SPIR-V semantics), which accepts loose uniforms and matches what the GL driver is
handed. Vulkan support is NOT claimed here: VulkanNova compiles but is never
invoked (no FlushVulkanNova call site exists), and the SPIR-V that
tools/compile_nova_spirv.py produces comes from the HLSL twin, not this file.
Converting the loose uniforms into blocks to satisfy -V would change the upload
ABI that Renderer3D.cpp's glGetUniformLocation calls depend on.

It also asserts the property this exercise exists to protect: GetJitter() must
read u_Jitter and must NOT call Halton -- in BOTH twins, since they are
hand-maintained and drift between them is silent.

Exit 0 = valid, 1 = failure.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SHADER_DIR = Path(__file__).resolve().parent.parent / "DemonCore-Editor" / "assets" / "shaders"
GLSL = SHADER_DIR / "Renderer3D_NovaRenderer.glsl"
HLSL = SHADER_DIR / "Renderer3D_NovaRenderer.hlsl"

# Shared algorithm sources that Renderer3D_NovaRenderer.glsl #includes. The
# compile happens in a temp dir, so these are copied in beside it (see main()).
# The HLSL twin needs no equivalent list: D3DCompileFromFile runs with
# D3D_COMPILE_STANDARD_FILE_INCLUDE and resolves its own includes relative to
# the .hlsl's real directory.
#
# CURRENTLY EMPTY, deliberately. A shared-algorithm experiment moved RunBilateralBlur
# into NovaShared_Bilateral.h and was REJECTED -- see docs/NOVA_SHADER_PIPELINE.md.
# The GLSL half of that arrangement needed GL_GOOGLE_include_directive, which
# OpenGLShader::PreProcess cannot provide (it only splits on "#type" and never
# expands an include), so the OpenGL backend would have silently depended on a
# driver extension the engine never checks while this harness still passed.
#
# Do NOT re-add an entry here without first making OpenGLShader::PreProcess expand
# #include relative to the shader's own directory. Populating this list makes the
# verifier resolve includes the ENGINE cannot, which is precisely the green-CI /
# broken-backend failure the rejection was about.
NOVA_SHARED_INCLUDES: list[str] = []


def find_glslang():
    for cand in [
        r"C:\VulkanSDK\1.3.296.0\Bin\glslangValidator.exe",
        r"C:\VulkanSDK\1.4.313.0\Bin\glslangValidator.exe",
    ]:
        if Path(cand).exists():
            return cand
    # Fall back to PATH.
    from shutil import which
    w = which("glslangValidator.exe") or which("glslangValidator")
    return w


def check_getjitter(path, rettype):
    """The regression guard, run against BOTH twins.

    Before this session's fix, GetJitter computed Halton(index % 16, 2/3) on the
    GPU, so the 16-offset repeat the generated blue noise was built to remove was
    still shipping on every backend -- u_Jitter was declared and never read.
    The twins are hand-maintained separately, so one fixing and the other not is
    a silent, per-backend divergence that only a screenshot would otherwise catch.
    """
    src = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(rettype + r"\s+GetJitter\s*\([^)]*\)\s*\{(.*?)\n\}",
                  src, re.DOTALL)
    if not m:
        print(f"FAIL GetJitter not found in {path.name}")
        return False
    body = m.group(1)
    if "Halton" in body:
        print(f"FAIL {path.name}: GetJitter still calls Halton "
              f"-- the 16-offset repeat is back")
        return False
    if "u_Jitter" not in body:
        print(f"FAIL {path.name}: GetJitter does not read u_Jitter "
              f"-- the blue noise is being ignored")
        return False
    print(f"PASS {path.name}: GetJitter reads u_Jitter, no Halton")
    return True


def check_twin_agreement(glsl_src, hlsl_src):
    """The twins must agree on behaviour, not merely both compile.

    Each file compiles on its own, so a semantic divergence between them is
    invisible until one backend renders differently from the other. Every check
    below corresponds to a real defect that was fixed in ONE twin and would have
    shipped as a per-backend rendering bug.
    """
    checks = []

    # 1. History validity must be gated on FrameIndex > 0. On frame 0 the
    #    history buffer has never been written, so an ungated tap blends
    #    uninitialised memory. The GLSL twin shipped without this for a long
    #    time while HLSL had it.
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        ok = bool(re.search(r"validHistory\s*=\s*u_FrameIndex\s*>\s*0", src))
        checks.append((ok, f"{name}: validHistory gated on u_FrameIndex > 0"))

    # 2. The history-UV clamp must bound BOTH sides. min()-only leaves the
    #    lower side unclamped, so a fast leftward/upward camera motion samples
    #    outside the valid subregion and streaks the frame edge with stale data.
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        ok = bool(re.search(r"histUV\s*=\s*clamp\(histUV", src))
        checks.append((ok, f"{name}: history UV clamped on both sides"))

    # 3. The mip clamp must be widened past 4 in BOTH twins. It used to be
    #    clamp(mipLevel, 0, 4) while ComputeMipLevels allocates a full chain
    #    (12 levels for 2048^2), making levels 5-11 unreachable and leaving
    #    ~33 MB of uploaded mips unreadable. The HLSL twin's comment claimed the
    #    cap was "a deliberate quality/performance limit", which rationalised an
    #    accident -- so assert the VALUE, not the intent.
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        m = re.search(r"mipLevel\s*=\s*clamp\(mipLevel,\s*0\.0f,\s*([\d.]+)f?\)", src)
        ok = bool(m) and float(m.group(1)) > 4.0
        detail = f" (found {m.group(1)})" if m else " (no clamp found)"
        checks.append((ok, f"{name}: mip clamp widened past 4{detail}"))

    # 4. Neither twin may still be computing Halton jitter anywhere reachable.
    #    The old GetJitter body is gone; assert no Halton call survives in it.
    for name, src, rettype in (("glsl", glsl_src, "vec2"), ("hlsl", hlsl_src, "float2")):
        m = re.search(rettype + r"\s+GetJitter\s*\([^)]*\)\s*\{(.*?)\n\}",
                      src, re.DOTALL)
        ok = bool(m) and "Halton" not in m.group(1) and "u_Jitter" in m.group(1)
        checks.append((ok, f"{name}: GetJitter reads u_Jitter, no Halton"))

    print()
    all_ok = True
    for ok, label in checks:
        print(("  PASS  " if ok else "  FAIL  ") + label)
        all_ok = all_ok and ok
    return all_ok


def main():
    if not GLSL.exists():
        print(f"FAIL not found: {GLSL}")
        return 1
    glslang = find_glslang()
    if not glslang:
        print("FAIL glslangValidator not found (checked VulkanSDK and PATH)")
        return 1

    src = GLSL.read_text(encoding="utf-8", errors="replace")
    # The file's real #version line is COMMENTED OUT (the GL driver supplies it)
    # and there is a "#type compute" GL LOADER directive, neither of which is
    # GLSL. Left in place glslang reports a bogus 'es-profile shader' error and
    # 'invalid directive: type'. Supply the version the file's own header cites
    # as the floor for image2D and SSBOs.
    src = re.sub(r"^\s*#version.*$", "", src, flags=re.MULTILINE)
    src = re.sub(r"^\s*#type\s+compute.*$", "", src, flags=re.MULTILINE)
    src = "#version 430\n" + src

    with tempfile.TemporaryDirectory() as td:
        # .comp, not .glsl: glslangValidator keys partly off the extension and
        # treats a .glsl argument as a request to emit a C header, printing its
        # usage instead of compiling.
        f = Path(td) / "nova.comp"
        out_spv = Path(td) / "nova.spv"
        f.write_text(src, encoding="utf-8")
        # RunBilateralBlur is shared with the HLSL twin via an #include of
        # NovaShared_Bilateral.h, which glslangValidator resolves RELATIVE TO THE
        # COMPILING FILE -- and that file is this temp copy, not the shader dir.
        # So the include has to be copied in beside it or the compile fails with
        # "Could not process include directive". Copy every NOVA_SHARED_* the
        # shader pulls in, so adding a second shared pass needs only a new entry
        # here.
        for inc in NOVA_SHARED_INCLUDES:
            src_inc = SHADER_DIR / inc
            if not src_inc.exists():
                print(f"FAIL {GLSL.name} includes {inc} but it is missing from {SHADER_DIR}")
                return 1
            shutil.copy2(src_inc, Path(td) / inc)
        # Guard the reason this list is empty. The verifier resolving an include is
        # NOT the same as the engine resolving it: OpenGLShader::PreProcess does not
        # expand includes at all. So an #include appearing in the GLSL is a defect
        # unless that preprocessing has been fixed, and it would otherwise be a
        # green test over a shader the OpenGL driver cannot actually load.
        m = re.search(r'^\s*#include\s+[<"]([^>"]+)[>"]', src, re.M)
        if m and m.group(1) not in NOVA_SHARED_INCLUDES:
            print(f"FAIL {GLSL.name} has #include {m.group(1)} but NOVA_SHARED_INCLUDES "
                  f"is empty. The engine's OpenGL path cannot expand includes; see "
                  f"docs/NOVA_SHADER_PIPELINE.md.")
            return 1
        # -G = OpenGL SPIR-V semantics, which is what the GL backend needs and
        # which permits the loose uniforms this file is written with. NOT -V:
        # Vulkan semantics reject loose non-opaque uniforms outright.
        r = subprocess.run([glslang, "-G", "--auto-map-locations", "--auto-map-bindings",
                            str(f), "-o", str(out_spv)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not out_spv.exists():
            print("FAIL glslangValidator (-G, OpenGL semantics) rejected the shader:")
            print((r.stdout + r.stderr)[-2500:])
            return 1
        print(f"PASS GLSL compiles under OpenGL SPIR-V semantics "
              f"({out_spv.stat().st_size:,} bytes)")

    ok = check_twin_agreement(
        GLSL.read_text(encoding="utf-8", errors="replace"),
        HLSL.read_text(encoding="utf-8", errors="replace"),
    )
    print()
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())