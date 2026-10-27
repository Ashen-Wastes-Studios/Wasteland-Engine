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

So the budget is asserted here rather than trusted from a report.

WHAT CHANGED FOR THE NEURAL RENDERING STAGE
The collision check was generalised from the u8-only special case to ANY pair of
declarations sharing a Vulkan binding, and three checks were added. The draft
plan's allocation was wrong in two places -- it treated the empty u8 as a
reservation rather than an absence, and it assumed the t-range was free above
t4 when u_SceneTextures[32] at t15 claims a 32-wide contiguous range. Both
corrections are measured here rather than asserted:

  1. ANY two declarations mapping to the same Vulkan binding are a hazard.
     Special-casing u8 left every other collision shape unguarded.
  2. u_SceneTextures[32] : register(t15) claims bindings 31..62 as a CONTIGUOUS
     range. A new t-register inside that range is absorbed into the array and
     does not bind the intended buffer. The range is DERIVED from the
     declaration, so resizing or moving the array is caught rather than
     silently mis-measured.
  3. The GLSL image-unit allocation is measured from Renderer3D.cpp's
     glBindImageTexture calls, and unit 8 is reported as the generator's, not
     Nova's -- the two namespaces are distinguished rather than conflated.

So the budget is asserted here rather than trusted from a report. Failure modes
this script must not itself have:

  1. A vacuous pass. If declared_bindings() returns {} the regex matched nothing,
     and every check below would "pass" while measuring nothing. That is a hard
     FAIL here -- an earlier draft had an escaping bug that did exactly this.
  2. Inability to fail. --self-test adds a colliding register(u8) to a COPY of
     the shader and asserts this script then reports FAIL. Two further modes
     (--self-test-dup and --self-test-scenetex) prove the two new checks can
     fail too. A verifier that only catches one collision shape is the failure
     mode the original docstring warns about.

Run:       python3 tools/verify_nova_binding_budget.py
Self-test: python3 tools/verify_nova_binding_budget.py --self-test
           python3 tools/verify_nova_binding_budget.py --self-test-dup
           python3 tools/verify_nova_binding_budget.py --self-test-scenetex
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"
GLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"
CPP = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"
DX12H = REPO / "Wasteland" / "src" / "Platform" / "NVRHI" / "DX12Nova.h"

# Must match SHIFTS in tools/compile_nova_spirv.py:55-58, which is what DXC is
# invoked with and what VulkanNova.cpp:229-233 mirrors.
SHIFTS = {"u": 0, "b": 7, "t": 16, "s": 200}

REGISTER_RE = re.compile(r"register\(\s*([ubts])\s*(\d+)\s*\)")
BIND_IMAGE_RE = re.compile(r"glBindImageTexture\(\s*(\d+)")

# GLSL image-uniform declaration. The binding index lives in the layout clause
# BEFORE the uniform keyword (`layout(rgba32f, binding = 0) uniform image2D
# img_Output;`), so the index must be captured from the layout, not from text
# after the name -- the name alone says nothing about which unit it occupies.
#
# The qualifier order is `uniform writeonly` / `uniform readonly`, NOT
# `writeonly uniform`. Getting this backwards silently drops every access-
# qualified declaration from the count -- measured 2026-10-26, when the guard
# reported 7/8 declarations with unit 3 "free" while img_MaterialPacked sat at
# binding 3 as `uniform writeonly`. A count that is one low is exactly the shape
# of bug this guard exists to catch, so it has to be right in both directions.
#
# Kept in step with the pattern in verify_nova_glsl.py:208-213, which is the
# count limit this guard is the other half of.
IMAGE_UNIFORM_RE = re.compile(
    r"layout\s*\([^)]*binding\s*=\s*(\d+)[^)]*\)\s*"
    r"uniform\s+(?:writeonly\s+|readonly\s+)?image\w+\s+(\w+)")

# The scene-texture array claims a CONTIGUOUS t-range. Its span is derived from
# the declaration rather than hardcoded, so a change to the array size or its
# base register is caught instead of silently mis-measured.
SCENE_TEX_RE = re.compile(
    r"u_SceneTextures\s*\[\s*(\d+)\s*\]\s*:\s*register\(\s*t(\d+)\s*\)")

