"""Keyframe editing -- the operations the Animation Timeline's drag and
right-click call (Task A.5).

The panel itself is ImGui and cannot run headlessly, but the edits it makes are
engine calls, and those CAN be executed and asserted here. A drag is
RotationTrack::MoveKey; a right-click on a key is RemoveKeyAtTime; a right-click
on a lane is AddKey. This suite drives all three against a real clip and checks
the values, not just the counts.

The tolerance is the point of half these checks. A drag lands on a float that is
never exactly the key's original time, so MoveKey must match within 1e-6 or it
refuses almost every real edit. The panel's own hit test uses the same epsilon,
and the two agreeing is what keeps a drag from being a no-op.
"""

import os
import sys

from Wasteland import Animation, Quat, Vec3

PASSED = 0
FAILED = []


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def identity():
    """The only safe 'no rotation' from Python.

    Quat() is all zeros and Quat(0,0,0,1) is NOT identity -- the binding forwards
    (x,y,z,w) to glm's (w,x,y,z), so the literal puts 1 in the x slot. Only
    FromAxisAngle is unambiguous.
    """
    return Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0)


def build_clip():
    clip = Animation.AnimationClip()
    # One bone, two rotation keys. A single bone is enough: the edit path under
    # test is per-track, and a second bone would only add rows to the panel.
    clip.AddRotationKey(0, 0.0, identity())
    clip.AddRotationKey(0, 2.0, identity())
    return clip


def test_rotation_move():
    clip = build_clip()
    track = clip.GetTrack(0)
    keys = track.Rotation.GetKeys()
    check(len(keys) == 2, "two rotation keys to start")
    if len(keys) != 2:
        return

    times = [k[0] for k in keys]
    check(times == [0.0, 2.0], "keys start at 0 and 2, got %s" % times)

    # Move the 2.0 key to 1.0.
    moved = track.Rotation.MoveKey(2.0, 1.0)
    check(moved, "MoveKey reports success")
    times = [k[0] for k in track.Rotation.GetKeys()]
    check(times == [0.0, 1.0], "keys are now 0 and 1, got %s" % times)
    check(len(track.Rotation.GetKeys()) == 2, "a move does not add a key")

    # Move the 0.0 key to 1.5 -- after the other key, so the list must re-sort.
    track.Rotation.MoveKey(0.0, 1.5)
    times = [k[0] for k in track.Rotation.GetKeys()]
    check(times == [1.0, 1.5], "a move past its neighbour re-sorts, got %s" % times)

    # A move from a time with no key must fail and change nothing.
    before = [k[0] for k in track.Rotation.GetKeys()]
    check(not track.Rotation.MoveKey(99.0, 5.0), "moving a missing key fails")
    after = [k[0] for k in track.Rotation.GetKeys()]
    check(before == after, "a failed move leaves the track untouched")

    # A no-op move must also fail: the panel relies on this to tell a click from
    # a drag, and treating a click as a move would rewrite the file for nothing.
    check(not track.Rotation.MoveKey(1.0, 1.0), "a zero-distance move fails")


def test_rotation_tolerance():
    clip = build_clip()
    track = clip.GetTrack(0)

    # The panel's drag computes a time from pixels, so it lands within a rounding
    # error of the key rather than on it exactly. 1e-9 is far below the 1e-6
    # tolerance and far above float noise on a value this small.
    moved = track.Rotation.MoveKey(2.0 + 1e-9, 0.5)
    check(moved, "a move from a time within 1e-6 succeeds")
    times = [k[0] for k in track.Rotation.GetKeys()]
    check(times == [0.0, 0.5], "the key moved to 0.5, got %s" % times)

    # Just outside the tolerance: a genuinely different time, which must NOT
    # match. Without this check the tolerance could be so wide that a drag onto a
    # neighbouring key moves the wrong one.
    clip = build_clip()
    track = clip.GetTrack(0)
    check(not track.Rotation.MoveKey(2.0 + 1e-3, 0.5),
          "a time 1e-3 away does not match a different key")


