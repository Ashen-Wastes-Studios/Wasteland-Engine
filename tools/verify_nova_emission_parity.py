#!/usr/bin/env python3
"""Check single-count surface emission parity in Nova shader twins.

This is a source-contract guard for the shared direct-lighting function. It does
not establish radiometric correctness, compilation on every driver, GPU output,
or visual quality.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "DemonCore-Editor" / "assets" / "shaders"
SOURCES = {
    "HLSL": (SHADER_DIR / "Renderer3D_NovaRenderer.hlsl", "float3", "float3(0.0f, 0.0f, 0.0f)"),
    "GLSL": (SHADER_DIR / "Renderer3D_NovaRenderer.glsl", "vec3", "vec3(0.0f)"),
}


class ContractError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def strip_comments(source):
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def direct_lighting_body(source, vector_type, label):
    code = strip_comments(source)
    signature = re.search(
        rf"\b{re.escape(vector_type)}\s+ComputeDirectLighting\s*\([^)]*\)\s*\{{",
        code,
    )
    require(signature is not None, f"{label}: ComputeDirectLighting definition not found")
    opening = code.find("{", signature.start(), signature.end())
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[opening + 1 : index]
    raise ContractError(f"{label}: unterminated ComputeDirectLighting body")


def check_body(body, label, expected_initializer):
    initializer = re.search(r"\bdirectLight\s*=\s*([^;]+);", body)
    require(initializer is not None, f"{label}: directLight initializer missing")
    require(
        re.sub(r"\s+", "", initializer.group(1))
        == re.sub(r"\s+", "", expected_initializer),
        f"{label}: directLight must start at zero before surface emission is added",
    )
    additions = re.findall(r"\bdirectLight\s*\+=\s*h\.emission\s*;", body)
    require(len(additions) == 1, f"{label}: expected exactly one surface-emission addition")
    require(
        len(re.findall(r"\bh\.emission\b", body)) == 1,
        f"{label}: surface emission appears outside its single addition",
    )


def expect_rejected(label, body, vector_type, initializer):
    try:
        check_body(body, label, initializer)
    except ContractError as error:
        print(f"PASS negative control: {label} rejected ({error})")
        return
    raise ContractError(f"negative control failed: accepted {label}")


def main():
    loaded = {}
    for label, (path, vector_type, initializer) in SOURCES.items():
        source = path.read_text(encoding="utf-8")
        body = direct_lighting_body(source, vector_type, label)
        check_body(body, label, initializer)
        loaded[label] = (body, vector_type, initializer)
        print(f"PASS {label}: surface emission is initialized to zero and added once")

    hlsl_body, hlsl_type, hlsl_zero = loaded["HLSL"]
    duplicated = re.sub(
        re.escape(hlsl_zero),
        "h.emission",
        hlsl_body,
        count=1,
    )
    require(duplicated != hlsl_body, "could not construct duplicate-emission negative control")
    expect_rejected("duplicated HLSL surface emission", duplicated, hlsl_type, hlsl_zero)

    glsl_body, glsl_type, glsl_zero = loaded["GLSL"]
    missing = re.sub(r"\bdirectLight\s*\+=\s*h\.emission\s*;", "", glsl_body, count=1)
    require(missing != glsl_body, "could not construct missing-emission negative control")
    expect_rejected("missing GLSL surface emission", missing, glsl_type, glsl_zero)

    print("Source checks only: no shader compilation, GPU execution, or visual proof.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError) as error:
        print(f"FAIL Nova emission parity: {error}", file=sys.stderr)
        raise SystemExit(1)
