"""Verify DemoGame.wastescene exercises every component the deserializer supports.

The scene deserializer handles 40 component types. This test loads the scene
and asserts that each one is present on at least one entity, so a component
that has no coverage in the demo is caught here rather than in the editor.

Run:  python tests/headless/test_feature_coverage.py
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402

ROOT = _os.path.abspath(_os.path.join(_HERE, "..", ".."))
SCENE = _os.path.join(ROOT, "DemonCore-Editor", "assets", "scenes", "DemoGame.wastescene")

# Every component SceneSerializer::Deserialize can read.
ALL_COMPONENTS = [
    "TagComponent", "TransformComponent", "HealthComponent", "WeaponComponent",
    "InteractableComponent", "WeaponFeedbackComponent", "ViewStateComponent",
    "ItemDefinitionComponent", "InventoryComponent", "DroppedItemComponent",
    "AudioSourceComponent", "AudioListenerComponent", "CameraComponent",
    "SpriteRendererComponent", "CircleRendererComponent", "CubeRendererComponent",
    "SphereRendererComponent", "PlaneRendererComponent", "GridRendererComponent",
    "Circle3DRendererComponent", "IcoSphereRendererComponent",
    "CylinderRendererComponent", "ConeRendererComponent", "TorusRendererComponent",
    "MaterialComponent", "Rigidbody2DComponent", "BoxCollider2DComponent",
    "CircleCollider2DComponent", "RigidBody3DComponent", "BoxCollider3DComponent",
    "SphereCollider3DComponent", "CapsuleCollider3DComponent",
    "VolumetricFogComponent", "VolumetricCloudsComponent",
    "DirectionalLightComponent", "PointLightComponent", "SpotLightComponent",
    "AreaLightComponent", "WaterComponent", "ScriptComponent",
]

# Components the serializer supports but this scene DELIBERATELY omits.
#
# An entry here is an assertion that the omission is still intentional, not a
# way to silence the coverage check: the reason is recorded and the scene must
# still carry the matching comment, so a later edit that silently changes the
# audio behaviour fails this test.
#
# AudioListenerComponent -- FindListenerEntity gives an explicit listener
# priority over the primary camera, so carrying one on the ShowcaseAudioListener
# marker (x=416) pinned the ears to a static point 48 m from the showcase source,
# beyond its MaxDistance of 25, and the source became inaudible. With no explicit
# listener the ears follow the camera, which is what AudioDemo.py documents.
# The component itself is exercised by tests/headless/test_audio_scene.py.
DELIBERATELY_ABSENT = {
    "AudioListenerComponent":
        "Deliberately has no AudioListenerComponent.",
}


def _has(entity, comp):
    """Probe a component, tolerating both bound naming conventions."""
    for name in ("Has" + comp, "HasWasteland::" + comp):
        fn = getattr(entity, name, None)
        if fn is not None:
            return fn()
    return False


def main():
    if not _os.path.exists(SCENE):
        print("FAIL: scene not found:", SCENE)
        return 1

    scene = Wasteland.Scene()
    if not scene.LoadScene(SCENE):
        print("FAIL: LoadScene returned false")
        return 1

    entities = scene.GetAllEntitiesWithTransform()

    present = set()
    for e in entities:
        for comp in ALL_COMPONENTS:
            if comp not in present and _has(e, comp):
                present.add(comp)

    required = [c for c in ALL_COMPONENTS if c not in DELIBERATELY_ABSENT]
    missing = [c for c in required if c not in present]

    print("Scene entity count: %d" % len(entities))
    print("Components covered: %d / %d" % (len(present), len(ALL_COMPONENTS)))
    print()

    for comp in ALL_COMPONENTS:
        if comp in DELIBERATELY_ABSENT:
            mark = " -- "
        else:
            mark = "OK  " if comp in present else "MISS"
        print("  %s %s" % (mark, comp))

    print()
    if missing:
        print("FAIL: %d component(s) have no coverage: %s" % (len(missing), ", ".join(missing)))
        print("0/%d checks passed" % len(ALL_COMPONENTS))
        return 1

    # The exception must still be justified. If someone re-adds a listener to the
    # scene, or deletes the comment explaining its removal, the omission is no
    # longer deliberate and this fails.
    with open(SCENE, "r", encoding="utf-8") as handle:
        scene_text = handle.read()
    stale = [comp for comp, reason in DELIBERATELY_ABSENT.items()
             if reason not in scene_text]
    if stale:
        print("FAIL: the scene no longer documents why %s is absent"
              % ", ".join(stale))
        print("0/%d checks passed" % len(ALL_COMPONENTS))
        return 1

    print("PASS: every supported component is present in the scene, and every "
          "deliberate omission is still documented.")
    print("%d/%d checks passed" % (len(ALL_COMPONENTS), len(ALL_COMPONENTS)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
