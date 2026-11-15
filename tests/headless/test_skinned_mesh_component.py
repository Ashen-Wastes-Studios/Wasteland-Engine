"""Headless suite for the skinned render path (Task A.3).

The CPU half is asserted numerically here: a two-bone "arm" is bound to an
entity through AnimationComponent, the elbow rotates 90 degrees, and the
deformed vertex positions are compared with hand-computed values. The GPU half
(Renderer3D::DrawSkinnedMesh, the Scene submit pass) cannot run headless, so
`tests/test_skinned_render_source.py` pins its wiring.

Geometry: shoulder at the origin, elbow one unit along +X, so
  skinning matrix(elbow) = T(1,0,0) * R * T(-1,0,0)
and a vertex at (2,0,0) bound to the elbow lands at (1,1,0) after a 90 degree
turn about +Z: it swings up around the elbow, not around the origin. A vertex at
(0,0,0) bound to the shoulder must not move. Those two numbers are exactly the
difference between "skinning works" and "everything rotates about the origin".
"""

import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_near

from Wasteland import (AnimationComponent, Animation, MeshVertex, Quat, Vec3,
                       GetDeformedPositions, SetAnimationMesh)

STEP = 1.0 / 60.0
QUARTER_TURN = math.pi / 2.0


def identity():
    """The identity rotation.

    NOT Quat(): the default constructor is all zeros (x=y=z=w=0), which is not a
    rotation at all, and Quat(0, 0, 0, 1) is not identity either because the
    binding forwards its (x, y, z, w) arguments to glm::quat(w, x, y, z). A zero
    angle through FromAxisAngle is the one spelling that is correct.
    """
    return Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.0)


def _arm_asset_path():
    """Writes a two-bone arm .wlanim and returns its path.

    Built through the same Save path a DCC export would use, so the test also
    covers a loaded rig being ready to skin (bind matrices captured on load).
    """
    asset = Animation.AnimationAsset()
    asset.Name = "Arm"

    skeleton = Animation.Skeleton()
    ok, err = skeleton.BuildFromNames(["Shoulder", "Elbow"], ["", "Shoulder"])
    check(ok, "BuildFromNames: %r" % (err,))

    # Elbow sits one unit out along +X in its parent's space.
    rest = Animation.Pose()
    rest.Resize(2)
    rest.SetBone(0, Animation.BoneTransform(
        Vec3(0.0, 0.0, 0.0), identity(), Vec3(1.0, 1.0, 1.0)))
    rest.SetBone(1, Animation.BoneTransform(
        Vec3(1.0, 0.0, 0.0), identity(), Vec3(1.0, 1.0, 1.0)))
    skeleton.SetRestPose(rest)
    asset.Skeleton = skeleton

    clip = Animation.AnimationClip("Bend")
    clip.SetLooping(True)
    turn = Quat.FromAxisAngle(Vec3(0.0, 0.0, 1.0), QUARTER_TURN)
    # Two plateaus, so both tests sample exact values rather than points on a
    # curve: identity held over [0, 0.5] (the "at rest" check samples at ~0.03s)
    # and 90 degrees held over [1.5, 2.5] (the bend check samples at ~1.85s).
    # Without the first hold the bend begins at t=0, and two ticks in the hand
    # has already risen by sin(3 deg) = 0.052.
    clip.AddRotationKey(1, 0.0, identity())
    clip.AddRotationKey(1, 0.5, identity())
    clip.AddRotationKey(1, 1.5, turn)
    clip.AddRotationKey(1, 2.5, turn)
    asset.Clips = [clip]

    state = Animation.AnimationState()
    state.Name = "Bend"
    state.Clip = clip
    state.Speed = 1.0
    state.Looping = True
    asset.StateMachine.AddState(state)
    asset.HasStateMachine = True

    path = os.path.join(tempfile.mkdtemp(prefix="wlanim_arm_"), "arm.wlanim")
    error = Animation.SaveAnimation(path, asset)
    check(error is None, "SaveAnimation: %r" % (error,))
    return path