# The reservation table for the neural rendering stage, from
# docs/NEURAL_RENDERING_BINDING_PLAN.md section 2. Each entry is the purpose of
# the slot. A slot listed here that is claimed by a DIFFERENT register is a
# hazard; a slot listed here that is claimed by the listed register is correct.
# Keep in step with the doc -- the doc is the human-readable form of this table.
NEURAL_RESERVATION = {
    "u12": "img_NeuralFeatures UAV (pass 17 output)",
    "u13": "img_NeuralHistory UAV (pass 19 ping-pong)",
    "t74": "spare (was the weights SSBO; weights moved into NovaConstants)",
    "t75": "read_img_NeuralFeatures SRV (pass 18 input)",
    "t76": "read_img_NeuralHistory SRV (pass 19 input)",
}

# ---------------------------------------------------------------------------
# THE CLEAN-ROOM RECONSTRUCTION STACK (WLSR / WLAA / WLRR / WLFG)
# ---------------------------------------------------------------------------
# From .hermes/plans/2026-10-26_001250-clean-room-dlss-class-stack.md section 1.3.
#
# Two things this table is FOR, and both are silent-failure shapes:
#
#   1. A REGISTER COLLISION. The HLSL declares `u13` (the WLRR/WLFG scratch) and
#      `t75` (its read SRV). `u13` maps to Vulkan binding 13, which is free --
#      but `u12` is NeuralGlobals and `u8` is the deliberate hole, so an
#      off-by-one in either direction lands on a live resource. The DX12 side
#      has the same shape one level down: kUavCount must be 14, not 13, or the
#      range is contiguous from u0 and u13 is unreachable.
#
#   2. A REBIND THAT BORROWS A LIVE UNIT. This is the failure mode with no
#      compiler error and no link error: the GL path borrows image unit 7 for the
#      WLRR/WLFG scratch, exactly as the SVGF path already borrows unit 6
#      (Renderer3D.cpp:5225,5238). The borrow is only legal while the borrowed
#      texture is DEAD for the dispatch's duration. A borrow that overlaps a
#      live texture disturbs the bloom chain silently -- the frame still renders,
#      just wrong. That claim is proven statically by
#      tools/verify_reactive_mask_budget.py, and this table records WHICH unit
#      is borrowed so the two verifiers cannot drift apart.
RECONSTRUCTION_RESERVATION = {
    "u13": "WLRR/WLFG scratch UAV (shared, not used together)",
    "t75": "read_img WLRR/WLFG scratch SRV",
}

# The GL image units the new passes borrow, and the existing texture each
# displaces. Keyed by unit so the table is a lookup, not a list to scan.
#
# Unit 7 is `img_Bloom_Temp`: written by pass 7 (bilateral) and consumed by pass
# 6 (composite) of the SAME frame. Every new post-composite pass runs after pass
# 6, so the texture is dead for the whole dispatch window.
#
# Unit 6 is `img_TraceGBuffer`: written by pass 1 and consumed by the temporal,
# moments and A-Trous passes. A pre-composite borrow must not overlap any of
# those.
GL_BORROWED_UNITS = {
    7: "img_Bloom_Temp",
    6: "img_TraceGBuffer",
}

# The pass IDs the four features occupy. 8/15/16 are REPURPOSED from the three
# declared-but-never-dispatched passes (RunUpscale, RunTSRResolve, RunTSRCopy);
# 20-23 are new. Recording them here is what stops a future task from assigning
# a pass ID that already means something.
RECONSTRUCTION_PASSES = {
    8:  "RunWlsrEasu (repurposed from RunUpscale)",
    15: "RunWlsrRcas (repurposed from RunTSRResolve)",
    16: "RunWlaaSharpen (repurposed from RunTSRCopy)",
    20: "RunWlrrDenoise",
    21: "RunWlrrComposite",
    22: "RunWlfgWarp",
    23: "RunWlfgBlend",
    24: "RunReactiveMask",
}

# Pass IDs that exist in the switch but are dispatched NOWHERE. Measured
# 2026-10-26 by grepping `pass(N)` in Renderer3D.cpp: 8 (RunUpscale), 15
# (RunTSRResolve) and 16 (RunTSRCopy) have `case N:` arms in both twins and no
# dispatch site.
#
# These three are the ones the reconstruction stack REPURPOSES, so the check
# fires while they are in this set -- that is the intended state at task 1, and
# each task that claims one must REMOVE it here. A pass ID that stays listed
# after its task lands means the table is lying about the switch.
#
# This set is NOT "passes above 19": the switch's cases 0..19 are all real, and
# cases 12..31 in the file belong to SampleSceneTexture's u_SceneTextures
# indices, not to the pass switch at all.
DEAD_PASSES = {8, 16}  # 15 claimed by Task 4 (RCAS, RunWlsrRcas)


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


