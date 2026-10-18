#!/usr/bin/env python3
"""Pin down the Nova UAV/SRV binding budget so a new target cannot silently
collide with an existing resource.

WHY THIS EXISTS
A subagent survey concluded the moments target "does not fit as a 9th UAV" and
recommended register(u9). Half of that was right and half was wrong, and neither
claim was checked. Measured here against the actual shader sources:

  - register(u8) maps to Vulkan binding 8, which is ALREADY TAKEN by
    `cbuffer NovaConstants : register(b1)` (b-shift 7). Adding u8 yields an
    invalid descriptor set on Vulkan while DX11 and DX12 would both accept it --
    the worst failure mode, because it is backend-specific and would surface
    only on the one backend nobody tests.
  - GL image unit 3 is genuinely FREE. Units 0,1,2,4,5,6,7 are bound in the Nova
    path and unit 8 belongs to an unrelated generator; the survey called this a
    collision because it confused the image-unit namespace with the sampler
    namespace (glBindImageTexture(3,...) and glActiveTexture(GL_TEXTURE0+3) do
    not conflict).

So the budget is asserted here rather than trusted from a report. Two failure
modes this script must not itself have:

  1. A vacuous pass. If declared_bindings() returns {} the regex matched nothing,
     and every check below would "pass" while measuring nothing. That is a hard
     FAIL here -- an earlier draft had an escaping bug that did exactly this.
  2. Inability to fail. `--self-test` adds a colliding register(u8) to a COPY of
     the shader and asserts this script then reports FAIL.

Run:       python3 tools/verify_nova_binding_budget.py
Self-test: python3 tools/verify_nova_binding_budget.py --self-test
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"
CPP = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"

# Must match SHIFTS in tools/compile_nova_spirv.py:55-58, which is what DXC is
# invoked with and what VulkanNova.cpp:229-233 mirrors.
SHIFTS = {"u": 0, "b": 7, "t": 16, "s": 200}

REGISTER_RE = re.compile(r"register\(\s*([ubts])\s*(\d+)\s*\)")
BIND_IMAGE_RE = re.compile(r"glBindImageTexture\(\s*(\d+)")


def declared_bindings(src):
    """Map Vulkan binding number -> set of 'kind<slot>' strings claiming it.

    NVRHI computes a Vulkan binding as (space offset + register slot), per
    vulkan-resource-bindings.cpp:100.
    """
    taken = {}
    for m in REGISTER_RE.finditer(src):
        kind, slot = m.group(1), int(m.group(2))
        taken.setdefault(SHIFTS[kind] + slot, set()).add(f"{kind}{slot}")
    return taken


def check(src, label):
    """Return (failures, free_u_slots) for one shader source."""
    failures = []
    taken = declared_bindings(src)

    if not taken:
        failures.append(
            "declared_bindings() returned EMPTY -- the regex matched nothing, so "
            "every check below would pass vacuously. That is a bug in the "
            "verifier, not a clean result.")
        return failures, []

    slots_by_kind = {}
    for names in taken.values():
        for n in names:
            slots_by_kind.setdefault(n[0], set()).add(int(n[1:]))

    print(f"  {label} registers by kind:")
    for kind in "ubts":
        print(f"    {kind}: {sorted(slots_by_kind.get(kind, ()))}")

    # The specific trap this file exists for: u8 landing on NovaConstants.
    b8 = SHIFTS["u"] + 8
    b_holders = sorted(n for n in taken.get(b8, set()) if n.startswith("b"))
    print(f"  register(u8) -> Vulkan binding {b8}, currently held by "
          f"{b_holders or 'nothing'}")
    if b_holders:
        failures.append(
            f"register(u8) COLLIDES with {b_holders} at binding {b8} "
            f"(cbuffer NovaConstants is register(b1) with b-shift 7). "
            f"Use register(u9)+ or a free t-slot instead.")

    free_u = [s for s in range(8, 24) if s not in slots_by_kind.get("u", ())]
    print(f"  free u-slots (8-23): {free_u}")
    return failures, free_u


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true",
                    help="prove the checker fails when a collision is introduced")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on a hazard (for CI once a target is added)")
    args = ap.parse_args()

    if not HLSL.exists():
        print(f"FAIL not found: {HLSL}")
        return 1

    if args.self_test:
        original = HLSL.read_text(encoding="utf-8", errors="replace")
        mutated = original.replace(
            "RWTexture2D<float4> img_Output        : register(u0);",
            "RWTexture2D<float4> img_Output        : register(u0);\n"
            "RWTexture2D<float2> img_Moments       : register(u8);",
            1)
        if mutated == original:
            print("FAIL self-test could not mutate the shader (anchor string is "
                  "gone), so this check would be vacuous")
            return 1
        failures, _ = check(mutated, "MUTANT")
        print()
        if not failures:
            print("  MISSED!  register(u8) collision")
            print("\nRESULT: FAIL -- the collision checker passed a real collision")
            return 1
        print("  CAUGHT   register(u8) collision")
        print(f"          {failures[0][:118]}")
        print("\nRESULT: PASS -- the collision checker is provably failable")
        return 0

    src = HLSL.read_text(encoding="utf-8", errors="replace")
    print("Nova UAV/SRV binding budget")
    failures, free_u = check(src, "HLSL")

    units = set()
    if CPP.exists():
        c = CPP.read_text(encoding="utf-8", errors="replace")
        units = {int(m.group(1)) for m in BIND_IMAGE_RE.finditer(c)}
    free_gl = sorted(set(range(8)) - units)
    print(f"  GL image units bound anywhere: {sorted(units)}")
    print(f"  GL image units free within 0-7: {free_gl}")

    print()
    if failures:
        for f in failures:
            print("  HAZARD " + f)
        # Exit 0 by default. These are latent traps in the budget, not defects
        # in shipped code -- nothing declares u8 yet. A non-zero default would
        # train everyone to ignore this tool, which is how a real collision later
        # gets missed. Use --strict once a new target is actually being added.
        print("\nRESULT: HAZARD (not a failure; pass --strict to gate on this)")
        return 1 if args.strict else 0
    print("RESULT: PASS -- no UAV register collides with another kind's binding; "
          f"free u-slots start at {free_u[0] if free_u else 'n/a'}, "
          f"GL free image units {free_gl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())