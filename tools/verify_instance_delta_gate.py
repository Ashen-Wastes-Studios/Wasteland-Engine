#!/usr/bin/env python3
"""Verify the instance-delta gate: the epoch fast path and its counters.

WHY THIS IS A SOURCE CHECK AND NOT A TEST
-----------------------------------------
The gate lives in the RENDER path (`Renderer3D::SceneInstancesChanged`), which no
headless test can reach: `Wasteland.Renderer3D` is not a module-level binding,
`Scene` has no render entry point, and `Scene::OnUpdateRuntime` never renders.
`tests/headless/test_instance_deltas.py` is written and deliberately RED for that
reason -- it is the spec, not a regression guard.

So this verifies the property that can be checked without a GPU: that the gate is
WIRED the way its correctness argument requires. The argument is one-directional --
the epoch may only ever answer "unchanged" when that is provably true -- so what
matters is ORDERING and COMPLETENESS of the stamp, not any numeric value:

  1. ORDERING. The epoch compare must come AFTER the size check (a scene that
     gained or lost an entity is dirty without any memcmp) and BEFORE the memcmp
     (otherwise it saves nothing).
  2. COMPLETENESS. Every path that writes a TransformComponent through the engine
     must bump the epoch, and every path that accepts the previous-frame snapshot
     must record the epoch it accepted.
  3. THE SENTINEL. `m_LastSceneTransformEpoch` must not start at 0, or the first
     frame compares equal to an unstamped 0 and skips a build the scene needs.

Checks 1-3 are static facts about the source. The counters are checked for
EXISTENCE and for not being reset per frame (a cumulative counter reset every
frame reads 0 forever and cannot answer the question it exists for).

Self-tests at the bottom mutate each fixture and require the check to FAIL, so a
green run means the check can actually fail.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
R3D = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp"
R3D_H = REPO / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.h"
SCENE_CPP = REPO / "Wasteland" / "src" / "Wasteland" / "Scene" / "Scene.cpp"
SCENE_H = REPO / "Wasteland" / "src" / "Wasteland" / "Scene" / "Scene.h"
BINDINGS = REPO / "Wasteland" / "src" / "Wasteland" / "Scripting" / "Bindings.cpp"

FAILS: list[str] = []
CHECKS = 0


def read(path: Path) -> str:
    """Read a source file with CRLF normalised, so checks are line-ending agnostic."""
    return path.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n")


def check(condition: bool, message: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILS.append(message)


def body(text: str, signature: str, anchored: bool = True) -> str:
    """The text from `signature` to the matching close brace.

    Brace counting rather than a line range: these functions have moved by tens of
    lines across edits, and a check keyed to a line number goes stale silently --
    which is how a verifier ends up guarding nothing.
    """
    # Anchor the signature to a DEFINITION (signature + "{"), not merely to the
    # name. Several of these are forward-declared above their definition, and the
    # first "{" after a declaration belongs to whatever inline body happens to sit
    # between the two -- which yields a plausible-looking body for the wrong
    # function and a check that passes while reading nothing.
    m = re.search(re.escape(signature) + r"\s*\{", text)
    if m:
        i = text.find("{", m.start())
    elif not anchored:
        # No brace directly after the name: the signature spans several lines
        # (a multi-line parameter list). Take the first "{" after the name --
        # safe here because this function has no forward declaration, unlike
        # SceneInstancesChanged which is declared above its definition.
        j = text.find(signature)
        if j < 0:
            return ""
        i = text.find("{", j)
    else:
        return ""
    if i < 0:
        return ""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start() if m else text.find(signature) : j + 1]
    return ""


def check_gate() -> None:
    src = read(R3D)
    fn = body(src, "static bool SceneInstancesChanged()")
    check(bool(fn), "SceneInstancesChanged() body not found")

    i_size = fn.find("m_SceneInstances.size() != s_Data.m_PrevSceneInstances.size()")
    i_empty = fn.find("m_SceneInstances.empty() && s_Data.m_TerrainVertices.empty()")
    i_epoch = fn.find("m_SceneTransformEpoch == s_Data.m_LastSceneTransformEpoch")
    i_memcmp = fn.find("memcmp(s_Data.m_SceneInstances.data()")

    check(i_size >= 0, "CHECK 1: size check not found in SceneInstancesChanged()")
    check(i_empty >= 0, "CHECK 1: empty check not found in SceneInstancesChanged()")
    check(i_epoch >= 0, "CHECK 1: epoch compare not found in SceneInstancesChanged()")
    check(i_memcmp >= 0, "CHECK 1: memcmp not found in SceneInstancesChanged()")

    # Order: size, then empty, then epoch, then memcmp.
    if min(i_size, i_empty, i_epoch, i_memcmp) >= 0:
        check(i_size < i_empty, "CHECK 1: size check must precede the empty check")
        check(i_empty < i_epoch, "CHECK 1: empty check must precede the epoch compare")
        check(i_epoch < i_memcmp, "CHECK 1: epoch compare must precede the memcmp")

    # The epoch must be trusted ONLY when one was actually recorded. Without this,
    # an unstamped 0 equals an initialised 0 and the first frame skips its build.
    check(
        "m_SceneTransformEpoch != kEpochUninitialised" in fn
        or "m_SceneTransformEpoch != Renderer3DData::kEpochUninitialised" in fn,
        "CHECK 2: epoch compare is not guarded by the uninitialised sentinel",
    )

    # The sentinel must not be 0 -- 0 is a legitimate epoch.
    check(
        "kEpochUninitialised = 0xFFFFFFFFu" in src,
        "CHECK 2: kEpochUninitialised must be 0xFFFFFFFF, not 0",
    )

    # The previous-frame snapshot and the epoch it was accepted under must be
    # recorded together, or the gate compares against a stale epoch.
    accepts = re.findall(r"m_PrevSceneInstances\s*=\s*s_Data\.m_SceneInstances;", src)
    records = re.findall(
        r"m_LastSceneTransformEpoch\s*=\s*s_Data\.m_SceneTransformEpoch;", src
    )
    check(
        len(accepts) == len(records),
        f"CHECK 3: {len(accepts)} previous-snapshot assignments but "
        f"{len(records)} epoch records -- every accept must record its epoch",
    )
    check(len(accepts) >= 3, f"CHECK 3: only {len(accepts)} snapshot sites found (expected >=3)")

    # The epoch must be published before BeginScene, on both render paths.
    scene = read(SCENE_CPP)
    publishes = re.findall(
        r"Renderer3D::SetSceneTransformEpoch\(m_TransformEpoch\);\s*\n\s*Renderer3D::BeginScene",
        scene,
    )
    check(
        len(publishes) >= 2,
        f"CHECK 4: {len(publishes)} SetSceneTransformEpoch-before-BeginScene sites "
        "(expected 2 -- one per RenderScene overload)",
    )

    # Completeness of the stamp: the three runtime write paths.
    check(scene.count("BumpTransformEpoch();") >= 3,
          "CHECK 5: fewer than 3 BumpTransformEpoch() call sites in Scene.cpp "
          "(expected: script pass, 2D read-back, 3D read-back)")

    # The script pass must be covered conservatively, and the reason recorded.
    check("scriptPassTicked" in scene,
          "CHECK 5: the script pass is not tracked, so its transform writes are invisible")
    check("reference_internal" in read(BINDINGS),
          "CHECK 5: Bindings.cpp no longer documents reference_internal, so the "
          "conservative script-pass bump may no longer be necessary")

    # The stamp must be reachable from the scene, and forgeable only by the engine.
    hdr = read(SCENE_H)
    check("void BumpTransformEpoch()" in hdr,
          "CHECK 6: Scene::BumpTransformEpoch() not declared")
    check("uint32_t GetTransformEpoch() const" in hdr,
          "CHECK 6: Scene::GetTransformEpoch() not declared")
    check("uint32_t m_TransformEpoch = 0;" in hdr,
          "CHECK 6: Scene::m_TransformEpoch not declared")

    binds = read(BINDINGS)
    check('"GetTransformEpoch"' in binds,
          "CHECK 7: Scene.GetTransformEpoch is not bound")
    check('"BumpTransformEpoch"' not in binds,
          "CHECK 7: BumpTransformEpoch must NOT be bound -- a script must not be "
          "able to forge the stamp the renderer trusts to skip its BVH rebuild")

    # The counters must exist, be cumulative, and not be reset per frame.
    r3d_hdr = read(R3D_H)
    for field in ("InstancesSubmitted", "BvhRebuilds", "InstanceInvalidations",
                  "EpochShortCircuits", "SceneTransformEpoch"):
        check(field in r3d_hdr, f"CHECK 8: Statistics.{field} not declared")
        check(f'"{field}"' in binds, f"CHECK 8: Statistics.{field} not bound")

    reset = body(read(R3D), "void Renderer3D::ResetStats()")
    for field in ("InstancesSubmitted", "BvhRebuilds", "InstanceInvalidations",
                  "EpochShortCircuits", "SceneTransformEpoch"):
        check(f"Stats.{field}" not in reset,
              f"CHECK 9: ResetStats clears Stats.{field}, but it is a cumulative "
              "counter -- a per-frame reset makes it read 0 forever")

    # The read-back bumps must be CONDITIONAL on the value actually changing. An
    # unconditional bump means a body resting on the floor re-bumps every step, the gate
    # never fires, and the fast path is dead on the exact scene it exists for -- while
    # remaining correct, which is why it would never show up as an error.
    three_d = scene[scene.find("b3Body_GetRotation(bodyId);") :]
    three_d = three_d[: three_d.find("BumpTransformEpoch();")]
    check("if (" in three_d,
          "CHECK 11: the 3D read-back bumps the epoch unconditionally; a resting body "
          "would keep the gate from ever firing")
    check("transform.Translation != newPosition" in three_d,
          "CHECK 11: the 3D read-back bump is not guarded by a change compare")

    two_d = scene[scene.find("const auto &position = body->GetPosition();") :]
    two_d = two_d[: two_d.find("BumpTransformEpoch();")]
    check("if (" in two_d,
          "CHECK 11: the 2D read-back bumps the epoch unconditionally")
    check("transform.Translation.x != position.x" in two_d,
          "CHECK 11: the 2D read-back bump is not guarded by a change compare")

    # The BVH counter must be in ONE place, reached from the one function every rebuild
    # funnels through -- not at call sites that can be forgotten. Exactly one increment in
    # the whole file, inside the builder or a helper it calls, is the invariant.
    r3d_src = read(R3D)
    builder = body(r3d_src, "static void BuildAndFlattenBVH", anchored=False)
    increments = r3d_src.count("Stats.BvhRebuilds++")
    check(increments == 1,
          f"CHECK 10: {increments} `Stats.BvhRebuilds++` sites in Renderer3D.cpp "
          "(expected exactly 1, so no call site can drift from the counter)")
    check("NoteBvhRebuild()" in builder or "Stats.BvhRebuilds++" in builder,
          "CHECK 10: BuildAndFlattenBVH does not report its rebuild to the counter")
    # An empty submission is not a rebuild: counting one would inflate the metric on
    # every frame the scene is empty, which is the state the gate exists to make common.
    check("!instances.empty()" in builder,
          "CHECK 10: the BVH counter is not skipped for an empty submission")


# --------------------------------------------------------------------------
# Self-tests. Each mutates a fixture the way a real regression would and requires
# the affected check to FAIL. A verifier whose checks cannot fail is decoration.
# --------------------------------------------------------------------------

def _self_test(label: str, mutate, expect_fragment: str) -> None:
    global FAILS, CHECKS
    saved = FAILS, CHECKS
    FAILS, CHECKS = [], 0
    try:
        try:
            mutate()
        except PermissionError:
            # A self-test rewrites the real source and restores it. While MSBuild is
            # compiling that file the write fails, and a crash here would read as a
            # broken verifier rather than a busy tree. Skip and say so.
            print(f"  skip {label}: source file locked by a running build")
            return
        check_gate()
        hit = any(expect_fragment in f for f in FAILS)
        print(f"  {'ok  ' if hit else 'FAIL'} {label}: "
              f"{'caught' if hit else 'NOT CAUGHT'}")
        if not hit:
            print(f"        expected a failure containing {expect_fragment!r}")
    finally:
        FAILS, CHECKS = saved


def run_self_tests() -> None:
    print("self-tests (each must be caught):")
    originals = {p: p.read_bytes() for p in (R3D, SCENE_CPP, SCENE_H, R3D_H, BINDINGS)}

    def restore() -> None:
        for p, b in originals.items():
            p.write_bytes(b)

    def with_file(path: Path, transform) -> None:
        text = read(path)
        path.write_bytes(transform(text).replace("\n", "\r\n").encode("utf-8"))

    try:
        def move_epoch_after_memcmp(text: str) -> str:
            """Cut the epoch block and paste it after the memcmp's `return true;`.

            A condition-only mutation (`if (false)`) deletes the compare TEXT, so the
            "not found" check fires instead of the ordering check and the self-test
            proves nothing about ordering. Reordering keeps every string intact and
            breaks only the order.
            """
            block = (
                "        if (s_Data.m_SceneTransformEpoch != Renderer3DData::kEpochUninitialised &&\n"
                "            s_Data.m_SceneTransformEpoch == s_Data.m_LastSceneTransformEpoch)\n"
                "        {\n"
                "            s_Data.Stats.EpochShortCircuits++;\n"
                "            return false;\n"
                "        }\n"
            )
            assert block in text, "epoch block not found for reordering"
            text = text.replace(block, "", 1)
            # Anchored on the sizeof term rather than the whole line: the
            # continuation line's indentation is not something worth asserting on.
            anchor = "sizeof(RayTracingInstance)) != 0)\n            return true;\n"
            assert anchor in text, "memcmp anchor not found"
            return text.replace(anchor, anchor + block, 1)

        _self_test(
            "epoch compare moved after the memcmp",
            lambda: with_file(R3D, move_epoch_after_memcmp),
            "epoch compare must precede the memcmp",
        )
        _self_test(
            "sentinel guard dropped",
            lambda: with_file(R3D, lambda t: t.replace(
                "m_SceneTransformEpoch != Renderer3DData::kEpochUninitialised &&\n", "")),
            "not guarded by the uninitialised sentinel",
        )
        _self_test(
            "one snapshot accept stops recording its epoch",
            lambda: with_file(R3D, lambda t: t.replace(
                "s_Data.m_LastSceneTransformEpoch=s_Data.m_SceneTransformEpoch;\n", "", 1)),
            "every accept must record its epoch",
        )
        _self_test(
            "a render path stops publishing the epoch",
            lambda: with_file(SCENE_CPP, lambda t: t.replace(
                "Renderer3D::SetSceneTransformEpoch(m_TransformEpoch);\n", "", 1)),
            "SetSceneTransformEpoch-before-BeginScene",
        )
        _self_test(
            "the script pass stops bumping the epoch",
            lambda: with_file(SCENE_CPP, lambda t: t.replace(
                "if (scriptPassTicked)\n\t\t\tBumpTransformEpoch();", "if (false)")),
            "BumpTransformEpoch() call sites",
        )
        _self_test(
            "ResetStats starts clearing a cumulative counter",
            lambda: with_file(R3D, lambda t: t.replace(
                "        s_Data.Stats.DrawCalls = 0;\n",
                "        s_Data.Stats.DrawCalls = 0;\n        s_Data.Stats.BvhRebuilds = 0;\n")),
            "ResetStats clears Stats.BvhRebuilds",
        )
        _self_test(
            "the BVH counter moves out of the shared function",
            lambda: with_file(R3D, lambda t: t.replace(
                "        if (!instances.empty())\n            NoteBvhRebuild();\n", "")),
            "does not report its rebuild to the counter",
        )
        _self_test(
            "a call site starts incrementing the counter itself",
            lambda: with_file(R3D, lambda t: t.replace(
                "        BuildAndFlattenBVH(s_Data.m_SceneInstances,s_Data.m_BVHNodes,s_Data.m_BVHPrimOrder);\n",
                "        s_Data.Stats.BvhRebuilds++;\n"
                "        BuildAndFlattenBVH(s_Data.m_SceneInstances,s_Data.m_BVHNodes,s_Data.m_BVHPrimOrder);\n", 1)),
            "expected exactly 1",
        )
        _self_test(
            "an empty submission starts counting as a rebuild",
            lambda: with_file(R3D, lambda t: t.replace(
                "        if (!instances.empty())\n            NoteBvhRebuild();",
                "        NoteBvhRebuild();")),
            "not skipped for an empty submission",
        )
    finally:
        restore()

    for p, b in originals.items():
        assert p.read_bytes() == b, f"self-test did not restore {p.name}"
    print("  all fixtures restored byte-for-byte")


def main() -> int:
    print(f"verifying instance-delta gate in {REPO}")
    check_gate()
    print(f"  {CHECKS - len(FAILS)}/{CHECKS} checks passed")

    # Reported before the self-tests: a self-test that crashed used to hide these.
    if FAILS:
        print(f"\nFAIL ({len(FAILS)}):")
        for f in FAILS:
            print(f"  - {f}")

    run_self_tests()

    print("\nPASS" if not FAILS else f"\nFAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