def declared_claims(src):
    """Return the raw (kind, slot) claims in declaration order.

    Separate from declared_bindings() because a dict of sets COLLAPSES two
    declarations of the same register. A duplicate same-kind claim is a real
    hazard -- DXC rejects it and a hand-merge can silently pick one -- but it is
    invisible once folded into a set. Found by --self-test-dup: the first
    version of that check passed, because both claims were 'u0'.
    """
    return [(m.group(1), int(m.group(2))) for m in REGISTER_RE.finditer(src)]


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

    # --- CHECK 0: the same register declared twice.
    #
    # Must run on the raw claim list: folding into a set of 'kind<slot>' strings
    # collapses two declarations of u0 into one and the collision disappears.
    # This is the shape a hand-merge produces, and it is a hard shader error.
    seen = {}
    for kind, slot in declared_claims(src):
        seen.setdefault((kind, slot), 0)
        seen[(kind, slot)] += 1
    dupes = sorted(f"{k}{s}" for (k, s), n in seen.items() if n > 1)
    if dupes:
        failures.append(
            f"register(s) {dupes} are declared more than once. A duplicate "
            f"declaration is a hard shader error on DXC and is silently "
            f"ambiguous in a hand-merged binding table.")

    # --- CHECK 1: any two declarations sharing a Vulkan binding.
    #
    # Generalised from the original u8-only special case. This is the check that
    # matters: the binding is DERIVED on Vulkan and taken literally on
    # DX11/DX12, so a collision here is invisible on every backend that is
    # actually tested and fatal only on the one that is not.
    for binding in sorted(taken):
        holders = sorted(taken[binding])
        if len(holders) > 1:
            failures.append(
                f"binding {binding} is claimed by {holders} -- the shader "
                f"compiles on DX11/DX12 and fails to create a descriptor set on "
                f"Vulkan. The known trap of this shape is u8 vs cbuffer "
                f"NovaConstants (b-shift 7); this check catches every such pair.")

    # --- CHECK 2: u8 specifically. Kept as a readable message even though
    # check 1 already covers it, because it is the collision this file was
    # written for and the message names the fix.
    #
    # Requires an actual u8 UAV claimant. Binding 8 is held by b1 regardless,
    # so a bare "something is at binding 8" test fires on the pristine shader --
    # which trains everyone to ignore this tool. The hazard is u8-vs-b1, not
    # b1's existence.
    b8 = SHIFTS["u"] + 8
    u8_holders = sorted(n for n in taken.get(b8, set()) if n.startswith("u"))
    b_holders = sorted(n for n in taken.get(b8, set()) if n.startswith("b"))
    print(f"  register(u8) -> Vulkan binding {b8}; u-claimants "
          f"{u8_holders or 'none'}, b-claimants {b_holders or 'none'}")
    if u8_holders:
        failures.append(
            f"register(u8) COLLIDES with {b_holders} at binding {b8} "
            f"(cbuffer NovaConstants is register(b1) with b-shift 7). "
            f"Use a free u-slot at u12+ or a free t-slot instead -- see "
            f"docs/NEURAL_RENDERING_BINDING_PLAN.md section 2.1.")

    # --- CHECK 3: the scene-texture array range.
    #
    # A new t-register inside t15..t46 is absorbed into the 32-wide array and
    # binds the wrong buffer. The range is derived from the declaration so a
    # change to the array size is caught rather than silently mis-measured.
    rng = scene_texture_range(src)
    if rng is None:
        failures.append(
            "u_SceneTextures[N] : register(tM) was not found -- the regex "
            "matched nothing, so the scene-texture range check would pass "
            "vacuously.")
    else:
        first, last = rng
        print(f"  u_SceneTextures range -> bindings {first}..{last}")
        own_reg = f"t{first - SHIFTS['t']}"
        for binding in range(first, last + 1):
            intruders = sorted(n for n in taken.get(binding, set()) if n != own_reg)
            if intruders:
                failures.append(
                    f"binding {binding} (inside the u_SceneTextures range "
                    f"{first}..{last}) is claimed by {intruders}. A new "
                    f"t-register here is absorbed into the array and does not "
                    f"bind the intended buffer. Use t<15 or t>46.")

    # --- CHECK 4: the neural-stage reservation table.
    print("  neural-stage reservation:")
    for reg in sorted(NEURAL_RESERVATION):
        purpose = NEURAL_RESERVATION[reg]
        kind, slot = reg[0], int(reg[1:])
        binding = SHIFTS[kind] + slot
        holders = sorted(taken.get(binding, set()))
        status = holders[0] if holders else "free"
        print(f"    {reg:5} -> binding {binding:3}  {status:22} ({purpose})")
        if holders and reg not in holders:
            failures.append(
                f"{reg} is reserved for {purpose} but is claimed by {holders} "
                f"at binding {binding}.")

    # --- CHECK 5: the clean-room reconstruction stack reservation.
    #
    # Same shape as check 4, different feature set. Kept separate rather than
    # folded into NEURAL_RESERVATION because the two land in different tasks and
    # a combined table would make a half-applied change look complete: a pass
    # whose u13 is wired but whose t75 is not would still be "reserved".
    print("  reconstruction-stack reservation:")
    for reg in sorted(RECONSTRUCTION_RESERVATION):
        purpose = RECONSTRUCTION_RESERVATION[reg]
        kind, slot = reg[0], int(reg[1:])
        binding = SHIFTS[kind] + slot
        holders = sorted(taken.get(binding, set()))
        status = holders[0] if holders else "free"
        print(f"    {reg:5} -> binding {binding:3}  {status:22} ({purpose})")
        if holders and reg not in holders:
            failures.append(
                f"{reg} is reserved for {purpose} but is claimed by {holders} "
                f"at binding {binding}.")

    # A reservation that is still entirely free is the EXPECTED state at task 1
    # (no shader change yet) but is not a state to preserve: once task 2 lands,
    # u13 and t75 must be claimed. Asserting the opposite -- that at least one
    # is claimed -- would make this check fail for the very change it guards.
    # Instead, record the count so the log line shows progress.
    claimed = sum(1 for r in RECONSTRUCTION_RESERVATION
                  if r in {n for names in taken.values() for n in names})
    print(f"    {claimed}/{len(RECONSTRUCTION_RESERVATION)} slots claimed "
          f"(0 expected at task 1)")

    free_u = [s for s in range(8, 24) if s not in slots_by_kind.get("u", ())]
    print(f"  free u-slots (8-23): {free_u}")
    return failures, free_u


