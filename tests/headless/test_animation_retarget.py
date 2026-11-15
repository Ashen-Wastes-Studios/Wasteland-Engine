"""Retargeting -- a clip authored on one rig played on a different rig (Task A.7).

The plan's scope is deliberately narrow: a bone-NAME mapping plus a proportional
scale. No motion extraction, no IK. So the assertions here are about identity,
not about quality:

  * a track is re-keyed onto the bone with the same NAME, not the same index --
    which is the whole point, since two rigs order their bones differently;
  * a target bone with no source counterpart gets NO track, and therefore holds
    its rest transform -- AnimationClip::Sample seeds every bone from the rest
    pose and skips out-of-range indices, so "no track" is exactly "at rest";
  * a source bone with no counterpart is dropped rather than guessed at;
  * translations are scaled, rotations are not (there is no meaningful 1.3x
    rotation);
  * the mapping survives a save/load round trip, and a malformed one is an ERROR
    rather than a silent drop -- the format's standing rule.

The numeric checks sample a real clip through a real Skeleton, so a retarget that
silently re-keyed to the wrong bone would fail here rather than looking right in
an editor screenshot.
"""

import math
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
    (x,y,z,w) to glm's (w,x,y,z). Only FromAxisAngle is unambiguous.
    """
    return Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0)


def build_skeleton(names, parents, translations):
    """A skeleton with an explicit rest pose.

    `translations` is parallel to `names`: the rest TRANSLATION of each bone, which
    is also what HeightRatio measures. The hierarchy comes from `parents` (a name
    per bone, empty for a root), so the rig is built the way a DCC export would
    describe it rather than by index.
    """
    skel = Animation.Skeleton()
    ok, error = skel.BuildFromNames(names, parents)
    if not ok:
        raise RuntimeError("BuildFromNames failed: " + error)

    pose = Animation.Pose()
    pose.Resize(len(names))
    for i, translation in enumerate(translations):
        pose.SetBone(i, Animation.BoneTransform(translation, identity(), Vec3(1.0, 1.0, 1.0)))
    skel.SetRestPose(pose)
    return skel


def bone_index(skel, name):
    idx = skel.FindBone(name)
    if idx is None:
        raise RuntimeError("no bone named " + name)
    return int(idx)


def build_source_clip():
    """A 2-bone rig's walk cycle: Root strides forward, Arm swings."""
    clip = Animation.AnimationClip("Walk")
    clip.SetLooping(True)
    clip.SetPlayRate(1.0)

    root = bone_index(SOURCE, "Root")
    arm = bone_index(SOURCE, "Arm")

    # Root translates forward 1 m over the cycle.
    clip.AddPositionKey(root, 0.0, Vec3(0.0, 0.0, 0.0))
    clip.AddPositionKey(root, 1.0, Vec3(0.0, 0.0, 1.0))

    # Arm hangs at its rest offset and swings about Z.
    clip.AddPositionKey(arm, 0.0, Vec3(1.0, 0.0, 0.0))
    clip.AddPositionKey(arm, 1.0, Vec3(1.0, 0.0, 0.0))
    swing = Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), math.radians(45.0))
    clip.AddRotationKey(arm, 0.0, identity())
    clip.AddRotationKey(arm, 1.0, swing)
    return clip


# --- the two rigs -----------------------------------------------------------
#
# Different bone COUNTS and different orders, which is the case retargeting
# exists for. The target's chain is twice as long in rest translation, so the
# proportional scale is 2.0.
#
#   source:  Root(0,0,0) -> Arm(1,0,0)                       chain length 1.0
#   target:  Root(0,0,0) -> Spine(0,2,0) -> Arm(0,4,0)       chain length 4.0
#
# The target's "Arm" is at index 2 while the source's is at index 1, so a retarget
# that copied tracks by INDEX would put the swing on the target's Spine.

SOURCE = build_skeleton(
    ["Root", "Arm"],
    ["", "Root"],
    [Vec3(0.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)],
)

TARGET = build_skeleton(
    ["Root", "Spine", "Arm"],
    ["", "Root", "Spine"],
    [Vec3(0.0, 0.0, 0.0), Vec3(0.0, 2.0, 0.0), Vec3(0.0, 4.0, 0.0)],
)


