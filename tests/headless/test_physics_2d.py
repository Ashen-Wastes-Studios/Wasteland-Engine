"""Physics 2D: does a body added AFTER startup actually simulate?

The route-map item 5.1 claim was that 2D had no equivalent of the 3D
``SyncPendingPhysicsBodies`` pass -- so ``Rigidbody2DComponent::RuntimeBody``
was assigned in exactly one place in the whole engine (``OnPhysics2DStart``),
and a 2D rigidbody added after startup got no Box2D body ever. It would hang
in mid-air: never falling, never colliding, never blocking anything, with no
error anywhere.

That claim is now ``Scene::SyncPendingPhysics2D`` / ``Scene::CreatePhysicsBody2D``
in Scene.cpp, and this suite is the reproducer for it, written from the
components' own contracts rather than from the claim.

Cases, deliberately ordered so a green suite cannot come from a body that
never existed at all:

* ``test_the_control_case_still_simulates`` -- a body added BEFORE
  ``OnSimulationStart()`` must fall. Proves the harness, the world and the
  solver work in 2D, so a red late-spawn case cannot be blamed on the setup.
* ``test_a_body_with_no_collider_does_not_move`` -- a body with NO collider gets
  a Box2D body (RuntimeBody becomes non-null) but zero mass, so it must NOT
  move. This separates "no body was created" from "a body was created with no
  shape", which are different bugs with the same silent symptom.
* ``test_a_body_added_after_startup_still_simulates`` -- THE 5.1 CASE. Falls.
* ``test_a_late_body_rests_on_a_late_floor`` -- a late STATIC floor plus a late
  dynamic box: the box must stop on it. Covers the third symptom in the claim,
  "never blocks anything", which falling alone would not catch.
* ``test_the_sync_pass_is_idempotent`` -- a startup body must keep the SAME
  Box2D body pointer across steps. Guards the ``RuntimeBody != nullptr``
  early-return in CreatePhysicsBody2D: without it the per-step pass would
  create a second body every sub-step and orphan the first.
* ``test_a_30fps_caller_advances_real_time`` -- the fixed-timestep accumulator
  works for 2D too.

Every assertion reads back a position or a handle. A suite that only proves
nothing crashed cannot tell a working solver from one that never ran.

NO 2D EQUIVALENT OF ``test_zero_gravity_holds_a_body_up``: 3D's
``RigidBody3DComponent`` has a ``GravityScale`` and 2D's
``Rigidbody2DComponent`` does not (Components.h -- it carries only ``Type`` and
``FixedRotation``). There is no way to hold a 2D body up from script, so there
is nothing to assert.

Run:  python tests/headless/runner.py test_physics_2d
"""

from __future__ import annotations

import ctypes
import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    BoxCollider2DComponent,
    CircleCollider2DComponent,
    Entity,
    Rigidbody2DBodyType,
    Rigidbody2DComponent,
    Scene,
    Vec2,
    Vec3,
)
from runner import check, check_ge, check_le  # noqa: E402

STEP = 1.0 / 60.0
TOL = 0.30

# The 2D component accessors are bound through BIND_COMP_*, which stringifies
# the FULLY QUALIFIED C++ type -- so the Python-visible names are literally
# "AddOrReplaceWasteland::Rigidbody2DComponent". That is not a valid identifier
# and cannot be called with dot syntax. BIND_GAMEPLAY_COMP takes a bare name and
# would produce the usable AddOrReplaceRigidbody2DComponent, but it is applied
# to the 3D physics set and NOT to the 2D set (Bindings.cpp). So the 2D
# components are reachable only through getattr.
#
# Resolved by a helper rather than at import on purpose. A module-level getattr
# that raises kills the import, and this runner has no per-module isolation --
# one suite raising at import takes the whole run and loses the aggregate line.
# Resolving inside the helpers turns a rename into ordinary test failures.


def _accessor(mangled):
    """The named Entity method, or a function that raises a clear message."""
    fn = getattr(Entity, mangled, None)
    if fn is not None:
        return fn

    def _missing(*_args, **_kwargs):
        raise AssertionError(
            "Entity has no %r -- the 2D component accessors come from "
            "BIND_COMP_*, which stringifies the qualified C++ type. If these "
            "were renamed, this suite cannot reach Rigidbody2DComponent at "
            "all." % mangled
        )

    return _missing


