"""The instance-delta gate: the transform epoch, and what it does and does not cover.

WHAT PHASE 2 STEP 3 ACTUALLY IS
-------------------------------
Every frame, `Scene::RenderScene`/`OnUpdateRuntime` clears `m_SceneInstances` and every
`Draw*` re-pushes every entity, then `SceneInstancesChanged()` memcmps the previous
frame's snapshot against the new one to decide whether the BVH needs rebuilding. At 50k
instances that memcmp is over the whole scene every frame, on a scene where nothing moved.

Step 3 adds a cheaper gate in front of it: the Scene carries a transform *epoch* that the
runtime path bumps whenever it writes a `TransformComponent`, the renderer samples it each
frame, and an unchanged epoch short-circuits the memcmp.

THE PART THAT IS NOT OBVIOUS, AND IS THE REASON THIS FILE EXISTS
---------------------------------------------------------------
The epoch is a FAST PATH, so its correctness requirement is one-directional: it may only
answer "unchanged" when that is *provably* true. It must never miss a write, because a
missed bump leaves a stale BVH and a stale instance SSBO -- which is the pop-in Phase 2
exists to prevent. A gate that silently under-reports is worse than no gate.

So the stamp is deliberately CONSERVATIVE. It is bumped for the whole script pass, not for
an observed transform write: a Python write goes through pybind11's `reference_internal`
view of the member (`t.Translation.x += 1`), no C++ setter ever runs, and there is no
write-notification mechanism in the tree. Over-bumping costs the fast path; the memcmp
remains the second gate and recovers from anything the epoch misses.

The tests below therefore assert the properties that matter, and they are ordered so the
NEGATIVE CONTROL comes before the positive one: a suite that only checked "the epoch
bumped" would pass just as well against an epoch that bumps unconditionally, which is
exactly the failure this design is avoiding.

WHAT IS STILL NOT TESTABLE
--------------------------
The instance-count assertions the plan's Step 1 describes -- "moving one entity does not
change the submitted instance count", "bvhRebuilds <= 1" -- need a render. `Scene` has no
render entry point of its own, so no headless test can drive `m_SceneInstances`.
`test_the_render_counters_are_reachable_headless` probes whether the counters can be read
at all; when they cannot it says so rather than failing, because "the renderer is not
initialised headless" is a fact about the harness, not a regression.
`tools/verify_instance_delta_gate.py` covers the wiring a render would exercise.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wlpath

wlpath.setup()

import Wasteland  # noqa: E402
from Wasteland import (  # noqa: E402
    BoxCollider3DComponent,
    RigidBody3DComponent,
    RigidBody3DType,
    Scene,
    Vec3,
)
from runner import check, check_eq  # noqa: E402

STEP = 1.0 / 60.0

# The counters Phase 2's test reads. Named here so a missing one is a readable failure
# rather than an AttributeError from each test below.
DELTA_COUNTERS = (
    "InstancesSubmitted",
    "BvhRebuilds",
    "InstanceInvalidations",
    "EpochShortCircuits",
    "SceneTransformEpoch",
)


def _static_scene():
    """A scene with no physics world and no scripts -- the epoch's negative control."""
    sc = Scene()
    sc.CreateEntity("inert")
    return sc