def test_build_map():
    mapping = Animation.AnimationRetarget.BuildMap(SOURCE, TARGET)
    pairs = {m.SourceBone: m.TargetBone for m in mapping}

    check(len(mapping) == 2, "BuildMap resolves both source bones (got %d)" % len(mapping))
    check(pairs.get("Root") == "Root", "Root maps to Root")
    check(pairs.get("Arm") == "Arm", "Arm maps to Arm by NAME, not by index")
    check("Spine" not in pairs.values(), "the target's extra bone is not a mapping target")


def test_normalise():
    check(Animation.AnimationRetarget.NormaliseBoneName("Spine_01") == "spine01",
          "NormaliseBoneName strips separators and lowercases")
    check(Animation.AnimationRetarget.NormaliseBoneName("Spine01") == "spine01",
          "NormaliseBoneName('Spine01') agrees")
    check(Animation.AnimationRetarget.NormaliseBoneName("") == "",
          "NormaliseBoneName of an empty name is empty, not a crash")


def test_normalised_match():
    """A rig whose bones are written with different separators still maps.

    Normalisation strips non-alphanumerics and lowercases, so "Root_01" and
    "root01" are the same bone. It does NOT make "Arm" match "arm_l" -- those are
    different bones, and a normaliser that guessed would be the exact silent
    mis-mapping this feature exists to prevent.
    """
    separator_rig = build_skeleton(
        ["Root_01", "Arm_L"],
        ["", "Root_01"],
        [Vec3(0.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)],
    )
    alt = build_skeleton(
        ["root01", "Arm01"],
        ["", "root01"],
        [Vec3(0.0, 0.0, 0.0), Vec3(0.0, 2.0, 0.0)],
    )
    mapping = Animation.AnimationRetarget.BuildMap(separator_rig, alt)
    pairs = {m.SourceBone: m.TargetBone for m in mapping}
    check(pairs.get("Root_01") == "root01",
          "normalised match Root_01 -> root01 (got %r)" % pairs.get("Root_01"))

    # And the negative case: a merely similar name must NOT match.
    check(pairs.get("Arm_L") != "Arm01",
          "Arm_L does not silently match the differently-named Arm01")


def test_override_beats_automatic():
    """An explicit pairing wins over an automatic one."""
    overrides = [Animation.RetargetMapping()]
    overrides[0].SourceBone = "Arm"
    overrides[0].TargetBone = "Spine"

    mapping = Animation.AnimationRetarget.BuildMap(SOURCE, TARGET, overrides)
    pairs = {m.SourceBone: m.TargetBone for m in mapping}
    check(pairs.get("Arm") == "Spine", "an override redirects Arm onto Spine")
    check(pairs.get("Root") == "Root", "unmapped bones still match automatically")


def test_height_ratio():
    ratio = Animation.AnimationRetarget.HeightRatio(SOURCE, TARGET)
    check(abs(ratio - 4.0) < 1e-4,
          "HeightRatio is target/source chain length (expected 4.0, got %.4f)" % ratio)

    same = Animation.AnimationRetarget.HeightRatio(SOURCE, SOURCE)
    check(abs(same - 1.0) < 1e-6, "a rig against itself has ratio 1.0")

    # A rig with no rest translations has no measurable height; dividing by zero
    # would be worse than reporting "no scale".
    flat = build_skeleton(["A", "B"], ["", "A"],
                          [Vec3(0.0, 0.0, 0.0), Vec3(0.0, 0.0, 0.0)])
    check(Animation.AnimationRetarget.HeightRatio(flat, TARGET) == 1.0,
          "a skeleton with zero rest translations yields 1.0, not inf or nan")


def test_retarget_rekeys_by_name():
    """The load-bearing assertion: the swing lands on Arm, not on index 1 (Spine)."""
    clip = build_source_clip()
    retargeted = Animation.AnimationRetarget.RetargetClip(
        clip, SOURCE, TARGET, [], 2.0)

    check(retargeted.HasTrack(bone_index(TARGET, "Root")), "Root keeps a track")
    check(retargeted.HasTrack(bone_index(TARGET, "Arm")),
          "Arm keeps a track -- re-keyed by name, not by index")
    check(not retargeted.HasTrack(bone_index(TARGET, "Spine")),
          "the target's extra bone gets NO track")

    check(retargeted.GetName() == "Walk", "the retargeted clip keeps its name")
    check(retargeted.IsLooping(), "looping survives a retarget")
    check(abs(retargeted.GetPlayRate() - 1.0) < 1e-6, "play rate survives a retarget")