_add_rb2d = _accessor("AddOrReplaceWasteland::Rigidbody2DComponent")
_add_box2d = _accessor("AddOrReplaceWasteland::BoxCollider2DComponent")
_add_circ2d = _accessor("AddOrReplaceWasteland::CircleCollider2DComponent")
_rb2d_of = _accessor("GetWasteland::Rigidbody2DComponent")


def _y(entity):
    return entity.GetPosition().y


def _runtime_body(entity):
    """The live b2Body* the engine stored, as the Python handle it becomes.

    A null void* reads back as None; a real one as a pybind capsule. Use
    _is_body() to test presence and _body_ptr() to compare two of them --
    `is` on the capsule compares PYTHON OBJECT identity, and pybind builds a
    fresh capsule per call, so it reports False for one unchanged pointer.
    """
    return _rb2d_of(entity).RuntimeBody


# Resolving a capsule back to the address it wraps. pybind11 stores a void* as
# a PyCapsule with a NULL name, so the name argument is None. Without this a
# test cannot compare handles at all, and "the same body" is not checkable.
try:
    _capsule_ptr = ctypes.pythonapi.PyCapsule_GetPointer
    _capsule_ptr.argtypes = [ctypes.py_object, ctypes.c_char_p]
    _capsule_ptr.restype = ctypes.c_void_p
except AttributeError:  # pragma: no cover - non-CPython
    _capsule_ptr = None


def _body_ptr(handle):
    """The address inside a RuntimeBody capsule, or None for a null handle.

    Comparing addresses rather than capsule objects is what makes the
    idempotency check meaningful.

    LIMITATION, and the reason this is a regression guard rather than a bug
    reproducer: a freed b2Body's address can be handed back to the next
    allocation, so a rebuild at the same address would read as "unchanged".
    Nothing here destroys a body, so the old allocation is never recycled
    within a test -- but it is a real limit of pointer comparison, not a
    property of the engine.
    """
    if handle is None:
        return None
    if _capsule_ptr is None:
        raise AssertionError("PyCapsule_GetPointer unavailable; cannot compare "
                             "2D body handles on this interpreter")
    return _capsule_ptr(handle, None)


def _is_body(entity):
    """Whether the engine has created a Box2D body for this entity yet."""
    return _body_ptr(_runtime_body(entity)) is not None


def _box(entity, half_w=0.5, half_h=0.5, density=1.0):
    c = BoxCollider2DComponent()
    # Size is passed to SetAsBox as HALF-extents despite the name -- the
    # documented misname in Components.h. So 0.5 here is a 1x1 box.
    c.Size = Vec2(half_w, half_h)
    c.Density = density
    _add_box2d(entity, c)


def _circle(entity, radius=0.5, density=1.0):
    c = CircleCollider2DComponent()
    c.Radius = radius
    c.Density = density
    _add_circ2d(entity, c)


def _rigidbody(entity, body_type):
    rb = Rigidbody2DComponent()
    rb.Type = body_type
    _add_rb2d(entity, rb)


def run(sc, seconds, step=STEP):
    for _ in range(int(seconds / step)):
        sc.StepPhysics(step)


def two_body_scene(shape, drop_y=4.0, floor_top=0.0):
    """A static box floor plus a dynamic dropper, both added BEFORE start.

    Mirrors test_physics_3d.py's two_body_scene exactly, so the two suites are
    comparable shape-for-shape and a difference between them is a difference
    in the engine rather than in the harness.
    """
    sc = Scene()

    floor = sc.CreateEntity("floor")
    floor.SetPosition(Vec3(0.0, floor_top - 0.5, 0.0))
    _rigidbody(floor, Rigidbody2DBodyType.Static)
    _box(floor, 20.0, 0.5, density=0.0)

    ball = sc.CreateEntity("ball")
    ball.SetPosition(Vec3(0.0, drop_y, 0.0))
    _rigidbody(ball, Rigidbody2DBodyType.Dynamic)
    if shape == "circle":
        _circle(ball, 0.5, 1.0)
    else:
        _box(ball, 0.5, 0.5, 1.0)

    # OnPhysics2DStart is NOT bound to Python. OnSimulationStart() is, and it
    # calls both physics starts -- without it there is no Box2D world, no
    # solver, and every body sits at its authored position forever.
    sc.OnSimulationStart()
    return sc, ball


