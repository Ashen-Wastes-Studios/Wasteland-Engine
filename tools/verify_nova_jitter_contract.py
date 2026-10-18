#!/usr/bin/env python3
"""Check Nova's cross-backend blue-noise jitter source contract.

These are source-level checks only. They do not compile or run the renderer and
are not visual proof that jitter looks correct on any GPU/backend.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / "Wasteland/src/Wasteland/Renderer/Renderer3D.cpp"
BLUE_NOISE = ROOT / "Wasteland/src/Wasteland/Renderer/BlueNoise.cpp"
SHADER_DIR = ROOT / "DemonCore-Editor/assets/shaders"
SHADERS = {
    "HLSL": SHADER_DIR / "Renderer3D_NovaRenderer.hlsl",
    "GLSL": SHADER_DIR / "Renderer3D_NovaRenderer.glsl",
}


class ContractError(AssertionError):
    """A source contract was absent or did not match its required value."""


def read_source(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContractError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc


def strip_comments(source: str) -> str:
    """Replace C/C++/shader comments with whitespace, preserving code and lines.

    Quoted strings and character literals are kept intact, so comment-like text
    in a literal cannot hide or manufacture a source contract.
    """
    out: list[str] = []
    i = 0
    state = "code"
    quote = ""
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == '"' or ch == "'":
                quote = ch
                state = "quoted"
                out.append(ch)
                i += 1
            elif ch == "/" and nxt == "/":
                state = "line_comment"
                out.extend((" ", " "))
                i += 2
            elif ch == "/" and nxt == "*":
                state = "block_comment"
                out.extend((" ", " "))
                i += 2
            else:
                out.append(ch)
                i += 1
        elif state == "quoted":
            out.append(ch)
            if ch == "\\" and i + 1 < len(source):
                out.append(source[i + 1])
                i += 2
            else:
                if ch == quote:
                    state = "code"
                i += 1
        elif state == "line_comment":
            if ch == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            i += 1
        else:  # block_comment
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                state = "code"
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
    return "".join(out)


def _matching_brace(source: str, opening: int) -> int:
    depth = 0
    i = opening
    quote = ""
    while i < len(source):
        ch = source[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch == '"' or ch == "'":
            quote = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ContractError("unterminated function body")


def function_body(source: str, signature: str, label: str) -> str:
    code = strip_comments(source)
    match = re.search(signature, code, flags=re.MULTILINE)
    if not match:
        raise ContractError(f"{label}: function definition not found")
    opening = code.find("{", match.start(), match.end())
    if opening < 0:
        raise ContractError(f"{label}: function body not found")
    closing = _matching_brace(code, opening)
    return code[opening + 1 : closing]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def check_blue_noise(blue_source: str, renderer_source: str) -> None:
    sample = function_body(
        blue_source,
        r"\bvoid\s+BlueNoise::Sample\s*\([^)]*\)\s*\{",
        "BlueNoise::Sample",
    )
    jitter = function_body(
        blue_source,
        r"\bvoid\s+BlueNoise::Jitter\s*\([^)]*\)\s*\{",
        "BlueNoise::Jitter",
    )
    get_jitter = function_body(
        renderer_source,
        r"\bglm::vec2\s+Renderer3D::GetCurrentJitter\s*\([^)]*\)\s*\{",
        "Renderer3D::GetCurrentJitter",
    )

    require(
        re.search(r"\bSample\s*\(\s*width\s*,\s*height\s*,\s*frameIndex\s*,\s*0\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;", jitter),
        "BlueNoise::Jitter must sample the frame/pixel-indexed blue-noise source",
    )
    sampled = re.search(
        r"\bSample\s*\(\s*width\s*,\s*height\s*,\s*frameIndex\s*,\s*0\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;",
        jitter,
    )
    assert sampled is not None
    x_name, y_name = sampled.groups()
    require(
        re.search(rf"\boutJitterX\s*=\s*{re.escape(x_name)}\s*-\s*0\.5f\s*;", jitter)
        and re.search(rf"\boutJitterY\s*=\s*{re.escape(y_name)}\s*-\s*0\.5f\s*;", jitter),
        "BlueNoise::Jitter must pass both sampled channels through as centered pixel offsets",
    )
    require(
        "s_Volume[" in sample and "Hash(" in sample and "s_Loaded" in sample,
        "BlueNoise::Sample must retain both the loaded-volume path and its deterministic fallback",
    )

    call = re.search(
        r"\bBlueNoise::Jitter\s*\(\s*static_cast\s*<\s*uint32_t\s*>\s*\(\s*frameIndex\s*\)\s*,\s*"
        r"static_cast\s*<\s*uint32_t\s*>\s*\(\s*viewportSize\.x\s*\)\s*,\s*"
        r"static_cast\s*<\s*uint32_t\s*>\s*\(\s*viewportSize\.y\s*\)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;",
        get_jitter,
    )
    require(call is not None, "GetCurrentJitter must source its offsets from BlueNoise::Jitter")
    assert call is not None
    x_name, y_name = call.groups()
    require(
        re.search(rf"\breturn\s+glm::vec2\s*\(\s*{re.escape(x_name)}\s*,\s*{re.escape(y_name)}\s*\)\s*;", get_jitter),
        "GetCurrentJitter must return the two values produced by BlueNoise::Jitter",
    )


def check_opengl_upload(renderer_source: str) -> None:
    body = function_body(
        renderer_source,
        r"\bvoid\s+Renderer3D::Flush\s*\(\s*\)\s*\{",
        "OpenGL Renderer3D::Flush",
    )
    computed = re.search(
        r"\bglm::vec2\s+(\w+)\s*=\s*GetCurrentJitter\s*\(\s*s_Data\.FrameIndex\s*,\s*viewportSize\s*\)\s*;",
        body,
    )
    require(computed is not None, "OpenGL flush must compute jitter with GetCurrentJitter for the trace viewport")
    assert computed is not None
    value = re.escape(computed.group(1))
    upload = re.search(
        rf"\bglUniform2f\s*\(\s*s_Data\.Loc_Jitter\s*,\s*{value}\.x\s*,\s*{value}\.y\s*\)\s*;",
        body,
    )
    require(upload is not None, "OpenGL must upload the GetCurrentJitter result to u_Jitter via Loc_Jitter")
    first_dispatch = re.search(r"\bglDispatchCompute\s*\(", body)
    require(
        first_dispatch is not None and upload.start() < first_dispatch.start(),
        "OpenGL must upload u_Jitter before its first compute dispatch",
    )
    require(
        re.search(r"\bglm::vec2\s+viewportSize\s*=\s*glm::vec2\s*\(\s*width\s*,\s*height\s*\)\s*;", body)
        and re.search(r"\buint32_t\s+width\s*=\s*s_Data\.RayTracingWidth\s*;", body)
        and re.search(r"\buint32_t\s+height\s*=\s*s_Data\.RayTracingHeight\s*;", body),
        "OpenGL jitter must be sized for the actual ray-tracing target",
    )


def check_nova_upload(renderer_source: str, function_name: str, backend_handle: str) -> None:
    body = function_body(
        renderer_source,
        rf"\bstatic\s+void\s+{re.escape(function_name)}\s*\(\s*\)\s*\{{",
        function_name,
    )
    require(
        re.search(rf"\bauto\s*&\s*n\s*=\s*\*\s*{re.escape(backend_handle)}\s*;", body),
        f"{function_name} must upload through its {backend_handle} Nova instance",
    )
    computed = re.search(
        r"\bglm::vec2\s+(\w+)\s*=\s*Renderer3D::GetCurrentJitter\s*\(\s*s_Data\.FrameIndex\s*,\s*"
        r"glm::vec2\s*\(\s*\(float\)s_Data\.RayTracingWidth\s*,\s*\(float\)s_Data\.RayTracingHeight\s*\)\s*\)\s*;",
        body,
    )
    require(computed is not None, f"{function_name} must compute jitter from GetCurrentJitter at the trace size")
    assert computed is not None
    value = re.escape(computed.group(1))
    upload = re.search(rf"\bn\.Set\s*\(\s*\"u_Jitter\"\s*,\s*{value}\s*\)\s*;", body)
    require(upload is not None, f"{function_name} must upload the actual GetCurrentJitter result as u_Jitter")
    schedule = re.search(r"\bRunNovaPassSchedule\s*\(", body)
    require(
        schedule is not None and upload.start() < schedule.start(),
        f"{function_name} must upload u_Jitter before the Nova dispatch schedule",
    )


def check_shader(shader_source: str, language: str) -> None:
    code = strip_comments(shader_source)
    if language == "HLSL":
        declaration = r"\bfloat2\s+u_Jitter\s*;"
        vector_type = "float2"
    else:
        declaration = r"\buniform\s+vec2\s+u_Jitter\s*;"
        vector_type = "vec2"
    require(re.search(declaration, code), f"{language} shader must declare its u_Jitter input")

    jitter_body = function_body(
        code,
        rf"\b{vector_type}\s+GetJitter\s*\(\s*int\s+frameIndex\s*,\s*float\s+cameraMoved\s*\)\s*\{{",
        f"{language} GetJitter",
    )
    normalized = re.sub(r"\s+", " ", jitter_body).strip()
    require(
        normalized == "return u_Jitter;",
        f"{language} GetJitter must return u_Jitter directly (no Halton/random placeholder)",
    )
    require(
        re.search(
            rf"\b{vector_type}\s+jitter\s*=\s*GetJitter\s*\(\s*u_FrameIndex\s*,\s*u_CameraMoved\s*\)\s*;",
            code,
        ),
        f"{language} primary-ray path must obtain its jitter through GetJitter",
    )
    number = r"2(?:\.0)?f?"
    minus_one = r"1(?:\.0)?f?"
    require(
        re.search(
            rf"\b{vector_type}\s+jitteredUV\s*=\s*\(\s*\(\s*{vector_type}\s*\(\s*pixelCoords\s*\)\s*\+\s*jitter\s*\)\s*/\s*"
            rf"{vector_type}\s*\(\s*traceSize\s*\)\s*\)\s*\*\s*{number}\s*-\s*{minus_one}\s*;",
            code,
        ),
        f"{language} primary-ray coordinates must consume that jitter in pixel space",
    )


def expect_rejected(label: str, check, *args) -> None:
    try:
        check(*args)
    except ContractError as exc:
        print(f"PASS negative control: {label} rejected ({exc})")
        return
    raise ContractError(f"negative control failed: verifier accepted {label}")


def main() -> int:
    try:
        renderer_source = read_source(RENDERER)
        blue_source = read_source(BLUE_NOISE)
        shader_sources = {name: read_source(path) for name, path in SHADERS.items()}

        check_blue_noise(blue_source, renderer_source)
        print("PASS BlueNoise source -> GetCurrentJitter returns centered sampled offsets")

        check_opengl_upload(renderer_source)
        print("PASS OpenGL uploads the computed jitter to u_Jitter")

        nova_backends = (
            ("DX11", "FlushDX11Nova", "s_Nova"),
            ("DX12", "FlushDX12Nova", "s_Nova12"),
            ("Vulkan", "FlushVulkanNova", "s_NovaVk"),
        )
        for label, function_name, backend_handle in nova_backends:
            check_nova_upload(renderer_source, function_name, backend_handle)
            print(f"PASS {label} uploads the computed jitter to u_Jitter")

        for language, shader_source in shader_sources.items():
            check_shader(shader_source, language)
            print(f"PASS {language} GetJitter returns u_Jitter and the primary-ray path consumes it")

        # Mutate source in memory only: prove the same checks reject the two key regressions.
        executable_renderer = strip_comments(renderer_source)
        dx11_body = function_body(
            executable_renderer,
            r"\bstatic\s+void\s+FlushDX11Nova\s*\(\s*\)\s*\{",
            "DX11 missing-upload negative control",
        )
        no_upload, count = re.subn(
            r"\bn\.Set\s*\(\s*\"u_Jitter\"\s*,\s*jit\s*\)\s*;",
            "",
            dx11_body,
            count=1,
        )
        require(count == 1, "could not construct missing-upload negative control from current DX11 code")
        mutated_renderer = executable_renderer.replace(dx11_body, no_upload, 1)
        expect_rejected(
            "missing DX11 u_Jitter upload",
            check_nova_upload,
            mutated_renderer,
            "FlushDX11Nova",
            "s_Nova",
        )

        late_upload_body, count = re.subn(
            r"\bn\.Set\s*\(\s*\"u_Jitter\"\s*,\s*jit\s*\)\s*;",
            "",
            dx11_body,
            count=1,
        )
        require(count == 1, "could not construct late-upload negative control from current DX11 code")
        dispatch = re.search(
            r"\bRunNovaPassSchedule\s*\(\s*n\s*,\s*textures\s*\)\s*;",
            late_upload_body,
        )
        require(dispatch is not None, "could not locate DX11 dispatch schedule for late-upload control")
        late_upload_body = (
            late_upload_body[: dispatch.end()]
            + '\n        n.Set("u_Jitter", jit);'
            + late_upload_body[dispatch.end() :]
        )
        late_renderer = executable_renderer.replace(dx11_body, late_upload_body, 1)
        expect_rejected(
            "DX11 u_Jitter upload after the dispatch schedule",
            check_nova_upload,
            late_renderer,
            "FlushDX11Nova",
            "s_Nova",
        )

        hlsl_code = strip_comments(shader_sources["HLSL"])
        random_placeholder, count = re.subn(
            r"\breturn\s+u_Jitter\s*;",
            "return float2(random(), random());",
            hlsl_code,
            count=1,
        )
        require(count == 1, "could not construct random-jitter negative control from current HLSL code")
        expect_rejected("shader random placeholder jitter", check_shader, random_placeholder, "HLSL")

        print("Source checks only: passing does not establish compiled/runtime behavior or visual quality.")
        return 0
    except ContractError as exc:
        print(f"FAIL Nova jitter source contract: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