def test_extra_bone_holds_rest():
    """Sampling the retargeted clip leaves the unmapped bone at its rest transform.

    This is the plan's verification, asserted numerically rather than visually.
    """
    clip = build_source_clip()
    retargeted = Animation.AnimationRetarget.RetargetClip(
        clip, SOURCE, TARGET, [], 2.0)

    pose = retargeted.Sample(TARGET, 1.0)
    spine = bone_index(TARGET, "Spine")

    bone = pose.GetBone(spine)
    check(bone is not None, "the extra bone is present in the sampled pose")
    if bone is not None:
        t = bone.Translation
        check(abs(t.x - 0.0) < 1e-5 and abs(t.y - 2.0) < 1e-5 and abs(t.z - 0.0) < 1e-5,
              "the extra bone holds its rest translation (got %.3f, %.3f, %.3f)"
              % (t.x, t.y, t.z))
        rot = bone.Rotation
        check(abs(abs(rot.w) - 1.0) < 1e-5 and abs(rot.x) < 1e-5 and
              abs(rot.y) < 1e-5 and abs(rot.z) < 1e-5,
              "the extra bone holds its rest (identity) rotation "
              "(got x=%.4f y=%.4f z=%.4f w=%.4f)"
              % (rot.x, rot.y, rot.z, rot.w))


def test_translation_is_scaled_rotation_is_not():
    clip = build_source_clip()

    # Scale 1.0 first, to isolate the scale effect from the re-keying.
    plain = Animation.AnimationRetarget.RetargetClip(clip, SOURCE, TARGET, [], 1.0)
    pose_plain = plain.Sample(TARGET, 1.0)
    root = bone_index(TARGET, "Root")
    t_plain = pose_plain.GetBone(root).Translation

    check(abs(t_plain.z - 1.0) < 1e-5,
          "a scale of 1.0 reproduces the source's 1 m stride (got %.4f)" % t_plain.z)

    doubled = Animation.AnimationRetarget.RetargetClip(clip, SOURCE, TARGET, [], 2.0)
    pose_doubled = doubled.Sample(TARGET, 1.0)
    t_doubled = pose_doubled.GetBone(root).Translation
    check(abs(t_doubled.z - 2.0) < 1e-4,
          "a scale of 2.0 doubles the stride (got %.4f)" % t_doubled.z)

    # Rotation must NOT be scaled -- a 45-degree swing stays 45 degrees. A
    # quaternion and its negation are the same rotation, so compare |components|.
    arm = bone_index(TARGET, "Arm")
    swing = pose_doubled.GetBone(arm).Rotation
    s = math.sin(math.radians(22.5))
    c = math.cos(math.radians(22.5))
    check(abs(abs(swing.z) - s) < 1e-4 and abs(abs(swing.w) - c) < 1e-4,
          "rotation is not scaled (|z|=%.4f |w|=%.4f, expected %.4f/%.4f)"
          % (abs(swing.z), abs(swing.w), s, c))
    check(abs(swing.x) < 1e-5 and abs(swing.y) < 1e-5,
          "the swing is still purely about Z")