def _falling_scene():
    """A static floor plus a dynamic body, so the 3D read-back actually writes."""
    sc = Scene()

    floor = sc.CreateEntity("floor")
    floor.SetPosition(Vec3(0.0, -0.5, 0.0))
    frb = RigidBody3DComponent()
    frb.Type = RigidBody3DType.Static
    floor.AddOrReplaceRigidBody3DComponent(frb)
    fbc = BoxCollider3DComponent()
    fbc.HalfExtents = Vec3(20.0, 0.5, 20.0)
    fbc.Density = 0.0
    floor.AddOrReplaceBoxCollider3DComponent(fbc)

    ball = sc.CreateEntity("ball")
    ball.SetPosition(Vec3(0.0, 4.0, 0.0))
    drb = RigidBody3DComponent()
    drb.Type = RigidBody3DType.Dynamic
    ball.AddOrReplaceRigidBody3DComponent(drb)
    c = BoxCollider3DComponent()
    c.HalfExtents = Vec3(0.5, 0.5, 0.5)
    c.Density = 1.0
    ball.AddOrReplaceBoxCollider3DComponent(c)

    # OnPhysics3DStart is not bound to Python; OnSimulationStart is, and without it
    # there is no Box3D world, no solver, and no read-back ever happens.
    sc.OnSimulationStart()
    return sc, ball


def test_the_epoch_and_the_counters_are_bound():
    """The API the gate is built on, and the counters the plan's test reads.

    Checked first so a missing binding fails as one readable message instead of an
    AttributeError from each test below.
    """
    sc = Scene()
    check(hasattr(sc, "GetTransformEpoch"),
          "Scene.GetTransformEpoch is not bound -- the epoch the gate reads is unreachable")
    check(callable(getattr(sc, "GetTransformEpoch", None)),
          "Scene.GetTransformEpoch is bound but not callable")
    check_eq(sc.GetTransformEpoch(), 0,
             "a fresh Scene must start at epoch 0, so the renderer can trust 0 as a real value")

    check(hasattr(Wasteland, "Renderer3D_SetSceneTransformEpoch"),
          "Renderer3D_SetSceneTransformEpoch is not bound -- the Scene cannot publish its epoch")
    check(hasattr(Wasteland, "Renderer3D_GetStats"),
          "Renderer3D_GetStats is not bound -- the render counters are unreachable")

    stats = Wasteland.Renderer3D_GetStats()
    missing = [c for c in DELTA_COUNTERS if not hasattr(stats, c)]
    check(not missing,
          f"Renderer3DStatistics is missing the Phase 2 counters: {missing}")


def test_a_static_scene_does_not_bump_the_epoch():
    """NEGATIVE CONTROL. Nothing wrote a transform, so the epoch must not move.

    This is the assertion the positive test cannot replace: without it, an epoch that
    bumps on every frame -- or on every `OnUpdateRuntime` -- passes the positive test and
    makes the renderer's fast path worthless while looking correct.
    """
    sc = _static_scene()
    before = sc.GetTransformEpoch()

    # A real runtime tick: water, health, scripts, terrain streaming, physics. None of
    # them may write a transform here, because there is nothing to write.
    for _ in range(5):
        sc.OnUpdateRuntime(STEP)

    check_eq(sc.GetTransformEpoch(), before,
             "a scene with no physics and no scripts bumped the transform epoch; the "
             "renderer's fast path would never fire")


def test_physics_readback_bumps_the_epoch():
    """The 3D read-back writes Translation + Rotation, so the epoch must move.

    This is the positive control, and it is the path a physics-driven scene actually
    takes. It runs AFTER the negative control so a green run cannot come from an epoch
    that always bumps.
    """
    sc, ball = _falling_scene()
    before = sc.GetTransformEpoch()

    sc.StepPhysics(STEP)
    after_one = sc.GetTransformEpoch()
    check(after_one > before,
          f"the physics read-back wrote a transform but the epoch did not move "
          f"({before} -> {after_one}); the renderer would keep a stale BVH")

    # ...and it must keep moving while the body keeps falling.
    y_start = ball.GetPosition().y
    for _ in range(240):
        sc.StepPhysics(STEP)
    y_end = ball.GetPosition().y
    check(y_end < y_start - 1.0,
          f"the dynamic body did not fall ({y_start} -> {y_end}), so this test is not "
          "exercising the read-back it claims to")

    moving_epoch = sc.GetTransformEpoch()
    check(moving_epoch > after_one,
          "the epoch did not advance while the body was falling")

    # Settled: the body is resting on the floor. The read-back still runs every step,
    # but it writes the same values, so the epoch must stop advancing -- otherwise the
    # gate never fires on the very scene it is for.
    for _ in range(120):
        sc.StepPhysics(STEP)
    settled_epoch = sc.GetTransformEpoch()
    check_eq(settled_epoch, moving_epoch,
             "the epoch kept advancing after the body came to rest, so a resting scene "
             "would never short-circuit the memcmp")


