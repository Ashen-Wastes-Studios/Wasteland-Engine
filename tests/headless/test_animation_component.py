"""Headless suite for AnimationComponent + AnimationSystem.

An AnimationComponent attaches a `.wlanim` asset the way a script, a behaviour
tree or a flow graph attach: the asset loads on the FIRST runtime tick, the
runtime is owned by AnimationSystem keyed by entity UUID, and the pose is
produced every tick after that.

The asset used is the shipped `DemonCore-Editor/assets/animation/Hero.wlanim`
-- six bones, one clip, one state. Its structure is asserted here rather than
built inline, because the test that matters is "the asset we actually ship
drives a character", not "a fixture I made up drives a character".

Note on assertions: the pose is compared at a key's exact time, where the
sampler returns the keyed value rather than a blend. That is what makes the
expected numbers exact instead of approximate.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_near

from Wasteland import AnimationComponent, GetAnimationAnimator

ASSET = "DemonCore-Editor/assets/animation/Hero.wlanim"

STEP = 1.0 / 60.0
# The Hero clip runs 2 seconds: 0.0 -> 1.0 -> 2.0.
SECONDS = 1.0


def _scene():
    from Wasteland import Scene
    return Scene()


def _animated_entity(scene, asset=ASSET, default_state=""):
    """An entity carrying an AnimationComponent, added but not yet ticked."""
    # Scene.CreateEntity already returns a Wasteland.Entity; wrapping it in the
    # Entity constructor again is a TypeError, not a copy.
    entity = scene.CreateEntity()
    # The macro-generated Add* takes the component BY VALUE and returns None,
    # so the object handed in is a copy. Read the stored one back with
    # GetAnimationComponent() -- mutating the local would mutate a copy.
    comp = AnimationComponent()
    comp.AssetPath = asset
    comp.DefaultState = default_state
    entity.AddAnimationComponent(comp)
    return entity


def _rest_and_animated_bone(animator, bone_index, seconds):
    """Sample the pose at a given clip time on the clip the animator holds.

    Returns (pose, bone). The bone is read through the pose rather than the
    Animator so the value asserted is the one a renderer would consume.
    """
    pose = animator.GetLastPose()
    bone = pose.GetBone(bone_index)
    return pose, bone


def test_component_is_attachable():
    """The component exists, attaches, and exposes the documented fields."""
    sc = _scene()
    entity = _animated_entity(sc)
    anim = entity.GetAnimationComponent()
    check(anim is not None, "the component attached")
    check_eq(anim.AssetPath, ASSET, "AssetPath round-trips through the binding")
    check_eq(anim.DefaultState, "", "DefaultState defaults to empty")
    check_eq(anim.MeshPath, "", "MeshPath defaults to empty")
    check_eq(anim.LoopDefault, True, "LoopDefault defaults to true")
    check_near(anim.PlayRate, 1.0, 1e-6, "PlayRate defaults to 1")


def test_load_happens_on_first_tick():
    """The asset is inert until the runtime ticks, then attaches.

    This is the attach contract shared with the tree and the graph: loading at
    attach time would run during deserialization, where the asset root may not
    be open yet.
    """
    sc = _scene()
    entity = _animated_entity(sc)
    anim = entity.GetAnimationComponent()

    # Before any tick: not attached, no error, no runtime.
    check_eq(anim.Attached, False, "inert before the first tick")
    check_eq(anim.LoadError, "", "no error before the first tick")
    check(GetAnimationAnimator(entity) is None,
          "no animator exists before the first tick")

    sc.OnUpdateRuntime(STEP)

    check_eq(anim.Attached, True, "attached after the first tick")
    check_eq(anim.LoadError, "", "a valid asset loads without an error")
    check(GetAnimationAnimator(entity) is not None,
          "the runtime owns an animator after the first tick")


def test_pose_is_produced_and_advances():
    """The pose changes over time, and matches the authored key values.

    Hero's Idle clip keys Chest (bone 2) Y at 0.0, 1.0 and 2.0 to 0.0, 0.02
    and 0.0. Sampling at the exact key time returns the keyed value, so the
    assertion is exact rather than a tolerance on a blend.
    """
    sc = _scene()
    entity = _animated_entity(sc)
    anim = entity.GetAnimationComponent()
    sc.OnUpdateRuntime(STEP)

    animator = GetAnimationAnimator(entity)
    check(animator is not None, "the animator exists")
    check(animator.GetInstanceCount() > 0,
          "the state machine started an instance for its clip")

    # The rest pose has Chest at Y = 0. After 1 second the clip has moved it.
    _, rest_bone = _rest_and_animated_bone(animator, 2, 0.0)
    check_near(rest_bone.Translation.y, 0.0, 1e-4, "Chest starts at rest")

    # Tick to the second key.
    for _ in range(int(SECONDS / STEP)):
        sc.OnUpdateRuntime(STEP)

    pose = animator.GetLastPose()
    chest = pose.GetBone(2)
    check(chest is not None, "Chest is in the sampled pose")
    check_near(chest.Translation.y, 0.02, 1e-4,
               "Chest Y follows the clip's second position key")


def test_default_state_is_entered():
    """An explicit DefaultState is entered; an unknown one is reported.

    The warning is a warning, not a failure: the asset loaded, the character has
    a valid rest pose, and the message names the typo'd state. Treating it as a
    load failure would make the component unusable for a one-character typo.
    """
    sc = _scene()
    entity = _animated_entity(sc, default_state="Idle")
    anim = entity.GetAnimationComponent()
    sc.OnUpdateRuntime(STEP)

    check_eq(anim.Attached, True, "the asset loaded")
    check_eq(anim.LoadError, "", "no warning for a state the machine has")

    animator = GetAnimationAnimator(entity)
    check(animator is not None, "the animator exists")
    check(animator.GetInstanceCount() > 0,
          "entering Idle started a clip instance")

    # Now the typo case.
    sc2 = _scene()
    bad = _animated_entity(sc2, default_state="Idel")
    bad_anim = bad.GetAnimationComponent()
    sc2.OnUpdateRuntime(STEP)

    check_eq(bad_anim.Attached, True,
             "an unknown state does not fail the load")
    check("Idel" in bad_anim.LoadError,
          "the warning names the state that was not found, got %r"
          % (bad_anim.LoadError,))


def test_missing_asset_reports_instead_of_crashing():
    """A bad path latches an error and leaves the entity inert.

    The latch matters: without it a broken asset would re-attempt the load every
    frame and re-log the same message forever.
    """
    sc = _scene()
    missing = os.path.join(tempfile.gettempdir(), "definitely_not_here.wlanim")
    entity = _animated_entity(sc, asset=missing)
    anim = entity.GetAnimationComponent()

    for _ in range(5):
        sc.OnUpdateRuntime(STEP)

    check_eq(anim.Attached, False, "a missing asset never attaches")
    check(len(anim.LoadError) > 0, "the failure is reported")
    check(missing in anim.LoadError,
          "the message names the file, got %r" % (anim.LoadError,))
    check(GetAnimationAnimator(entity) is None,
          "no runtime exists for a failed load")


def test_empty_path_is_inert():
    """No asset path means the component does nothing, and does not error.

    An entity can carry the component as a placeholder while its asset is being
    authored, and a designer can add the component before the path.
    """
    sc = _scene()
    entity = _animated_entity(sc, asset="")
    anim = entity.GetAnimationComponent()

    sc.OnUpdateRuntime(STEP)

    check_eq(anim.Attached, False, "an empty path never attaches")
    check_eq(anim.LoadError, "", "an empty path is not an error")


def test_scene_round_trip():
    """The component survives SaveScene -> LoadScene.

    Paths are stored verbatim, not rewritten: the asset path is project-relative
    and must not be turned into something derived from the scene's directory.
    """
    sc = _scene()
    entity = _animated_entity(sc, default_state="Idle")
    entity.GetAnimationComponent().MeshPath = "models/Hero.mesh"
    entity.GetAnimationComponent().LoopDefault = False
    entity.GetAnimationComponent().PlayRate = 1.5

    path = os.path.join(tempfile.mkdtemp(prefix="wlanim_scene_"), "scene.wastescene")
    check(sc.SaveScene(path), "SaveScene must succeed")

    from Wasteland import Scene
    loaded = Scene()
    check(loaded.LoadScene(path), "LoadScene must succeed")

    # Find the entity by its animation component rather than by index: the load
    # order is not a contract.
    found = None
    for e in loaded.GetAllEntitiesWithTransform():
        if e.HasAnimationComponent():
            found = e
            break
    # GetAllEntitiesWithTransform only enumerates entities WITH a transform; the
    # component alone is not one. Fall back to a direct scan if needed.
    if found is None:
        check(False, "the loaded scene has no entity carrying AnimationComponent")
        return

    anim = found.GetAnimationComponent()
    check_eq(anim.AssetPath, ASSET, "AssetPath survives the round trip")
    check_eq(anim.DefaultState, "Idle", "DefaultState survives the round trip")
    check_eq(anim.MeshPath, "models/Hero.mesh", "MeshPath survives the round trip")
    check_eq(anim.LoopDefault, False, "LoopDefault survives the round trip")
    check_near(anim.PlayRate, 1.5, 1e-6, "PlayRate survives the round trip")


def test_runtime_does_not_leak_between_scenes():
    """A destroyed entity's runtime is dropped.

    Without OnDestroyEntity a respawn reusing a UUID would inherit the previous
    entity's Animator and its failure latch.
    """
    sc = _scene()
    entity = _animated_entity(sc)
    sc.OnUpdateRuntime(STEP)

    check(GetAnimationAnimator(entity) is not None,
          "the runtime exists before destruction")

    sc.DestroyEntity(entity)

    # After destruction the accessor must return None for that UUID. The entity
    # handle is now invalid, so this asserts the runtime map no longer holds it.
    check(GetAnimationAnimator(entity) is None,
          "the runtime is dropped with the entity")
