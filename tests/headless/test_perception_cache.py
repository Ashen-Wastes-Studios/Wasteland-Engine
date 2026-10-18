"""NB.7 perception: shared throttled snapshot, sight cone, real line of sight.

Three separate claims, tested separately because they fail differently.

1. THE CACHE. `SenseNearest` used to scan every entity, per NPC, per tick --
   O(R*N) per frame. It now reads a snapshot rebuilt on an interval, so the cost
   is O(R) once per interval no matter how many NPCs ask. The assertion that
   matters is the SCAN COUNT, not the wall clock: a timing test passes on a
   fast machine and on a cached one. So this counts how often the entity list is
   actually walked, with N NPCs asking N*R times.

   The cost claim is only worth making if the cache is SHARED. A cache on the
   tree would be one full scan per NPC -- exactly the thing being fixed -- so
   there is a test for two NPCs sharing one scan.

2. THE CONE. `HalfAngle` restricts sensing to a cone around the agent's facing.
   Default 0 means "no cone", and that is the assertion worth making: 0 is the
   value an author leaves on a pin they do not care about, so if 0 were taken
   literally as a zero-width cone every untouched `SenseNearest` in every
   authored tree would silently go blind.

3. LINE OF SIGHT. `RequireLOS` traces against geometry. The pre-NB.7 version
   returned only a distance test, which is why the fallback path matters: with
   no physics world there is nothing to trace, and refusing to see anything
   there would make every NPC blind in a headless test.

Run:  python tests/headless/test_perception_cache.py
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()

COMPILER = r"C:\dev\Wasteland\Wasteland\src\Wasteland\VisualBlocks\Compiler.cpp"

_failures: list[str] = []
_passes = 0


def check(cond, msg):
    global _passes
    if cond:
        _passes += 1
    else:
        _failures.append(msg)


# --------------------------------------------------------------------------
# The shim, extracted and run against fakes: same reason as the pathfinding
# suite -- the rare branches are exactly the ones a 20-minute build never gets
# to exercise.
# --------------------------------------------------------------------------

import ast  # noqa: E402
import math  # noqa: E402
import types  # noqa: E402


def load_shim() -> str:
    text = open(COMPILER, encoding="utf-8", errors="surrogateescape").read()
    start = text.find('R"WL_SHIM(')
    assert start >= 0, "no shim literal in Compiler.cpp"
    start += len('R"WL_SHIM(')
    shim = text[start:text.find(')WL_SHIM"', start)]
    ast.parse(shim)
    return shim


class V:
    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


class Tr:
    def __init__(self, x, y, z, yaw=0.0):
        self.Translation = V(x, y, z)
        self.Rotation = V(0, yaw, 0)
        self.Scale = V(1, 1, 1)


class Health:
    def __init__(self, dead=False):
        self.IsDead = dead
        self.Max = 100.0
        self.Current = 0.0 if dead else 100.0


class Ent:
    """An entity that also COUNTS how often it was offered to a scan."""

    def __init__(self, x=0.0, y=0.0, z=0.0, yaw=0.0, dead=False, health=True,
                 uid=1):
        self.p = (x, y, z)
        self.yaw = yaw
        self.uid = uid
        self._health = Health(dead) if health else None

    def GetTransform(self):
        return Tr(*self.p, yaw=self.yaw)

    def SetTransform(self, translation, rotation, scale=None):
        self.p = (translation.x, translation.y, translation.z)
        self.yaw = rotation.y

    def GetUUID(self):
        return self.uid

    def HasHealthComponent(self):
        return self._health is not None

    def GetHealthComponent(self):
        return self._health


class Scene:
    """Counts GetAllEntitiesWithTransform -- i.e. how often the world is scanned."""

    def __init__(self, entities):
        self.entities = list(entities)
        self.scans = 0

    def GetAllEntitiesWithTransform(self):
        self.scans += 1
        return list(self.entities)

    def GetNavMesh(self):
        return None

    def Raycast(self, *a):
        return (False, V(0, 0, 0), V(0, 0, 0), None, 0.0, 0.0)


class Tree:
    def __init__(self, scene=None):
        self.node_states = {}
        self.scene = scene


class BB:
    def __init__(self):
        self.vals = {}
        self.valid = set()

    def set(self, k, v):
        self.vals[k] = v
        self.valid.add(k)

    def clear(self, k):
        self.vals.pop(k, None)
        self.valid.discard(k)

    def get(self, k):
        return self.vals.get(k)


class Ctx:
    def __init__(self, tree, ent):
        self.tree = tree
        self.entity = ent
        self.bb = BB()
        self.seconds = 0.1
        self.now = 0.0


def build_module(shim):
    import Wasteland
    m = types.ModuleType("shim_under_test")
    m.__dict__.update(dict(
        Wasteland=Wasteland, Vec3=Wasteland.Vec3,
        SUCCESS="SUCCESS", FAILURE="FAILURE", RUNNING="RUNNING"))
    exec(compile(shim, "<shim>", "exec"), m.__dict__)
    return m


def world(M, count, yaw=0.0):
    """One agent at the origin facing `yaw`, plus `count` targets spread along +X."""
    npc = Ent(0, 0, 0, yaw=yaw, uid=999)
    targets = [Ent(2.0 + i * 2.0, 0, 0, uid=100 + i) for i in range(count)]
    sc = Scene([npc] + targets)
    ctx = Ctx(Tree(sc), npc)
    return npc, targets, sc, ctx


# --------------------------------------------------------------------------

def test_the_snapshot_is_shared_across_npcs():
    """The whole point: N NPCs asking must not mean N scans."""
    M = build_module(load_shim())
    targets = [Ent(2.0 + i, 0, 0, uid=200 + i) for i in range(10)]
    sc = Scene([Ent(0, 0, 0, uid=999)] + targets)

    agents = []
    for n in range(8):
        a = Ent(0, 0, 0, uid=1000 + n)
        agents.append(Ctx(Tree(sc), a))

    TICKS = 30
    INTERVAL = 0.2
    for tick in range(TICKS):
        for ctx in agents:
            ctx.now = tick * 0.1
            M._sense_nearest(ctx.entity, ctx, "target", 50.0)

    # One scan per interval, not one per agent per tick.
    #
    # The bound is derived, not guessed: TICKS * 0.1 s of simulated time is 3.0 s,
    # and at a 0.2 s interval that is at most 3.0/0.2 = 15 refreshes (plus one for
    # a cold start, plus a tick's worth of float slop). Before the cache this was
    # TICKS * len(agents) = 240 scans. Asserting the derived ceiling rather than a
    # tight magic number keeps the test meaningful if either side is retuned.
    ceiling = int(TICKS * 0.1 / INTERVAL) + 2
    unshared = TICKS * len(agents)
    check(sc.scans <= ceiling,
          "%d ticks x %d NPCs caused %d world scans; the ceiling for a shared "
          "snapshot is %d (uncached would be %d)"
          % (TICKS, len(agents), sc.scans, ceiling, unshared))
    check(sc.scans >= 1, "nothing was ever scanned")

    # The cache must live on the SCENE, not the tree: a per-tree cache is one
    # full scan per NPC, which is precisely the cost being removed.
    cache = getattr(sc, "_wl_perception", None)
    check(cache is not None,
          "no perception cache on the scene; the snapshot is being rebuilt or "
          "stored somewhere that is not shared between NPCs")


def test_the_snapshot_refreshes_so_dead_targets_retire():
    """A cache that never refreshes makes NPCs shoot corpses."""
    M = build_module(load_shim())
    npc, targets, sc, ctx = world(M, 2)
    M._sense_nearest(npc, ctx, "target", 50.0)
    first = ctx.bb.get("target")
    check(first is not None, "no target acquired at all")

    # Kill everything, then advance past the interval.
    for t in targets:
        t._health = Health(dead=True)
    ctx.now = 1.0
    M._sense_nearest(npc, ctx, "target", 50.0)
    check(ctx.bb.get("target") is None,
          "a dead target is still held in the blackboard after the snapshot "
          "refreshed -- the NPC will attack a corpse")
    check("target" not in ctx.bb.valid,
          "the key must be CLEARED, not set to None: a set(None) key reads as "
          "valid forever and a BlackboardIsSet gate never re-acquires")


def test_default_half_angle_is_omnidirectional():
    """HalfAngle 0 must mean 'no cone', not a zero-width cone."""
    M = build_module(load_shim())
    # Target directly BEHIND the agent (yaw 0 = +Z, so behind is -Z).
    npc = Ent(0, 0, 0, yaw=0.0, uid=999)
    behind = Ent(0, 0, -5.0, uid=1001)
    sc = Scene([npc, behind])
    ctx = Ctx(Tree(sc), npc)
    r = M._sense_nearest(npc, ctx, "target", 50.0)
    check(r == "SUCCESS", "sensing returned %r" % (r,))
    check(ctx.bb.get("target") is behind,
          "with no cone configured, a target directly behind was not acquired. "
          "HalfAngle=0 must mean 'all directions', or every untouched "
          "SenseNearest in every authored tree goes blind")


def test_a_narrow_cone_excludes_what_is_behind():
    """The discriminating half: the same scene, a cone set, different answer."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, yaw=0.0, uid=999)     # facing +Z
    behind = Ent(0, 0, -5.0, uid=1001)
    ahead = Ent(0, 0, 5.0, uid=1002)
    sc = Scene([npc, behind, ahead])
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, 45.0)
    got = ctx.bb.get("target")
    check(got is ahead,
          "a 45-degree cone acquired %r; it should pick the target ahead and "
          "ignore the one directly behind" % (got,))