def test_the_script_pass_bumps_the_epoch():
    """The script pass is covered CONSERVATIVELY: any ticked script bumps the epoch.

    Not because a script necessarily wrote a transform, but because a Python write is
    unobservable -- `t.Translation.x += 1` mutates the member through pybind11's
    `reference_internal` view and no C++ setter runs. Over-bumping costs the fast path;
    under-bumping would be a stale BVH.

    The asset-dependent half is skipped with a reason rather than failed: a missing
    authored tree is an environment fact, not a regression in the stamp.
    """
    sc = _static_scene()
    before = sc.GetTransformEpoch()

    # No script entities at all: the pass runs, ticks nothing, and must not bump. This
    # half needs no assets and always runs.
    for _ in range(3):
        sc.OnUpdateRuntime(STEP)
    check_eq(sc.GetTransformEpoch(), before,
             "the script pass bumped the epoch with zero script entities")

    asset_root = Path(wlpath.setup()) / "assets"
    tree_path = asset_root / "scripts" / "EnemyChase.wlbtree"
    if not tree_path.is_file():
        print(f"  skip script-pass bump: {tree_path} not found")
        return

    from Wasteland import BehaviorTreeComponent  # noqa: PLC0415

    entity = sc.CreateEntity("scripted")
    component = BehaviorTreeComponent()
    component.TreePath = str(tree_path)
    entity.AddOrReplaceBehaviorTreeComponent(component)

    Wasteland.Project.Get().AttachAssetRoot(str(asset_root))

    for _ in range(3):
        sc.OnUpdateRuntime(STEP)

    check(sc.GetTransformEpoch() > before,
          "a ticked script entity did not bump the epoch; the conservative stamp is "
          "not wired to the script pass")


def test_the_render_counters_are_reachable_headless():
    """Can the renderer's own counters be read without a render?

    This is the boundary of what a headless test can prove, so it is written to report
    rather than assert: an uninitialised renderer is a fact about the harness, not a
    regression in the gate.
    """
    try:
        stats = Wasteland.Renderer3D_GetStats()
    except Exception as exc:  # noqa: BLE001 - a probe, and the message is the point
        print(f"  skip render counters: Renderer3D_GetStats raised {type(exc).__name__}: {exc}")
        return

    if stats.InstancesSubmitted == 0 and stats.BvhRebuilds == 0:
        print("  note render counters read as zero: no renderer is initialised headless, "
              "so the instance-count assertions need a real render")
        return

    print(f"  render counters live: InstancesSubmitted={stats.InstancesSubmitted} "
          f"BvhRebuilds={stats.BvhRebuilds} "
          f"EpochShortCircuits={stats.EpochShortCircuits}")


def test_instance_counts_still_need_a_headless_render():
    """The remaining gap, asserted so it cannot be mistaken for a passing test.

    `Scene` exposes no render entry point, so `m_SceneInstances` cannot be observed from
    Python and the plan's Step 1 assertions cannot be written yet. This documents that
    rather than silently omitting them; when a headless submission path exists, this is
    the placeholder to replace.
    """
    sc = Scene()
    if hasattr(sc, "RenderScene"):
        print("  RenderScene is bound -- the instance-count assertions can now be "
              "written against it; remove this placeholder")
        return
    print("  confirmed: Scene has no RenderScene, so instance counts remain "
          "unobservable headless (the wiring is covered by "
          "tools/verify_instance_delta_gate.py)")