SHAPES = ("box", "circle")


def test_the_control_case_still_simulates():
    """A body present BEFORE OnSimulationStart must fall. The setup works."""
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=6.0)
        y0 = _y(ball)
        run(sc, 0.5)
        y1 = _y(ball)
        print("  %-7s y %.3f -> %.3f" % (shape, y0, y1))
        check(y1 < y0,
              "the control case must fall, or the suite proves nothing "
              "(%s after 0.5s y=%.4f)" % (shape, y1))


def test_a_body_with_no_collider_does_not_move():
    """No collider means no fixture, so no mass, so no motion -- but a BODY.

    This is the negative control for the whole suite. Without it, "the body did
    not move" is ambiguous between the bug (no body was ever created) and
    correct Box2D behaviour (a body with no shape has zero mass). The two
    assertions together separate them: RuntimeBody must be non-null, and the
    entity must not have moved.
    """
    for shape in ("none",):
        sc = Scene()
        sc.OnSimulationStart()
        e = sc.CreateEntity("shapeless")
        e.SetPosition(Vec3(0.0, 5.0, 0.0))
        _rigidbody(e, Rigidbody2DBodyType.Dynamic)
        # Deliberately no collider, whichever shape the loop would otherwise use.

        run(sc, 0.2)
        check(_is_body(e),
              "a dynamic 2D body with no collider must still GET a Box2D body")
        y0 = _y(e)
        run(sc, 0.5)
        y1 = _y(e)
        print("  %-7s y %.4f -> %.4f (no collider)" % (shape, y0, y1))
        check_le(abs(y1 - y0), 0.001,
                 "a fixtureless body has zero mass and must not move "
                 "(%.4f -> %.4f)" % (y0, y1))


def test_a_body_added_after_startup_still_simulates():
    """THE 5.1 CASE. A body added after OnSimulationStart must still fall.

    Before SyncPendingPhysics2D existed, OnPhysics2DStart was the only place a
    b2Body was ever created, so RuntimeBody stayed null forever and the entity
    sat at its authored position with nothing logged.
    """
    for shape in SHAPES:
        sc = Scene()
        sc.OnSimulationStart()  # world first, so the body really is late
        late = sc.CreateEntity("late")
        late.SetPosition(Vec3(0.0, 5.0, 0.0))
        _rigidbody(late, Rigidbody2DBodyType.Dynamic)
        if shape == "circle":
            _circle(late, 0.5, 1.0)
        else:
            _box(late, 0.5, 0.5, 1.0)

        # One step before anything is asserted: the body must exist by then,
        # or the fall below could be reading a transform that merely happens
        # to move.
        run(sc, STEP)
        print("  %-7s b2Body after 1 step: 0x%x"
              % (shape, _body_ptr(_runtime_body(late)) or 0))
        check(_is_body(late),
              "a %s added after startup must get a Box2D body within one step"
              % shape)

        y0 = _y(late)
        run(sc, 0.5)
        y1 = _y(late)
        print("  %-7s y %.3f -> %.3f" % (shape, y0, y1))
        check(y1 < y0,
              "a %s added after startup must still fall (y=%.4f)" % (shape, y1))
        # A real fall, not a nudge: 0.5s of 9.8 m/s^2 is ~1.2m. Anything under
        # a tenth of that means the solver barely ran.
        check(y0 - y1 > 0.1,
              "a %s added after startup must fall a real distance, not a "
              "hair's breadth (fell %.4f in 0.5s)" % (shape, y0 - y1))


