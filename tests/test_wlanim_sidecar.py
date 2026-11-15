"""C.5 -- the Blender exporter sidecar: .wlanim + wasteland_meta.json.

WHAT THIS COVERS
----------------
  * The axis conversion, which is the whole reason this task exists. Blender is
    Z-up, glTF and the engine are Y-up, and the conversion is a PERMUTATION with
    a sign -- not a per-axis sign flip. Getting that wrong shears an animation
    silently.
  * The sidecar builders and the hand-rolled YAML emitter.
  * THE ROUND TRIP THE PLAN ASKS FOR: a rigged cube is imported from a real
    .glb, a .wlanim sidecar is built from what the engine read out of it, the
    sidecar is written and loaded back, and the bone hierarchy and clip duration
    are asserted to match.

WHAT THIS DOES NOT COVER
------------------------
`wasteland_export.py` -- the Blender glue -- is NOT imported here and never has
been. It imports `bpy` at module level and Blender is not installed on this
machine. Every Blender-facing code path (the exporter operator, the f-curve
reading, the parameter introspection, the panel) is unexecuted. The plan's
stated verification includes a screenshot of the Blender export dialog; that
cannot exist here.

The sidecar conversion lives in `wasteland_sidecar.py` precisely so it IS
covered. If the Blender half is ever wrong, it will be wrong on top of a
verified foundation.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bin", "Release-windows-x86_64",
                                "DemonCore-Editor"))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "dcc"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "dcc", "maya"))

import Wasteland
import wasteland_sidecar as sidecar

# The Maya wrapper imports `maya.cmds` LAZILY, inside export_selection, so the
# module itself is importable without Maya -- which is what makes its
# Euler-to-quaternion conversion and its unit query testable here.
import wasteland_export as maya_export

# The rigged-cube fixture, built byte by byte in the C.2 test. Imported rather
# than duplicated: a second copy of a hand-built .glb would drift from the one
# the importer is tested against.
import test_gltf_import as gltf_fixture


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _import_rigged_cube():
    """The .glb fixture, imported. Returns (bone_names, bone_parents, clips)."""
    blob = gltf_fixture.build_cube_with_skeleton()
    json_text, binary = gltf_fixture._split_glb(blob)
    result = Wasteland.Mesh.ImportGltfFromText(json_text, binary)
    assert result.Error == "", result.Error
    return list(result.BoneNames), list(result.BoneParents), list(result.Clips)


def _bones_from_import(bone_names, bone_parents):
    """Bone index lists -> sidecar bones. Parents are INDICES from the glTF
    import and NAMES in the .wlanim format, so the translation is the whole job.
    """
    bones = []
    for index, name in enumerate(bone_names):
        parent_index = bone_parents[index]
        parent = bone_names[parent_index] if parent_index >= 0 else ""
        bones.append(sidecar.build_bone(name, parent))
    return bones


def _tracks_from_import(clip, bone_names):
    """glTF clip channels -> sidecar tracks.

    The glTF clip is a flat list of channels sharing one key-time array; the
    sidecar is per-bone tracks each carrying per-axis curves. The regrouping is
    the shape change a DCC export also has to make.

    `channel.Bone` is a bone INDEX and the sidecar's `bone:` field is a NAME --
    the format uses stable string keys everywhere so a re-export that reorders
    bones still binds. Passing the index through would produce a track naming a
    bone that does not exist.
    """
    tracks = {}
    for channel_index, channel in enumerate(clip.Channels):
        bone = bone_names[channel.Bone]
        values = clip.Values[channel_index]
        times = clip.KeyTimes
        per_axis = [[], [], []]
        for key_index, time in enumerate(times):
            value = list(values[key_index])
            for axis in range(3):
                per_axis[axis].append((time, value[axis], "LINEAR"))
        tracks.setdefault(bone, {}).update(
            sidecar.convert_position_channels(per_axis))
    return [sidecar.build_track(bone, channels)
            for bone, channels in tracks.items()]


def _write_asset(asset, path):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(sidecar.emit_asset(asset))


# ---------------------------------------------------------------------------
# 1. the axis conversion
# ---------------------------------------------------------------------------

def test_the_vector_conversion_is_a_permutation_with_a_sign():
    """(x, y, z)_blender -> (x, -z, y)_gltf.

    The sign is the part that makes this a rotation rather than a mirror: a
    mirror would flip handedness and every cross product in the rig.
    """
    assert sidecar.convert_vector_blender_to_gltf([1.0, 2.0, 3.0]) == \
        [1.0, -3.0, 2.0]
    assert sidecar.convert_vector_blender_to_gltf([0.0, 0.0, 0.0]) == \
        [0.0, 0.0, 0.0]


def test_the_vector_conversion_preserves_length():
    """A rotation must not scale anything. This is the cheapest possible check
    that the permutation is orthogonal."""
    import math
    for value in ([1.0, 2.0, 3.0], [0.5, -0.25, 4.0], [-1.0, 0.0, 0.0]):
        before = math.sqrt(sum(component * component for component in value))
        after = math.sqrt(sum(component * component
                              for component in sidecar
                              .convert_vector_blender_to_gltf(value)))
        assert abs(before - after) < 1e-6, (value, before, after)


def test_the_quaternion_conversion_carries_the_axis_into_the_new_basis():
    """The worked example from the module docstring.

    Blender's +90 deg about Z takes +X to +Y. In glTF space Blender's +Y is
    glTF's +Z, so the same rotation must take +X to +Z -- a -90 deg about glTF's
    Y. q is w-first.
    """
    import math
    half = math.sin(math.pi / 4.0)
    converted = sidecar.convert_quaternion_blender_to_gltf(
        [math.cos(math.pi / 4.0), 0.0, 0.0, half])
    # A -90 deg about +Y is (cos45, 0, -sin45, 0), w-first.
    assert abs(converted[0] - math.cos(math.pi / 4.0)) < 1e-6
    assert abs(converted[1]) < 1e-6
    assert abs(converted[2] + half) < 1e-6, converted
    assert abs(converted[3]) < 1e-6


def test_the_quaternion_conversion_leaves_identity_alone():
    """The identity rotation is the same in every basis, so a conversion that
    moved it would be wrong on every bone at rest."""
    converted = sidecar.convert_quaternion_blender_to_gltf([1.0, 0.0, 0.0, 0.0])
    assert abs(converted[0] - 1.0) < 1e-6
    assert abs(converted[1]) < 1e-6
    assert abs(converted[2]) < 1e-6
    assert abs(converted[3]) < 1e-6


def test_the_quaternion_conversion_preserves_norm():
    """A quaternion that stops being unit length is no longer a rotation, and
    the engine would normalise it silently -- losing the authored angle."""
    import math
    quaternion = [math.cos(math.pi / 6.0), 0.0, math.sin(math.pi / 6.0), 0.0]
    converted = sidecar.convert_quaternion_blender_to_gltf(quaternion)
    norm = math.sqrt(sum(component * component for component in converted))
    assert abs(norm - 1.0) < 1e-6, norm


def test_a_position_channel_set_swaps_the_axes():
    """glTF position_y carries Blender's NEGATED z and position_z carries
    Blender's y. Converting each channel independently would leave them
    swapped, which is the bug this function exists to prevent."""
    per_axis = [
        [(0.0, 1.0, "LINEAR"), (1.0, 2.0, "LINEAR")],   # blender x
        [(0.0, 3.0, "LINEAR"), (1.0, 4.0, "LINEAR")],   # blender y
        [(0.0, 5.0, "LINEAR"), (1.0, 6.0, "LINEAR")],   # blender z
    ]
    channels = sidecar.convert_position_channels(per_axis)
    assert [key["v"] for key in channels["position_x"]] == [1.0, 2.0], \
        "gltf x must carry blender x unchanged"
    assert [key["v"] for key in channels["position_y"]] == [-5.0, -6.0], \
        "gltf y must carry NEGATED blender z"
    assert [key["v"] for key in channels["position_z"]] == [3.0, 4.0], \
        "gltf z must carry blender y"


def test_a_missing_axis_is_omitted_not_written_empty():
    """The engine's format writes only the channels that were authored. An
    empty channel would read back as a track that does nothing and look like a
    bug rather than an absence."""
    per_axis = [
        [(0.0, 1.0, "LINEAR")],
        [],
        [(0.0, 5.0, "LINEAR")],
    ]
    channels = sidecar.convert_position_channels(per_axis)
    assert set(channels) == {"position_x", "position_y"}, \
        "blender y is gltf z, so omitting it omits position_z"


# ---------------------------------------------------------------------------
# 2. the builders and the emitter
# ---------------------------------------------------------------------------

def test_a_root_bone_parent_is_an_empty_string_not_null():
    """YAML null reads back as an ABSENT key in the engine, which is
    indistinguishable from a bone that legitimately has no parent. The empty
    string is the format's explicit "no parent"."""
    bone = sidecar.build_bone("Root")
    assert bone["parent"] == ""
    text = sidecar.emit_asset(sidecar.build_asset("Rig", [bone], []))
    assert 'parent: ""' in text, text


