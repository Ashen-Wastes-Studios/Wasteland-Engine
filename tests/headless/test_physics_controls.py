"""Driving a 3D rigid body from script: impulse, velocity set, velocity read.

The gap this suite exists to close: a script could READ physics indefinitely
and still not be able to act on it. ``grep -rn 'ApplyImpulse|SetVelocity|
AddForce' Wasteland/src/`` returned ZERO hits -- so nothing could jump, nothing
could be knocked back, and nothing could be launched. Reading a velocity is not
driving a body.

WHAT IS DELIBERATELY NOT HERE
-----------------------------
``RigidBody3DComponent::RuntimeBody`` is already bound to Python, and exposing
it as a shortcut for "apply an impulse to this handle" would have been about
twenty lines instead of this. It is a raw, packed, PROCESS-LOCAL Box3D body id.
A handle that outlives ``OnPhysics3DStop()`` is a use-after-free, not an API,
and a handle copied into a save file is meaningless in another process. So the
lookup happens inside the engine (``Scene::ResolveBody3D``) and Python only
ever sees a ``bool``. That is why these tests drive things by ENTITY rather
than by handle, and why the failure mode they assert is ``False`` and not an
exception.

Two setup facts this file depends on, both of which produce a confidently
green suite that proves nothing when missed:

* ``OnSimulationStart()`` is the ONLY bound way to bring a physics world up.
  ``OnPhysics3DStart`` is private by design. Without it there is no Box3D
  world, no solver, and every body sits at its authored position forever.
* A component handed to ``AddOrReplace*`` is a SNAPSHOT COPY. Configuring it,
  adding it, and then reading ``.Position`` back off the local object shows the
  attach-time value for ever, so every assertion here reads the position back
  off the ENTITY instead.

Run:  python tests/headless/runner.py physics_controls
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath  # noqa: E402

wlpath.setup()

from Wasteland import (  # noqa: E402
    BoxCollider3DComponent,
    RigidBody3DComponent,
    RigidBody3DType,
    Scene,
    Vec3,
)
from runner import check, check_eq, check_ge, check_le  # noqa: E402

STEP = 1.0 / 60.0


def run(sc, seconds, step=STEP):
    for _ in range(int(seconds / step)):
        sc.StepPhysics(step)


def body_scene(start=(0.0, 0.0, 0.0), gravity=0.0):
    """A scene with ONE dynamic box body and nothing else.

    GravityScale defaults to 0 here so gravity never competes with the thing
    under test: an upward impulse and a downward one have to be
    distinguishable from free fall, not measured against it.

    The scene is returned alongside the entity and must stay referenced -- an
    Entity holds a raw Scene*, so letting the scene go out of scope leaves
    every handle dangling.
    """
    sc = Scene()
    e = sc.CreateEntity("body")
    e.SetPosition(Vec3(*start))

    rb = RigidBody3DComponent()
    rb.Type = RigidBody3DType.Dynamic
    rb.GravityScale = gravity
    e.AddOrReplaceRigidBody3DComponent(rb)

    col = BoxCollider3DComponent()
    col.HalfExtents = Vec3(0.5, 0.5, 0.5)
    col.Density = 1.0
    e.AddOrReplaceBoxCollider3DComponent(col)

    # Without this there is no world, so every call below returns False and
    # every assertion below would pass for the wrong reason.
    sc.OnSimulationStart()
    return sc, e


def y(entity):
    return entity.GetPosition().y


def test_an_upward_impulse_lifts_a_suspended_body():
    """A suspended body (GravityScale = 0) given an upward impulse must RISE.

    GravityScale = 0 is what makes this a clean read: the only thing that can
    move the body up is the impulse. With gravity on, a rise would still be
    plausible for a while and a missing impulse would be masked.
    """
    sc, e = body_scene(start=(0.0, 0.0, 0.0), gravity=0.0)
    y0 = y(e)

    # 1x1x1 box, density 1 -> mass 8 (Box3D computes it from the shape's
    # volume). A 24 N*s impulse is therefore 3 m/s of upward velocity.
    check(sc.ApplyImpulse(e, Vec3(0.0, 24.0, 0.0)),
          "an impulse on a live dynamic body must report True")

    run(sc, 0.25)
    y1 = y(e)
    print("  impulse +Y: y %.3f -> %.3f" % (y0, y1))
    check(y1 > y0 + 0.01,
          "a suspended body given an upward impulse must RISE (y %.4f -> %.4f); "
          "a fall here means the impulse never reached the solver"
          % (y0, y1))

    # Direction matters as much as magnitude: the impulse must not have been
    # applied to a fixed axis or applied as a uniform burst.
    check_le(abs(e.GetPosition().x), 0.01,
             "an impulse purely on +Y must not move x (x=%.4f)"
             % e.GetPosition().x)
    check_le(abs(e.GetPosition().z), 0.01,
             "an impulse purely on +Y must not move z (z=%.4f)"
             % e.GetPosition().z)


def test_set_linear_velocity_stops_a_falling_body():
    """SetLinearVelocity(0) must arrest a falling body.

    Gravity is ON here and the body is dropped from height, so it has real
    downward velocity by the time we stop it. This is the scripted "catch the
    player" case: the engine must overwrite the velocity, not add to it.
    """
    sc, e = body_scene(start=(0.0, 20.0, 0.0), gravity=1.0)
    run(sc, 0.5)

    v = sc.GetLinearVelocity(e)
    check(v is not None, "a live body must report a velocity, not None")
    vy_before = v.y
    check(vy_before < -1.0,
          "the body must actually be FALLING before the stop is tested "
          "(vy=%.4f); the setup is wrong, not the method" % vy_before)
    print("  falling at vy=%.3f" % vy_before)

    check(sc.SetLinearVelocity(e, Vec3(0.0, 0.0, 0.0)),
          "setting velocity on a live dynamic body must report True")

    v = sc.GetLinearVelocity(e)
    check(v is not None and abs(v.y) < 0.05,
          "SetLinearVelocity(0) must zero the velocity immediately (vy=%s)"
          % ("None" if v is None else "%.4f" % v.y))

    # Zeroing velocity is not the same as removing gravity, so the body must
    # resume falling immediately after -- and fall SLOWER than it was.
    y_at_stop = y(e)
    run(sc, 0.2)
    y_after = y(e)
    print("  y at stop %.3f, 0.2s later %.3f" % (y_at_stop, y_after))
    check(y_after < y_at_stop - 0.01,
          "with gravity still on, a stopped body must resume falling "
          "(y %.4f -> %.4f)" % (y_at_stop, y_after))
    check_ge(y_after, y_at_stop - 0.9,
             "a stopped body must not teleport; free fall for 0.2s is ~0.20m "
             "(y %.4f -> %.4f)" % (y_at_stop, y_after))


def test_the_opposite_impulse_cancels_rather_than_accumulates():
    """Reapplying the impulse with the opposite sign must STOP the body.

    This is the assertion that distinguishes a real impulse from a
    mis-accumulating one. If the implementation kept adding velocity instead
    of applying a delta, or if the same impulse were somehow retained between
    calls, the second call would double the speed and the body would sail past
    instead of stopping.
    """
    sc, e = body_scene(start=(0.0, 0.0, 0.0), gravity=0.0)

    check(sc.ApplyImpulse(e, Vec3(0.0, 20.0, 0.0)), "first impulse must apply")
    run(sc, 0.2)
    v = sc.GetLinearVelocity(e)
    check(v is not None and v.y > 0.5,
          "after the first impulse the body must be moving UP (vy=%s)"
          % ("None" if v is None else "%.4f" % v.y))
    print("  after +impulse: vy=%.3f" % v.y)

    check(sc.ApplyImpulse(e, Vec3(0.0, -20.0, 0.0)),
          "the opposite impulse must also apply")
    v = sc.GetLinearVelocity(e)
    check(v is not None and abs(v.y) < 0.35,
          "an equal and opposite impulse must cancel the velocity, not "
          "double it (vy=%s; >0.35 means impulses are accumulating)"
          % ("None" if v is None else "%.4f" % v.y))
    print("  after -impulse: vy=%.3f" % v.y)

    # And the body must actually be near-stationary in space, not merely
    # reading a small velocity.
    y_now = y(e)
    run(sc, 0.25)
    check_le(abs(y(e) - y_now), 0.05,
             "a cancelled body must stay put (y %.4f -> %.4f)"
             % (y_now, y(e)))


def test_every_method_reports_false_for_an_entity_with_no_body():
    """No RigidBody3DComponent: return False, do NOT raise.

    A raise is the wrong contract here. A gameplay script asking "can I push
    this?" wants an answer it can branch on; an exception would tear down the
    whole script frame over what is an ordinary state of the world -- and it
    would do so on every entity that is not a physics body, which is most of
    them.

    The entity here is a live, valid entity with a TransformComponent: the
    only thing missing is the rigid body. That distinction is the point -- it
    is what proves the False comes from the missing body and not from a dead
    entity handle or an empty scene.
    """
    sc = Scene()
    e = sc.CreateEntity("not-a-body")
    e.SetPosition(Vec3(1.0, 2.0, 3.0))
    sc.OnSimulationStart()

    check(sc.ApplyImpulse(e, Vec3(0.0, 10.0, 0.0)) is False,
          "ApplyImpulse on an entity with no rigid body must return False")
    check(sc.SetLinearVelocity(e, Vec3(1.0, 0.0, 0.0)) is False,
          "SetLinearVelocity on an entity with no rigid body must return False")
    check(sc.GetLinearVelocity(e) is None,
          "GetLinearVelocity on an entity with no rigid body must return None")

    # Returning the right falsy VALUE and not merely not-raising is the half
    # that is easy to get wrong: a binding that returned None for ApplyImpulse
    # would satisfy "did not raise" and be useless to the caller.
    check_eq(sc.ApplyImpulse(e, Vec3(0.0, 10.0, 0.0)), False,
             "ApplyImpulse must return exactly False, not None or 0")
    check_eq(sc.SetLinearVelocity(e, Vec3(0.0, 0.0, 0.0)), False,
             "SetLinearVelocity must return exactly False, not None or 0")

    # The scene must survive all of that, and the entity must be unharmed --
    # a guard that wrote through a null handle would corrupt the transform.
    check(sc.IsEntityValid(e), "the entity must still be valid afterwards")
    check_le(abs(e.GetPosition().y - 2.0), 0.001,
             "a refused impulse must not move the entity (y=%.4f)"
             % e.GetPosition().y)


def test_the_methods_report_false_before_any_world_exists():
    """No OnSimulationStart means no world, so there is no body to drive.

    Distinct from the previous test on purpose: the entity there HAS no
    component, this one HAS a component and a perfectly valid body id, but no
    world to drive it in. Both must fail closed, and the second failure mode is
    the one that would be a crash if the b3World_IsValid guard were missing.
    """
    sc = Scene()
    e = sc.CreateEntity("body")
    rb = RigidBody3DComponent()
    rb.Type = RigidBody3DType.Dynamic
    e.AddOrReplaceRigidBody3DComponent(rb)
    # NOTE: OnSimulationStart() is deliberately NOT called here.

    check_eq(sc.ApplyImpulse(e, Vec3(0.0, 10.0, 0.0)), False,
             "with no physics world, ApplyImpulse must return False")
    check_eq(sc.SetLinearVelocity(e, Vec3(0.0, 5.0, 0.0)), False,
             "with no physics world, SetLinearVelocity must return False")
    check_eq(sc.GetLinearVelocity(e), None,
             "with no physics world, GetLinearVelocity must return None")

    # And it must not have moved, because there is nothing to move it.
    check_le(abs(y(e)), 0.001, "a body with no world must not move (y=%.4f)" % y(e))


def test_get_linear_velocity_reports_the_value_the_engine_is_using():
    """The read must agree with the trajectory, or it is a decorative number.

    Setting a known velocity and then measuring free travel checks the getter
    against the solver rather than against the setter: a getter that echoed
    back whatever was last written would pass a round-trip test and still be
    wrong for a script reading velocity before it touches anything.
    """
    sc, e = body_scene(start=(0.0, 0.0, 0.0), gravity=1.0)

    check(sc.SetLinearVelocity(e, Vec3(0.0, 0.0, 0.0)), "set velocity must apply")
    check(sc.SetLinearVelocity(e, Vec3(4.0, 0.0, 0.0)),
          "a second SetLinearVelocity must apply")
    v = sc.GetLinearVelocity(e)
    check(v is not None, "a live body must report a velocity")
    print("  set vx=4.0, read back vx=%.3f" % v.x)
    check_ge(v.x, 3.5, "the read must reflect the velocity that was set (vx=%.4f)"
             % v.x)

    # Over the next tenth of a second gravity adds about -0.98 m/s to y while
    # x is untouched, so vx must still be close to what was set. This is what
    # a setter-then-read cannot fake: the engine has been stepping in between.
    run(sc, 0.1)
    v2 = sc.GetLinearVelocity(e)
    check(v2 is not None, "velocity must still be readable after stepping")
    check_le(abs(v2.x - 4.0), 0.5,
             "a free-flying body keeps its horizontal velocity (vx=%.4f)"
             % v2.x)
    check(v2.y < -0.3,
          "gravity must be acting on y while the body flies (vy=%.4f); a flat "
          "zero here means the getter is not reading the live solver"
          % v2.y)