def test_a_late_body_rests_on_a_late_floor():
    """A late static floor must STOP a late dynamic body. Covers "never blocks".

    Falling proves the body was simulated; landing proves its FIXTURE was
    created too. A body created without its collider falls forever, so this
    case fails if only the rigidbody half of the sync landed.
    """
    for shape in SHAPES:
        sc = Scene()
        sc.OnSimulationStart()

        floor = sc.CreateEntity("late_floor")
        floor.SetPosition(Vec3(0.0, -0.5, 0.0))
        _rigidbody(floor, Rigidbody2DBodyType.Static)
        _box(floor, 20.0, 0.5, density=0.0)

        ball = sc.CreateEntity("late_ball")
        ball.SetPosition(Vec3(0.0, 4.0, 0.0))
        _rigidbody(ball, Rigidbody2DBodyType.Dynamic)
        if shape == "circle":
            _circle(ball, 0.5, 1.0)
        else:
            _box(ball, 0.5, 0.5, 1.0)

        run(sc, 4.0)
        y = _y(ball)
        print("  %-7s resting y = %.4f  (late floor top 0 + half-height 0.5)"
              % (shape, y))
        check(y > 0.0,
              "a %s must not sink through a LATE static floor (y=%.4f)"
              % (shape, y))
        check_le(abs(y - 0.5), TOL,
                 "a %s should come to rest at ~0.5 on a late floor, got %.4f"
                 % (shape, y))

        # Still moving? A body that passed the reading above only by luck
        # while sinking still fails here.
        run(sc, 2.0)
        y_after = _y(ball)
        print("  %-7s 2s later    = %.4f" % (shape, y_after))
        check_le(abs(y_after - y), 0.05,
                 "a resting %s must not keep moving (%.4f -> %.4f)"
                 % (shape, y, y_after))


def test_the_sync_pass_is_idempotent():
    """A startup body must keep the SAME Box2D body across steps.

    CreatePhysicsBody2D early-returns on a non-null RuntimeBody. That guard is
    load-bearing: the sync pass runs every sub-step, so without it each entity
    would get a fresh body each time, leaking the previous one and orphaning
    its fixtures -- and the position would jump backwards each step as the
    fresh body was rebuilt at the current transform. Identity of the stored
    handle is the only thing that distinguishes "synced" from "rebuilt 60
    times a second", and nothing else in the public surface exposes it.
    """
    sc, ball = two_body_scene("box", drop_y=5.0)

    body0 = _body_ptr(_runtime_body(ball))
    check(body0 is not None,
          "a body present at OnSimulationStart must have a b2Body immediately")
    run(sc, 0.25)
    body1 = _body_ptr(_runtime_body(ball))
    print("  body stable across 15 steps: 0x%x -> 0x%x" % (body0, body1))
    check(body0 == body1,
          "the per-step sync pass must not rebuild an existing 2D body "
          "(0x%x -> 0x%x)" % (body0 or 0, body1 or 0))

    # Two steps in ONE frame, which is where a per-sub-step rebuild would show
    # up twice and where the accumulator path differs from repeated frames.
    run(sc, 1.0 / 30.0)
    body2 = _body_ptr(_runtime_body(ball))
    check(body1 == body2,
          "two sub-steps in one frame must still reuse the same 2D body "
          "(0x%x -> 0x%x)" % (body1 or 0, body2 or 0))


def test_a_30fps_caller_advances_real_time():
    """The fixed-timestep accumulator drives 2D as well as 3D."""
    for shape in SHAPES:
        sc, ball = two_body_scene(shape, drop_y=10.0, floor_top=-40.0)
        y0 = _y(ball)
        # 30 frames at 30fps == 1.0s of real time. 2D gravity is -9.8, so
        # free fall is ~4.9m.
        run(sc, 1.0, step=1.0 / 30.0)
        fell = y0 - _y(ball)
        print("  %-7s fell %.3f m in 1.0s of 30fps frames (ideal ~4.9)"
              % (shape, fell))
        check_le(abs(fell - 4.9), 0.8,
                 "30fps must advance real time for a %s (fell %.4f, ideal ~4.9)"
                 % (shape, fell))
        # Lower bound, so a solver that is only incidentally close cannot pass
        # by having barely moved at all. check_le alone would accept a fall of
        # ~4.1m AND a fall of 0.05m.
        check_ge(fell, 4.1,
                 "a %s dropped for 1.0s of real time must fall at least 4.1m, "
                 "got %.4f" % (shape, fell))