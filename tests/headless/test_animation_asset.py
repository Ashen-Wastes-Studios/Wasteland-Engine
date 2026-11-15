"""Headless suite for the .wlanim animation asset format.

Covers AnimationSerializer: emit, parse, and the load/save file path.

A small rig (3 bones), one clip with position/rotation/scale keys, and a
two-state machine are built in memory, round-tripped through text, and
asserted back. Negative controls cover the error paths a designer or an
importer actually hits: malformed YAML, an unknown key, an unknown bone, an
unknown clip, and a wrong-length float sequence.

The assertions are on STRUCTURE, not on the emitted bytes. A byte-level
golden file would pin the emitter's formatting to the current yaml-cpp
version, so any library upgrade would fail the suite for a change that did
not alter a single value.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_near

from Wasteland import Animation, Quat, Vec3


def _build_asset():
    """A 3-bone rig, one clip, and a two-state machine.

    Shared by the round-trip tests so they all exercise the same structure.
    """
    asset = Animation.AnimationAsset()
    asset.Name = "Hero"

    skeleton = Animation.Skeleton()
    # BuildFromNames returns (ok, error) rather than raising: a rig with a
    # parent listed after its child, or a name repeated twice, is a reportable
    # authoring error rather than a crash.
    ok, error = skeleton.BuildFromNames(
        ["Root", "Spine", "Head"], ["", "Root", "Spine"]
    )
    check(ok, "BuildFromNames: %r" % (error,))
    asset.Skeleton = skeleton

    clip = Animation.AnimationClip("Idle")
    clip.SetLooping(True)
    clip.SetPlayRate(1.0)
    # Two position keys on Spine's Y, two rotation keys, one scale key on Head.
    # Quat's constructor is (x, y, z, w), which is NOT the [w, x, y, z] order the
    # .wlanim format uses -- the two must be kept apart or a round trip silently
    # rotates every bone by the wrong amount.
    clip.AddPositionKey(1, 0.0, Vec3(0.0, 0.0, 0.0))
    clip.AddPositionKey(1, 1.0, Vec3(0.0, 1.0, 0.0))
    clip.AddRotationKey(1, 0.0, Quat(0.0, 0.0, 0.0, 1.0))
    clip.AddRotationKey(1, 1.0, Quat(0.0, 0.7071, 0.0, 0.7071))
    clip.AddScaleKey(2, 0.0, Vec3(1.0, 1.0, 1.0))
    asset.Clips = [clip]

    idle = Animation.AnimationState()
    idle.Name = "Idle"
    idle.Clip = clip
    idle.Speed = 1.0
    idle.Looping = True
    idle.TransitionDuration = 0.2

    walk = Animation.AnimationState()
    walk.Name = "Walk"
    walk.Clip = clip
    walk.Speed = 1.5
    walk.Looping = True
    walk.TransitionDuration = 0.3

    asset.StateMachine.AddState(idle)
    asset.StateMachine.AddState(walk)

    transition = Animation.AnimationTransition()
    transition.FromState = "Idle"
    transition.ToState = "Walk"
    transition.Trigger = "Move"
    transition.Duration = 0.25
    asset.StateMachine.AddTransition(transition)

    asset.HasStateMachine = True
    return asset


def _parse_or_fail(text):
    asset = Animation.AnimationAsset()
    error = Animation.ParseAnimation(text, asset)
    check(error is None, "ParseAnimation should succeed, got %r" % (error,))
    return asset


def test_emit_round_trip():
    """The asset survives emit -> parse with its structure intact."""
    asset = _build_asset()
    parsed = _parse_or_fail(Animation.EmitAnimation(asset))

    check_eq(parsed.Name, "Hero", "asset name")
    check(parsed.Skeleton is not None, "skeleton")
    check_eq(parsed.Skeleton.GetBoneCount(), 3, "bone count")

    check_eq(len(parsed.Clips), 1, "clip count")
    clip = parsed.Clips[0]
    check_eq(clip.GetName(), "Idle", "clip name")
    check_eq(clip.IsLooping(), True, "looping")
    check_near(clip.GetPlayRate(), 1.0, 1e-6, "play rate")


def test_bone_hierarchy():
    """Parent links survive, which is what makes a pose a skeleton."""
    parsed = _parse_or_fail(Animation.EmitAnimation(_build_asset()))

    check_eq(parsed.Skeleton.GetBoneName(0), "Root", "bone 0")
    check_eq(parsed.Skeleton.GetBoneName(1), "Spine", "bone 1")
    check_eq(parsed.Skeleton.GetBoneName(2), "Head", "bone 2")

    # Root has no parent; Spine's is Root; Head's is Spine. An index round trip
    # that silently retargeted a bone would break every pose sampled from this.
    #
    # BoneIndex has no int __eq__ (it only compares against another BoneIndex),
    # so the sentinel is read through its .Value property.
    check_eq(parsed.Skeleton.GetParent(0).Value, -1, "Root parent")
    check_eq(
        parsed.Skeleton.GetBoneName(parsed.Skeleton.GetParent(1)), "Root",
        "Spine parent",
    )
    check_eq(
        parsed.Skeleton.GetBoneName(parsed.Skeleton.GetParent(2)), "Spine",
        "Head parent",
    )


def test_clip_key_counts():
    """No key is merged or dropped on the round trip.

    Counted per channel rather than per track, because AddKey REPLACES a key at
    an existing time: a format that lost the time would collapse two keys into
    one and the motion would change without any error.
    """
    parsed = _parse_or_fail(Animation.EmitAnimation(_build_asset()))
    clip = parsed.Clips[0]

    spine = clip.GetTrack(1)
    check(spine is not None, "Spine has a track")
    check_eq(spine.GetPositionCurve(1).GetKeyCount(), 2, "Spine Y position keys")
    check_eq(spine.Rotation.GetKeyCount(), 2, "Spine rotation keys")

    head = clip.GetTrack(2)
    check(head is not None, "Head has a track")
    check_eq(head.GetScaleCurve(0).GetKeyCount(), 1, "Head X scale keys")

    # Root was never animated, so it must not gain a track. A format that wrote
    # every bone would make "has this bone been animated" unanswerable.
    check(clip.GetTrack(0) is None, "Root has no track")


def test_state_machine_round_trip():
    """States and their transitions survive, including the trigger name."""
    parsed = _parse_or_fail(Animation.EmitAnimation(_build_asset()))

    check(parsed.HasStateMachine, "the machine is present")
    check_eq(parsed.StateMachine.GetStateCount(), 2, "state count")
    check(parsed.StateMachine.HasState("Idle"), "Idle state")
    check(parsed.StateMachine.HasState("Walk"), "Walk state")

    # The clip is bound BY NAME, so the round trip must re-resolve it.
    idle = parsed.StateMachine.FindState("Idle")
    check(idle is not None, "Idle state lookup")
    check(idle.Clip is not None, "Idle state has a clip")
    check_eq(idle.Clip.GetName(), "Idle", "Idle state's clip name")

    transitions = parsed.StateMachine.GetTransitions()
    check_eq(len(transitions), 1, "transition count")
    check_eq(transitions[0].FromState, "Idle", "transition from")
    check_eq(transitions[0].ToState, "Walk", "transition to")
    check_eq(transitions[0].Trigger, "Move", "transition trigger")
    check_near(transitions[0].Duration, 0.25, 1e-6, "transition duration")


def test_deterministic_emit():
    """The same asset emits byte-identical text.

    This is what makes a committed .wlanim diffable: two saves of an unchanged
    asset must not produce a spurious diff.
    """
    asset = _build_asset()
    check_eq(Animation.EmitAnimation(asset), Animation.EmitAnimation(asset),
             "emit is deterministic")


def test_save_load_file():
    """The file path round-trips through real bytes on disk."""
    asset = _build_asset()
    path = os.path.join(tempfile.mkdtemp(prefix="wlanim_"), "hero.wlanim")

    error = Animation.SaveAnimation(path, asset)
    check(error is None, "SaveAnimation should succeed, got %r" % (error,))
    check(os.path.exists(path), "the file was written")

    loaded = Animation.AnimationAsset()
    error = Animation.LoadAnimation(path, loaded)
    check(error is None, "LoadAnimation should succeed, got %r" % (error,))
    check_eq(loaded.Name, "Hero", "asset name")
    check_eq(loaded.Skeleton.GetBoneCount(), 3, "bone count")
    check_eq(len(loaded.Clips), 1, "clip count")


def test_rotation_values_survive():
    """Quaternion VALUES survive the round trip, not just the key count.

    This is the assertion that catches a memory-layout mistake. glm::quat stores
    its members {x, y, z, w}, so taking &q.w and reading four floats emits w
    followed by three floats of ADJACENT MEMORY. Those numbers look valid, so
    only a value comparison notices.

    Asserted through the sampled pose rather than the raw key list: the pose is
    what a viewer actually sees, so it is the value worth pinning. (GetKeys is
    not bound on the curves, which is a second reason to go through Sample.)
    """
    asset = _build_asset()
    parsed = _parse_or_fail(Animation.EmitAnimation(asset))

    # The same clip sampled before and after the round trip must give the same
    # pose. This is the assertion that catches a memory-layout bug: the emitted
    # numbers change, so the parsed pose changes, and no structural check
    # (key counts, bone counts, "it loaded") notices.
    #
    # Compared against the SOURCE pose rather than a hand-computed rotation,
    # because Python's Quat constructor does not mean what its parameter names
    # claim: the binding names them (x, y, z, w) but forwards positionally to
    # glm::quat(w, x, y, z). Quoting a literal expected rotation here would bake
    # that mismatch into the test.
    before = asset.Clips[0].Sample(asset.Skeleton, 1.0)
    after = parsed.Clips[0].Sample(parsed.Skeleton, 1.0)
    source = before.GetBone(1)
    result = after.GetBone(1)
    check(source is not None and result is not None, "Spine bone in both poses")
    check_near(result.Rotation.x, source.Rotation.x, 1e-5, "sampled rotation x")
    check_near(result.Rotation.y, source.Rotation.y, 1e-5, "sampled rotation y")
    check_near(result.Rotation.z, source.Rotation.z, 1e-5, "sampled rotation z")
    check_near(result.Rotation.w, source.Rotation.w, 1e-5, "sampled rotation w")


def test_rest_pose_values_survive():
    """A non-identity REST pose survives the round trip with its values intact.

    The rest pose is a second quaternion emit site, separate from clip keys. It
    shipped with the same &q.w contiguous-read defect as the clip path and was
    only caught when a skinned arm was displaced; test_rotation_values_survive
    covers clips only. Compared against the source pose rather than a literal,
    for the same reason (Quat's constructor argument order).
    """
    asset = _build_asset()
    rest = Animation.Pose()
    rest.Resize(3)
    turn = Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.6)
    rest.SetBone(0, Animation.BoneTransform(
        Vec3(0.0, 0.0, 0.0), Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0),
        Vec3(1.0, 1.0, 1.0)))
    rest.SetBone(1, Animation.BoneTransform(
        Vec3(0.0, 2.0, 0.0), turn, Vec3(1.0, 1.0, 1.0)))
    rest.SetBone(2, Animation.BoneTransform(
        Vec3(0.0, 1.0, 0.0), Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0),
        Vec3(1.0, 1.0, 1.0)))
    asset.Skeleton.SetRestPose(rest)

    parsed = _parse_or_fail(Animation.EmitAnimation(asset))
    want = asset.Skeleton.GetRestPose().GetBone(1)
    got = parsed.Skeleton.GetRestPose().GetBone(1)
    check_near(got.Translation.y, 2.0, 1e-5, "rest translation y")
    check_near(got.Rotation.x, want.Rotation.x, 1e-5, "rest rotation x")
    check_near(got.Rotation.y, want.Rotation.y, 1e-5, "rest rotation y")
    check_near(got.Rotation.z, want.Rotation.z, 1e-5, "rest rotation z")
    check_near(got.Rotation.w, want.Rotation.w, 1e-5, "rest rotation w")


# --------------------------------------------------------------- error paths
#
# A bool return is not something a designer can act on, so every failure below
# must come back as an AnimationError that NAMES the problem.


def test_malformed_yaml_reports():
    asset = Animation.AnimationAsset()
    error = Animation.ParseAnimation("schema: [", asset)
    check(error is not None, "malformed YAML is an error")
    check(isinstance(error.Message, str) and len(error.Message) > 0,
          "the error has a message")


def test_unknown_root_key_reports():
    """A typo must be reported, not silently ignored.

    Silently dropping an unknown key means a hand-authored file that says
    `clip:` instead of `clips:` loads as an asset with no clips and no warning.
    """
    asset = Animation.AnimationAsset()
    error = Animation.ParseAnimation("schema: 1\nclip: []\n", asset)
    check(error is not None, "an unknown root key is an error")
    check("clip" in error.Message,
          "the message names the offending key, got %r" % (error.Message,))


def test_unknown_bone_reports():
    """A track naming a bone the skeleton does not have is an error."""
    asset = Animation.AnimationAsset()
    text = (
        "schema: 1\n"
        "skeleton:\n"
        "  bones:\n"
        "    - { name: Root, parent: \"\" }\n"
        "clips:\n"
        "  - name: Idle\n"
        "    tracks:\n"
        "      - bone: Missing\n"
        "        rotation:\n"
        "          - { t: 0.0, v: [1, 0, 0, 0] }\n"
    )
    error = Animation.ParseAnimation(text, asset)
    check(error is not None, "an unknown bone is an error")
    check("Missing" in error.Message,
          "the message names the bone, got %r" % (error.Message,))


def test_unknown_clip_reports():
    """A state naming a clip the file does not contain is an error."""
    asset = Animation.AnimationAsset()
    text = (
        "schema: 1\n"
        "clips:\n"
        "  - name: Idle\n"
        "state_machine:\n"
        "  states:\n"
        "    - { name: Idle, clip: Nowhere }\n"
    )
    error = Animation.ParseAnimation(text, asset)
    check(error is not None, "an unknown clip is an error")
    check("Nowhere" in error.Message,
          "the message names the clip, got %r" % (error.Message,))


def test_short_float_sequence_reports():
    """A vec3 with two floats is an error, not a silent zero fill."""
    asset = Animation.AnimationAsset()
    text = (
        "schema: 1\n"
        "skeleton:\n"
        "  bones:\n"
        "    - { name: Root, parent: \"\" }\n"
        "  rest_pose:\n"
        "    - { bone: Root, t: [0, 0], q: [1, 0, 0, 0], s: [1, 1, 1] }\n"
    )
    error = Animation.ParseAnimation(text, asset)
    check(error is not None, "a short float sequence is an error")


def test_clip_without_skeleton_reports():
    """A track cannot be placed without a skeleton, so it must say so."""
    asset = Animation.AnimationAsset()
    text = (
        "schema: 1\n"
        "clips:\n"
        "  - name: Idle\n"
        "    tracks:\n"
        "      - bone: Root\n"
        "        rotation:\n"
        "          - { t: 0.0, v: [1, 0, 0, 0] }\n"
    )
    error = Animation.ParseAnimation(text, asset)
    check(error is not None, "a track with no skeleton is an error")


def test_load_missing_file_reports():
    asset = Animation.AnimationAsset()
    error = Animation.LoadAnimation(
        os.path.join(tempfile.gettempdir(), "definitely_not_here.wlanim"), asset
    )
    check(error is not None, "a missing file is an error")


def test_error_format_names_the_context():
    """Format() produces one line a designer can act on."""
    asset = Animation.AnimationAsset()
    text = (
        "schema: 1\n"
        "clips:\n"
        "  - name: Idle\n"
        "state_machine:\n"
        "  states:\n"
        "    - { name: Idle, clip: Nowhere }\n"
    )
    error = Animation.ParseAnimation(text, asset)
    check(error is not None, "parse failed")
    formatted = error.Format("hero.wlanim")
    check("hero.wlanim" in formatted, "the file name is included")
    check("Nowhere" in formatted, "the offending clip is included")


def test_parse_failure_leaves_the_asset_untouched():
    """A failed parse must not half-populate the caller's asset.

    The all-or-nothing contract: a caller that ignores the return value must not
    end up with a rig from one file and clips from another.
    """
    asset = Animation.AnimationAsset()
    asset.Name = "Sentinel"
    error = Animation.ParseAnimation("schema: [", asset)
    check(error is not None, "parse failed")
    check_eq(asset.Name, "Sentinel",
             "the caller's asset is untouched by a failed parse")
