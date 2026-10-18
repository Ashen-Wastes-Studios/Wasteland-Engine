"""Reproduce the editor's Play transition: Scene::Copy must preserve every
entity and every component the renderer needs.

The editor's OnScenePlay does exactly `m_ActiveScene = Scene::Copy(m_EditorScene)`.
The editor viewport renders the ORIGINAL scene; Play renders the COPY. So if
Copy drops components, the buildings are still visible in the editor and vanish
the moment Play is pressed -- which is the reported symptom.

Run:  python tests/headless/test_play_copy.py
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


def _snapshot(scene):
    """Count entities and how many carry each component the renderer needs."""
    ents = scene.GetAllEntitiesWithTransform()
    counts = {
        "entities": len(ents),
        "transform": 0,
        "cube": 0,
        "material": 0,
        "cube_and_material": 0,
    }
    for e in ents:
        # Bound names carry the namespace prefix, e.g. "HasWasteland::TransformComponent".
        if getattr(e, "HasWasteland::TransformComponent")():
            counts["transform"] += 1
        if getattr(e, "HasWasteland::CubeRendererComponent")():
            counts["cube"] += 1
        if getattr(e, "HasWasteland::MaterialComponent")():
            counts["material"] += 1
        if getattr(e, "HasWasteland::CubeRendererComponent")() and getattr(e, "HasWasteland::MaterialComponent")():
            counts["cube_and_material"] += 1
    return counts


def main():
    if not _os.path.exists(SCENE):
        print("FAIL: scene not found:", SCENE)
        return 1

    scene = Wasteland.Scene()
    if not scene.LoadScene(SCENE):
        print("FAIL: LoadScene returned false")
        return 1

    before = _snapshot(scene)
    print("BEFORE (editor scene, what the viewport shows):")
    for k, v in before.items():
        print(f"   {k:18} {v}")

    copy = Wasteland.Scene.Copy(scene)
    after = _snapshot(copy)
    print("AFTER  (Scene::Copy, i.e. what Play renders):")
    for k, v in after.items():
        print(f"   {k:18} {v}")

    print()
    ok = True
    for k in before:
        if before[k] != after[k]:
            print(f"   MISMATCH {k}: {before[k]} -> {after[k]}")
            ok = False

    if ok:
        print("PASS: Scene::Copy preserved every entity and component.")
        return 0
    print("FAIL: Scene::Copy lost entities or components.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
