"""Source contract for retargeting (Task A.7).

`tests/headless/test_animation_retarget.py` proves the behaviour; this suite proves
the code is wired the way the plan requires. It reads the sources rather than
importing the module, so it runs against a stale or absent build and catches a
registration that was never made -- the class of mistake that otherwise shows up
only as an AttributeError at the end of a ten-minute build.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "Wasteland", "src", "Wasteland")
EDITOR = os.path.join(ROOT, "DemonCore-Editor", "src")

PASSED = 0
FAILED = []


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def read_engine(*parts):
    with open(os.path.join(ENGINE, *parts), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def read_repo(*parts):
    with open(os.path.join(ROOT, *parts), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def test_files_exist_and_are_registered():
    header = os.path.join(ENGINE, "Animation", "AnimationRetarget.h")
    source = os.path.join(ENGINE, "Animation", "AnimationRetarget.cpp")
    check(os.path.isfile(header), "AnimationRetarget.h exists")
    check(os.path.isfile(source), "AnimationRetarget.cpp exists")

    project = read_repo("Wasteland", "Wasteland.vcxproj")
    check("Animation\\AnimationRetarget.h" in project,
          "AnimationRetarget.h is registered in Wasteland.vcxproj")
    check("Animation\\AnimationRetarget.cpp" in project,
          "AnimationRetarget.cpp is registered in Wasteland.vcxproj")

    filters = read_repo("Wasteland", "Wasteland.vcxproj.filters")
    check("AnimationRetarget.h" in filters,
          "AnimationRetarget.h is in the .filters file (Solution Explorer)")
    check("AnimationRetarget.cpp" in filters,
          "AnimationRetarget.cpp is in the .filters file")


def test_mapping_type_is_reachable_from_the_asset():
    """AnimationAsset stores AnimationRetarget::Mapping, so the header must be
    included -- a forward declaration cannot name a nested type."""
    header = read_engine("Animation", "AnimationSerializer.h")
    check('#include "Wasteland/Animation/AnimationRetarget.h"' in header,
          "AnimationSerializer.h includes AnimationRetarget.h")

    # And the asset actually carries the two fields.
    check("std::vector<AnimationRetarget::Mapping> Retarget;" in header,
          "AnimationAsset::Retarget is a vector of the nested Mapping type")
    check("float RetargetPositionScale = 1.0f;" in header,
          "AnimationAsset::RetargetPositionScale defaults to 1.0")

    # No include cycle: neither dependency may reach back into the serializer.
    retarget_h = read_engine("Animation", "AnimationRetarget.h")
    check("AnimationSerializer.h" not in retarget_h,
          "AnimationRetarget.h does not include the serializer (no cycle)")


def test_serialiser_handles_the_section():
    source = read_engine("Animation", "AnimationSerializer.cpp")

    check('key == "retarget"' in source,
          "'retarget' is a known root key, so a typo is reported not ignored")
    check('out << YAML::Key << "retarget"' in source,
          "the retarget section is emitted")
    check('"position_scale"' in source, "the scale is emitted")
    check('"retarget.bones must be a sequence"' in source,
          "a non-sequence retarget.bones is an error")
    check('"a retarget mapping needs both \'from\' and \'to\'"' in source,
          "a mapping missing a side is an error")
    check('"a retarget mapping has an empty bone name"' in source,
          "an empty bone name is an error")

    # The emitted order must not follow the vector's order, which carries no
    # meaning -- an unsorted emission rewrites the file on every save.
    check("std::vector<AnimationRetarget::Mapping> sorted(asset.Retarget.begin()" in source,
          "the mapping is sorted before emission")

    # Emitted only when non-empty, matching the `editor:` section's rule.
    check('if (!asset.Retarget.empty())' in source,
          "the retarget section is emitted only when the asset has a mapping")


def test_retarget_uses_bone_names_not_indices():
    """The whole feature is name-based; an index-keyed retarget would silently
    animate the wrong bone on a rig whose bones are ordered differently."""
    source = read_engine("Animation", "AnimationRetarget.cpp")

    check("NormaliseBoneName" in source, "names are normalised before comparison")
    check("GetBoneName" in source, "the implementation resolves bone NAMES")
    check("targetIndex" in source, "tracks are re-keyed through a name->index table")

    # BoneIndex's member is Value, not Index. Using .Index does not compile, and
    # this pins the correct spelling so a future edit does not reach for it.
    check(".Index" not in source,
          "the implementation does not use the non-existent BoneIndex::Index")


def test_proportional_scale_is_finite():
    """A height ratio that divides by zero would propagate inf/nan into every
    translation of the retargeted clip."""
    source = read_engine("Animation", "AnimationRetarget.cpp")
    check("sourceH <= 0.0f || targetH <= 0.0f" in source,
          "the height ratio guards against a zero denominator")
    check("return 1.0f;" in source,
          "the guard returns 1.0 (a pure re-key), not inf or nan")


def test_rotations_are_not_scaled():
    """There is no meaningful proportional rotation; only translations are scaled."""
    source = read_engine("Animation", "AnimationRetarget.cpp")
    check("result.AddRotationKey(targetBone, key.Time, key.Value, key.Mode)" in source,
          "rotation keys are copied with their value unchanged")


def test_bindings_expose_the_api():
    bindings = read_engine("Scripting", "Bindings.cpp")

    check('"AnimationRetarget"' in bindings, "AnimationRetarget is bound")
    check('"RetargetMapping"' in bindings, "the Mapping type is bound")
    for method in ("BuildMap", "HeightRatio", "RetargetClip",
                   "SourceBoneForTarget", "NormaliseBoneName"):
        check('"%s"' % method in bindings,
              "AnimationRetarget.%s is bound" % method)

    # The container-copy hazard: def_readwrite on a vector hands Python a copy, so
    # the write path must be accessors.
    check('"AddRetargetMapping"' in bindings, "AddRetargetMapping is bound")
    check('"RemoveRetargetMapping"' in bindings, "RemoveRetargetMapping is bound")
    check('"GetRetargetCount"' in bindings, "GetRetargetCount is bound")
    check('"GetRetargetSourceBone"' in bindings, "GetRetargetSourceBone is bound")
    check('"GetRetargetTargetBone"' in bindings, "GetRetargetTargetBone is bound")
    check('"ClearRetarget"' in bindings, "ClearRetarget is bound")
    check('"RetargetPositionScale"' in bindings,
          "the asset's scale is readable from Python")


def test_headless_suite_exists():
    suite = os.path.join(ROOT, "tests", "headless", "test_animation_retarget.py")
    check(os.path.isfile(suite), "the headless retarget suite exists")

    text = read_repo("tests", "headless", "test_animation_retarget.py")
    # The plan's verification: the extra bone holds its rest transform.
    check("holds its rest translation" in text,
          "the suite asserts the unmapped bone holds its rest translation")
    check("rotation is not scaled" in text,
          "the suite asserts rotations are not scaled")


def main():
    test_files_exist_and_are_registered()
    test_mapping_type_is_reachable_from_the_asset()
    test_serialiser_handles_the_section()
    test_retarget_uses_bone_names_not_indices()
    test_proportional_scale_is_finite()
    test_rotations_are_not_scaled()
    test_bindings_expose_the_api()
    test_headless_suite_exists()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    if FAILED:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
