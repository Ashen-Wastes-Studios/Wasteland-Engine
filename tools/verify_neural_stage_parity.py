#!/usr/bin/env python3
"""Check HLSL/GLSL parity of the indirect-lighting estimator region.

Why this verifier and not the existing one
-----------------------------------------
`verify_nova_shader_parity.py` compares two small functions
(`ComputeDirectLighting`, `GetJitter`). The region this one covers -- the
indirect bounce and its post-processing -- is far larger and lives inline in the
shading function rather than in a named helper, so it was never compared. That
gap is where the in-sample neural GI blend lived: a block present in one twin and
absent from the other, or carrying a different constant, would have shipped with
rc=0 from the existing verifier.

It matters more now, not less. Task 3 removed the in-estimator neural blend from
both twins at once. Two independent edits to the same 60-line region is exactly
the shape of change that produces a silent divergence -- and a divergence here is
invisible in a build log, because nothing compiles shaders at build time
(`docs/NOVA_SHADER_PIPELINE.md:86-89`).

What it checks
--------------
The region from `int maxIndirectRays` up to the mirror-reflection block. That is
the whole indirect estimator: the ray budget, the Russian-roulette lobe choice,
the bounce trace, and the accumulation. Comments are stripped and the remaining
statements are normalized the same way as `verify_nova_shader_parity.py`, so
HLSL/GLSL spelling differences (`float3`/`vec3`, `lerp_safe`/`mix`, `MakeRay`/
`Ray`, `(int)(x)`/`int(x)`) do not register as differences.

What it does NOT check
----------------------
It is a source-structure check. It does not compile a shader, run a GPU, or
prove the two backends render the same image. A semantic change that preserves
the token sequence -- e.g. swapping two operands that happen to commute -- passes
here and would need the pixel comparison in Task 3 step 6 to catch.

The negative control
--------------------
Mutating a constant in one twin must FAIL. Without that, a verifier whose
extraction silently returns an empty region passes vacuously -- which is a real
failure mode in this repo (see `verify_nova_glsl_negative_control.py`, where two
controls are exactly that).

Usage
-----
    python tools/verify_neural_stage_parity.py

Exit 0 = the regions match. Exit 1 = they differ, or a control was accepted.
Stdlib only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "DemonCore-Editor" / "assets" / "shaders"
HLSL_PATH = SHADER_DIR / "Renderer3D_NovaRenderer.hlsl"
GLSL_PATH = SHADER_DIR / "Renderer3D_NovaRenderer.glsl"

# The region boundaries. Both markers appear verbatim in both twins. The start
# marker is the ray-budget declaration rather than a function signature because
# this code is inline in the shading function, not a helper of its own.
START_MARKER = "int maxIndirectRays"
END_MARKER = "Sharp scene reflection for metals with no traced bounce"

# The two constants the region must carry identically. Task 3 removed the
# neural blend that used to key on u_NeuralLightStrength; if either twin still
# references it here, the removal was incomplete on that side.
FORBIDDEN_TOKENS = ("u_NeuralLightStrength", "wl_neural_indirect")


class ContractError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def extract_region(text: str, label: str) -> str:
    """The estimator region, from the ray budget to the mirror block.

    The markers are located in the RAW text and comments are stripped from the
    extracted slice. Stripping first would delete the end marker, which lives
    inside a `//` comment, and the search would fail on every file.
    """
    start = text.find(START_MARKER)
    require(start >= 0, f"{label}: start marker not found: {START_MARKER!r}")
    end = text.find(END_MARKER, start)
    require(end > start, f"{label}: end marker not found after start: {END_MARKER!r}")
    region = strip_comments(text[start:end])
    require(len(region.strip()) > 200, f"{label}: extracted region is implausibly "
                                       f"short ({len(region)} chars) -- the markers "
                                       f"may have moved, which would make this check "
                                       f"vacuous")
    return region


def normalize_statement(text: str) -> str:
    """Canonical form for HLSL/GLSL comparison. Mirrors verify_nova_shader_parity."""
    text = re.sub(r"\bfloat3\b", "vec3", text)
    text = re.sub(r"\bfloat2\b", "vec2", text)
    text = re.sub(r"\bfloat4\b", "vec4", text)
    text = re.sub(r"\blerp_safe\b", "mix", text)
    text = re.sub(r"\bMakeRay\b", "Ray", text)
    text = re.sub(r"\(int\)\s*\(", "int(", text)
    text = re.sub(r"\(uint\)\s*\(", "uint(", text)
    text = re.sub(r"\(float\)\s*\(", "float(", text)
    # Scalar splat vs explicit components. The twins spell the same value
    # differently by idiom: HLSL writes vec3(a, a, a) where GLSL writes
    # vec3(a), which is a scalar splat and means the identical thing. Without
    # this normalization the comparison reports a difference on a line neither
    # task touched -- which is how a real divergence gets ignored.
    def expand(match, width):
        arg = match.group(1).strip()
        return f"vec{width}({', '.join([arg] * width)})"
    text = re.sub(r"vec3\(([^,()]+)\)", lambda m: expand(m, 3), text)
    text = re.sub(r"vec4\(([^,()]+)\)", lambda m: expand(m, 4), text)
    return text


def statements_of(region: str) -> list:
    body = re.sub(r"^\s*#.*$", "", region, flags=re.M)
    body = body.replace("{", "").replace("}", "")
    out = []
    for raw in body.split(";"):
        collapsed = re.sub(r"\s+", " ", raw).strip()
        if collapsed:
            out.append(normalize_statement(collapsed))
    return out


def tokens_of(statements: list) -> list:
    pattern = r"[A-Za-z_]\w*|\d+\.\d+|\d+|->|<=|>=|==|!=|&&|\|\||[-+*/%=<>!&|^~]"
    return [tok for stmt in statements for tok in re.findall(pattern, stmt)]


def regions():
    hlsl = HLSL_PATH.read_text(encoding="utf-8")
    glsl = GLSL_PATH.read_text(encoding="utf-8")
    return (
        extract_region(hlsl, "HLSL"),
        extract_region(glsl, "GLSL"),
    )


def compare(label=""):
    """Return (hlsl_tokens, glsl_tokens) for the estimator region."""
    hlsl_region, glsl_region = regions()
    hlsl_tokens = tokens_of(statements_of(hlsl_region))
    glsl_tokens = tokens_of(statements_of(glsl_region))
    if label:
        print(f"{label}: {len(hlsl_tokens)} HLSL tokens vs {len(glsl_tokens)} GLSL tokens")
    return hlsl_tokens, glsl_tokens


def check_forbidden(label: str):
    """Neither twin may still reference the removed in-estimator neural GI."""
    for name, path in (("HLSL", HLSL_PATH), ("GLSL", GLSL_PATH)):
        region = extract_region(path.read_text(encoding="utf-8"), name)
        for token in FORBIDDEN_TOKENS:
            require(
                token not in region,
                f"{name}: estimator region still references {token} -- the "
                f"in-estimator neural GI removal was incomplete on this side",
            )
    print(f"PASS {label}: no in-estimator neural GI references remain")


def negative_control():
    """Mutating one twin's constant must FAIL. Guards against a vacuous pass."""
    mutated_old = "tileHit.normal * 0.001f"
    mutated_new = "tileHit.normal * 0.002f"
    for name, path, other in (("HLSL", HLSL_PATH, GLSL_PATH), ("GLSL", GLSL_PATH, HLSL_PATH)):
        original = path.read_text(encoding="utf-8")
        require(mutated_old in strip_comments(original),
                f"negative control: anchor {mutated_old!r} not found in {name} -- "
                f"the mutation would not apply and the control proves nothing")
        try:
            path.write_text(original.replace(mutated_old, mutated_new, 1), encoding="utf-8")
            hlsl_tokens, glsl_tokens = compare()
            require(
                hlsl_tokens != glsl_tokens,
                f"negative control failed: accepted a mutated {name} constant",
            )
            print(f"PASS negative control: {name} bounce offset mutation detected")
        finally:
            path.write_text(original, encoding="utf-8")


def main() -> int:
    hlsl_tokens, glsl_tokens = compare(label="estimator region")
    require(
        hlsl_tokens == glsl_tokens,
        "indirect-lighting estimator regions differ between HLSL and GLSL",
    )
    print("PASS: HLSL/GLSL indirect-lighting estimator regions match")

    check_forbidden("removed in-estimator neural GI")

    negative_control()

    print("Source check only: no shader compilation, GPU execution, or visual proof.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError) as error:
        print(f"FAIL neural stage parity: {error}", file=sys.stderr)
        raise SystemExit(1)
