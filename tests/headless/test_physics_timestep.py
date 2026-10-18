"""Frame-rate independence of Scene::StepPhysics.

The gap this suite exists to close: StepPhysics used to clamp dt to 1/30 and
DISCARD the excess. That is the right call for a single enormous dt, and the
wrong one for an ordinary slow frame -- clamping throws away real time, so the
simulation runs slower than the clock, and the slower the frame the worse it
gets.

The pre-existing 30fps test passed only because 30fps is exactly the clamp
boundary. Measured against the real module, 1.0s of free fall (ideal 4.905m):

    144fps -> 4.914m (100%)      30fps -> 4.946m (100%)
     20fps -> 2.207m  (45%)      10fps -> 0.559m  (11%)

So a machine struggling at 10fps ran the game in quarter-speed, with no error,
no log, and a green suite. Asserting only at 30fps is what let it survive.

This suite asserts real time advances at EVERY rate, including the ones the old
test never tried.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_ge, check_le

import wlpath

wlpath.setup()

from Wasteland import (Vec3, Scene, RigidBody3DComponent, BoxCollider3DComponent,
                       RigidBody3DType)

IDEAL = 0.5 * 9.81 * 1.0     # free fall from rest, g=9.81


def free_fall(fps, seconds=1.0, drop_y=10.0):
    """How far a Dynamic body actually falls in `seconds` at `fps`.

    Type=Dynamic AND a collider with density are both required: without them the
    body has no mass and never moves at ANY frame rate. That is a property of the
    engine, not of this suite, but getting it wrong yields a confident table of
    zeroes -- which is what happened the first two times this measurement was
    written.
    """
    sc = Scene()
    e = sc.CreateEntity("ball")
    e.SetPosition(Vec3(0.0, drop_y, 0.0))
    rb = RigidBody3DComponent()
    rb.Type = RigidBody3DType.Dynamic
    rb.GravityScale = 1.0
    e.AddOrReplaceRigidBody3DComponent(rb)
    c = BoxCollider3DComponent()
    c.HalfExtents = Vec3(0.5, 0.5, 0.5)
    c.Density = 1.0
    e.AddOrReplaceBoxCollider3DComponent(c)

    # OnSimulationStart is the only bound physics-start entry point; without it
    # there is no Box3D world and nothing moves.
    sc.OnSimulationStart()

    dt = 1.0 / fps
    for _ in range(int(round(seconds / dt))):
        sc.StepPhysics(dt)
    return drop_y - e.GetPosition().y


def test_a_body_advances_real_time_at_every_frame_rate():
    """StepPhysics used to clamp dt to 1/30 and DISCARD the excess.

    Measured against the pre-fix module, 1.0s of free fall (ideal 4.905m):

        144fps -> 4.914m (100%)      30fps -> 4.946m (100%)
         20fps -> 2.207m  (45%)      10fps -> 0.559m  (11%)

    So a machine struggling at 10fps ran the game in quarter speed, with no
    error, no log, and a green suite. The pre-existing 30fps test passed only
    because 30fps is exactly the clamp boundary -- which is the whole reason the
    defect survived. 20/15/10 are asserted here for that reason.
    """
    for fps in (144, 60, 30, 20, 15, 10):
        fell = free_fall(fps)
        pct = 100.0 * fell / IDEAL
        check_ge(pct, 95.0,
                 "%3dfps must advance real time (fell %.3fm = %.0f%% of ideal "
                 "%.3fm); under 95%% means the frame's time is being discarded"
                 % (fps, fell, pct, IDEAL))


def test_the_step_size_is_constant_so_trajectories_agree_across_rates():
    """Not merely "each rate moved something" -- the same second of simulated
    time must produce the SAME displacement, or the physics is still frame-rate
    dependent and the 95% threshold above is just loose enough to hide it."""
    distances = {fps: free_fall(fps) for fps in (144, 60, 30, 20, 15, 10)}
    spread = max(distances.values()) - min(distances.values())
    check_le(spread, IDEAL * 0.08,
             "all frame rates must land within %.2fm of each other; spread was "
             "%.3fm over %r" % (IDEAL * 0.08, spread,
                                {k: round(v, 3) for k, v in distances.items()}))


def test_a_long_stall_is_bounded_rather_than_replayed():
    """An unbounded accumulator would bank 10s and then run 600 sub-steps in one
    frame -- a worse stall than the one it was recovering from. The drop-backlog
    path is the tunnelling protection the original clamp provided."""
    sc = Scene()
    e = sc.CreateEntity("ball")
    e.SetPosition(Vec3(0.0, 100.0, 0.0))
    rb = RigidBody3DComponent()
    rb.Type = RigidBody3DType.Dynamic
    rb.GravityScale = 1.0
    e.AddOrReplaceRigidBody3DComponent(rb)
    c = BoxCollider3DComponent()
    c.HalfExtents = Vec3(0.5, 0.5, 0.5)
    c.Density = 1.0
    e.AddOrReplaceBoxCollider3DComponent(c)
    sc.OnSimulationStart()

    sc.StepPhysics(10.0)          # one enormous frame
    y = e.GetPosition().y
    check(y > 90.0,
          "a 10s stall must not integrate 10s of fall (y=%.2f); tunnelling "
          "protection is lost" % y)
