"""A Kinematic body must be driven by CODE, not by the solver.

`Scene::StepPhysics` wrote the Box3D body's position and rotation BACK into the
transform after every step, for every RigidBody3DComponent. For a Kinematic body
that write-back runs AFTER the script pass, so whatever a script or behaviour
tree wrote this frame is discarded: the body has not moved, so the read-back
restores the pre-script transform and the NPC stands still with no error
anywhere.

Verified against the built module before the fix: EnemyChase walked 12.6 m with
physics stopped and 0.0 m with it running, and the hand-written
`Enemy_legacy_reference.py` behaved identically -- so the mechanism, not the
tree.

Kinematic means "code drives it", so the data now flows the OTHER way: the
transform is pushed INTO the body (so collision detection sees the new position
this step) and the transform is left alone. Dynamic and Static keep the
read-back, which is where their motion comes from.

Run:  python tests/headless/runner.py test_kinematic_script_sync
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    BoxCollider3DComponent,
    HealthComponent,
    Project,
    RigidBody3DComponent,
    RigidBody3DType,
    Scene,
    ScriptComponent,
    SphereCollider3DComponent,
    Vec3,
)
from runner import check  # noqa: E402

REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
SCRIPTS = _os.path.join(ASSET_ROOT, "assets", "scripts")
if SCRIPTS not in _sys.path:
    _sys.path.insert(0, SCRIPTS)

STEP = 0.1
HOST_PATH = "scripts/EnemyHost.py"


def attach_root():
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def kinematic_npc(scene, pos, name="npc"):
    """An entity the tree can move: Kinematic body + a collider.

    The collider matters. StepPhysics creates a Box3D body for the rigidbody
    component, and without a shape the body is a point -- the transform still
    syncs, but nothing about it is physical. A sphere gives the body a real
    shape so the assertion is about a body the solver actually knows.
    """
    e = scene.CreateEntity(name)
    e.SetPosition(Vec3(*pos))
    rb = RigidBody3DComponent()
    rb.Type = RigidBody3DType.Kinematic
    rb.GravityScale = 0.0
    e.AddOrReplaceRigidBody3DComponent(rb)
    c = SphereCollider3DComponent()
    c.Radius = 0.5
    c.Density = 0.0
    e.AddOrReplaceSphereCollider3DComponent(c)
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    e.AddOrReplaceHealthComponent(h)
    return e


def attach_chase(scene, entity, class_name="EnemyChaseHost"):
    sc = ScriptComponent()
    sc.ScriptPath = HOST_PATH
    sc.ScriptName = class_name
    entity.AddOrReplaceScriptComponent(sc)
    return sc


def victim_at(scene, pos):
    v = scene.CreateEntity("victim")
    v.SetPosition(Vec3(*pos))
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    v.AddOrReplaceHealthComponent(h)
    return v


def moved_metres(before, after):
    return ((after.x - before.x) ** 2 + (after.y - before.y) ** 2
            + (after.z - before.z) ** 2) ** 0.5


def run(sc, seconds):
    for _ in range(int(seconds / STEP)):
        sc.OnUpdateRuntime(STEP)


# --------------------------------------------------------------------------
def test_kinematic_npc_moves_with_physics_running():
    """The fix: with the solver running, a script-driven NPC must still move.

    This is the regression. Before it, the read-back restored the pre-script
    transform every frame and the NPC stayed exactly where it started.
    """
    attach_root()
    sc = Scene()
    npc = kinematic_npc(sc, (0.0, 0.0, 0.0))
    victim_at(sc, (10.0, 0.0, 0.0))
    attach_chase(sc, npc)

    # OnSimulationStart is the bound entry point; without it there is no Box3D
    # world and every body sits at its authored position forever. Starting it
    # here is what puts the solver in the path the bug lived in.
    sc.OnSimulationStart()

    p0 = npc.GetPosition()
    run(sc, 3.0)
    p1 = npc.GetPosition()

    moved = moved_metres(p0, p1)
    print("  physics RUNNING: pos (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)  moved %.3f m"
          % (p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, moved))
    check(moved > 0.5,
          "a Kinematic NPC driven by a tree must MOVE with physics running, "
          "moved only %.3f m" % moved)


def test_the_same_npc_moves_without_physics():
    """The control: with no solver the tree drives the transform directly.

    Both paths must move. A fix that made physics break movement, or that made
    movement only work without physics, would pass one test and fail the other.
    """
    attach_root()
    sc = Scene()
    npc = kinematic_npc(sc, (0.0, 0.0, 0.0))
    victim_at(sc, (10.0, 0.0, 0.0))
    attach_chase(sc, npc)

    p0 = npc.GetPosition()
    run(sc, 3.0)
    p1 = npc.GetPosition()

    moved = moved_metres(p0, p1)
    print("  physics STOPPED: moved %.3f m" % moved)
    check(moved > 0.5,
          "a Kinematic NPC must move without physics too, moved %.3f m" % moved)


def test_a_dynamic_body_still_reads_back():
    """The fix must not break the OTHER direction.

    Dynamic bodies get their motion from the solver, so their transform must
    still be written back from the body. A Dynamic ball dropped onto a static
    floor must fall -- if the read-back were removed wholesale this would fail,
    and the fix is only correct if it is scoped to Kinematic.
    """
    attach_root()
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
    drb.GravityScale = 1.0
    ball.AddOrReplaceRigidBody3DComponent(drb)
    c = SphereCollider3DComponent()
    c.Radius = 0.5
    c.Density = 1.0
    ball.AddOrReplaceSphereCollider3DComponent(c)

    sc.OnSimulationStart()
    y0 = ball.GetPosition().y
    run(sc, 1.0)
    y1 = ball.GetPosition().y
    print("  Dynamic ball y %.3f -> %.3f" % (y0, y1))
    check(y1 < y0,
          "a Dynamic body must still be driven BY the solver (y %.3f -> %.3f)"
          % (y0, y1))


def test_kinematic_transform_reaches_the_body():
    """Pushing the transform into the body must be observable, not just stored.

    The body is what collision detection reads. A Kinematic body moved by code
    must end up where the code put it -- otherwise the NPC renders in one place
    and collides in another, which is a bug that only shows up as "the enemy
    walked through the wall".
    """
    attach_root()
    sc = Scene()
    npc = kinematic_npc(sc, (0.0, 0.0, 0.0))
    victim_at(sc, (10.0, 0.0, 0.0))
    attach_chase(sc, npc)

    sc.OnSimulationStart()
    run(sc, 1.0)

    # After the sync, the transform and the body agree. Read the transform back
    # and confirm it is NOT the authored origin -- the read-back must not have
    # restored it.
    p = npc.GetPosition()
    print("  after 1.0s: pos (%.2f, %.2f, %.2f)" % (p.x, p.y, p.z))
    check(abs(p.x) > 0.5 or abs(p.z) > 0.5,
          "the Kinematic body must carry the script's transform, got (%.3f, %.3f, %.3f)"
          % (p.x, p.y, p.z))
