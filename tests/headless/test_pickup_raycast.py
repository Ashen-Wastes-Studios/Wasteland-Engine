"""Verify the pickup interaction actually works.

The pickup popup is driven by Scene::Raycast, which is a PHYSICS query
(Box3D against m_Physics3DWorld). An entity is only hittable if it owns a
physics shape, which means a RigidBody3DComponent plus a collider. The five
pickups shipped with renderers but no colliders, so the ray passed straight
through them, InteractableComponent.Label was never written, and the C++ HUD
had nothing to draw.

This test starts the 3D physics world, then raycasts from a player's eye
height at each pickup. It fails if any pickup cannot be hit.

Run:  python tests/headless/test_pickup_raycast.py
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

# Tag -> world position, matching the scene file.
PICKUPS = {
    "HealthPotion": (-100.0, 0.5, 1100.0),
    "AmmoBox": (80.0, 0.5, 1000.0),
    "SwordPickup": (150.0, 0.5, 900.0),
    "GoldBar": (-60.0, 0.3, 800.0),
    "Bandage": (100.0, 0.2, 700.0),
}

EYE_HEIGHT = 1.7
STANDOFF = 2.0


def main():
    if not _os.path.exists(SCENE):
        print("FAIL: scene not found:", SCENE)
        return 1

    scene = Wasteland.Scene()
    if not scene.LoadScene(SCENE):
        print("FAIL: LoadScene returned false")
        return 1

    # Physics bodies are only built once the 3D world exists.
    scene.OnRuntimeStart()
    for _ in range(5):
        scene.StepPhysics(0.016)

    ok = True
    for tag, pos in PICKUPS.items():
        origin = Wasteland.Vec3(pos[0], EYE_HEIGHT, pos[2] + STANDOFF)
        direction = Wasteland.Vec3(
            pos[0] - origin.x,
            pos[1] - origin.y,
            pos[2] - origin.z,
        )

        hit, entity, _point, _normal, distance, hittable = scene.Raycast(
            origin, direction, 3.0
        )

        interactable = entity.TryGetInteractableComponent()
        has_interactable = interactable is not None

        good = hit and has_interactable
        ok = ok and good
        print(
            "  %-14s hit=%-5s hittable=%-5s dist=%-6.2f interactable=%-5s %s"
            % (tag, hit, hittable, distance, has_interactable, "OK" if good else "MISS")
        )

    print()
    if ok:
        print("PASS: every pickup is raycast-hittable.")
        return 0
    print("FAIL: at least one pickup could not be hit.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