def test_an_asset_without_a_state_machine_omits_the_block():
    asset = sidecar.build_asset("Rig", [sidecar.build_bone("Root")], [])
    assert "state_machine" not in asset
    assert "state_machine" not in sidecar.emit_asset(asset)


def test_an_asset_with_states_includes_the_block():
    asset = sidecar.build_asset(
        "Rig", [sidecar.build_bone("Root")], [],
        states=[{"name": "Idle", "clip": "Idle"}],
        transitions=[{"from": "Idle", "to": "Idle", "trigger": "None"}])
    assert "state_machine" in asset
    text = sidecar.emit_asset(asset)
    assert "state_machine:" in text


def test_the_emitter_writes_valid_yaml():
    """Validated by the ENGINE, not by a YAML library -- the test interpreter
    has no PyYAML, and the engine's parser is what actually has to read this."""
    asset = sidecar.build_asset(
        "Rig",
        [sidecar.build_bone("Root"), sidecar.build_bone("Child", "Root")],
        [sidecar.build_clip("Idle", [
            sidecar.build_track("Child", {
                "position_x": [sidecar.build_keyframe(0.0, 0.0),
                               sidecar.build_keyframe(1.0, 2.0)],
                "rotation": [sidecar.build_keyframe(0.0, [1.0, 0.0, 0.0, 0.0])],
            })
        ])],
        states=[{"name": "Idle", "clip": "Idle"}],
        transitions=[])
    text = sidecar.emit_asset(asset)

    loaded = Wasteland.Animation.AnimationAsset()
    error = Wasteland.Animation.LoadAnimation  # noqa: F841  (existence check)
    result = Wasteland.Animation.ParseAnimation(text, loaded)
    assert result is None, f"the engine rejected the emitted YAML: {result}"

    assert loaded.Name == "Rig"
    assert loaded.Skeleton.GetBoneCount() == 2
    assert loaded.Skeleton.GetBoneName(0) == "Root"
    assert loaded.Skeleton.GetBoneName(1) == "Child"
    assert loaded.Clips[0].GetName() == "Idle"


