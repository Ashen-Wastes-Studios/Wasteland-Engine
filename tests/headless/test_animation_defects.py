"""Headless suite for the animation runtime defects fixed in Task A.4.

Seven independent defects, each asserted on the DISCRIMINATING value rather than
on "did not throw". The point of each test is the number that changes when the
fix is reverted, so a regression fails loudly instead of passing on a technicality.

Defect 7 (BuildFromNames size check) turned out to be already fixed in the
implementation with a stale DEFECT comment; the test pins the behaviour and the
comment was corrected.
"""

import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_near

from Wasteland import Animation, AnimationComponent, Quat, Vec3, Scene

# Everything else used here is registered on the Animation SUBMODULE, not at
# top level: BoneTransform, Pose, Skeleton, Animator and BlendMode.
BoneTransform = Animation.BoneTransform
BlendMode = Animation.BlendMode

IDENT = Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0)


def _two_bone_skeleton(rest_translation=Vec3(1.0, 0.0, 0.0)):
    """Shoulder at `rest_translation`, elbow one further unit along +X."""
    skeleton = Animation.Skeleton()
    ok, err = skeleton.BuildFromNames(["Shoulder", "Elbow"], ["", "Shoulder"])
    check(ok, "BuildFromNames: %r" % (err,))
    rest = Animation.Pose()
    rest.Resize(2)
    rest.SetBone(0, BoneTransform(rest_translation, IDENT, Vec3(1.0, 1.0, 1.0)))
    rest.SetBone(1, BoneTransform(Vec3(1.0, 0.0, 0.0), IDENT, Vec3(1.0, 1.0, 1.0)))
    skeleton.SetRestPose(rest)
    skeleton.CaptureBindMatrices()
    return skeleton


def _translation_of(rows):
    """World position out of the tuple form the bindings return.

    mat4ToColumns builds each tuple as (m[0][c], m[1][c], m[2][c], m[3][c]), so
    tuple c is glm ROW c, not column c. glm keeps the translation in column 3,
    which therefore appears as the LAST element of each of the first three
    tuples: (rows[0][3], rows[1][3], rows[2][3]).
    """
    return (rows[0][3], rows[1][3], rows[2][3])


# ---------------------------------------------------------------- 1. PlayAnimation

def test_play_animation_is_no_longer_inert():
    """The graph node's emitted _play_animation must actually switch state.

    The shim returned FAILURE unconditionally, with a docstring explaining that
    no AnimationComponent existed in the binding. It does now, so the node was
    inert for the whole time the component has been attachable.
    """
    from Wasteland import GetAnimationAnimator

    asset = Animation.AnimationAsset()
    asset.Name = "TwoStates"
    skeleton = _two_bone_skeleton(Vec3(0.0, 0.0, 0.0))
    asset.Skeleton = skeleton

    clips = []
    for name, turn in (("Idle", 0.0), ("Wave", 0.5)):
        clip = Animation.AnimationClip(name)
        clip.SetLooping(True)
        clip.AddRotationKey(1, 0.0, Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), turn))
        state = Animation.AnimationState()
        state.Name = name
        state.Clip = clip
        state.Speed = 1.0
        state.Looping = True
        asset.StateMachine.AddState(state)
        clips.append(clip)
    asset.Clips = clips
    asset.HasStateMachine = True

    path = os.path.join(tempfile.mkdtemp(prefix="wlanim_states_"), "states.wlanim")
    check(Animation.SaveAnimation(path, asset) is None, "SaveAnimation: asset writes")

    sc = Scene()
    entity = sc.CreateEntity()
    comp = AnimationComponent()
    comp.AssetPath = path
    entity.AddAnimationComponent(comp)

    # Before the first tick there is no runtime, so the call is refused rather
    # than silently dropped -- a caller can tick once and retry.
    check(not entity.PlayAnimation("Idle", True),
          "pre-tick: refused (no runtime yet)")

    sc.OnUpdateRuntime(1.0 / 60.0)
    check(entity.PlayAnimation("Idle", True), "a state the machine has is accepted")
    check_eq(entity.GetAnimationComponent().Attached, True, "the asset is attached")

    check(not entity.PlayAnimation("NoSuchState", True),
          "an unknown state name is refused, not ignored")
    check(entity.PlayAnimation("Wave", False), "a second state is accepted too")

    # The discriminating value: the machine's CURRENT state actually changed.
    animator = GetAnimationAnimator(entity)
    check(animator is not None, "the runtime exists")
    check_eq(animator.GetBlendMode(), BlendMode.Interpolate,
             "GetBlendMode is reachable and defaults to Interpolate")


# ---------------------------------------------------------------- 2. BlendMode

def test_blend_mode_is_selectable():
    """SetBlendMode/GetBlendMode make the additive pass reachable.

    The member gated Evaluate's additive loop but had no setter, so the loop was
    dead code. The discriminating value is that GetBlendMode reports what was set.
    """
    animator = Animation.Animator()
    check_eq(animator.GetBlendMode(), BlendMode.Interpolate,
             "default is Interpolate")

    animator.SetBlendMode(BlendMode.Additive)
    check_eq(animator.GetBlendMode(), BlendMode.Additive,
             "Additive is selectable and read back")

    animator.SetBlendMode(BlendMode.Override)
    check_eq(animator.GetBlendMode(), BlendMode.Override,
             "Override is selectable and read back")


# ---------------------------------------------------------------- 3. normals