def test_the_cone_follows_facing():
    """Turn the agent and the accepted target changes. Proves it is a cone."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, yaw=0.0, uid=999)
    behind = Ent(0, 0, -5.0, uid=1001)
    sc = Scene([npc, behind])
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, 45.0)
    check(ctx.bb.get("target") is None,
          "facing +Z, the -Z target was acquired through a 45-degree cone")

    npc.yaw = 180.0                            # now facing -Z
    ctx.now = 1.0                              # past the interval, so it rescans
    M._sense_nearest(npc, ctx, "target", 50.0, 45.0)
    check(ctx.bb.get("target") is behind,
          "after turning to face -Z the same target was not acquired; the cone "
          "is not following the agent's facing")


def test_entities_without_health_are_not_acquired():
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    scenery = Ent(0, 0, 3.0, uid=1001, health=False)
    sc = Scene([npc, scenery])
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0)
    check(ctx.bb.get("target") is None,
          "an entity with no HealthComponent was acquired as a target")


def test_no_scene_is_a_clear_not_a_crash():
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    ctx = Ctx(None, npc)                       # tree with no scene
    r = M._sense_nearest(npc, ctx, "target", 50.0)
    check(r == "SUCCESS", "sensing with no scene returned %r, expected SUCCESS" % (r,))
    check("target" not in ctx.bb.valid, "the key must be cleared, not set to None")


def test_los_defaults_to_visible():
    """RequireLOS=False must behave exactly like pre-NB.7: distance only."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    far = Ent(0, 0, 3.0, uid=1001)
    sc = Scene([npc, far])
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, None, False)
    check(ctx.bb.get("target") is far,
          "with RequireLOS off, a visible-in-practice target was not acquired; "
          "the default must preserve the old behaviour exactly")


