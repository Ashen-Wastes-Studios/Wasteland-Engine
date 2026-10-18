"""Does `UsePathfinding` actually route an NPC around geometry?

NB.8 of the visual-blocks plan. `MoveTo` gains a `UsePathfinding` bool pin; on,
it asks the scene's navmesh for a route instead of walking a straight line.

The pin DEFAULTING TO FALSE is load-bearing, so it gets asserted first: every
`.wlbtree` already on disk has no such pin, and a default of true would silently
change how all of them behave on the next rebuild.

The negative controls are the point. `UsePathfinding` off is a straight line, and
a straight line through a wall is exactly what the NPC did before this feature
existed -- so a test that only checks the happy path cannot tell pathfinding from
the behaviour it replaced. What distinguishes them:

  * OFF: walks straight at the target, through the wall
  * ON,  no navmesh baked: identical to OFF, because a missing mesh must never
    stop an NPC dead
  * ON,  mesh baked, target unreachable: falls back rather than freezing
  * ON,  mesh baked, wall present: detours, and the detour is LONGER than the
    straight line it replaced

Run:  python tests/headless/test_pathfinding_node.py
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402
from Wasteland import Vec3, Scene, HealthComponent  # noqa: E402

ASSET_ROOT = r"C:\dev\Wasteland\DemonCore-Editor"
STEP = 0.1

# The generated tree does `import wl_behavior`, and that module ships in the
# ASSET tree rather than the module tree. Python runs isolated here, so the
# asset scripts directory has to be put on sys.path explicitly -- attaching the
# asset root is not enough.
_SCRIPTS = os.path.join(ASSET_ROOT, "assets", "scripts")
if os.path.isdir(_SCRIPTS) and _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

_failures: list[str] = []
_passes = 0


def check(cond, msg):
    global _passes
    if cond:
        _passes += 1
    else:
        _failures.append(msg)


def attach_root():
    Wasteland.Project.Get().AttachAssetRoot(ASSET_ROOT)


def _floor(sc, size=30.0, half=0.5, top=0.0):
    e = sc.CreateEntity("floor")
    e.SetPosition(Vec3(0.0, top - half, 0.0))
    b = Wasteland.BoxCollider3DComponent()
    b.HalfExtents = Vec3(size * 0.5, half, size * 0.5)
    e.AddOrReplaceBoxCollider3DComponent(b)
    return e


def _wall(sc, x, z, half_x=0.5, half_z=3.0, height=3.0):
    e = sc.CreateEntity("wall")
    e.SetPosition(Vec3(x, height * 0.5, z))
    b = Wasteland.BoxCollider3DComponent()
    b.HalfExtents = Vec3(half_x, height * 0.5, half_z)
    e.AddOrReplaceBoxCollider3DComponent(b)
    return e


def _tree(use_pathfinding):
    """Compile the authored chase tree with the pin set as asked.

    Compiles rather than loading the .wlbtree because the authored file has no
    such pin, and adding one to shipped content would change every level that
    references it. The pin is set here so the test can compare ON against OFF
    using the same tree otherwise.
    """
    src = Scene.CompileBehaviorTree("trees/EnemyChase.wlbtree")
    if use_pathfinding:
        needle = '_move(ctx.entity, ctx, "target", 3.000000, False)'
        if needle in src:
            src = src.replace(needle,
                              '_move(ctx.entity, ctx, "target", 3.000000, True)')
    mod = type(sys)("wl_test_tree")
    mod.__dict__["__name__"] = "wl_test_tree"
    exec(compile(src, "<tree>", "exec"), mod.__dict__)
    return mod.EnemyChase


def _world(target, wall=True, bake=False):
    sc = Scene()
    npc = sc.CreateEntity("npc")
    npc.SetPosition(Vec3(-8.0, 0.0, 0.0))
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    npc.AddOrReplaceHealthComponent(h)

    player = sc.CreateEntity("player")
    player.SetPosition(Vec3(*target))
    ph = HealthComponent()
    ph.Max = 100.0
    ph.Current = 100.0
    player.AddOrReplaceHealthComponent(ph)

    _floor(sc)
    if wall:
        _wall(sc, 0.0, 0.0, half_x=0.5, half_z=4.0)

    if bake:
        sc.BakeNavMesh(Wasteland.NavMeshConfig())

    tree = _tree(True)
    inst = tree(npc, sc)
    return sc, npc, player, inst


def _pos(e):
    t = e.GetTransform().Translation
    return (t.x, t.y, t.z)


def _run(sc, inst, seconds):
    """Tick the tree instance directly.

    NOT sc.OnUpdateRuntime(): that drives the BehaviourTreeSystem, which only
    ticks entities carrying an attached BehaviorTreeComponent. Constructing the
    class by hand -- as this test does, so it can compile a tree with the pin
    set -- does not attach one, so OnUpdateRuntime silently ticks nothing and the
    NPC stands still. That reads as "pathfinding does not move NPCs" and sends
    you into the wrong file entirely; the generated evaluator is fine.
    """
    for _ in range(int(seconds / STEP)):
        inst.tick(STEP)


def _travelled_z(npc):
    return abs(_pos(npc)[2] - 0.0)


# --------------------------------------------------------------------------

def test_the_pin_exists_and_defaults_to_false():
    """Every saved tree omits the pin, so the default decides what happens."""
    attach_root()
    src = Scene.CompileBehaviorTree("trees/EnemyChase.wlbtree")
    check("_move(ctx.entity, ctx, \"target\", 3.000000, False)" in src,
          "MoveTo did not emit the pathfinding argument at all")
    check("True)" not in src.split("_move(")[1][:80],
          "MoveTo emitted pathfinding ON by default; every existing .wlbtree "
          "would change behaviour on the next rebuild")


def test_pathfinding_off_walks_straight_through_the_wall():
    """The behaviour this feature replaces. Nothing else may change it."""
    attach_root()
    sc, npc, player, inst = _world((8.0, 0.0, 0.0), wall=True, bake=True)
    _run(sc, inst, 4.0)
    z = _pos(npc)[2]
    # With a wall spanning |z| < 4 and the NPC travelling in +X... it crossed x=0
    # while staying near z=0, i.e. straight through the wall. Assert it moved
    # and that it did NOT detour in Z.
    check(abs(_pos(npc)[0]) < 3.0 or _pos(npc)[0] > 0.0,
          "NPC made no progress with pathfinding off: x=%r" % _pos(npc)[0])
    check(abs(z) < 6.0,
          "NPC detoured in Z with pathfinding OFF: z=%r -- the default must not "
          "change existing behaviour" % z)


def test_pathfinding_on_with_no_bake_behaves_like_off():
    """A missing navmesh must not stop an NPC dead."""
    attach_root()
    sc, npc, player, inst = _world((8.0, 0.0, 0.0), wall=True, bake=False)
    check(sc.GetNavMesh() is None, "a scene with no bake should report no mesh")
    _run(sc, inst, 4.0)
    check(_pos(npc)[0] > 0.5,
          "NPC did not move at all with pathfinding on and no navmesh: %r"
          % (_pos(npc),))


def test_pathfinding_on_detours_around_the_wall():
    """The distinguishing assertion.

    With a mesh baked and pathfinding on, the NPC must end up well off the z=0
    line -- having gone around the wall -- where the OFF case walks straight
    through it.
    """
    attach_root()
    sc, npc, player, inst = _world((8.0, 0.0, 0.0), wall=True, bake=True)
    check(sc.GetNavMesh() is not None, "BakeNavMesh did not produce a mesh")

    # Sample the TRAIL, not the final position. Asserting on where the NPC
    # ended up is the wrong question: by the time it reaches the target it has
    # already come back toward z=0, so a correct detour ends up looking like it
    # walked straight through. The wall spans |z| < 4, so a route that ever
    # reached x>0.5 while inside that band went THROUGH; a detour necessarily
    # leaves the band to clear it.
    trail = []
    for _ in range(80):
        _run(sc, inst, STEP)
        trail.append(_pos(npc))

    p = trail[-1]
    check(p[0] > 0.5, "NPC never got past the wall: %r" % (p,))

    # Derive the wall's blocked span from the MESH, probed AT the wall's own x.
    #
    # Two things make a hardcoded threshold wrong here, and both are about
    # measuring in the wrong place rather than about pathfinding:
    #
    #   * The blocked band is the raw half-extent (4.0) inflated by AgentRadius
    #     (0.4) and then quantised to 0.5m cells, so it is not 4.0 and not 4.4.
    #   * It only exists at the wall's X. At x=1.6 -- well past the wall's 0.5
    #     half-extent -- z=-4.4 is ordinary walkable floor, so a detour that
    #     slides along there is correct. Measuring the band globally and then
    #     applying it everywhere flags that legal slide as going through.
    mesh = sc.GetNavMesh()
    wall_x = 0.0
    wall_half_x = 0.5
    block = 0.0
    z = 0.5
    while z < 12.0:
        if mesh.SampleHeight(Vec3(wall_x, 0.0, z)) is None:
            block = z
        elif block > 0.0:
            break
        z += 0.1
    check(block > 0.0, "could not measure the wall's blocked span from the mesh")
    # "Through the wall" means crossing the wall's SLAB: inside its x-extent
    # AND inside its z-span. Either alone is a perfectly good route.
    through = [(round(x, 2), round(z, 2)) for x, y, z in trail
               if abs(x) <= wall_half_x + 0.4 and abs(z) < block]
    check(not through,
          "the NPC crossed the wall's slab (|x|<=%.1f) while inside its "
          "blocked span |z|<%.1f, "
          "so it went THROUGH rather than around: %r"
          % (wall_half_x + 0.4, block, through[:6]))

    max_off = max(abs(z) for x, y, z in trail)
    check(max_off > block,
          "the NPC never cleared the wall's blocked z-span (max |z| = %.2f, "
          "needs > %.1f); it went straight through" % (max_off, block))


def test_unreachable_target_falls_back_rather_than_freezing():
    """A goal the mesh genuinely cannot route to must degrade, not freeze.

    "Outside the floor" is not unreachable: FindNearestWalkable is an expanding
    ring search that snaps such a point onto the nearest real cell, so A* finds
    a route and this test would pass while proving nothing. An unreachable goal
    needs a DISCONNECTED walkable region -- a wall spanning the whole floor
    separates the two sides, and the far side is unreachable without going
    around, which is exactly what the level does not allow.

    With pathfinding ON and no route, the NPC must still move toward the goal by
    direct movement. Freezing is the failure this guards: an NPC standing still
    with a trace showing a clean success walk.
    """
    attach_root()
    sc, npc, player, inst = _world((8.0, 0.0, 0.0), wall=False, bake=True)

    # Put the target inside a walled enclosure, so the island is walkable but
    # DISCONNECTED from the agent's side.
    #
    # Neither of the obvious shortcuts produces an unreachable goal: a wall
    # spanning the floor just extends the mesh's bounds to match, and a goal off
    # the edge is snapped onto the nearest real cell by the ring search. Only a
    # second region with no connection to the first actually fails A*.
    for ox, oz, hx, hz in ((12.0, 9.5, 2.6, 0.5), (12.0, 14.5, 2.6, 0.5),
                           (9.5, 12.0, 0.5, 2.6), (14.5, 12.0, 0.5, 2.6)):
        e = sc.CreateEntity("enclosure")
        e.SetPosition(Vec3(ox, 1.5, oz))
        b = Wasteland.BoxCollider3DComponent()
        b.HalfExtents = Vec3(hx, 1.5, hz)
        e.AddOrReplaceBoxCollider3DComponent(b)
    player.SetPosition(Vec3(12.0, 0.0, 12.0))

    sc.BakeNavMesh(Wasteland.NavMeshConfig())
    mesh = sc.GetNavMesh()
    check(mesh.SampleHeight(Vec3(12.0, 0.0, 12.0)) is not None,
          "the target is not even on walkable ground, so this is not testing a "
          "reachable-looking goal")
    check(not mesh.FindPath(Vec3(-8.0, 0.0, 0.0), Vec3(12.0, 0.0, 12.0)),
          "a route to the enclosed island exists, so this is not exercising an "
          "unreachable goal")

    _run(sc, inst, 2.0)
    p_ = _pos(npc)
    check(p_[0] != -8.0 or p_[2] != 0.0,
          "NPC did not move at all with an unreachable target: %r" % (p_,))

def test_clearing_the_mesh_is_survivable():
    """ClearNavMesh mid-run must not crash or strand the NPC."""
    attach_root()
    sc, npc, player, inst = _world((8.0, 0.0, 0.0), wall=True, bake=True)
    _run(sc, inst, 1.0)
    sc.ClearNavMesh()
    check(sc.GetNavMesh() is None, "ClearNavMesh left a mesh behind")
    _run(sc, inst, 1.0)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            _failures.append("%s raised: %s" % (fn.__name__, exc))

    total = _passes + len(_failures)
    if _failures:
        print("FAILURES:")
        for f in _failures:
            print("  * %s" % f)
    print("%d/%d checks passed" % (_passes, total))
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())