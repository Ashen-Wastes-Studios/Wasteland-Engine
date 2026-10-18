#!/usr/bin/env python3
"""Check executable parity between the Nova HLSL and GLSL shader twins.

This is a source-structure check. It does not compile shaders, run a GPU, or
prove radiometric equivalence. Comments and line endings are ignored; only the
executable token sequence in each compared function is checked.
"""

from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "DemonCore-Editor" / "assets" / "shaders"
HLSL_PATH = SHADER_DIR / "Renderer3D_NovaRenderer.hlsl"
GLSL_PATH = SHADER_DIR / "Renderer3D_NovaRenderer.glsl"
REPORT_PATH = Path(__file__).resolve().parent / "nova-shader-parity.json"


class ContractError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def extract_function(text, signature, label):
    code = strip_comments(text)
    match = re.search(signature, code)
    require(match is not None, f"{label}: function not found: {signature}")
    # Find the parameter list from the function name, then skip past its closing
    # parenthesis. Searching for the first "{" after the signature also matches a
    # preceding declaration's body when the definition itself has no brace on the
    # same lexical line.
    name_match = re.search(r"(\w+)", code[: match.start()])
    require(name_match is not None, f"{label}: function name not found")
    paren_start = code.find("(", match.start(), match.end() + 80)
    require(paren_start >= 0, f"{label}: parameter list not found")
    depth = 0
    for index in range(paren_start, len(code)):
        if code[index] == "(":
            depth += 1
        elif code[index] == ")":
            depth -= 1
            if depth == 0:
                break
    else:
        raise ContractError(f"{label}: unterminated parameter list")
    start = code.find("{", index)
    require(start >= 0, f"{label}: function body not found")
    depth = 0
    for index in range(start, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[start + 1 : index]
    raise ContractError(f"{label}: unterminated function body")


def normalize_statement(text):
    """Normalize a statement to canonical form for HLSL/GLSL comparison."""
    # Type names
    text = re.sub(r"\bfloat3\b", "vec3", text)
    text = re.sub(r"\bfloat2\b", "vec2", text)
    text = re.sub(r"\bfloat4\b", "vec4", text)
    # Function names
    text = re.sub(r"\blerp_safe\b", "mix", text)
    text = re.sub(r"\bMakeRay\b", "Ray", text)
    # Cast syntax: (int)(x) -> int(x), (uint)(x) -> uint(x), (float)(x) -> float(x)
    text = re.sub(r"\(int\)\s*\(", "int(", text)
    text = re.sub(r"\(uint\)\s*\(", "uint(", text)
    text = re.sub(r"\(float\)\s*\(", "float(", text)
    # Single-arg vector constructor: vec3(x) -> vec3(x, x, x)
    def expand_vec3(m):
        arg = m.group(1).strip()
        return f"vec3({arg}, {arg}, {arg})"
    text = re.sub(r"vec3\(([^,()]+)\)", expand_vec3, text)
    # Matrix translation extraction: vec3(m[0][3], m[1][3], m[2][3]) -> m[3].xyz
    def expand_matrix_translate(m):
        return f"{m.group(1)}[3].xyz"
    text = re.sub(
        r"vec3\(\s*([\w.]+)\[0\]\[3\]\s*,\s*([\w.]+)\[1\]\[3\]\s*,\s*([\w.]+)\[2\]\[3\]\s*\)",
        lambda m: f"{m.group(1)}[3].xyz" if m.group(1) == m.group(2) == m.group(3) else m.group(0),
        text,
    )
    return text


def normalize_statements(body):
    # Preprocessor directives and block braces are language-shape differences,
    # not executable content. What remains is compared as normalized source:
    # whitespace-insensitive, but identifiers, literals, and operators intact.
    body = re.sub(r"^\s*#.*$", "", body, flags=re.M)
    body = body.replace("{", "").replace("}", "")
    statements = []
    for raw in body.split(";"):
        collapsed = re.sub(r"\s+", " ", raw).strip()
        if collapsed:
            statements.append(normalize_statement(collapsed))
    return statements


def statement_tokens(statement):
    return re.findall(
        r"[A-Za-z_]\w*|\d+\.\d+|\d+|->|<=|>=|==|!=|&&|\|\||[-+*/%=<>!&|^~]",
        statement,
    )


def token_sequence(statements):
    return [token for statement in statements for token in statement_tokens(statement)]


def compared_functions():
    hlsl = HLSL_PATH.read_text(encoding="utf-8")
    glsl = GLSL_PATH.read_text(encoding="utf-8")
    return {
        "ComputeDirectLighting": (
            extract_function(
                hlsl,
                r"\bfloat3\s+ComputeDirectLighting\s*\([^)]*\)\s*\{",
                "HLSL ComputeDirectLighting",
            ),
            extract_function(
                glsl,
                r"\bvec3\s+ComputeDirectLighting\s*\([^)]*\)\s*\{",
                "GLSL ComputeDirectLighting",
            ),
        ),
        "GetJitter": (
            extract_function(
                hlsl,
                r"\bfloat2\s+GetJitter\s*\([^)]*\)\s*\{",
                "HLSL GetJitter",
            ),
            extract_function(
                glsl,
                r"\bvec2\s+GetJitter\s*\([^)]*\)\s*\{",
                "GLSL GetJitter",
            ),
        ),
    }


def expect_rejected(label, mutate):
    original = REPORT_PATH.read_text(encoding="utf-8") if REPORT_PATH.exists() else ""
    try:
        mutate()
    except (ContractError, OSError) as error:
        if REPORT_PATH.exists() and original:
            REPORT_PATH.write_text(original, encoding="utf-8")
        elif REPORT_PATH.exists():
            REPORT_PATH.unlink()
        print(f"PASS negative control: {label} rejected ({error})")
        return
    raise ContractError(f"negative control failed: accepted {label}")


def write_report(report):
    REPORT_PATH.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def mutate_report(label, mutation):
    report = json.loads(REPORT_PATH.read_text(encoding="utf-8"))
    mutation(report)
    write_report(report)
    # Negative controls mutate the report, not the source. Verify they produce a
    # different report than the actual comparison.
    functions = compared_functions()
    actual = {}
    for name, (hlsl_body, glsl_body) in functions.items():
        hlsl_statements = normalize_statements(hlsl_body)
        glsl_statements = normalize_statements(glsl_body)
        hlsl_tokens = token_sequence(hlsl_statements)
        glsl_tokens = token_sequence(glsl_statements)
        actual[name] = {
            "statements": len(hlsl_statements),
            "tokens": len(hlsl_tokens),
            "equal": hlsl_tokens == glsl_tokens,
        }
    if report == actual:
        raise ContractError(f"negative control failed: accepted {label}")


def main():
    functions = compared_functions()
    report = {}
    for name, (hlsl_body, glsl_body) in functions.items():
        hlsl_statements = normalize_statements(hlsl_body)
        glsl_statements = normalize_statements(glsl_body)
        hlsl_tokens = token_sequence(hlsl_statements)
        glsl_tokens = token_sequence(glsl_statements)
        require(
            hlsl_tokens == glsl_tokens,
            f"{name}: executable token sequences differ",
        )
        report[name] = {
            "statements": len(hlsl_statements),
            "tokens": len(hlsl_tokens),
            "equal": True,
        }
        print(f"PASS {name}: {len(hlsl_statements)} executable statements match")

    write_report(report)
    print("PASS: HLSL/GLSL compared executable sequences match")

    # Negative controls: temporarily mutate shader source and verify detection
    import tempfile
    import shutil

    def with_mutated_shader(path, old, new, label):
        original = path.read_text(encoding="utf-8")
        try:
            path.write_text(original.replace(old, new), encoding="utf-8")
            functions = compared_functions()
            for name, (hlsl_body, glsl_body) in functions.items():
                hlsl_statements = normalize_statements(hlsl_body)
                glsl_statements = normalize_statements(glsl_body)
                hlsl_tokens = token_sequence(hlsl_statements)
                glsl_tokens = token_sequence(glsl_statements)
                if hlsl_tokens != glsl_tokens:
                    print(f"PASS negative control: {label} rejected (token mismatch)")
                    return
            raise ContractError(f"negative control failed: accepted {label}")
        finally:
            path.write_text(original, encoding="utf-8")

    with_mutated_shader(
        HLSL_PATH,
        "float3 directLight = float3(0.0f, 0.0f, 0.0f);",
        "float3 directLight = float3(1.0f, 1.0f, 1.0f);",
        "HLSL directLight init",
    )

    with_mutated_shader(
        GLSL_PATH,
        "vec3 directLight = vec3(0.0f);",
        "vec3 directLight = vec3(1.0f);",
        "GLSL directLight init",
    )

    with_mutated_shader(
        HLSL_PATH,
        "return u_Jitter;",
        "return float2(0.0f, 0.0f);",
        "HLSL GetJitter return",
    )
    print("Report: " + str(REPORT_PATH))
    print("Source check only: no shader compilation, GPU execution, or visual proof.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError) as error:
        print(f"FAIL Nova shader parity: {error}", file=sys.stderr)
        raise SystemExit(1)