def test_unmatched_source_bone_is_dropped():
    """A source bone with no counterpart is dropped, not guessed at.

    If it were re-keyed by index, the target's Spine would inherit the source's
    third bone motion -- the exact failure the name mapping exists to prevent.
    """
    three = build_skeleton(
        ["Root", "Arm", "Tail"],
        ["", "Root", "Arm"],
        [Vec3(0.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0), Vec3(1.0, 1.0, 0.0)],
    )
    clip = Animation.AnimationClip("Three")
    tail = bone_index(three, "Tail")
    clip.AddPositionKey(tail, 0.0, Vec3(1.0, 1.0, 0.0))
    clip.AddPositionKey(tail, 1.0, Vec3(1.0, 5.0, 0.0))

    retargeted = Animation.AnimationRetarget.RetargetClip(clip, three, TARGET, [], 1.0)

    # Only Tail had a track, and Tail has no counterpart on the target -- so the
    # retargeted clip is empty. Re-keying by INDEX would have put Tail's motion on
    # the target's index 2, which is "Arm"; the name mapping drops it instead.
    check(retargeted.IsEmpty(),
          "an unmatched source bone contributes nothing (clip is empty)")

    # Sampling the empty clip must leave every target bone at rest, including the
    # two bones an index-based retarget would have written to.
    pose = retargeted.Sample(TARGET, 1.0)
    for name, expected_y in (("Root", 0.0), ("Spine", 2.0), ("Arm", 4.0)):
        bone = pose.GetBone(bone_index(TARGET, name))
        t = bone.Translation
        check(abs(t.y - expected_y) < 1e-5,
              "%s holds its rest Y=%.1f after an empty retarget (got %.3f)"
              % (name, expected_y, t.y))


def test_empty_clip_is_not_a_failure():
    """A clip with no tracks retargets to an empty clip, not an error."""
    empty = Animation.AnimationClip("Empty")
    retargeted = Animation.AnimationRetarget.RetargetClip(empty, SOURCE, TARGET, [], 1.0)
    check(retargeted.GetName() == "Empty", "an empty clip keeps its name")
    check(retargeted.IsEmpty(), "an empty clip retargets to an empty clip")


def test_asset_accessors():
    """The Python write path for the mapping.

    `asset.Retarget` is a def_readwrite on a std::vector, which hands Python a
    COPY -- `asset.Retarget[0].TargetBone = "x"` silently no-ops. These accessors
    are the real path, and the test exists largely to prove that.
    """
    asset = Animation.AnimationAsset()
    check(asset.GetRetargetCount() == 0, "a fresh asset has no retarget mapping")
    check(asset.RetargetPositionScale == 1.0, "a fresh asset has scale 1.0")

    asset.AddRetargetMapping("Root", "Root")
    asset.AddRetargetMapping("Arm", "Arm")
    check(asset.GetRetargetCount() == 2, "AddRetargetMapping appends")
    check(asset.GetRetargetSourceBone(0) == "Root", "GetRetargetSourceBone(0)")
    check(asset.GetRetargetTargetBone(1) == "Arm", "GetRetargetTargetBone(1)")

    check(asset.GetRetargetSourceBone(99) == "", "an out-of-range index reads as empty")
    check(not asset.RemoveRetargetMapping(99), "an out-of-range remove is refused")
    check(asset.RemoveRetargetMapping(0), "a valid remove succeeds")
    check(asset.GetRetargetCount() == 1, "the remove actually removed one")
    check(asset.GetRetargetSourceBone(0) == "Arm", "the remaining mapping shifted down")

    asset.RetargetPositionScale = 2.5
    check(asset.RetargetPositionScale == 2.5, "the scale is a plain readwrite float")

    asset.ClearRetarget()
    check(asset.GetRetargetCount() == 0, "ClearRetarget empties the mapping")
    check(asset.RetargetPositionScale == 1.0, "ClearRetarget resets the scale to 1.0")


def test_serialiser_round_trip():
    """The `retarget:` section survives a save/load, and is emitted only when set."""
    asset = Animation.AnimationAsset()
    asset.Name = "Retargeted"
    asset.AddRetargetMapping("Arm", "Arm")
    asset.AddRetargetMapping("Root", "Root")
    asset.RetargetPositionScale = 2.0

    text = Animation.EmitAnimation(asset)

    check("retarget:" in text, "the retarget section is emitted when the asset has a mapping")
    check("position_scale: 2" in text, "the scale is emitted")
    # Sorted by source name, so the emission is stable across runs.
    check(text.index("from: Arm") < text.index("from: Root"),
          "the mapping is emitted sorted by source name")

    path = os.path.join(os.environ.get("TMPDIR", "."), "retarget_roundtrip.wlanim")
    err = Animation.SaveAnimation(path, asset)
    check(err is None, "SaveAnimation succeeds")
    if err is not None:
        print("   ", err.Message)

    loaded = Animation.AnimationAsset()
    err = Animation.LoadAnimation(path, loaded)
    check(err is None, "LoadAnimation succeeds")
    if err is not None:
        print("   ", err.Message)
        return

    check(loaded.GetRetargetCount() == 2, "the mapping round-trips (got %d)"
          % loaded.GetRetargetCount())
    check(loaded.RetargetPositionScale == 2.0, "the scale round-trips")
    check(loaded.GetRetargetSourceBone(0) == "Arm" and
          loaded.GetRetargetTargetBone(0) == "Arm", "the first mapping round-trips")
    check(loaded.GetRetargetSourceBone(1) == "Root" and
          loaded.GetRetargetTargetBone(1) == "Root", "the second mapping round-trips")


