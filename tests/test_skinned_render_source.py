"""Source contract for the skinned render path (Task A.3).

The deformation maths is asserted numerically in
tests/headless/test_skinned_mesh_component.py. What a headless process cannot
reach is the GPU half, so this pins the wiring that would otherwise fail
silently: an entity that skins correctly in a test but is never drawn.

Each check names the failure it prevents.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "Wasteland", "src", "Wasteland")

PASSED = 0
FAILED = []


def read(*parts):
    with open(os.path.join(SRC, *parts), encoding="utf-8", errors="replace") as f:
        return f.read()


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def main():
    scene = read("Scene", "Scene.cpp")
    r3d_h = read("Renderer", "Renderer3D.h")
    r3d = read("Renderer", "Renderer3D.cpp")
    system = read("Scene", "AnimationSystem.cpp")
    serializer = read("Animation", "AnimationSerializer.cpp")

    # 1. The renderer entry point exists and is declared where callers look.
    check("DrawSkinnedMesh" in r3d_h, "Renderer3D.h declares DrawSkinnedMesh")
    check(re.search(r"void\s+Renderer3D::DrawSkinnedMesh", r3d),
          "Renderer3D.cpp defines DrawSkinnedMesh")

    # 2. It must reach the SAME batch as every other primitive, via the shared
    #    function -- a private second batch would be a second thing to flush.
    #
    #    The chain is TWO hops since C.3: DrawSkinnedMesh delegates to DrawMesh
    #    (one body, two names) and DrawMesh calls DrawProceduralPrimitive. The
    #    property under test is unchanged -- the chain terminates at the shared
    #    function and at nothing else -- but checking only the first hop would
    #    pass just as happily on a private second batch.
    skinned = re.search(r"void\s+Renderer3D::DrawSkinnedMesh.*?\n    \}", r3d, re.S)
    check(skinned is not None, "DrawSkinnedMesh body is present")
    if skinned is not None:
        check("DrawMesh" in skinned.group(0),
              "DrawSkinnedMesh delegates to DrawMesh (one body, two names)")
    static_mesh = re.search(r"void\s+Renderer3D::DrawMesh.*?\n    \}", r3d, re.S)
    check(static_mesh is not None and "DrawProceduralPrimitive" in static_mesh.group(0),
          "DrawMesh submits through DrawProceduralPrimitive")

    # 3. The Nova guard: a negative shape must skip the ray-tracing instance, or a
    #    phantom analytic primitive appears at every character's origin.
    check(re.search(r"RayTracingEnabled\s*\|\|\s*shape\s*<\s*0", r3d),
          "DrawProceduralPrimitive skips the tracer for shape < 0")
    check(re.search(r"DrawProceduralPrimitive\(transform,\s*mesh,\s*material\.Albedo,\s*material,\s*-1",
                    r3d),
          "the raster-only shape -1 is still passed")

    # 4. The scene submits it, in the shared function (runtime AND editor).
    submit = re.search(r"void Scene::SubmitMeshPrimitives\(\).*?\n\t\}\n", scene, re.S)
    check(submit is not None and "DrawSkinnedMesh" in submit.group(0),
          "SubmitMeshPrimitives submits skinned meshes (so editor and runtime both draw)")
    check(submit is not None and
          "view<TransformComponent, AnimationComponent, MaterialComponent>" in submit.group(0),
          "the skinned view requires a MaterialComponent like every other shape")
    check(submit is not None and "GetDeformedMesh" in submit.group(0),
          "the scene draws the deformed mesh, not the bind-pose geometry")

    # 5. The runtime deforms every tick and rebuilds normals.
    check("DeformRuntime" in system and "ComputeSkinningMatrices" in system,
          "AnimationSystem computes skinning matrices each tick")
    check("RebuildNormals" in system,
          "normals are rebuilt from deformed positions (Skin moves positions only)")

    # 6. The 64-bone palette cap from the plan.
    check(re.search(r"GetBoneCount\(\)\s*>\s*64", system),
          "a rig over 64 bones is refused")

    # 7. A loaded rig must have its bind matrices captured, or the whole rest
    #    offset is applied on top of the mesh.
    check("CaptureBindMatrices" in serializer,
          ".wlanim load captures bind matrices")
    cap = serializer.find("CaptureBindMatrices")
    rest = serializer.find("SetRestPose")
    check(rest != -1 and cap > rest,
          "bind matrices are captured AFTER the rest pose is set (they read it)")

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
