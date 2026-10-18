"""LOS against the REAL Scene.Raycast contract -- not a mock.

Why this suite exists. `_los_blocked` shipped reading `hit[3]` for the entity,
but `Scene.Raycast` returns:

    (hit, entity, point, normal, distance, hittable)
                ^entity^        ^normal^   ^hittable^
                index 1         index 3    index 5

Index 3 is the NORMAL -- a Vec3, never None. So two branches were dead:
`hit_entity is None` never fired, so world geometry never counted as blocking,
and `hit_entity.GetUUID()` raised into a bare `except`, so the
ignore-the-endpoint check never ran either.

It survived because the only test guarding it mocked Raycast as
`(True, Vec3, Vec3, None, ...)` -- a Vec3 in the entity slot and None in the
normal slot. That is the FLOW-GRAPH order, not the engine's, so the fixture
agreed with the bug instead of with the contract. The suite passed 13/13 green
while HasLineOfSight reported every collider-bearing target as blocked.

Two things this suite does that the old one did not:

  * It builds real geometry with Static rigid bodies and uses the REAL
    `Scene.Raycast`. A collider with no RigidBody is never in the broadphase,
    so it silently tests nothing.
  * It asserts the three-way table directly, so a regression names which case
    broke rather than showing a count.

The bare `except Exception: return False` around the Raycast is load-bearing
and was nearly masked here too: an incomplete harness that forgets to inject
`Wasteland` into the generated module's globals raises NameError, gets
swallowed, and the helper reports "not blocked" for every input -- which looks
exactly like a working clear-line result. That is why the module injection
below is explicit.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_ge, check_le

import wlpath

wlpath.setup()

import Wasteland
from Wasteland import (Vec3, Scene, BoxCollider3DComponent,
                       RigidBody3DComponent, RigidBody3DType)


def _shim():
    """The generated shim, with `Wasteland` injected the way the compiler does.

    The generated module gets `Wasteland` from its runtime globals. Without it,
    every `Wasteland.Vec3(...)` in the shim raises NameError, the helper's bare
    except swallows it, and _los_blocked answers False for EVERY input --
    indistinguishable from a correct clear-line result. That is not
    hypothetical: it is exactly what happened while this suite was being
    written, and it would have made the wall case pass for the wrong reason.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    path = os.path.join(root, "Wasteland", "src", "Wasteland", "VisualBlocks",
                        "Compiler.cpp")
    src = open(path, encoding="utf-8", errors="replace").read()
    i = src.find('R"WL_SHIM(') + len('R"WL_SHIM(')
    ns = {"Wasteland": Wasteland}
    exec(compile(src[i:src.find(')WL_SHIM"', i)], "<shim>", "exec"), ns)
    return ns


def _world():
    """A floor plus a wall spanning x in [-5,5], z in [-0.5,0.5], y in [0,3].

    Static rigid bodies are required. A collider with no RigidBodyComponent is
    never inserted into the broadphase, so the ray passes through it and an
    occlusion test built that way asserts nothing at all.
    """
    sc = Scene()
    for name, pos, half in (("floor", (0, -0.5, 0), (20, 0.5, 20)),
                            ("wall", (0, 1.5, 0), (5, 1.5, 0.5))):
        e = sc.CreateEntity(name)
        e.SetPosition(Vec3(*pos))
        rb = RigidBody3DComponent()
        rb.Type = RigidBody3DType.Static
        e.AddOrReplaceRigidBody3DComponent(rb)
        c = BoxCollider3DComponent()
        c.HalfExtents = Vec3(*half)
        c.Density = 0.0
        e.AddOrReplaceBoxCollider3DComponent(c)
    sc.OnSimulationStart()
    return sc


class _Tree:
    def __init__(self, scene):
        self.scene = scene


class _Ctx:
    def __init__(self, tree):
        self.tree = tree


def test_the_raycast_tuple_is_hit_entity_point_normal_distance_hittable():
    """Pin the real contract, so a wrong index fails loudly and locally.

    This is the assertion whose absence let the bug live: the shim read index 3
    for the entity while every mock in the suite agreed with it.
    """
    raw = _world().Raycast(Vec3(0, 1, -5), Vec3(0, 0, 10), 10.0)
    check_eq(len(raw), 6, "Raycast must return six values")
    check(isinstance(raw[1], Wasteland.Entity),
          "index 1 is the Entity, got %s" % type(raw[1]).__name__)
    check(isinstance(raw[3], Wasteland.Vec3),
          "index 3 is the NORMAL Vec3, not the entity; got %s"
          % type(raw[3]).__name__)
    check_eq(raw[5], False, "index 5 hittable is False for world geometry")


def test_world_geometry_between_two_points_blocks_line_of_sight():
    """The bug: reading hit[3] made every ray report CLEAR.

    All three of these returned False before the fix, because index 3 is the
    normal -- a Vec3 that is never None -- so the world-geometry branch was
    unreachable.
    """
    sc = _world()
    ctx = _Ctx(_Tree(sc))
    los = _shim()["_los_blocked"]

    check_eq(los(ctx, 0, 1, -9, 0, 1, -8), False,
             "a clear line in open air must not be blocked")
    check_eq(los(ctx, 0, 1, -5, 0, 1, 5), True,
             "a ray straight through the wall must be blocked; if this fails, "
             "check that the wall has a Static RigidBody -- a bare collider is "
             "not in the broadphase and asserts nothing")
    check_eq(los(ctx, 0, 1, 8, 0, 1, 9), False,
             "a line past the end of the wall must be clear")


def test_an_untraceable_scene_is_not_claimed_to_be_blocked():
    """"I cannot prove it is blocked" and "it is blocked" are different claims."""
    class _NoScene:
        scene = None

    los = _shim()["_los_blocked"]
    check_eq(los(_Ctx(_NoScene()), 0, 1, -5, 0, 1, 5), False,
             "with no scene, LOS must report clear rather than assume blocked")