def test_no_retarget_section_is_byte_stable():
    """An asset with no mapping must not grow a `retarget:` section.

    Same rule as the `editor:` section: a file that has never been retargeted is
    byte-for-byte what it was before the section existed.
    """
    asset = Animation.AnimationAsset()
    asset.Name = "Plain"
    text = Animation.EmitAnimation(asset)
    check("retarget" not in text, "no retarget section is emitted for a plain asset")
    check("editor" not in text, "no editor section is emitted for a plain asset")


def test_malformed_retarget_is_an_error():
    """A bad mapping is an error, not a silent drop -- the format's standing rule."""
    good = Animation.EmitAnimation(_asset_with_mapping())

    # A mapping missing its 'to' field.
    missing_to = good.replace("to: Arm\n", "")
    scratch = Animation.AnimationAsset()
    err = Animation.ParseAnimation(missing_to, scratch)
    check(err is not None, "a mapping with no 'to' is an error")

    # An empty bone name.
    empty_name = good.replace("from: Arm", "from: \"\"")
    scratch = Animation.AnimationAsset()
    err = Animation.ParseAnimation(empty_name, scratch)
    check(err is not None, "a mapping with an empty source name is an error")

    # A retarget section that is not a mapping at all.
    not_a_map = good.replace("retarget:\n", "retarget: 3\n")
    scratch = Animation.AnimationAsset()
    err = Animation.ParseAnimation(not_a_map, scratch)
    check(err is not None, "a retarget section that is not a mapping is an error")

    # NOTE: a non-scalar `position_scale` is NOT reported -- yaml-cpp's
    # as<float>() throws out of Parse, which violates the format's stated
    # "reports instead of throwing" rule. That hazard is pre-existing and
    # file-wide (every other section's as<float> is unguarded too); it is out of
    # scope for retargeting and is recorded rather than papered over here.


def _asset_with_mapping():
    asset = Animation.AnimationAsset()
    asset.Name = "Mapped"
    asset.AddRetargetMapping("Arm", "Arm")
    asset.AddRetargetMapping("Root", "Root")
    asset.RetargetPositionScale = 2.0
    return asset


def __headless_teardown__():
    """Turn accumulated `check()` failures into a suite failure.

    The runner counts test FUNCTIONS, so a `check()` that merely prints would
    leave a red suite green. The runner does report teardown errors, which makes
    this the hook that keeps the aggregate honest. Every sibling suite in
    tests/headless/ has the same weakness; this one does not inherit it.
    """
    if FAILED:
        raise AssertionError("%d check(s) failed: %s"
                             % (len(FAILED), "; ".join(FAILED)))


def main():
    test_build_map()
    test_normalise()
    test_normalised_match()
    test_override_beats_automatic()
    test_height_ratio()
    test_retarget_rekeys_by_name()
    test_extra_bone_holds_rest()
    test_translation_is_scaled_rotation_is_not()
    test_unmatched_source_bone_is_dropped()
    test_empty_clip_is_not_a_failure()
    test_asset_accessors()
    test_serialiser_round_trip()
    test_no_retarget_section_is_byte_stable()
    test_malformed_retarget_is_an_error()

    print("%d/%d checks passed" % (PASSED, PASSED + len(FAILED)))
    if FAILED:
        print("FAILED checks:")
        for message in FAILED:
            print("  -", message)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