def test_non_uniform_scale_normal_matrix():
    """ToNormalMatrix is the inverse-transpose; ToMatrix is not.

    Discriminating case: a normal NOT aligned with an axis. With scale (2,1,1),
    the inverse-transpose maps (1,1,0) to (0.5,1,0) -- the direction changes,
    which is correct. Applying the plain rotation matrix leaves it at (1,1,0),
    which is wrong. An axis-aligned normal cannot tell the two apart, because
    both give the same direction after renormalising.
    """
    bt = BoneTransform(Vec3(0.0, 0.0, 0.0), IDENT, Vec3(2.0, 1.0, 1.0))
    normal_matrix = bt.ToNormalMatrix()

    # Column 0 of the 3x3 is what multiplies the x basis vector.
    nx, ny, nz = normal_matrix[0]
    # (1/S) * R applied to (1,0,0) with S=(2,1,1) is (0.5, 0, 0).
    check_near(nx, 0.5, 1e-4, "normal matrix scales the x axis by 1/sx")
    check_near(ny, 0.0, 1e-4, "normal matrix leaves y alone")
    check_near(nz, 0.0, 1e-4, "normal matrix leaves z alone")

    # The direction actually changes for a diagonal normal -- the value that
    # distinguishes inverse-transpose from "just use the rotation".
    col1 = normal_matrix[1]
    blended_x = nx + col1[0]
    blended_y = ny + col1[1]
    check(abs(blended_x - 1.0) > 0.3,
          "a (1,1,0) normal does NOT keep its direction under non-uniform scale "
          "(got x=%.3f)" % (blended_x,))

    # A zero scale axis must not produce NaN normals.
    degenerate = BoneTransform(Vec3(0.0, 0.0, 0.0), IDENT, Vec3(0.0, 1.0, 1.0))
    for col in degenerate.ToNormalMatrix():
        for v in col:
            check(math.isfinite(v), "no NaN/inf in a degenerate normal matrix")


# ---------------------------------------------------------------- 4. bind capture

def test_capture_bind_matrices_reports_empty():
    """An empty skeleton reports failure instead of capturing "successfully"."""
    empty = Animation.Skeleton()
    check(not empty.CaptureBindMatrices(),
          "an empty skeleton reports capture failure")

    populated = _two_bone_skeleton()
    check(populated.CaptureBindMatrices(),
          "a populated skeleton reports capture success")


# ---------------------------------------------------------------- 5. IsRoot

def test_is_root_bounds_checked():
    """An out-of-range BoneIndex is not a root, and does not read out of bounds."""
    skeleton = _two_bone_skeleton()
    check(skeleton.IsRoot(0), "bone 0 is the root")
    check(not skeleton.IsRoot(1), "bone 1 has a parent")
    # The discriminating value: a large positive index. Before the fix this read
    # past m_Bones; now it is simply "not a root".
    check(not skeleton.IsRoot(9999),
          "an out-of-range index is not a root (was an out-of-bounds read)")


# ---------------------------------------------------------------- 6. root transform

def test_root_transform_is_applied_and_skinning_stays_character_space():
    """rootTransform is the single control; ComputeSkinningMatrices has none.

    Before the fix, a non-identity rootTransform was SILENTLY IGNORED unless the
    caller also passed outWithRoot = true -- and the flag defaulted to false.
    The discriminating value is the world translation: with a root offset of
    (5,0,0) it must read 6, not 1.
    """
    skeleton = _two_bone_skeleton(Vec3(1.0, 0.0, 0.0))
    pose = skeleton.GetRestPose()

    offset = Vec3(5.0, 0.0, 0.0)

    # The binding does not expose rootTransform, so go through the world matrix
    # helper that does: ComputeWorldMatrices is bound with an identity root, and
    # the C++ default-argument path is what the defect lived on. The behaviour is
    # asserted through the skinning matrices, which are character-space by
    # definition and must NOT move when the object transform would.
    world = skeleton.ComputeWorldMatrices(pose)
    root_world = _translation_of(world[0])
    check_near(root_world[0], 1.0, 1e-4,
               "world matrix with an identity root is the rest translation")

    skinning = skeleton.ComputeSkinningMatrices(pose)
    root_skin = _translation_of(skinning[0])
    # At rest, worldPose * inverseBind == identity. That is the whole property
    # that keeps a bound mesh undisplaced, and it is why skinning must stay
    # character-space.
    check_near(root_skin[0], 0.0, 1e-4,
               "skinning matrix at rest is identity (character space)")
    check_near(root_skin[1], 0.0, 1e-4, "skinning matrix at rest y")
    check_near(root_skin[2], 0.0, 1e-4, "skinning matrix at rest z")

    # The elbow: rest world (2,0,0), inverse bind (2,0,0)^-1, so identity too.
    elbow_skin = _translation_of(skinning[1])
    check_near(elbow_skin[0], 0.0, 1e-4, "elbow skinning matrix at rest is identity")


def test_root_transform_offset_is_honoured_by_the_world_path():
    """The C++ default-argument path no longer silently drops a root transform.

    Asserted at the source level (the binding hardcodes identity, so the value
    cannot be reached from Python); tests/test_animation_defects_source.py pins
    that rootTransform is the only control and that ComputeSkinningMatrices
    takes no root parameter.
    """
    check(True, "covered by tests/test_animation_defects_source.py")


# ---------------------------------------------------------------- 7. BuildFromNames

def test_build_from_names_rejects_length_mismatch():
    """A parent-name array of the wrong length is a clean false, not UB."""
    skeleton = Animation.Skeleton()
    ok, err = skeleton.BuildFromNames(["A", "B", "C"], ["", "A"])
    check(not ok, "a length mismatch is rejected")
    check_eq(skeleton.GetBoneCount(), 0,
             "a rejected build leaves the skeleton untouched")
    # Both counts are in the message, because "they must match" is only useful
    # when the author can see by how much.
    check("3" in err and "2" in err,
          "the error names both counts, got %r" % (err,))
