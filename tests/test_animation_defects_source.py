"""Source contract for Task A.4's ComputeWorldMatrices / ComputeSkinningMatrices fix.

The behaviour is asserted numerically in
tests/headless/test_animation_defects.py. What a headless process cannot reach is
the SIGNATURE: the whole defect was that two parameters controlled one thing, and
the binding deliberately hardcodes an identity root, so no Python value can
exercise the difference. This pins the shape of the API so the ambiguity cannot
come back.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "Animation")

PASSED = 0
FAILED = []


def read(name):
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(text):
    """Remove // comments so a check cannot be satisfied by a comment mentioning
    the very identifier it is looking for."""
    return "\n".join(line.split("//", 1)[0] for line in text.split("\n"))


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def main():
    header = strip_comments(read("Skeleton.h"))
    animator_h = strip_comments(read("Animator.h"))
    impl = read("Skeleton.cpp")
    bindings = read(os.path.join("..", "Scripting", "Bindings.cpp"))

    # 1. outWithRoot is gone from the declaration.
    decl = re.search(r"void ComputeWorldMatrices\(.*?\)\s*const;", header, re.S)
    check(decl is not None, "ComputeWorldMatrices is declared")
    if decl:
        check("outWithRoot" not in decl.group(0),
              "ComputeWorldMatrices no longer takes outWithRoot")
        check("rootTransform" in decl.group(0),
              "ComputeWorldMatrices still takes rootTransform (the single control)")

    # 2. ComputeSkinningMatrices takes no root transform at all.
    skin_decl = re.search(r"void ComputeSkinningMatrices\(.*?\)\s*const;", header, re.S)
    check(skin_decl is not None, "ComputeSkinningMatrices is declared")
    if skin_decl:
        check("rootTransform" not in skin_decl.group(0),
              "ComputeSkinningMatrices takes no root transform (character-space by definition)")
        check("outWithRoot" not in skin_decl.group(0),
              "ComputeSkinningMatrices has no outWithRoot either")

    # 3. No caller anywhere still passes four arguments.
    for name, text in (("Skeleton.cpp", impl), ("Animator.cpp", read("Animator.cpp")),
                       ("AnimationSystem.cpp", read(os.path.join("..", "Scene", "AnimationSystem.cpp"))),
                       ("Bindings.cpp", bindings)):
        check("ComputeWorldMatrices(pose, outSkinning, rootTransform, false)" not in text,
              "%s: no stale 4-argument ComputeWorldMatrices call" % name)

    # 4. CaptureBindMatrices returns bool and the serializer checks it.
    check(re.search(r"bool\s+Skeleton::CaptureBindMatrices", impl),
          "CaptureBindMatrices is defined returning bool")
    serializer = read("AnimationSerializer.cpp")
    check(re.search(r"if\s*\(\s*!\s*skeleton->CaptureBindMatrices\(\)\s*\)", serializer),
          "the serializer checks the capture result rather than ignoring it")

    # 5. IsRoot bounds-checks before indexing.
    isroot = re.search(r"bool IsRoot\(BoneIndex index\) const\s*\{.*?\}", header, re.S)
    check(isroot is not None, "IsRoot is defined inline")
    if isroot:
        check(">= m_Bones.size()" in isroot.group(0),
              "IsRoot tests the index against m_Bones.size() before indexing")

    # 6. BlendMode is settable and the enum is bound.
    check(re.search(r"void SetBlendMode\(BlendMode", animator_h),
          "Animator has SetBlendMode")
    check(re.search(r"BlendMode GetBlendMode\(\) const", animator_h),
          "Animator has GetBlendMode")
    check('py::enum_<Wasteland::Animation::Animator::BlendMode>' in bindings,
          "BlendMode is bound, so a script has a value to pass")

    # 7. The PlayAnimation shim calls the runtime instead of returning FAILURE.
    compiler = read(os.path.join("..", "VisualBlocks", "Compiler.cpp"))
    shim = re.search(r"def _play_animation\(.*?\n(.*?)\n\ndef ", compiler, re.S)
    check(shim is not None, "_play_animation shim is present")
    if shim:
        check("return SUCCESS" in shim.group(1),
              "the shim can return SUCCESS (it previously returned FAILURE unconditionally)")
        check("FAILURE" in shim.group(1),
              "the shim still returns FAILURE for the unrecoverable cases")

    # 8. BuildFromNames enforces the length contract.
    build = re.search(r"bool Skeleton::BuildFromNames\(.*?\n\t\t\{.*?\n\t\t\}", impl, re.S)
    check(build is not None, "BuildFromNames is defined")
    if build:
        check("names.size() != parentNames.size()" in build.group(0),
              "BuildFromNames rejects a length mismatch up front")

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
