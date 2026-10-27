#!/usr/bin/env python3
"""Prove the reactive mask's storage channel is free at the moment it is read.

WHY THIS EXISTS
---------------
The reactive mask (task 2 of the clean-room reconstruction stack) is stored in
a spare channel of an EXISTING image rather than in a new image uniform. That
is forced by a hard limit: the GLSL may declare at most 8 image uniforms
(verify_nova_glsl.py:213; the driver rejects a ninth with C7624), all 8 are in
use, and the reconstruction stack must not add a ninth.

Storing the mask in a spare channel is safe only while the channel is genuinely
unread for the whole window between the mask pass writing it and the consuming
pass reading it. If some other pass reads that channel in between, the mask is
a silent no-op: the feature runs, produces plausible-looking output, and does
nothing. There is no compiler error, no link error, and no log line. That is
exactly the failure class this repo keeps hitting (a plumbed-but-dead
u_ReconstructionFilter for months; a ninth image uniform that failed the whole
GL program to LINK silently).

So this verifier proves the deadness claim STATICALLY, from pass order, rather
than hoping a screenshot catches it.

WHAT IT CHECKS
--------------
1. The mask's channel is not read by anything, on the image the mask writes.
   A read of `.a` on the mask image is a hazard unless it is the consumer.
2. The mask's writer runs after the image's other writers and before the image's
   other readers, i.e. the channel is not overwritten between write and read.
3. The writer pass is actually dispatched, and the consumer pass is dispatched
   after it. An undispatched writer means the mask holds stale data from the
   previous frame.
4. The consuming passes are declared in BOTH twins -- a parity requirement this
   repo treats as load-bearing, because a parity tool cannot see a scope error
   and the HLSL is what DX11/DX12/Vulkan all consume.

NEGATIVE CONTROLS
-----------------
--self-test injects each hazard shape and asserts this verifier detects it. A
verifier that cannot fail is a comment, not a guard.

MEASURED 2026-10-26
-------------------
* `img_Bloom_Temp` (binding 7) is written by pass 7 (`RunBilateralBlur`) with
  `vec4(finalFiltered, 1.0f)` at .glsl:4504 and .glsl:5137 -- rgb AND alpha.
* Nothing in the shader reads `imageLoad(...).a` at all, so the alpha is
  write-only: written, never read. The channel is free for the mask.
* The correct statement is "nothing READS it", not "nothing writes it". The
  draft plan said the latter; this verifier encodes the former, because a
  future change that writes the alpha again would still be safe, while one that
  reads it would not.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SHADER_GLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"
SHADER_HLSL = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"
RENDERER_CPP = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"

# The mask's storage: image name -> channel. Kept as a table so the second
# feature that wants a spare channel (WLRR's residual gate) can reuse the shape.
MASK_STORAGE = {
    "img_Bloom_Temp": "a",
}

# The pass that writes the mask, and the passes that read it. Filled in by task
# 2; until then the table is empty and the verifier reports that state rather
# than pretending the pass exists.
MASK_WRITER_PASS = 24
# Passes that read the reactive mask and must therefore be dispatched AFTER the
# mask writer (pass 24).
#
# 6  = RunComposite, the EASU/WLAA sharpen path. This is the live consumer from
#      task 3 onward. It was NOT in this set while the mask's only readers were
#      the undispatched passes 8/15/16, and the omission cost two rounds of
#      debugging: the dispatch-order check had no consumer to scope its window
#      against, so it could not see the stale-read bug at all.
# 8  = RunUpscale (WLSR EASU), 15 = RunTSRResolve, 16 = RunTSRCopy -- declared,
#      not yet dispatched. Kept here so they are checked as soon as they are
#      wired up.
MASK_CONSUMER_PASSES = {6, 8, 15, 16}

# How the mask is referenced in source. GLSL uses imageStore/imageLoad; HLSL
# uses IMG_STORE/IMG_LOAD macros. Both are matched so the parity check is real.
GLSL_STORE_RE = re.compile(r"imageStore\s*\(\s*(\w+)\s*,")
GLSL_LOAD_RE = re.compile(r"imageLoad\s*\(\s*(\w+)\s*,")
HLSL_STORE_RE = re.compile(r"IMG_STORE\s*\(\s*(\w+)\s*,")
HLSL_LOAD_RE = re.compile(r"IMG_LOAD\s*\(\s*(\w+)\s*,")

# A read of a specific channel, e.g. `imageLoad(img_X, p).a` or `IMG_LOAD(...).a`.
CHANNEL_READ_RE = re.compile(
    r"(?:imageLoad|IMG_LOAD)\s*\(\s*(\w+)\s*[^)]*\)\s*\.\s*([rgba])")
# A write of a specific channel. GLSL/HLSL both write whole vec4s, so a channel
# write is only expressible as a swizzle; match it anyway so a future partial
# write is caught.
CHANNEL_WRITE_RE = re.compile(
    r"(?:imageStore|IMG_STORE)\s*\(\s*(\w+)\s*,[^;]*\)\s*;")

PASS_ARM_RE = re.compile(
    r"case\s+(\d+)\s*:\s*(?:\n\s*)?Run\w+\s*\(\s*\)\s*;\s*break\s*;")
DISPATCH_RE = re.compile(r"\bpass\((\d+)\s*[,)]")


def pass_bodies(src: str) -> dict[str, str]:
    """Map function name -> body text, for the passes this verifier reasons about.

    Only whole functions whose name starts with `Run` are needed; the shader's
    other functions are irrelevant to the channel-liveness question.
    """
    bodies: dict[str, str] = {}
    for m in re.finditer(r"void\s+(Run\w+)\s*\(\s*\)\s*\{", src):
        name = m.group(1)
        start = m.end()
        depth = 1
        i = start
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        bodies[name] = src[start:i]
    return bodies


def pass_for_arm(src: str, pass_id: int) -> str | None:
    """Return the function name a pass switch arm dispatches, or None."""
    m = re.search(
        r"case\s+%d\s*:\s*(?:\n\s*)?Run(\w+)\s*\(\s*\)\s*;\s*break\s*;" % pass_id,
        src)
    return "Run" + m.group(1) if m else None


def dispatched_passes(cpp: str) -> set[int]:
    return {int(m.group(1)) for m in DISPATCH_RE.finditer(cpp)}


def dispatch_sequence(cpp: str) -> list[int]:
    """The order passes are dispatched, first occurrence only.

    Derived from the `pass(N)` call sites in RunNovaPassSchedule. The order is
    not ascending and must not be "fixed": pass 1 must precede pass 0 because
    pass 2's reprojection consumes the G-buffer pass 1 writes.

    Used to prove the mask survives until a consumer reads it -- a channel-
    liveness check alone cannot see an overwrite that happens later.
    """
    seen: list[int] = []
    for m in DISPATCH_RE.finditer(cpp):
        p = int(m.group(1))
        if p not in seen:
            seen.append(p)
    return seen


def check(src: str, twin: str, cpp: str, label: str) -> list[str]:
    failures: list[str] = []
    bodies = pass_bodies(src)
    dispatched = dispatched_passes(cpp)
    dispatch_order = dispatch_sequence(cpp)

    print(f"  [{label}] pass arms / dispatch / channel liveness")

    # ---- 1. The writer must exist and be dispatched.
    writer_fn = pass_for_arm(src, MASK_WRITER_PASS)
    if writer_fn is None:
        # Task 1 state: the pass does not exist yet. Not a failure -- but say so
        # loudly, because a silent "no checks run" is how a guard goes vacuous.
        print(f"    pass {MASK_WRITER_PASS}: not declared yet (task 2 adds it)")
        return failures
    if writer_fn not in bodies:
        failures.append(
            f"pass {MASK_WRITER_PASS} dispatches {writer_fn} but no such "
            f"function is defined -- the arm would not compile.")
        return failures
    if MASK_WRITER_PASS not in dispatched:
        failures.append(
            f"pass {MASK_WRITER_PASS} ({writer_fn}) is declared but never "
            f"dispatched from Renderer3D.cpp. The mask would hold whatever the "
            f"image contained last frame, and every consumer would read stale "
            f"data with no error.")
        print(f"    pass {MASK_WRITER_PASS} ({writer_fn}): DECLARED BUT NOT DISPATCHED")
    else:
        print(f"    pass {MASK_WRITER_PASS} ({writer_fn}): declared and dispatched")

    # ---- 2. The writer must write the mask image.
    writer_body = bodies[writer_fn]
    stores = GLSL_STORE_RE.findall(writer_body) or HLSL_STORE_RE.findall(writer_body)
    for image in MASK_STORAGE:
        if image not in stores:
            failures.append(
                f"pass {MASK_WRITER_PASS} ({writer_fn}) does not write "
                f"{image}, which is where the reactive mask is stored "
                f"(MASK_STORAGE). The mask would be unwritten.")

    # ---- 3. Nothing may READ the mask channel except the declared consumers.
    for fn, body in bodies.items():
        for image, channel in CHANNEL_READ_RE.findall(body):
            if image not in MASK_STORAGE or MASK_STORAGE[image] != channel:
                continue
            owner_pass = next(
                (p for p in range(0, 64)
                 if (f := pass_for_arm(src, p)) == fn), None)
            if owner_pass in MASK_CONSUMER_PASSES:
                print(f"    {fn} reads {image}.{channel} (consumer, pass {owner_pass})")
                continue
            failures.append(
                f"{fn} reads {image}.{channel}, which is the reactive mask's "
                f"storage channel, but is not in MASK_CONSUMER_PASSES "
                f"{sorted(MASK_CONSUMER_PASSES)}. The mask is a spare channel "
                f"of an image other passes also use; a read here either consumes "
                f"the mask early or means the channel is not spare at all.")

    # ---- 4b. DISPATCH ORDER. The channel-liveness check above proves nothing
    # reads the mask early; it does NOT prove the mask survives until a consumer
    # reads it, nor that the consumer reads the CURRENT frame's mask. Two
    # distinct bugs hid here, and both shipped:
    #
    #   (a) pass 24 was first placed after pass 2, so pass 7 and pass 4
    #       overwrote the mask before pass 6 read it -> the mask was destroyed.
    #   (b) it was then placed LAST, which fixed (a) but violated the other
    #       half: pass 6 (the consumer) now ran BEFORE the mask was written, so
    #       it read the previous frame's mask. Measured: masked-EASU was
    #       bit-identical to bilinear (0 of 3,383,732 pixels) even though the
    #       mask itself was a correct, structured 0.08..0.50.
    #
    # So the constraint is a WINDOW, not an ordering: the mask writer must be
    # after every other WRITER of the image, and before every DECLARED CONSUMER.
    #
    # tools/verify_reactive_mask_budget.py's dispatch-order check enforces
    # both halves; the first version checked channel liveness only.
    # (a) OVERWRITE: a writer of the image that runs after the mask writer and
    #     before the consumer destroys the mask. A writer that runs after the
    #     consumer does not matter -- the consumer has already read the mask.
    #     Pass 4 (bloom H) is exactly that case: it runs after pass 6, so it
    #     overwrites the image for the NEXT frame, which is correct behaviour.
    #     Flagging it made this check report a hazard on correct code.
    mask_fn = pass_for_arm(src, MASK_WRITER_PASS)
    if mask_fn is not None and MASK_WRITER_PASS in dispatched:
        mask_idx = dispatch_order.index(MASK_WRITER_PASS) if MASK_WRITER_PASS in dispatch_order else None
        for image in MASK_STORAGE:
            writers = [fn for fn, body in bodies.items()
                       if image in (GLSL_STORE_RE.findall(body)
                                    or HLSL_STORE_RE.findall(body))]
            # The consumer defines the deadline: a writer only destroys the mask
            # if it lands between the mask writer and that consumer.
            for cp in MASK_CONSUMER_PASSES:
                if cp not in dispatched:
                    continue
                cidx = dispatch_order.index(cp)
                for fn in writers:
                    if fn == mask_fn:
                        continue
                    wp = next((p for p in range(0, 64)
                               if pass_for_arm(src, p) == fn and p in dispatched), None)
                    if wp is None or mask_idx is None:
                        continue
                    widx = dispatch_order.index(wp)
                    if widx > mask_idx and widx < cidx:
                        failures.append(
                            f"pass {wp} ({fn}) writes {image}, which holds the "
                            f"reactive mask, and is dispatched between the mask "
                            f"writer (pass {MASK_WRITER_PASS}) and its consumer "
                            f"(pass {cp}). The mask is overwritten before the "
                            f"consumer reads it -- a silent no-op.")
        # Consumers must run AFTER the writer. Scoped to DECLARED consumers:
        # passes 5 and 6 also read img_Bloom_Temp (the bloom/composite chain)
        # but they are not mask consumers, and flagging them would be a false
        # positive that makes this check unusable.
        for cp in MASK_CONSUMER_PASSES:
            if cp not in dispatched:
                continue
            cfn = pass_for_arm(src, cp)
            if cfn is None or mask_idx is None:
                continue
            if dispatch_order.index(cp) < mask_idx:
                failures.append(
                    f"pass {cp} ({cfn}) is a declared reactive-mask consumer "
                    f"but is dispatched BEFORE the mask writer (pass "
                    f"{MASK_WRITER_PASS}). It reads the previous frame's mask "
                    f"-- and an uninitialised one on frame 0 -- which is "
                    f"exactly why a correct mask produced no visible change. "
                    f"Move the mask dispatch to before pass {cp} and after "
                    f"every writer of {image}.")
        print(f"    dispatch order: {dispatch_order}")
        print(f"    mask writer (pass {MASK_WRITER_PASS}) at index {mask_idx}")

    # ---- 4. Every declared consumer must be dispatched AFTER the writer.
    #
    # Only flag a consumer that is DISPATCHED somewhere. A consumer that is
    # declared in the switch but dispatched nowhere is the plan's own pending
    # state -- passes 8/15/16 are dead until tasks 3-5 claim them -- and is not a
    # hazard the mask introduces. Reporting it would make this verifier red for
    # the entire duration of the plan, which is how a guard gets ignored.
    #
    # What IS a hazard: a consumer that is dispatched, because then it genuinely
    # reads the mask and the ordering matters.
    for p in sorted(MASK_CONSUMER_PASSES):
        fn = pass_for_arm(src, p)
        if fn is None:
            failures.append(
                f"consumer pass {p} is in MASK_CONSUMER_PASSES but has no "
                f"`case {p}:` arm in the {label} shader.")
            continue
        if p in dispatched:
            print(f"    consumer pass {p} ({fn}): dispatched, reads the mask")
        else:
            print(f"    consumer pass {p} ({fn}): declared, not yet dispatched")
    return failures


def check_parity(glsl: str, hlsl: str) -> list[str]:
    """The two twins must declare the same pass arms.

    Parity is load-bearing here, not stylistic: the HLSL is what DX11, DX12 and
    Vulkan all consume (the .spv is built from the HLSL, not the GLSL), so a
    mask implemented in only one twin is a feature that works on OpenGL and
    silently does nothing elsewhere.
    """
    failures: list[str] = []
    g = {int(m) for m in PASS_ARM_RE.findall(glsl)}
    h = {int(m) for m in PASS_ARM_RE.findall(hlsl)}
    only_g = sorted(g - h)
    only_h = sorted(h - g)
    if only_g:
        failures.append(
            f"pass arm(s) {only_g} exist in the GLSL twin but not the HLSL twin. "
            f"The HLSL is what DX11/DX12/Vulkan consume, so these passes would "
            f"be OpenGL-only.")
    if only_h:
        failures.append(
            f"pass arm(s) {only_h} exist in the HLSL twin but not the GLSL twin. "
            f"The GLSL is the path actually rendered here, so these passes would "
            f"never run.")
    if not failures:
        print(f"  [parity] both twins declare {len(g)} pass arms, identical sets")
    return failures


def self_test(glsl: str, hlsl: str, cpp: str) -> int:
    """Inject each hazard shape and assert it is detected."""
    print("reactive-mask budget -- self-test")
    print("  (each case must be DETECTED; a miss means that guard is vacuous)")
    ok = True

    # The self-test has to build a COMPLETE mask pass before any of the guards
    # can bite: without a declared writer arm the real check() returns early
    # ("not declared yet"), and a guard that returns early is indistinguishable
    # from a guard that passes. So every case below injects writer + arm +
    # dispatch, then breaks exactly one thing.
    #
    # The arm is injected ONLY IF ABSENT. The real tree already has
    # `case 24: RunReactiveMask(); break;` in both twins (task 2 added it), so
    # appending a second copy would make a later replace() a no-op -- the
    # appended copy would still match PASS_ARM_RE and parity would look intact
    # when it was not. Injecting conditionally keeps base() idempotent.
    WRITER_FN = "RunReactiveMask"
    REAL_ARM = f"case {MASK_WRITER_PASS}: {WRITER_FN}(); break;"
    WRITER_BODY = (
        f"\nvoid {WRITER_FN}() {{\n"
        "    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n"
        "    float mask = 1.0f;\n"
        f"    imageStore(img_Bloom_Temp, pos, vec4(0.0f, 0.0f, 0.0f, mask));\n"
        "}\n")
    DISPATCH = f"    pass({MASK_WRITER_PASS}, true);\n"

    def base():
        """A tree with a working mask pass: writer defined, armed, dispatched."""
        g = glsl
        if WRITER_FN not in g:
            g += WRITER_BODY
        if REAL_ARM not in g:
            g += REAL_ARM + "\n"
        c = cpp if f"pass({MASK_WRITER_PASS}" in cpp else cpp + DISPATCH
        return g, c

    def expect(label, fn):
        nonlocal ok
        try:
            failures = fn()
        except Exception as e:
            print(f"  FAIL {label}: guard raised {e!r}")
            ok = False
            return
        if failures:
            print(f"  ok   {label}")
            print(f"       {failures[0][:104]}")
        else:
            print(f"  FAIL {label}: mutation NOT detected")
            ok = False

    # Case 1 -- a pass that reads the mask's channel without being a consumer.
    # The exact silent-no-op shape: the mask is written, and something else
    # reads the same channel first.
    def stray_read():
        g, c = base()
        g += ("\nvoid RunStrayReader() {\n"
              "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
              "    float m = imageLoad(img_Bloom_Temp, p).a;\n"
              "}\n"
              "case 40: RunStrayReader(); break;\n")
        return check(g, hlsl, c, "MUTANT-1")
    expect("stray read of the mask channel", stray_read)

    # Case 2 -- the mask writer is never dispatched. The mask then holds stale
    # data from the previous frame, and every consumer reads it with no error.
    def undispatched_writer():
        g, _ = base()
        c = re.sub(r"\bpass\(\d+\s*[,)]", "pass(999,", cpp)
        return check(g, hlsl, c, "MUTANT-2")
    expect("mask writer not dispatched", undispatched_writer)

    # Case 3 -- parity: a pass arm present in only one twin. The real tree has
    # pass 24 in BOTH twins, so base() cannot create a break by adding one --
    # the mutation has to REMOVE the real arm from one twin. That is the shape
    # that actually happens in practice: a pass added to the GLSL and forgotten
    # in the HLSL (or the reverse), which is silent because the HLSL is what
    # DX11/DX12/Vulkan consume.
    #
    # The replacement must match the arm's ACTUAL indentation (8 spaces, as it
    # sits inside the switch). Replacing the appended copy from base() would
    # leave the real arm in place and the mutation would be a no-op -- which is
    # how this case failed on its first attempt.
    def parity_break():
        g, _ = base()
        g = g.replace("        case 24: RunReactiveMask(); break;",
                      "        case 24: break;", 1)
        return check_parity(g, hlsl)
    expect("pass arm missing from one twin", parity_break)

    # Case 4 -- the writer does not write the mask image at all.
    def no_mask_store():
        g, c = base()
        g = g.replace("imageStore(img_Bloom_Temp, pos",
                      "imageStore(img_Bloom, pos")
        return check(g, hlsl, c, "MUTANT-4")
    expect("mask writer writes the wrong image", no_mask_store)

    # Case 5 -- a consumer that is DISPATCHED but whose arm is missing from one
    # twin. The pass runs on one backend and not the other, with no error
    # anywhere. The mutation removes pass 8's arm from the GLSL while leaving a
    # dispatch site for it, so check_parity must catch the asymmetry.
    def dispatched_consumer_parity_break():
        g, c = base()
        c += "    pass(8, true);\n"
        g = g.replace("case 8: RunUpscale(); break;",
                      "case 8: break;", 1)
        return check_parity(g, hlsl)
    expect("dispatched consumer arm missing from one twin",
            dispatched_consumer_parity_break)

    # Case 6 -- DISPATCH ORDER (overwrite). The mask writer placed before a
    # writer of the same image that lands BETWEEN the mask and its consumer.
    # This is the bug that shipped in task 2's first version: pass 24 sat after
    # pass 2, and pass 7 (bilateral) overwrote img_Bloom_Temp before pass 6 read
    # it. A channel-liveness check cannot see this at all.
    #
    # The mutation must reproduce the real shape: the mask moves to the FRONT
    # and a mid-schedule writer stays between it and pass 6. Moving pass 4 (which
    # already runs after pass 6) would not be a hazard, which is exactly why the
    # check scopes the window by the consumer rather than by "any later writer".
    def writer_after_mask():
        g, c = base()
        mask_call = f"pass({MASK_WRITER_PASS});"
        c = c.replace(mask_call, "", 1)
        c = c.replace("pass(1);", "pass(1);pass(%d);" % MASK_WRITER_PASS, 1)
        return check(g, hlsl, c, "MUTANT-6")
    expect("mask writer dispatched before a later writer of the same image",
            writer_after_mask)

    # Case 7 -- DISPATCH ORDER (stale read). The consumer placed BEFORE the mask
    # writer: it reads the previous frame's mask. This is the second bug, and it
    # is the one that made a CORRECT mask produce no visible change -- masked-EASU
    # came out bit-identical to bilinear. A liveness check cannot see it either.
    def consumer_before_mask():
        g, c = base()
        mask_call = f"pass({MASK_WRITER_PASS});"
        c = c.replace(mask_call, "", 1)
        c = c.replace("pass(6);", "pass(6);pass(%d);" % MASK_WRITER_PASS, 1)
        return check(g, hlsl, c, "MUTANT-7")
    expect("consumer dispatched before the mask writer (stale mask read)",
            consumer_before_mask)

    print()
    if ok:
        print("RESULT: PASS -- all six reactive-mask mutations were detected")
    else:
        print("RESULT: FAIL -- a guard is vacuous; fix it before adding the pass")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on a hazard")
    args = ap.parse_args()

    if not SHADER_GLSL.exists():
        print(f"FAIL not found: {SHADER_GLSL}")
        return 1

    glsl = SHADER_GLSL.read_text(encoding="utf-8", errors="replace")
    hlsl = (SHADER_HLSL.read_text(encoding="utf-8", errors="replace")
            if SHADER_HLSL.exists() else "")
    cpp = (RENDERER_CPP.read_text(encoding="utf-8", errors="replace")
           if RENDERER_CPP.exists() else "")

    if args.self_test:
        return self_test(glsl, hlsl, cpp)

    print("Reactive-mask channel budget")
    failures = check(glsl, hlsl, cpp, "GLSL")
    if hlsl:
        failures.extend(check(hlsl, glsl, cpp, "HLSL"))
        failures.extend(check_parity(glsl, hlsl))

    print()
    if failures:
        for f in failures:
            print("  HAZARD " + f)
        print("\nRESULT: HAZARD (pass --strict to gate on this)")
        return 1 if args.strict else 0
    print("RESULT: PASS -- the mask's channel is written by its pass, read only "
          "by its consumers, and declared in both twins")
    return 0


if __name__ == "__main__":
    sys.exit(main())