def test_rotation_remove():
    clip = build_clip()
    track = clip.GetTrack(0)

    removed = track.Rotation.RemoveKeyAtTime(0.0)
    check(removed == 1, "RemoveKeyAtTime removed exactly one key, got %d" % removed)
    times = [k[0] for k in track.Rotation.GetKeys()]
    check(times == [2.0], "only the 2.0 key remains, got %s" % times)

    # Removing an absent key returns 0, which the panel uses to decide whether to
    # report a change.
    check(track.Rotation.RemoveKeyAtTime(0.0) == 0, "removing an absent key returns 0")

    # Tolerance applies here too, matching AnimationCurve.
    check(track.Rotation.RemoveKeyAtTime(2.0 + 1e-9) == 1,
          "remove honours the same 1e-6 tolerance")
    check(track.Rotation.GetKeys() == [], "the track is now empty")


def test_curve_move():
    # AnimationCurve is scalar: BoneTrack::Position is three of these, one per
    # axis, rather than one vec3 curve. That is why the binding takes a float.
    curve = Animation.AnimationCurve()
    curve.AddKey(0.0, 0.0)
    curve.AddKey(1.0, 1.0)
    curve.AddKey(2.0, 2.0)

    check(curve.MoveKey(1.0, 3.0), "AnimationCurve.MoveKey succeeds")
    keys = curve.GetKeys()
    check([k[0] for k in keys] == [0.0, 2.0, 3.0],
          "the middle key moved to the end and the list re-sorted, got %s"
          % [k[0] for k in keys])

    # The VALUE must travel with the key: a move that kept the old value would
    # silently change the animation while appearing to succeed.
    moved_value = None
    for t, v in keys:
        if abs(t - 3.0) < 1e-6:
            moved_value = v
    check(moved_value is not None and abs(moved_value - 1.0) < 1e-6,
          "the value followed the key to its new time, got %s" % (moved_value,))

    # Evaluate must agree with the edited track: a move that left the cache stale
    # would evaluate the old curve.
    check(abs(curve.Evaluate(2.5) - 1.5) < 1e-5,
          "evaluation reflects the move, got %s" % curve.Evaluate(2.5))


def test_curve_remove():
    curve = Animation.AnimationCurve()
    curve.AddKey(0.0, 0.0)
    curve.AddKey(1.0, 1.0)

    check(curve.RemoveKeyAtTime(0.0) == 1, "curve remove returns 1")
    check([k[0] for k in curve.GetKeys()] == [1.0], "only the 1.0 key remains")


def test_add_key_value_travels():
    """The panel's add-key path samples the pose at the clicked time and stores
    that value. This asserts the value is the one sampled, not a default."""
    clip = build_clip()
    track = clip.GetTrack(0)

    # A 90 degree turn about Z, stored at t=1.0.
    turn = Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 1.5707963267948966)
    track.Rotation.AddKey(1.0, turn)

    at_one = [k for k in track.Rotation.GetKeys() if abs(k[0] - 1.0) < 1e-6]
    check(len(at_one) == 1, "a key exists at 1.0")
    if not at_one:
        return

    value = at_one[0][1]
    # Compared by components rather than by rotating a vector: Quat has no Rotate
    # binding, and a quaternion and its negation are the same rotation anyway, so
    # an equality check on the raw fields would be wrong on half the values.
    #
    # 90 degrees about +Z is (x=0, y=0, z=sin45, w=cos45).
    s45 = 0.7071067811865476
    check(abs(value.x) < 1e-5 and abs(value.y) < 1e-5,
          "the turn is about Z, so x and y are zero, got (%s, %s)"
          % (value.x, value.y))
    check(abs(abs(value.z) - s45) < 1e-5 and abs(abs(value.w) - s45) < 1e-5,
          "the stored quaternion is the 90 degree turn, got (z=%s, w=%s)"
          % (value.z, value.w))


def main():
    test_rotation_move()
    test_rotation_tolerance()
    test_rotation_remove()
    test_curve_move()
    test_curve_remove()
    test_add_key_value_travels()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