def test_the_emitter_preserves_float_precision():
    """A rounded key value is a different animation. 0.02 must not become
    0.0199999996 -- that is the engine's own emitter losing precision, and a
    sidecar should not repeat it."""
    asset = sidecar.build_asset("Rig", [sidecar.build_bone("Root")], [
        sidecar.build_clip("Idle", [
            sidecar.build_track("Root", {
                "position_y": [sidecar.build_keyframe(0.0, 0.02)],
            })
        ])
    ])
    text = sidecar.emit_asset(asset)
    assert "0.02" in text, text

    loaded = Wasteland.Animation.AnimationAsset()
    assert Wasteland.Animation.ParseAnimation(text, loaded) is None
    # Duration is derived from the last key; the value itself is read back
    # through the clip's tracks, which the Python API does not enumerate. The
    # textual assertion above is the precision check.
    assert abs(loaded.Clips[0].GetDuration() - 0.0) < 1e-6


def test_an_empty_sequence_is_flow_style():
    asset = sidecar.build_asset("Rig", [sidecar.build_bone("Root")], [])
    text = sidecar.emit_asset(asset)
    assert "rest_pose: []" in text, text


def test_an_empty_mapping_is_flow_style():
    asset = sidecar.build_asset("Rig", [sidecar.build_bone("Root")], [])
    asset["skeleton"]["bones"] = []
    text = sidecar.emit_asset(asset)
    assert "bones: []" in text, text