def test_a_blocked_ray_hides_the_target():
    """The whole reason NB.7 replaced the distance-only version.

    The blocker is an ANONYMOUS geometry hit, not an entity. That is deliberate:
    an entity with a HealthComponent is a legitimate target, so a "wall" built
    as one is acquired as the nearest thing in range and this test passes or
    fails for the wrong reason. Real level geometry is not in the entity list
    and reports no entity on a hit -- so that is what is modelled here.
    """
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    target = Ent(0, 0, 5.0, uid=1002)
    sc = Scene([npc, target])

    # World geometry: hittable is False and there is no entity behind it. That
    # is the ONLY shape that actually exercises occlusion -- an earlier version
    # of this mock returned the TARGET as the hit entity, so _los_blocked's
    # ignore-the-endpoint branch correctly cleared it and the "wall" never
    # blocked anything. The assertion below passed for the wrong reason.
    blocked = lambda *a: (True, None, V(0, 0, 2.0), V(0, 0, -1), 3.0, False)
    sc.Raycast = blocked
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, None, True)
    check(ctx.bb.get("target") is None,
          "a target behind a wall was acquired with RequireLOS on; the old "
          "distance-only check could never have caught this")

    sc.Raycast = lambda *a: (False, None, V(0, 0, 0), V(0, 0, 0), 0.0, False)
    ctx.now = 1.0
    M._sense_nearest(npc, ctx, "target", 50.0, None, True)
    check(ctx.bb.get("target") is target,
          "with nothing blocking, RequireLOS still refused the target")

    # And the same geometry must NOT be reached for when the cone is off and
    # LOS is off: the default has to keep acquiring straight through walls.
    ctx.now = 2.0
    sc.Raycast = blocked
    M._sense_nearest(npc, ctx, "target", 50.0, None, False)
    check(ctx.bb.get("target") is target,
          "with RequireLOS off, the wall was still respected; the default must "
          "preserve the pre-NB.7 distance-only behaviour")