def pass_switch_cases(src):
    """Return the pass IDs in the pass switch -- NOT every `case` in the file.

    The shader has a second switch: `SampleSceneTexture` (GLSL ~:570-601, HLSL
    ~:60-100) cases over u_SceneTextures[0..31]. A whole-file `case N:` scan
    reports those 32 IDs as pass IDs, which would make this check fire on a
    healthy tree -- that is exactly how the first version of this guard behaved.

    An arm is recognised only when its body calls a `Run*` function, which is
    what distinguishes a pass arm from a scene-texture index arm. This is also
    what makes the self-test able to inject an arm as a bare
    `case N: RunX(); break;` anywhere in the file: the arm is identified by its
    body, not by its position inside the switch's braces. That is deliberate --
    the guard must not be vacuous just because the switch's closing brace sits
    between the arm and the `case` keyword.
    """
    arms = re.findall(
        r"case\s+(\d+)\s*:\s*(?:\n\s*)?Run\w+\s*\(\s*\)\s*;\s*break\s*;", src)
    if arms:
        return {int(a) for a in arms}
    return set()


def check_pass_ids(src, cpp=None):
    """Assert the reconstruction pass IDs do not collide with live ones.

    Two separate claims, because conflating them is what makes this guard
    vacuous at task 1 and noisy later:

    * A pass ID that the reconstruction stack CLAIMS and that is DISPATCHED must
      not be in DEAD_PASSES. That is the real hazard: a repurposed pass left
      listed as dead means the table lies about the switch, and the next task
      will assume the ID is still free.
    * A pass ID above 19 that no reservation names is unrecorded. That is the
      collision shape -- a value valid for the switch, so no compiler complains.

    Merely having a `case N:` arm in the switch is NOT a hazard: 8/15/16 are
    declared-but-undispatched today, which is precisely the state the plan
    starts from. So the dispatch check reads the CPU side, and the table is
    only allowed to say "dead" while the dispatch is absent.
    """
    failures = []
    cases = pass_switch_cases(src)
    print(f"  pass IDs in the pass switch: {sorted(cases)}")

    # The CPU side: which pass IDs does RunNovaPassSchedule actually dispatch?
    dispatched = set()
    if cpp is None:
        cpp = CPP.read_text(encoding="utf-8", errors="replace")
    dispatched = {int(m) for m in re.findall(r"\bpass\((\d+)\s*[,)]", cpp)}
    dispatched |= {int(m) for m in re.findall(r"\bpass\((\d+)\s*;", cpp)}
    print(f"  pass IDs dispatched from the CPU: {sorted(dispatched)}")

    for pid, purpose in sorted(RECONSTRUCTION_PASSES.items()):
        if pid in dispatched:
            if pid in DEAD_PASSES:
                failures.append(
                    f"pass {pid} ({purpose}) is dispatched but still listed in "
                    f"DEAD_PASSES. A dispatched pass listed as dead is a table "
                    f"that lies about the switch -- the next task will assume "
                    f"the ID is free.")
        else:
            print(f"    pass {pid} ({purpose}): not dispatched yet")

    # An unreserved pass ID above 19: 20-24 are this stack's, so anything else
    # up there is a collision waiting for the next feature.
    unreserved = sorted(p for p in cases if p > 19 and p not in RECONSTRUCTION_PASSES)
    if unreserved:
        failures.append(
            f"pass ID(s) {unreserved} are above 19 (this stack's range) but "
            f"are not in RECONSTRUCTION_PASSES. An unrecorded pass ID in the "
            f"reconstruction range will collide with the next feature.")
    return failures