def test_a_meta_file_records_the_axis_convention():
    meta = sidecar.build_meta(
        source="blender", up_axis="+Y", forward_axis="-Z", unit_scale=1.0,
        exporter_version="1.0.0", gltf_path="hero.glb",
        sidecar_path="hero.wlanim")
    text = sidecar.emit_meta(meta)
    parsed = json.loads(text)
    assert parsed["up_axis"] == "+Y"
    assert parsed["forward_axis"] == "-Z"
    assert parsed["gltf"] == "hero.glb"
    assert parsed["sidecar"] == "hero.wlanim"


# ---------------------------------------------------------------------------
# 3. the round trip the plan asks for
# ---------------------------------------------------------------------------

def test_the_sidecar_hierarchy_matches_the_gltf_import():
    """The plan's verification: import a rigged cube, then assert the bone
    hierarchy matches. The sidecar is built from what the engine READ OUT of
    the .glb, so a mismatch here means the sidecar's index->name translation or
    the emitter's structure is wrong."""
    bone_names, bone_parents, _clips = _import_rigged_cube()
    bones = _bones_from_import(bone_names, bone_parents)
    asset = sidecar.build_asset("RiggedCube", bones, [])

    text = sidecar.emit_asset(asset)
    loaded = Wasteland.Animation.AnimationAsset()
    assert Wasteland.Animation.ParseAnimation(text, loaded) is None

    assert loaded.Skeleton.GetBoneCount() == len(bone_names)
    for index, name in enumerate(bone_names):
        assert loaded.Skeleton.GetBoneName(index) == name, \
            f"bone {index} is {loaded.Skeleton.GetBoneName(index)!r}, " \
            f"expected {name!r}"

    for index, parent_index in enumerate(bone_parents):
        actual = loaded.Skeleton.GetParent(index)
        if parent_index < 0:
            assert actual.Value < 0, f"bone {index} should be a root"
        else:
            assert actual.Value == parent_index, \
                f"bone {index} parent is {actual.Value}, expected {parent_index}"


def test_the_sidecar_clip_duration_matches_the_gltf_import():
    """The second half of the plan's verification. Duration is DERIVED from the
    last key by the engine, so a clip that loads with the right duration has
    the right keys at the right times."""
    bone_names, bone_parents, clips = _import_rigged_cube()
    assert len(clips) == 1

    tracks = _tracks_from_import(clips[0], bone_names)
    asset = sidecar.build_asset(
        "RiggedCube",
        _bones_from_import(bone_names, bone_parents),
        [sidecar.build_clip(clips[0].Name, tracks, looping=True)])

    text = sidecar.emit_asset(asset)
    loaded = Wasteland.Animation.AnimationAsset()
    assert Wasteland.Animation.ParseAnimation(text, loaded) is None

    assert len(loaded.Clips) == 1
    assert loaded.Clips[0].GetName() == clips[0].Name
    assert abs(loaded.Clips[0].GetDuration() - clips[0].Duration) < 1e-6, \
        f"sidecar duration {loaded.Clips[0].GetDuration()} != " \
        f"glTF duration {clips[0].Duration}"