def test_the_target_entity_itself_does_not_block():
    """A ray that ends ON the target must not read as blocked by it."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    target = Ent(0, 0, 5.0, uid=1002)
    sc = Scene([npc, target])

    def raycast(origin, direction, distance, *rest):
        return (True, target, V(0, 0, 5.0), V(0, 0, -1), distance, True)
    sc.Raycast = raycast

    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, None, True)
    check(ctx.bb.get("target") is target,
          "the target blocked its own line of sight; every NPC would be blind "
          "to anything it is looking directly at")


def test_world_geometry_with_no_entity_does_block():
    """A hit with no entity is level geometry, and level geometry blocks."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    target = Ent(0, 0, 5.0, uid=1002)
    sc = Scene([npc, target])
    sc.Raycast = lambda *a: (True, None, V(0, 0, 2.0), V(0, 0, -1), 3.0, False)
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0, None, True)
    check(ctx.bb.get("target") is None,
          "a raycast hitting anonymous world geometry was treated as clear")


def test_has_los_takes_ctx_now():
    """Signature changed deliberately: without ctx there is no scene to trace."""
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    target = Ent(0, 0, 5.0, uid=1002)
    sc = Scene([npc, target])
    # Anonymous geometry: a real wall is not an entity, so a hit on one carries
    # no entity and nothing for the target to be confused with.
    sc.Raycast = lambda *a: (True, None, V(0, 0, 2.0), V(0, 0, -1), 3.0, False)
    ctx = Ctx(Tree(sc), npc)
    check(M._has_los(ctx, npc, target, 50.0) is False,
          "_has_los said a target behind a wall was visible")
    sc.Raycast = lambda *a: (False, None, V(0, 0, 0), V(0, 0, 0), 0.0, False)
    check(M._has_los(ctx, npc, target, 50.0) is True,
          "_has_los refused a clear line of sight")
    check(M._has_los(ctx, npc, target, 1.0) is False,
          "_has_los ignored its range limit")


def test_the_npc_never_senses_itself():
    M = build_module(load_shim())
    npc = Ent(0, 0, 0, uid=999)
    sc = Scene([npc])                          # nothing but itself
    ctx = Ctx(Tree(sc), npc)
    M._sense_nearest(npc, ctx, "target", 50.0)
    check(ctx.bb.get("target") is None,
          "the agent sensed itself; it would walk to where it already stands")


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