def _arm_mesh():
    """Three vertices along +X: shoulder end, the elbow joint, the hand end.

    Vertices are bound so x <= 1 follows the shoulder and x > 1 follows the
    elbow (blendWidth 0, a hard split), which makes each expected position a
    single matrix product with no blend to reason about.
    """
    mesh = Animation.SkinnedMesh()
    for x in (0.0, 2.0):
        v = MeshVertex()
        v.Position = Vec3(x, 0.0, 0.0)
        mesh.AddVertex(v)
    # A degenerate triangle is enough: indices only need to be well formed.
    mesh.SetIndices([0, 1, 1])
    mesh.BindTwoBonesByAxis(0, 1, 0, 1.0, 0.0)
    mesh.NormalizeWeights()
    return mesh


def _animated_arm(scene, tick=True):
    entity = scene.CreateEntity()
    comp = AnimationComponent()
    comp.AssetPath = _arm_asset_path()
    entity.AddAnimationComponent(comp)
    if tick:
        scene.OnUpdateRuntime(STEP)
    return entity


def _scene():
    from Wasteland import Scene
    return Scene()


def test_deformed_positions_follow_the_elbow():
    """The hand swings around the ELBOW, not around the origin."""
    sc = _scene()
    entity = _animated_arm(sc)
    check(SetAnimationMesh(entity, _arm_mesh()),
          "the runtime accepts a valid skinned mesh")

    # ~1.85 seconds: inside the [1.5, 2.5] plateau at 90 degrees.
    for _ in range(110):
        sc.OnUpdateRuntime(STEP)

    positions = GetDeformedPositions(entity)
    check(positions is not None, "a deformed mesh exists after ticking")
    check_eq(len(positions), 2, "one deformed position per vertex")

    shoulder_end, hand = positions
    # Bound to the shoulder, which never moves.
    check_near(shoulder_end.x, 0.0, 1e-4, "shoulder-end x")
    check_near(shoulder_end.y, 0.0, 1e-4, "shoulder-end y")
    # T(1,0,0) * Rz(90) * T(-1,0,0) applied to (2,0,0) = (1, 1, 0).
    check_near(hand.x, 1.0, 1e-3, "hand x (swung about the elbow)")
    check_near(hand.y, 1.0, 1e-3, "hand y (swung about the elbow)")
    check_near(hand.z, 0.0, 1e-3, "hand z")


def test_rest_pose_is_not_displaced():
    """Before the bend begins the mesh sits exactly where it was authored.

    This is the bind-matrix check. If a loaded rig skipped CaptureBindMatrices
    the inverse bind would be identity and the hand would be displaced by the
    elbow's whole rest offset (to x = 3) instead of staying at x = 2.
    """
    sc = _scene()
    entity = _animated_arm(sc)
    check(SetAnimationMesh(entity, _arm_mesh()), "mesh accepted")

    # One more tick so a deformed mesh exists, still at t ~ 2 frames (identity key).
    sc.OnUpdateRuntime(STEP)
    positions = GetDeformedPositions(entity)
    check(positions is not None, "a deformed mesh exists")

    hand = positions[1]
    check_near(hand.x, 2.0, 1e-2, "hand x at rest (bind matrices captured on load)")
    check_near(hand.y, 0.0, 1e-2, "hand y at rest")


def test_no_mesh_means_no_deformed_output():
    """An animated entity without a mesh produces a pose and nothing to draw."""
    sc = _scene()
    entity = _animated_arm(sc)
    sc.OnUpdateRuntime(STEP)
    check(GetDeformedPositions(entity) is None,
          "no mesh attached -> no deformed positions")


def test_set_mesh_before_the_runtime_exists_is_refused():
    """SetAnimationMesh returns False until the first tick creates the runtime."""
    sc = _scene()
    entity = _animated_arm(sc, tick=False)
    check(not SetAnimationMesh(entity, _arm_mesh()),
          "no runtime yet -> the mesh is refused, not silently dropped")

    sc.OnUpdateRuntime(STEP)
    check(SetAnimationMesh(entity, _arm_mesh()),
          "after the first tick the same call succeeds")


def test_invalid_mesh_is_refused():
    """A mesh with no weights fails Validate and is not attached."""
    sc = _scene()
    entity = _animated_arm(sc)
    unweighted = Animation.SkinnedMesh()
    v = MeshVertex()
    v.Position = Vec3(0.0, 0.0, 0.0)
    unweighted.AddVertex(v)
    check(not SetAnimationMesh(entity, unweighted),
          "an unweighted mesh is refused")
    sc.OnUpdateRuntime(STEP)
    check(GetDeformedPositions(entity) is None,
          "a refused mesh never produces output")