def test_the_sidecar_carries_no_rest_pose():
    """Rest poses are deliberately omitted: the engine derives them from the
    .glb's skin, which is already Y-up. A second copy would be a second source
    of truth that can disagree with the skin."""
    text = sidecar.emit_asset(
        sidecar.build_asset("Rig", [sidecar.build_bone("Root")], []))
    assert "rest_pose" in text
    assert "rest_pose: []" in text


def test_the_whole_sidecar_round_trips_through_a_file():
    """Emit -> write -> LoadAnimation, the path the engine actually takes."""
    import tempfile
    bone_names, bone_parents, clips = _import_rigged_cube()
    asset = sidecar.build_asset(
        "RiggedCube",
        _bones_from_import(bone_names, bone_parents),
        [sidecar.build_clip(clips[0].Name, _tracks_from_import(clips[0], bone_names))])

    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "RiggedCube.wlanim")
        _write_asset(asset, path)
        assert os.path.isfile(path)

        loaded = Wasteland.Animation.AnimationAsset()
        result = Wasteland.Animation.LoadAnimation(path, loaded)
        assert result is None, \
            f"load failed: {result.Format('sidecar')}"

    assert loaded.Name == "RiggedCube"
    assert loaded.Skeleton.GetBoneCount() == len(bone_names)
    assert len(loaded.Clips) == 1


def test_a_sidecar_written_from_a_dcc_style_rig_loads():
    """A rig shaped like a real export -- a chain with a rotation track -- not
    just the fixture's translation clip."""
    bones = [
        sidecar.build_bone("Root"),
        sidecar.build_bone("Spine", "Root"),
        sidecar.build_bone("Head", "Spine"),
    ]
    tracks = [sidecar.build_track("Spine", {
        "rotation": [
            sidecar.build_keyframe(0.0, [1.0, 0.0, 0.0, 0.0]),
            sidecar.build_keyframe(0.5, [0.99955, 0.0, 0.0, 0.0299955]),
            sidecar.build_keyframe(1.0, [1.0, 0.0, 0.0, 0.0]),
        ],
        "position_y": [
            sidecar.build_keyframe(0.0, 0.0),
            sidecar.build_keyframe(1.0, 0.02),
        ],
    })]
    asset = sidecar.build_asset("Hero", bones, [
        sidecar.build_clip("Idle", tracks, looping=True)])

    text = sidecar.emit_asset(asset)
    loaded = Wasteland.Animation.AnimationAsset()
    assert Wasteland.Animation.ParseAnimation(text, loaded) is None

    assert loaded.Name == "Hero"
    assert loaded.Skeleton.GetBoneCount() == 3
    assert loaded.Skeleton.GetBoneName(2) == "Head"
    assert loaded.Skeleton.GetParent(2).Value == 1
    assert abs(loaded.Clips[0].GetDuration() - 1.0) < 1e-6
    assert loaded.Clips[0].IsLooping()


# ---------------------------------------------------------------------------
# 4. the Maya wrapper's pure-Python helpers
# ---------------------------------------------------------------------------
# These live in the Maya module but have no Maya dependency, so they are tested
# here rather than left to be discovered by an artist. The `maya.cmds` calls
# around them are NOT covered.

def test_maya_euler_to_quaternion_identity():
    quaternion = maya_export.euler_to_quaternion([0.0, 0.0, 0.0])
    assert abs(quaternion[0] - 1.0) < 1e-6
    assert abs(quaternion[1]) < 1e-6
    assert abs(quaternion[2]) < 1e-6
    assert abs(quaternion[3]) < 1e-6


def test_maya_euler_to_quaternion_a_quarter_turn_about_x():
    """Maya's rotate is DEGREES; the sidecar is a quaternion. 90 degrees about X
    is (cos45, sin45, 0, 0), w-first."""
    import math
    quaternion = maya_export.euler_to_quaternion([90.0, 0.0, 0.0])
    assert abs(quaternion[0] - math.cos(math.pi / 4.0)) < 1e-6
    assert abs(quaternion[1] - math.sin(math.pi / 4.0)) < 1e-6
    assert abs(quaternion[2]) < 1e-6
    assert abs(quaternion[3]) < 1e-6


