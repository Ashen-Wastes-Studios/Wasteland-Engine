"""Physics 3D: does a dynamic body actually rest on a static floor?

The item-14 plan claims a "box-hull contact bug": a static box floor pushes a
dynamic body UPWARD +0.05/step instead of letting it fall, and names
``test_physics_3d.py`` as the reproducer. That file did not exist, so the claim
was unverified for as long as it was written down. This is that reproducer,
written from the components' own contracts rather than from the claim.

What it asserts, per shape (box AND sphere, so a shape-specific bug shows up as
a shape-specific failure):

* a dynamic body dropped above a floor FALLS -- never rises,
* it comes to REST on the surface rather than sinking through it,
* it STAYS there (a body still sinking passes a single reading by luck),
* zero gravity holds a body up,
* a body created outside OnPhysics3DStart participates at all -- the plan claims
  late-spawned entities never simulate, and this settles that either way,
* a 30fps caller advances real time -- the plan claims StepPhysics clamps dt to
  1/30 and so runs at half speed, and this settles it too.

Every assertion reads back a position. A suite that only proves nothing crashed
cannot tell a working solver from one that steps a body into the floor forever.

Run:  python tests/headless/runner.py physics_3d
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
    RigidBody3DComponent,
    RigidBody3DType,
    Scene,
    SphereCollider3DComponent,
    Vec3,
)
from runner import check, check_le  # noqa: E402

STEP = 1.0 / 60.0
TOL = 0.30


def _y(entity):
    return entity.GetPosition().y


def two_body_scene(shape, drop_y=4.0, floor_top=0.0, gravity=1.0):
    """A static box floor plus a dynamic dropper, in one scene."""
    sc = Scene()

    floor = sc.CreateEntity("floor")
    floor.SetPosition(Vec3(0.0, floor_top - 0.5, 0.0))
    frb = RigidBody3DComponent()
    frb.Type = RigidBody3DType.Static
    floor.AddOrReplaceRigidBody3DComponent(frb)
    fbc = BoxCollider3DComponent()
    fbc.HalfExtents = Vec3(20.0, 0.5, 20.0)
    fbc.Density = 0.0
    floor.AddOrReplaceBoxCollider3DComponent(fbc)

    ball = sc.CreateEntity("ball")
    ball.SetPosition(Vec3(0.0, drop_y, 0.0))
    drb = RigidBody3DComponent()
    drb.Type = RigidBody3DType.Dynamic
    drb.GravityScale = gravity
    ball.AddOrReplaceRigidBody3DComponent(drb)
    if shape == "sphere":
        c = SphereCollider3DComponent()
        c.Radius = 0.5
        c.Density = 1.0
        ball.AddOrReplaceSphereCollider3DComponent(c)
    else:
        c = BoxCollider3DComponent()
        c.HalfExtents = Vec3(0.5, 0.5, 0.5)
        c.Density = 1.0
        ball.AddOrReplaceBoxCollider3DComponent(c)

    # OnPhysics3DStart is NOT bound to Python. OnSimulationStart() is, and it
    # calls both physics starts -- without it there is no Box3D world, no
    # solver, and every body sits at its authored position forever.
    sc.OnSimulationStart()
    return sc, ball


def run(sc, seconds, step=STEP):
    for _ in range(int(seconds / step)):
        sc.StepPhysics(step)


SHAPES = ("box", "sphere")


def test_a_dynamic_body_falls():
    """A dropped body must FALL. The plan claimed a box floor pushes one UP."""
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=6.0)
        y0 = _y(ball)
        run(sc, 0.5)
        y1 = _y(ball)
        print("  %-7s y %.3f -> %.3f" % (shape, y0, y1))
        check(y1 < y0,
              "a %s dropped from 6.0 must FALL, not rise (after 0.5s y=%.4f)"
              % (shape, y1))


def test_it_rests_on_the_floor_and_stays():
    """It must land ON the surface and STOP there."""
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=4.0, floor_top=0.0)
        run(sc, 4.0)
        y = _y(ball)
        print("  %-7s resting y = %.4f  (floor top 0 + half-height 0.5 = 0.5)"
              % (shape, y))
        check(y > 0.0, "a %s must not sink through a static floor (y=%.4f)"
              % (shape, y))
        check_le(abs(y - 0.5), TOL,
                 "a %s should come to rest at ~0.5, got %.4f" % (shape, y))

        # A body still sinking passes the reading above only if it happened to
        # cross 0.5 on the way down, so require it to stay put.
        run(sc, 2.0)
        y_after = _y(ball)
        print("  %-7s 2s later    = %.4f" % (shape, y_after))
        check_le(abs(y_after - y), 0.05,
                 "a resting %s must not keep moving (%.4f -> %.4f)"
                 % (shape, y, y_after))


def test_zero_gravity_holds_a_body_up():
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=3.0, gravity=0.0)
        run(sc, 1.0)
        y = _y(ball)
        print("  %-7s y = %.4f" % (shape, y))
        check_le(abs(y - 3.0), 0.05,
                 "GravityScale=0 must hold a %s up (y=%.4f)" % (shape, y))


def test_a_body_added_after_startup_still_simulates():
    """The plan claimed late-spawned entities never simulate."""
    for shape in SHAPES:
        sc = Scene()
        sc.OnSimulationStart()  # world first, so the body really is late
        late = sc.CreateEntity("late")
        late.SetPosition(Vec3(0.0, 5.0, 0.0))
        run(sc, 0.2)
        rb = RigidBody3DComponent()
        rb.Type = RigidBody3DType.Dynamic
        rb.GravityScale = 1.0
        late.AddOrReplaceRigidBody3DComponent(rb)
        if shape == "sphere":
            c = SphereCollider3DComponent()
            c.Radius = 0.5
            c.Density = 1.0
            late.AddOrReplaceSphereCollider3DComponent(c)
        else:
            c = BoxCollider3DComponent()
            c.HalfExtents = Vec3(0.5, 0.5, 0.5)
            c.Density = 1.0
            late.AddOrReplaceBoxCollider3DComponent(c)

        y0 = _y(late)
        run(sc, 0.5)
        y1 = _y(late)
        print("  %-7s y %.3f -> %.3f" % (shape, y0, y1))
        check(y1 < y0, "a %s added after startup must still fall (y=%.4f)"
              % (shape, y1))


def test_a_30fps_caller_advances_real_time():
    """The plan claimed StepPhysics clamps to 1/30 and runs at half speed."""
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=10.0, floor_top=-40.0)
        y0 = _y(ball)
        # 30 frames at 30fps == 1.0s of real time. Free fall is ~4.905m.
        run(sc, 1.0, step=1.0 / 30.0)
        fell = y0 - _y(ball)
        print("  %-7s fell %.3f m in 1.0s of 30fps frames (ideal ~4.905)"
              % (shape, fell))
        check_le(abs(fell - 4.905), 0.8,
                 "30fps must advance real time for a %s (fell %.4f, ideal ~4.905)"
                 % (shape, fell))