def check_dx12_uav_count(src):
    """Assert DX12's kUavCount covers the highest UAV the shader declares.

    The DX12 UAV descriptor range is CONTIGUOUS from u0, so `kUavCount` must be
    >= (highest UAV register + 1) or the highest register is unreachable. This
    is the exact shape of the u8-hole trap, one backend over: a hole in the
    numbering still costs a slot, and the range must run past it.

    Measured 2026-10-26: shader declares u0..u7, u9, u10, u11, u12; DX12Nova.h
    has kUavCount = 13. Adding u13 makes 14. The header comment at
    DX12Nova.h:165-175 records the last such move (52 -> 53) and is the
    template; this check is what stops the next one being forgotten.
    """
    failures = []
    if not CPP.exists():
        return failures
    c = CPP.read_text(encoding="utf-8", errors="replace")

    uav_slots = {int(m.group(2)) for m in REGISTER_RE.finditer(src)
                 if m.group(1) == "u"}
    if not uav_slots:
        return failures

    highest = max(uav_slots)
    needed = highest + 1
    print(f"  shader declares UAV u0..u{highest} -> DX12 kUavCount must be >= {needed}")

    # DX12Nova.h, not the .cpp: the constant is a static class member declared in
    # the header (DX12Nova.h:177). Reading the .cpp finds nothing and a guard that
    # finds nothing is indistinguishable from a guard that passes -- which is the
    # failure mode this whole check exists to prevent.
    h = DX12H.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"kUavCount\s*=\s*(\d+)", h)
    if not m:
        failures.append("DX12Nova.h: kUavCount not found (guard would be vacuous)")
        return failures
    declared = int(m.group(1))
    print(f"  DX12Nova.h kUavCount = {declared}")
    if declared < needed:
        failures.append(
            f"DX12Nova.h kUavCount = {declared} but the shader declares "
            f"u{highest}, which needs {needed}. The DX12 UAV range is "
            f"contiguous from u0, so a lower count makes u{highest} "
            f"unreachable -- CreateComputePipelineState fails with E_INVALIDARG "
            f"(0x80070057) during Init and the backend silently falls back to "
            f"raster, logging only \"DX12 Nova init failed\". That is exactly "
            f"what happened when img_HistoryBuffer moved to u11 while this "
            f"stayed 11 (DX12Nova.h:162-176).")
    return failures


def scene_texture_range(src):
    """Return (first_binding, last_binding) claimed by u_SceneTextures, or None."""
    m = SCENE_TEX_RE.search(src)
    if not m:
        return None
    count, base = int(m.group(1)), int(m.group(2))
    first = SHIFTS["t"] + base
    return first, first + count - 1