def test_maya_euler_to_quaternion_preserves_norm():
    """A quaternion that stops being unit-length is no longer a rotation, and
    the engine would normalise it silently -- losing the authored angle."""
    import math
    for degrees in ([30.0, 45.0, 60.0], [-120.0, 15.0, 75.0],
                    [180.0, 0.0, 0.0], [0.0, 0.0, -270.0]):
        quaternion = maya_export.euler_to_quaternion(degrees)
        norm = math.sqrt(sum(component * component for component in quaternion))
        assert abs(norm - 1.0) < 1e-6, (degrees, norm)


def test_maya_the_rotation_order_changes_the_result():
    """Euler composition order matters. If the wrapper ignored `rotateOrder`
    and always used XYZ, a rig authored with a different order would animate
    about the wrong axes while still looking like a plausible animation."""
    degrees = [30.0, 45.0, 60.0]
    xyz = maya_export.euler_to_quaternion(degrees, "xyz")
    zyx = maya_export.euler_to_quaternion(degrees, "zyx")
    assert xyz != zyx, "XYZ and ZYX must differ for a non-trivial rotation"


def test_maya_an_unknown_rotation_order_falls_back_to_xyz():
    quaternion = maya_export.euler_to_quaternion([30.0, 45.0, 60.0], "bogus")
    assert quaternion == maya_export.euler_to_quaternion([30.0, 45.0, 60.0],
                                                         "xyz")


class _FakeCmds:
    """The slice of `maya.cmds` that `_metres_per_unit` touches."""

    def __init__(self, unit):
        self._unit = unit

    def currentUnit(self, query=False, linear=False):
        return self._unit


def test_maya_the_working_unit_is_read_not_assumed():
    """Maya's default is CENTIMETRES. Assuming metres would make every export
    100x too small; assuming nothing would make the meta file a lie."""
    factor, name = maya_export._metres_per_unit(_FakeCmds("cm"))
    assert abs(factor - 0.01) < 1e-9, (factor, name)

    factor, name = maya_export._metres_per_unit(_FakeCmds("m"))
    assert abs(factor - 1.0) < 1e-9

    factor, name = maya_export._metres_per_unit(_FakeCmds("mm"))
    assert abs(factor - 0.001) < 1e-9

    factor, name = maya_export._metres_per_unit(_FakeCmds("ft"))
    assert abs(factor - 0.3048) < 1e-9


def test_maya_an_unreadable_unit_falls_back_with_a_reason():
    """A query that raises must not crash the export; it falls back to the
    Maya default and SAYS SO in the meta file."""
    class _BrokenCmds:
        def currentUnit(self, query=False, linear=False):
            raise RuntimeError("no scene")

    factor, name = maya_export._metres_per_unit(_BrokenCmds())
    assert abs(factor - 0.01) < 1e-9
    assert "assumed" in name, name


def test_maya_the_sidecar_format_is_the_shared_one():
    """Both DCCs must write the same format through the same module. If the
    Maya path ever grew its own emitter, this is the assertion that would
    notice."""
    import inspect
    source = inspect.getsource(maya_export)
    assert "sidecar.emit_asset" in source
    assert "sidecar.build_asset" in source
    assert "wasteland_sidecar" in source


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    # One check per test, the same convention as tests/test_material_serializer.py
    # and the other standalone suites. The runner sums the "checks passed" line,
    # so a suite that omits it is reported as UNCOUNTED rather than as passing.
    checks = 0
    failed = 0
    for name, test in tests:
        try:
            test()
            print(f"  {name} ...")
        except AssertionError as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {error}")
        except Exception as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {type(error).__name__}: {error}")
        checks += 1

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{checks - failed}/{checks} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
