#!/usr/bin/env python3
"""Fail if the CPU pushes a Nova constant the shader does not declare.

Why this exists
---------------
`NovaConstants::Set(name, value)` writes into a staging buffer at the offset the
D3D11/D3D12 reflection gives for `name`. A name the shader does not declare
returns `end()` from that lookup. That used to be a **silent return**: the write
vanished and the shader kept whatever stale value was in the buffer -- typically
zero from the last resize -- with no diagnostic. The result is a silently wrong
render, not a failure, which is exactly why it survives review.

The runtime now warns once per unknown name (`DX11Nova.h`, `DX12Nova.cpp`), but
a warning in a log nobody reads is a weak guard. This script is the strong one:
it fails deterministically, offline, on the exact condition the warning reports.

Scope
-----
The NVRHI path only. The OpenGL path does not use `Set()` -- it calls
`glUniform1i(glGetUniformLocation(prog, "u_Foo"), ...)` -- so a different
mechanism and a different failure mode (a `glGetUniformLocation` returning -1,
which is its own silent no-op). Check that separately if it ever bites.

The HLSL is the only shader `Set()` resolves against: DX11Nova compiles
`Renderer3D_NovaRenderer.hlsl` and reflects it, and DX12Nova does the same. So
every name pushed by the CPU must be declared in that one file.

Deliberately NOT an error
-------------------------
A name that IS declared but never read is fine and expected: `u_MaxBounces` and
`u_SamplesPerPixel` are uploaded to hold cbuffer layout while the shader ignores
them. Dead-but-declared is legitimate; declared-on-one-side-only is the bug.

Usage
-----
    python tools/verify_nova_constant_names.py

Exit 0 = every pushed name is declared. Exit 1 = at least one is not.
Stdlib only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CPP = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"
HLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"

# The value types Set() is instantiated for, plus the scalar/vector/matrix types
# that appear in the cbuffer. `u_SceneTextures` is an array and is handled by the
# optional bracket group.
DECL = (
    r"(?:int|uint|float|bool|double|float[234]|int[234]|uint[234]|float4x4|mat4)"
    r"\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;"
)


def pushed_names() -> set:
    """Every name the CPU hands to Set().

    Matches `n.Set("u_Foo", ...)`. The receiver is `n` in the call sites this
    engine has; a future rename of the local means updating this regex, which is
    the correct failure -- a silently un-checked call site is the defect.
    """
    src = CPP.read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r'\bn\.Set\(\s*"([A-Za-z_]\w*)"', src))


def declared_names() -> set:
    """Every name declared inside `cbuffer NovaConstants` in the HLSL.

    Restricted to that block on purpose: the shader has function-local variables
    and parameters that would otherwise match and mask a genuinely missing
    constant.
    """
    lines = HLSL.read_text(encoding="utf-8", errors="replace").splitlines()
    block, inside = [], False
    for line in lines:
        if not inside:
            if re.search(r"\bcbuffer\s+NovaConstants\b", line):
                inside = True
            continue
        # A closing brace at column 0 ends the cbuffer. The file's cbuffer is
        # the only one and it is not nested.
        if line.startswith("}"):
            break
        block.append(line)
    text = "\n".join(block)
    # Strip comments first: a commented-out declaration would otherwise look
    # like a live one and hide a real gap.
    text = re.sub(r"//[^\n]*", "", text)
    return set(re.findall(DECL, text))


def main() -> int:
    if not CPP.is_file():
        print(f"FAIL  cannot read {CPP}", file=sys.stderr)
        return 1
    if not HLSL.is_file():
        print(f"FAIL  cannot read {HLSL}", file=sys.stderr)
        return 1

    pushed = pushed_names()
    declared = declared_names()

    if not pushed:
        # An empty result means the regex stopped matching -- a vacuous pass is
        # worse than a failure, because it looks like a clean check.
        print("FAIL  no n.Set() call sites matched -- the extraction is broken, "
              "not the shader", file=sys.stderr)
        return 1
    if not declared:
        print("FAIL  no cbuffer NovaConstants declarations matched -- the "
              "extraction is broken, not the shader", file=sys.stderr)
        return 1

    missing = sorted(pushed - declared)
    unused = sorted(declared - pushed)

    print(f"CPU pushes {len(pushed)} constants; HLSL declares {len(declared)}")

    if missing:
        print("\nFAIL  pushed by the CPU but NOT declared by the shader:")
        for name in missing:
            print(f"  - {name}")
        print("\nThe upload is dropped and the shader keeps its stale value. "
              "Either fix the name or add the declaration.")
        print("\nNote: a name that IS declared but never read is not an error "
              "here -- dead uniforms are legitimate.")
        return 1

    print(f"\nPASS  all {len(pushed)} pushed constants are declared by the shader")
    if unused:
        # Informational only. Declared-but-unpushed costs cbuffer space and is
        # worth knowing about, but it is not a correctness problem.
        print(f"      ({len(unused)} declared but never pushed: {', '.join(unused[:8])}"
              f"{' ...' if len(unused) > 8 else ''})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