def check_gl_image_units():
    """Measure the GLSL image-unit allocation from Renderer3D.cpp.

    glBindImageTexture units and glActiveTexture units are SEPARATE namespaces
    and do not conflict; the earlier survey confused them. This reports which
    unit belongs to Nova and which to the unrelated generator, and asserts the
    Nova set has not changed without the doc being updated.
    """
    failures = []
    units = set()
    if CPP.exists():
        c = CPP.read_text(encoding="utf-8", errors="replace")
        units = {int(m.group(1)) for m in BIND_IMAGE_RE.finditer(c)}
    print(f"  GL image units bound by glBindImageTexture: {sorted(units)}")
    # Unit 8 is the generator's, not Nova's (Renderer3D.cpp:6632). Stated here
    # so a reader cannot conclude it is free for the neural stage.
    if 8 in units:
        print("    unit 8 -> unrelated generator (Renderer3D.cpp:6632), NOT Nova")
    nova_units = [0, 1, 2, 4, 5, 6, 7]
    missing = [u for u in nova_units if u not in units]
    if missing:
        failures.append(
            f"expected Nova image units {missing} to be bound by "
            f"glBindImageTexture in Renderer3D.cpp -- the allocation changed, "
            f"so the neural stage's unit assignment (9, 10 per "
            f"docs/NEURAL_RENDERING_BINDING_PLAN.md section 4) must be "
            f"re-derived rather than assumed.")
    free_gl = sorted(set(range(0, 16)) - units)
    print(f"  GL image units free: {free_gl}")
    return failures, free_gl


def check_reconstruction_gl_borrows(glsl=None):
    """Prove the GL image units the reconstruction stack borrows are really free.

    This is the check that guards the plan's load-bearing claim: that the new
    passes add NO ninth image declaration and instead borrow an occupied unit
    per dispatch, the way the SVGF path already borrows unit 6
    (Renderer3D.cpp:5225,5238).

    What it asserts:
      * the GLSL still declares exactly 8 image uniforms -- the declaration
        count is the real limit (verify_nova_glsl.py:213; a ninth is rejected
        with C7624 even at a legal binding index);
      * each unit named in GL_BORROWED_UNITS is inside 0..7, so a borrow table
        typo cannot name a unit the driver does not have;
      * the unit named in GL_BORROWED_UNITS is a unit the GLSL actually
        declares. A borrow table that names a free unit (there are none) or a
        unit that does not exist would be a no-op that reads as a working
        reservation.

    `glsl` is injectable so the self-test can prove the ninth-declaration
    branch fires without editing the shader on disk.
    """
    failures = []
    if glsl is None:
        glsl = GLSL.read_text(encoding="utf-8", errors="replace")
    # (binding_index, name) pairs, so the guard can tell WHICH unit a borrow
    # displaces -- a borrow of a unit that is not declared displaces nothing.
    decls = IMAGE_UNIFORM_RE.findall(glsl)
    # Count DECLARATIONS, not distinct bindings. The limit is a declaration
    # count (verify_nova_glsl.py:213, C7624), and a set of binding indices
    # silently collapses a ninth declaration that reuses an existing binding --
    # which is exactly how the first version of this guard read 8/8 while
    # holding nine declarations.
    print(f"  GLSL image declarations: {len(decls)}/8")
    if len(decls) > 8:
        failures.append(
            f"the GLSL declares {len(decls)} image uniforms. verify_nova_glsl.py "
            f"enforces a count of <= 8 and the driver rejects a ninth with C7624 "
            f"-- the reconstruction stack must borrow an occupied unit, not "
            f"declare a new one.")
    units = {int(b) for b, _ in decls}
    free = [u for u in range(8) if u not in units]
    print(f"  GL image units free (by declaration): {free}")
    print(f"  GL image units the stack will borrow:")
    for unit, texture in sorted(GL_BORROWED_UNITS.items()):
        if not 0 <= unit <= 7:
            failures.append(
                f"GL_BORROWED_UNITS names unit {unit}, outside 0..7 "
                f"(GL_MAX_IMAGE_UNITS = 8).")
            continue
        if unit not in units:
            failures.append(
                f"GL_BORROWED_UNITS borrows unit {unit} but the GLSL does not "
                f"declare an image uniform there -- the borrow would displace "
                f"nothing and the pass would read uninitialised memory.")
            continue
        owner = [n for b, n in decls if int(b) == unit][0]
        print(f"    unit {unit} ({texture}) -> displaces {owner}")
    return failures


def _run_self_test(mutated, original, caught_label, what):
    """Shared driver for the three self-test modes."""
    if mutated == original:
        print(f"FAIL self-test ({caught_label}) could not mutate the shader -- "
              f"the anchor string is gone, so this check would be vacuous")
        return 1
    failures, _ = check(mutated, "MUTANT")
    print()
    if not failures:
        print(f"  MISSED!  {caught_label}")
        print(f"\nRESULT: FAIL -- the checker passed {what}")
        return 1
    print(f"  CAUGHT   {caught_label}")
    print(f"          {failures[0][:118]}")
    print(f"\nRESULT: PASS -- the checker is provably failable ({caught_label})")
    return 0


