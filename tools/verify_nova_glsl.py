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

It also asserts that the volumetric multi-light cap is IDENTICAL in both twins.
That cap is a performance multiplier (each march step runs one BVH traversal
per light), so a divergence there is both a 4x backend difference and a
lighting-correctness difference, and it compiles cleanly either way.

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

    # 1. EVERY history-validity declaration must be gated on FrameIndex > 0. On
    #    frame 0 the history buffer has never been written, so an ungated tap
    #    blends uninitialised memory. The GLSL twin shipped without this for a
    #    long time while HLSL had it.
    #
    #    The guard must be over ALL declarations, not "at least one". Each twin
    #    has FOUR `bool validHistory = ...;` declarations (temporal accumulation,
    #    bilateral blur, and two more with different condition shapes), and the
    #    old form -- re.search, first match anywhere -- passed vacuously as long
    #    as ONE of them was gated. Mutating two of the four therefore left the
    #    check green while shipping frame-0 history garbage in the other two.
    #    tools/verify_nova_glsl_negative_control.py caught exactly that.
    decl = re.compile(r"bool\s+validHistory\s*=\s*([^;]+);", re.DOTALL)
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        decls = decl.findall(src)
        ungated = [d.strip() for d in decls
                   if not re.search(r"u_FrameIndex\s*>\s*0", d)]
        ok = bool(decls) and not ungated
        detail = f" ({len(decls)} declarations, {len(ungated)} ungated)"
        checks.append((ok, f"{name}: every validHistory gated on u_FrameIndex > 0{detail}"))

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

    # 5. The trace G-buffer packs the world normal into .yzw -- its writer is
    #    float4(ndcDepth, normal * 0.5 + 0.5), so .r is DEPTH and the normal must
    #    decode from .gba. The GLSL twin read .rgb in five places (three of them
    #    pre-existing in RunBilateralBlur), decoding a normal whose first
    #    component is a depth value; the resulting edge-stop rejects valid
    #    neighbours and lets the filter smear across surfaces. Assert the decode
    #    channel rather than trusting it: BOTH files still compile when it is
    #    wrong, so compilation is no evidence and the guard would otherwise pass.
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        n_rgb = len(re.findall(r"s_NormalBuffer[^;]*?\)\.rgb", src))
        n_gba = len(re.findall(r"s_NormalBuffer[^;]*?\)\.gba", src))
        ok = n_gba > 0 and n_rgb == 0
        checks.append((ok, f"{name}: normal decoded from .gba not .rgb "
                           f"(gba={n_gba}, rgb={n_rgb})"))

    # 6. The SVGF luminance weight must be built from the NEIGHBOUR's moment so it
    #    varies per tap. A weight built from the CENTRE's variance alone is the
    #    same constant for all nine taps and cancels exactly in the kernel
    #    normalisation -- inert. The filter then silently degenerates to a plain
    #    bilateral blur while still compiling and still looking plausible.
    #    The old form was `1.0f + variance * 5.0f`.
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        inert = "1.0f + variance * 5.0f" in src
        uses_neighbour = bool(re.search(r"exp\(-abs\(moments\.x\s*-\s*neighborMean\)", src))
        ok = (not inert) and uses_neighbour
        checks.append((ok, f"{name}: variance weight varies per tap "
                           f"(inert_form={inert}, neighbour_form={uses_neighbour})"))

    # OpenGL guarantees only eight compute image uniforms. The driver failed
    # this shader at startup with C7624 when a ninth declaration became active,
    # even though the bindings themselves were all in the 0..7 range. Reuse
    # image unit 6 by pass instead of declaring separate TraceGBuffer/Moments
    # uniforms; the host rebinds that unit before the SVGF passes.
    image_pattern = r"uniform\s+(?:(?:readonly|writeonly)\s+)?image\w+\s+(\w+)"
    image_uniforms = re.findall(image_pattern, glsl_src)
    image_budget_ok = len(image_uniforms) <= 8
    checks.append((image_budget_ok, f"glsl: compute image uniforms fit guaranteed limit "
                                  f"(found {len(image_uniforms)}, maximum 8)"))

    # Negative control: a synthetic ninth image uniform must make the same
    # detector reject the source, even when it reuses a legal binding index.
    overflow_probe = glsl_src + chr(10) + "layout(rgba32f, binding = 7) uniform image2D _ImageBudgetProbe;" + chr(10)
    overflow_count = len(re.findall(image_pattern, overflow_probe))
    checks.append((overflow_count > 8, "negative control: ninth image uniform is detected"))

    # 8. The volumetric multi-light cap must be IDENTICAL in both twins.
    #    lightEnd = min(u_ALightCount, <cap>) bounds the per-step light loop, and
    #    every iteration of that loop runs IsOccluded() -- a COMPLETE BVH traversal
    #    over every scene instance, not a cheap test. The per-pixel cost is
    #    therefore steps x cap traversals: at 32 steps, cap 32 is 1024 traversals
    #    and cap 8 is 256, a 4x backend difference.
    #    Worse, it is a CORRECTNESS divergence, not a quality preference: the same
    #    scene and the same quality preset light differently depending on the
    #    backend. It compiles cleanly either way, so neither glslang nor fxc can
    #    see it -- only a source-level agreement check can.
    #    GLSL shipped 32 while HLSL shipped 8; that was the whole observed
    #    "OpenGL <4 fps vs DX ~20 fps" gap. Both now read WL_VOL_LIGHT_CAP, and
    #    this asserts the two files cannot drift apart again.
    # The cap is read from the #define, not from the min() call: the min() call
    # may legitimately say WL_VOL_LIGHT_CAP (that is the whole point of naming
    # it), so the NUMBER lives in exactly one place per twin and the define is
    # the only authoritative source. The two defines are then compared.
    cap_define = re.compile(r"(?m)^#define\s+WL_VOL_LIGHT_CAP\s+(\d+)")
    caps = {}
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        m = cap_define.search(src)
        # An unmatched detector is a FAILURE, not a pass: the mutator's target
        # string no longer existing would otherwise be silently accepted. See
        # "Every guard must be proven failable" in
        # references/shader-twins-and-guard-design.md.
        if not m:
            checks.append((False, f"{name}: #define WL_VOL_LIGHT_CAP with a numeric "
                                 f"value found"))
            continue
        caps[name] = int(m.group(1))
        checks.append((True, f"{name}: volumetric light cap = {caps[name]}"))

    if len(caps) == 2:
        same = caps["glsl"] == caps["hlsl"]
        detail = f" (glsl={caps['glsl']}, hlsl={caps['hlsl']})"
        checks.append((same, "twins agree on the volumetric light cap" +
                             ("" if same else detail)))

    # 8a. Both twins must actually USE the named constant in the min(), not just
    #     define it. A defined-but-unused symbol passes 8 and 8b while the loop
    #     still reads a hardcoded number -- the divergence survives, only now it
    #     is one step further from the guard. Assert the call site names it.
    #
    #     The cap is now applied through a local (`volLightCap`), because the
    #     runtime uniform u_VolLightCap overrides it and the local must be clamped
    #     to the compile-time array size. So the assertion accepts either form:
    #     the direct `min(u_ALightCount, WL_VOL_LIGHT_CAP)`, or a local that is
    #     itself derived from WL_VOL_LIGHT_CAP and then used in the min(). What
    #     the guard is really asserting is that the effective cap cannot be a bare
    #     number in either file.
    direct = re.compile(
        r"lightEnd\s*=\s*min\(\s*u_ALightCount\s*,\s*WL_VOL_LIGHT_CAP\s*\)")
    local_derived = re.compile(
        r"volLightCap\s*=\s*min\(\s*volLightCap\s*,\s*WL_VOL_LIGHT_CAP\s*\)")
    local_used = re.compile(
        r"lightEnd\s*=\s*min\(\s*u_ALightCount\s*,\s*volLightCap\s*\)")
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        ok = (direct.search(src) is not None) or (
            local_derived.search(src) is not None and local_used.search(src) is not None)
        checks.append((ok, f"{name}: volumetric light loop bounds on WL_VOL_LIGHT_CAP"))

    # 8b. Both twins must actually NAME the cap rather than hardcoding the
    #     number. A literal in each file is how they diverged in the first place,
    #     and it still passes the agreement check above while being two numbers
    #     that can drift. (Redundant with the detector above by design -- a
    #     second, independently-worded assertion so a single edit cannot
    #     silently disable the check.)
    for name, src in (("glsl", glsl_src), ("hlsl", hlsl_src)):
        ok = bool(re.search(r"(?m)^#define\s+WL_VOL_LIGHT_CAP\s+\d+", src))
        checks.append((ok, f"{name}: defines WL_VOL_LIGHT_CAP with a numeric value"))

    # Negative control: the cap detector must reject a divergent cap. This is the
    # exact defect that shipped -- GLSL 32, HLSL 8 -- injected into a temp copy.
    # A control that only mutates a report cannot detect a broken comparison.
    # 32 and 16 are chosen to differ from the real value (8) in BOTH twins, so
    # each substitution is a real edit rather than a no-op -- a substitution that
    # rewrites 8 as 8 changes nothing and the control passes vacuously.
    g_mutated = cap_define.sub("#define WL_VOL_LIGHT_CAP 32", glsl_src, count=1)
    h_mutated = cap_define.sub("#define WL_VOL_LIGHT_CAP 16", hlsl_src, count=1)
    if g_mutated == glsl_src or h_mutated == hlsl_src:
        checks.append((False, "negative control: cap mutator is vacuous -- the "
                             "#define no longer exists in a twin"))
    else:
        gm = cap_define.search(g_mutated)
        hm = cap_define.search(h_mutated)
        diverges = bool(gm and hm and gm.group(1) != hm.group(1))
        checks.append((diverges, f"negative control: divergent cap detected "
                                 f"(mutated glsl={gm.group(1) if gm else '?'}, "
                                 f"hlsl={hm.group(1) if hm else '?'})"))

    # Negative control: a twin that hardcodes the cap instead of naming it must
    # be rejected by check 8b even though check 8 would pass on the value alone.
    g_literal = cap_define.sub("#define WL_VOL_LIGHT_CAP", glsl_src, count=1)
    g_literal_ok = bool(cap_define.search(g_literal))
    checks.append((not g_literal_ok, "negative control: hardcoded cap (no #define) is detected"))

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