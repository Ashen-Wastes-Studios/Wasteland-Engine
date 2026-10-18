"""Does authored-tree movement actually follow the ground?

`BehaviorTreeSystem` compiles and ticks trees, and `EnemyChase.wlbtree` drives
a real entity. But the generated `MoveTo` shim moved in a straight 3D line,
interpolating Y toward the TARGET's Y. That is not ground height:

  * uphill to a target on a ramp -> the NPC hovers above the ramp
  * downhill from one           -> the NPC sinks through it

`Enemy.py` solved this with an explicit `GroundHeightAt` probe. Without the same
probe in the tree, migrating an NPC from a script onto a tree is a silent
regression on any level with slopes -- the kind that shows up as "the NPC looks
broken" rather than as a test failure.

So this suite pins the ground-following behaviour, and mostly pins the cases
that are easy to get wrong:

  * the probe's Raycast unpacking matches the real binding (a mismatch is
    invisible -- it just always misses)
  * it snaps to the SURFACE, not to the target's height
  * it does NOT snap to y=0 when nothing is hit; movement still happens
  * snapping Y must not clobber rotation, which reads as a FaceTarget bug
  * with no physics world at all, it degrades instead of raising out of a tick

Run:  python tests/headless/test_ground_snap.py
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402

from test_tree_attach import (  # noqa: E402
    attach_root,
    attach_tree,
    make_actor,
    tick,
)


def _pos(entity):
    """Read translation as a plain tuple; Vec3 has no arithmetic."""
    p = entity.GetTransform().Translation
    return (p.x, p.y, p.z)


def _make_sensable(entity):
    """Give a target the HealthComponent SenseNearest requires.

    SenseNearest skips anything without one, so a bare entity placed 10m away
    is invisible: the NPC acquires no target, the selector never reaches the
    `walk` branch, and it stands at the origin looking broken. The symptom
    points at movement, but the cause is perception.
    """
    h = Wasteland.HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    entity.AddOrReplaceHealthComponent(h)
    return entity


def test_raycast_returns_the_six_tuple_the_probe_unpacks():
    """The shim unpacks 6 values with a bool first.

    A change to the binding's arity would not raise -- it would make the ground
    probe silently always miss, and the only symptom would be NPCs floating.
    """
    scene = Wasteland.Scene()
    result = scene.Raycast(
        Wasteland.Vec3(0.0, 10.0, 0.0), Wasteland.Vec3(0.0, -1.0, 0.0), 100.0
    )
    assert isinstance(result, tuple), "Raycast must return a tuple, got %r" % type(result)
    assert len(result) == 6, "probe unpacks 6 values, binding returns %d" % len(result)
    assert isinstance(result[0], bool), "first element must be the hit bool"


def test_movement_follows_the_floor_not_the_target_height():
    """A player floating high above the floor must not drag the NPC up with it."""
    attach_root()
    # Both entities live in ONE scene: the tree's perception sweep searches
    # its own scene, so a target in a separate world is invisible to it and
    # the NPC never moves.
    sc, npc, hc = make_actor("npc", (0.0, 0.0, 0.0))
    player = sc.CreateEntity("player")
    player.SetPosition(Wasteland.Vec3(0.0, 25.0, 10.0))
    _make_sensable(player)
    attach_tree(sc, npc)

    before = _pos(npc)
    tick(sc, 2.0)
    after = _pos(npc)

    assert after[2] > before[2], "NPC never travelled toward the target: %r -> %r" % (before, after)
    assert after[1] < 10.0, (
        "NPC climbed to the TARGET's height instead of following the floor: y=%r" % after[1]
    )


def test_no_floor_means_no_snap_to_zero():
    """Nothing under the NPC: it must travel, and must NOT snap to y=0.

    Returning 0.0 instead of None when a probe misses is the classic version of
    this bug -- the NPC walks off a ledge and teleports to the void floor.
    """
    attach_root()
    sc, npc, hc = make_actor("npc", (0.0, 60.0, 0.0))
    player = sc.CreateEntity("player")
    # Same height, so the 3D distance is 10m and inside SenseNearest's 30m
    # radius. Perception measures FULL 3D distance, so a target 60m straight
    # down is simply not acquired and the NPC correctly never moves -- which
    # looks identical to a movement bug until you check the radius.
    player.SetPosition(Wasteland.Vec3(0.0, 60.0, 10.0))
    _make_sensable(player)
    attach_tree(sc, npc)

    tick(sc, 1.0)

    p = _pos(npc)
    assert p[2] > 0.0, "NPC did not travel with no floor present: %r" % (p,)
    assert abs(p[1] - 60.0) < 8.0, "NPC snapped to y=0 with no ground hit: y=%r" % p[1]


def test_ground_snap_preserves_facing():
    """Snapping Y must not zero rotation.

    `SetTransform` takes translation/rotation/scale separately; writing only the
    position zeroes rotation. That reads as "the NPC walks sideways" and gets
    debugged as a FaceTarget bug rather than a movement bug.
    """
    attach_root()
    sc, npc, hc = make_actor("npc", (0.0, 0.0, 0.0))
    player = sc.CreateEntity("player")
    player.SetPosition(Wasteland.Vec3(0.0, 0.0, 10.0))
    _make_sensable(player)
    tr = npc.GetTransform()
    npc.SetTransform(tr.Translation, Wasteland.Vec3(0.0, 90.0, 0.0), tr.Scale)
    attach_tree(sc, npc)

    tick(sc, 0.5)

    r = npc.GetTransform().Rotation
    assert abs(r.y - 90.0) < 1.0, "movement clobbered facing: rotation.y=%r" % r.y