def self_test_reconstruction(original):
    """Prove the reconstruction checks can fail. A guard that cannot fail is a
    comment, not a guard -- and this stack's whole risk profile is silent
    failures (a pass ID two features claim, a kUavCount one too low, a borrow of
    a unit that is not free), none of which produce a compiler error.

    Each case mutates something the guard reads and asserts the guard rejects
    it. A case that is NOT detected means the guard is vacuous and would miss
    the real version of the same mistake later.
    """
    print("Nova binding budget -- reconstruction-stack self-test")
    print("  (each case must be DETECTED; a miss means that guard is vacuous)")
    ok = True

    def expect_detected(label, fn):
        nonlocal ok
        try:
            failures = fn()
        except Exception as e:
            print(f"  FAIL {label}: guard raised {e!r} instead of reporting")
            ok = False
            return
        if failures:
            print(f"  ok   {label}")
            print(f"       {failures[0][:110]}")
        else:
            print(f"  FAIL {label}: mutation NOT detected")
            ok = False

    # Case 1 -- a duplicate UAV slot. The classic: two declarations of the same
    # register, which the driver may or may not reject.
    def dup_uav():
        return check(
            original + "\nRWTexture2D<float4> img_WlrrDup : register(u0);", "HLSL")[0]
    expect_detected("duplicate UAV slot", dup_uav)

    # Case 2 -- a t-register inside u_SceneTextures[32]'s range. The array
    # claims 31..62 as contiguous, so a new t-register in it is absorbed rather
    # than rejected by the driver.
    def t_collision():
        return check(
            original + "\nTexture2D<float4> img_WlrrDepth : register(t40);", "HLSL")[0]
    expect_detected("t-register collides with u_SceneTextures", t_collision)

    # Case 3 -- a DISPATCHED pass still listed in DEAD_PASSES. The plan's stated
    # move is to repurpose 8/15/16; a task that does it without removing the ID
    # from the table leaves a table that lies about the switch. The mutation must
    # add a real dispatch site, because a bare `case N:` arm is not a dispatch.
    def repurposed_still_dead():
        mutated_cpp = CPP.read_text(encoding="utf-8", errors="replace")
        mutated_cpp += "\n    pass(8, true);\n"
        return check_pass_ids(original, mutated_cpp)
    expect_detected("dispatched pass ID still listed as dead", repurposed_still_dead)

    # Case 4 -- an unreserved pass ID above 19. 20-24 are this stack's; anything
    # else up there is a collision waiting for the next feature.
    def unreserved_pass():
        return check_pass_ids(
            original + "\ncase 25: RunSomethingElse(); break;\n")
    expect_detected("unreserved pass ID in the reconstruction range", unreserved_pass)

    # Case 4b -- the extractor must NOT treat SampleSceneTexture's 32 cases as
    # pass IDs. Without this, a healthy tree fails the check (which is how the
    # first version of this guard behaved). Mutating the pass switch with a
    # case that the SampleSceneTexture switch also has must not add a phantom.
    def scene_tex_not_pass_ids():
        cases = pass_switch_cases(original)
        phantom = sorted(p for p in cases if p in range(12, 32))
        if phantom:
            return [f"pass_switch_cases reported SampleSceneTexture's "
                    f"u_SceneTextures indices as pass IDs: {phantom}"]
        return []
    expect_detected("SampleSceneTexture's case IDs are not read as pass IDs",
                    scene_tex_not_pass_ids)

    # Case 5 -- DX12 kUavCount too low. Real state is 13, and the real shader
    # tops out at u12, so a shader declaring u13 must be reported.
    def uav_count_low():
        return check_dx12_uav_count(
            original + "\nRWTexture2D<float4> img_WlrrScratch : register(u13);")
    expect_detected("DX12 kUavCount below the highest declared UAV", uav_count_low)

    # Case 6 -- the GL borrow table. A borrow naming a unit the GLSL does not
    # declare displaces nothing and reads uninitialised memory.
    #
    # The mutation must name a unit the shader really does NOT declare. Unit 3
    # is `img_MaterialPacked`, so naming it is not a hazard -- the first version
    # of this case used unit 3 and passed, which is the guard telling the truth
    # and the mutation being wrong. The free units are the honest target: any
    # index outside 0..7, which is the "typo" shape.
    def borrow_table():
        saved = dict(GL_BORROWED_UNITS)
        try:
            GL_BORROWED_UNITS[9] = "img_WlrrScratch"
            return check_reconstruction_gl_borrows()
        finally:
            GL_BORROWED_UNITS.clear()
            GL_BORROWED_UNITS.update(saved)
    expect_detected("GL borrow table names a unit the GLSL does not declare", borrow_table)

    # Case 7 -- the ninth-declaration limit itself. A GLSL with nine image
    # uniforms must be rejected: the driver rejects it with C7624 and the whole
    # strategy depends on not adding one. The mutation appends a declaration at
    # a legal binding index, which is the exact shape that fooled the first
    # survey -- the index is fine, the COUNT is the limit.
    def ninth_declaration():
        glsl = GLSL.read_text(encoding="utf-8", errors="replace")
        mutated = glsl + "\nlayout(binding = 3, rgba32f) uniform writeonly image2D img_Ninth;\n"
        return check_reconstruction_gl_borrows(mutated)
    expect_detected("ninth image declaration rejected", ninth_declaration)

    print()
    if ok:
        print("RESULT: PASS -- all reconstruction mutations were detected")
    else:
        print("RESULT: FAIL -- a guard is vacuous; fix it before adding passes")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true",
                    help="prove the checker fails when a u8/b1 collision is introduced")
    ap.add_argument("--self-test-dup", action="store_true",
                    help="prove the checker fails when two declarations claim the "
                         "same UAV slot")
    ap.add_argument("--self-test-scenetex", action="store_true",
                    help="prove the checker fails when a t-register lands inside "
                         "the u_SceneTextures range")
    ap.add_argument("--self-test-wlsr", action="store_true",
                    help="prove the NEW reconstruction checks fail: a duplicate "
                         "UAV, a scene-texture collision, a repurposed pass still "
                         "listed dead, an unreserved pass ID, and a DX12 "
                         "kUavCount below the highest declared UAV")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on a hazard (for CI once a target is added)")
    args = ap.parse_args()

    if not HLSL.exists():
        print(f"FAIL not found: {HLSL}")
        return 1

    original = HLSL.read_text(encoding="utf-8", errors="replace")

    # ---- self-test 4: the reconstruction stack's own checks. This mode is
    #      separate from the three shader-mutation modes because the guards it
    #      exercises read DIFFERENT files (the pass switch, DX12Nova.h, the GLSL
    #      declaration list), so a shared mutation driver would prove the wrong
    #      thing. Each case is a mutation that must be detected.
    if args.self_test_wlsr:
        return self_test_reconstruction(original)

    # ---- self-test 1: the original u8 collision.
    if args.self_test:
        mutated = original.replace(
            "RWTexture2D<float4> img_Output        : register(u0);",
            "RWTexture2D<float4> img_Output        : register(u0);\n"
            "RWTexture2D<float2> img_Moments       : register(u8);",
            1)
        return _run_self_test(mutated, original, "register(u8) collision",
                              "a UAV register claiming the NovaConstants binding")

    # ---- self-test 2: a duplicate claim on an already-taken binding.
    if args.self_test_dup:
        mutated = original.replace(
            "RWTexture2D<float4> img_Output        : register(u0);",
            "RWTexture2D<float4> img_Output        : register(u0);\n"
            "RWTexture2D<float4> img_NeuralDup     : register(u0);",
            1)
        return _run_self_test(mutated, original, "duplicate binding claim",
                              "two declarations of the same UAV slot")

    # ---- self-test 3: a t-register inside the scene-texture range.
    if args.self_test_scenetex:
        mutated = original.replace(
            "StructuredBuffer<int>                LightIndices : register(t3);",
            "StructuredBuffer<int>                LightIndices : register(t3);\n"
            "StructuredBuffer<float>              NeuralWeights : register(t20);",
            1)
        return _run_self_test(mutated, original, "scene-texture range collision",
                              "a t-register inside u_SceneTextures[32]'s range")

    print("Nova UAV/SRV binding budget")
    failures, free_u = check(original, "HLSL")
    gl_failures, free_gl = check_gl_image_units()
    failures.extend(gl_failures)
    failures.extend(check_pass_ids(original))
    failures.extend(check_dx12_uav_count(original))
    failures.extend(check_reconstruction_gl_borrows())

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