"""Task C.2: the glTF 2.0 importer.

The plan's verification: import a Blender-exported cube with a skeleton and
one clip; assert vertex count, bone count, key count, and that the clip's
duration matches the source.

No Blender on this machine, so the asset is BUILT here to the Khronos
specification -- a .glb with a JSON chunk and a BIN chunk, a two-joint
skeleton, and one translation clip. That is a stronger test than parsing a
sample file: every byte is chosen, so a mismatch is a bug in the importer
rather than a quirk of one exporter.
"""
import json
import os
import struct
import sys

_ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
_SCRIPTS = os.path.join(_ROOT, "DemonCore-Editor", "assets", "scripts")
for _path in (_MODULE_DIR, _SCRIPTS):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import Wasteland  # noqa: E402

Mesh = Wasteland.Mesh

FAILED = []
PASSED = 0


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)
        print("    FAIL:", message)


def check_eq(actual, expected, message):
    check(actual == expected, "%s (got %r, want %r)" % (message, actual, expected))


# ---------------------------------------------------------------------------
# A .glb builder.
#
# Written by hand because the point is to control every byte. A helper library
# would hide the very details the importer is being tested on.
# ---------------------------------------------------------------------------

class GlbBuilder:
    def __init__(self):
        self.binary = bytearray()
        self.buffer_views = []
        self.accessors = []

    def add_accessor(self, values, component_type, type_name):
        """Appends raw values to the BIN chunk and returns an accessor index."""
        if component_type == 5126:  # FLOAT
            raw = struct.pack("<%df" % len(values), *values)
        elif component_type == 5123:  # UNSIGNED_SHORT
            raw = struct.pack("<%dH" % len(values), *values)
        elif component_type == 5125:  # UNSIGNED_INT
            raw = struct.pack("<%dI" % len(values), *values)
        else:
            raise ValueError("unsupported component type %d" % component_type)

        offset = len(self.binary)
        self.binary.extend(raw)
        # glTF requires bufferViews to start on a 4-byte boundary.
        while len(self.binary) % 4 != 0:
            self.binary.append(0)

        self.buffer_views.append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(raw),
        })
        self.accessors.append({
            "bufferView": len(self.buffer_views) - 1,
            "componentType": component_type,
            "count": len(values) // self._components(type_name),
            "type": type_name,
        })
        return len(self.accessors) - 1

    @staticmethod
    def _components(type_name):
        return {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[type_name]

    def build(self, gltf):
        gltf["buffers"] = [{"byteLength": len(self.binary)}]
        gltf["bufferViews"] = self.buffer_views
        gltf["accessors"] = self.accessors

        json_chunk = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        while len(json_chunk) % 4 != 0:
            json_chunk += b" "
        bin_chunk = bytes(self.binary)
        while len(bin_chunk) % 4 != 0:
            bin_chunk += b"\x00"

        total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
        header = struct.pack("<III", 0x46546C67, 2, total)
        json_header = struct.pack("<II", len(json_chunk), 0x4E4F534A)
        bin_header = struct.pack("<II", len(bin_chunk), 0x004E4942)
        return header + json_header + json_chunk + bin_header + bin_chunk


def build_cube_with_skeleton():
    """A 1x1x1 cube, a two-joint chain, and one translation clip."""
    builder = GlbBuilder()

    # Eight corners of a unit cube centred on the origin.
    positions = [
        -0.5, -0.5, -0.5,
         0.5, -0.5, -0.5,
         0.5,  0.5, -0.5,
        -0.5,  0.5, -0.5,
        -0.5, -0.5,  0.5,
         0.5, -0.5,  0.5,
         0.5,  0.5,  0.5,
        -0.5,  0.5,  0.5,
    ]
    normals = [
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0,
        0.0, 0.0, -1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
    ]
    # 12 triangles, 36 indices.
    indices = [
        0, 2, 1, 0, 3, 2,  # back
        4, 5, 6, 4, 6, 7,  # front
        0, 1, 5, 0, 5, 4,  # bottom
        3, 7, 6, 3, 6, 2,  # top
        0, 4, 7, 0, 7, 3,  # left
        1, 2, 6, 1, 6, 5,  # right
    ]

    position_accessor = builder.add_accessor(positions, 5126, "VEC3")
    normal_accessor = builder.add_accessor(normals, 5126, "VEC3")
    index_accessor = builder.add_accessor(indices, 5123, "SCALAR")

    # Two bones: a root and its child.
    # Inverse bind matrices are identity -- the joints are at the origin.
    ibm = [1.0, 0.0, 0.0, 0.0,
           0.0, 1.0, 0.0, 0.0,
           0.0, 0.0, 1.0, 0.0,
           0.0, 0.0, 0.0, 1.0] * 2
    ibm_accessor = builder.add_accessor(ibm, 5126, "MAT4")

    # One translation clip: 3 keys at t = 0, 0.5, 1.0, moving +X.
    times = [0.0, 0.5, 1.0]
    values = [0.0, 0.0, 0.0,
              1.0, 0.0, 0.0,
              2.0, 0.0, 0.0]
    time_accessor = builder.add_accessor(times, 5126, "SCALAR")
    value_accessor = builder.add_accessor(values, 5126, "VEC3")

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "Root", "children": [1]},
            {"name": "Child"},
        ],
        "meshes": [{
            "name": "Cube",
            "primitives": [{
                "attributes": {
                    "POSITION": position_accessor,
                    "NORMAL": normal_accessor,
                },
                "indices": index_accessor,
                "mode": 4,
            }],
        }],
        "skins": [{
            "name": "CubeSkin",
            "joints": [0, 1],
            "inverseBindMatrices": ibm_accessor,
        }],
        "animations": [{
            "name": "SlideRight",
            "channels": [{
                "sampler": 0,
                "target": {"node": 1, "path": "translation"},
            }],
            "samplers": [{
                "input": time_accessor,
                "output": value_accessor,
                "interpolation": "LINEAR",
            }],
        }],
    }
    return builder.build(gltf)


def _split_glb(blob):
    """Splits a .glb into (json_text, bin_chunk), mirroring ImportGltf's own
    container handling. The test needs the parts because it calls the
    text-level entry point rather than writing a file."""
    magic, _version, _length = struct.unpack_from("<III", blob, 0)
    if magic != 0x46546C67:
        raise ValueError("not a .glb")
    offset = 12
    json_text = ""
    binary = b""
    while offset + 8 <= len(blob):
        chunk_length, chunk_type = struct.unpack_from("<II", blob, offset)
        offset += 8
        data = blob[offset:offset + chunk_length]
        if chunk_type == 0x4E4F534A:
            json_text = data.decode("utf-8")
        elif chunk_type == 0x004E4942:
            binary = data
        offset += chunk_length
    return json_text, binary


def _import_glb():
    return Mesh.ImportGltfFromText(*_split_glb(build_cube_with_skeleton()), ".")


def test_the_cube_imports_with_eight_unique_vertices():
    """Weld collapses the 36 de-indexed corners to 8 real vertices."""
    result = _import_glb()
    check(result.Ok(), "the asset must import: %s" % result.Error)
    if not result.Ok():
        return
    check_eq(result.Mesh.VertexCount, 8,
             "a cube has 8 unique positions after welding")
    check_eq(result.Mesh.FaceCount, 12, "a cube has 12 triangles")
    check_eq(result.Mesh.EdgeCount, 36, "12 triangles have 36 half-edges")


def test_the_skeleton_imports_in_bone_order():
    result = _import_glb()
    if not result.Ok():
        check(False, "import failed: %s" % result.Error)
        return
    check_eq(len(result.BoneNames), 2, "two joints are declared")
    check_eq(list(result.BoneNames), ["Root", "Child"],
             "bone names come from the node names, in joints[] order")
    check_eq(list(result.BoneParents), [-1, 0],
             "the child's parent is the root, resolved from nodes[].children")
    check_eq(len(result.InverseBindMatrices), 32,
             "2 bones x 16 floats of inverse bind matrices")


def test_the_clip_duration_matches_the_source():
    """The clip must run for exactly the last key time."""
    result = _import_glb()
    if not result.Ok():
        check(False, "import failed: %s" % result.Error)
        return
    check_eq(len(result.Clips), 1, "one animation is declared")
    clip = result.Clips[0]
    check_eq(clip.Name, "SlideRight", "the clip keeps its name")
    check(abs(clip.Duration - 1.0) < 1e-6,
          "the clip duration is the last key time (got %r)" % clip.Duration)
    check_eq(len(clip.KeyTimes), 3, "three key times")
    check_eq(len(clip.Channels), 1, "one channel")
    channel = clip.Channels[0]
    check_eq(channel.Path, "translation", "the channel targets translation")
    check_eq(channel.Bone, 1,
             "the channel targets node 1, which is bone index 1")
    check_eq(len(clip.Values), 1, "one value curve")
    check_eq(len(clip.Values[0]), 3, "three keys on that curve")
    first = clip.Values[0][0]
    last = clip.Values[0][2]
    check_eq(list(first), [0.0, 0.0, 0.0], "the first key is at the origin")
    check_eq(list(last), [2.0, 0.0, 0.0], "the last key has moved +2 on X")


def test_normals_survive_the_round_trip():
    """A glTF NORMAL accessor must reach the welded mesh unchanged."""
    result = _import_glb()
    if not result.Ok():
        check(False, "import failed: %s" % result.Error)
        return
    back = result.Mesh.Deindex()
    check_eq(len(back.Vertices), 36,
             "de-indexing restores one corner per triangle corner")
    # The first corner is on the back face, whose normal is -Z.
    normal = back.Vertices[0].Normal
    check(abs(normal.z + 1.0) < 1e-5,
          "the first corner keeps its -Z normal (got %r)"
          % ((normal.x, normal.y, normal.z),))


def test_a_missing_position_is_a_named_error():
    """A malformed asset must fail with a message that names the defect."""
    builder = GlbBuilder()
    builder.add_accessor([0.0, 0.0, 0.0], 5126, "VEC3")
    gltf = {
        "asset": {"version": "2.0"},
        "meshes": [{"primitives": [{"attributes": {}}]}],
        # Present but empty, so the importer reaches the attributes check
        # instead of failing on the missing accessors array first.
        "accessors": [],
    }
    result = Mesh.ImportGltfFromText(
        json.dumps(gltf), bytes(builder.binary), ".")
    check(not result.Ok(), "a primitive with no POSITION must fail")
    check("POSITION" in result.Error,
          "the error must name POSITION (got %r)" % result.Error)


def test_an_out_of_range_index_is_a_named_error():
    builder = GlbBuilder()
    positions = builder.add_accessor(
        [-0.5, -0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, -0.5], 5126, "VEC3")
    indices = builder.add_accessor([0, 1, 9], 5123, "SCALAR")
    gltf = {
        "asset": {"version": "2.0"},
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": positions},
            "indices": indices,
        }]}],
        "accessors": builder.accessors,
        "bufferViews": builder.buffer_views,
        "buffers": [{"byteLength": len(builder.binary)}],
    }
    result = Mesh.ImportGltfFromText(
        json.dumps(gltf), bytes(builder.binary), ".")
    check(not result.Ok(), "an index past the vertex count must fail")
    check("out of range" in result.Error,
          "the error must say the index is out of range (got %r)" % result.Error)


def test_malformed_json_reports_a_line_and_column():
    result = Mesh.ImportGltfFromText("{ not json", "", ".")
    check(not result.Ok(), "malformed JSON must fail")
    check("line" in result.Error and "column" in result.Error,
          "the error must carry a position (got %r)" % result.Error)


def test_an_unsupported_accessor_type_is_rejected_not_ignored():
    """Draco/morph territory: a MAT2 accessor is legal glTF but out of scope."""
    builder = GlbBuilder()
    builder.accessors.append({
        "bufferView": 0,
        "componentType": 5126,
        "count": 1,
        "type": "MAT2",
    })
    builder.buffer_views.append({"buffer": 0, "byteOffset": 0, "byteLength": 16})
    gltf = {
        "asset": {"version": "2.0"},
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0},
        }]}],
        # The builder fills these in only through build(), which this test
        # deliberately bypasses to craft an out-of-scope accessor.
        "accessors": builder.accessors,
        "bufferViews": builder.buffer_views,
        "buffers": [{"byteLength": len(builder.binary)}],
    }
    result = Mesh.ImportGltfFromText(
        json.dumps(gltf), bytes(builder.binary), ".")
    check(not result.Ok(), "an unsupported accessor type must be rejected")
    check("MAT2" in result.Error,
          "the error must name the type (got %r)" % result.Error)


def test_json_parser_handles_escapes_and_unicode():
    """The parser is tested directly: an import error and a JSON error look
    the same from the outside."""
    ok, value, error = Mesh.JsonParse(
        '{"name": "caf\\u00e9 \\"quoted\\"", "count": 2, "ratio": 1.5, '
        '"flag": true, "nothing": null}')
    check(ok, "the document must parse: %s" % error)
    if not ok:
        return
    check_eq(value.Find("name").AsString(), 'café "quoted"',
             "escapes and \\uXXXX decode correctly")
    # IsNumber() is asserted explicitly: a parser that writes the value but
    # not the type leaves IsNumber() false while AsNumber() still returns the
    # number, and every type-guarded caller silently rejects valid data.
    check(value.Find("count").IsNumber(),
          "a JSON number must be typed as a number")
    check_eq(value.Find("count").AsNumber(), 2.0, "integers parse as numbers")
    check(value.Find("ratio").IsNumber(),
          "a JSON fraction must be typed as a number")
    check_eq(value.Find("ratio").AsNumber(), 1.5, "fractions parse")
    check_eq(value.Find("flag").AsBool(), True, "true parses")
    check(value.Find("flag").IsBool(), "true must be typed as a bool")
    check(value.Find("nothing").IsNull(), "null parses")


def test_json_parser_rejects_trailing_content():
    ok, _, error = Mesh.JsonParse('{"a": 1} {"b": 2}')
    check(not ok, "two concatenated documents must be rejected")
    check("trailing" in error,
          "the error must name the problem (got %r)" % error)


if __name__ == "__main__":
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        print(f"  {name} ...")
        try:
            fn()
        except Exception as error:  # noqa: BLE001
            failed += 1
            print(f"    ERROR {error!r}")
    total = len(tests)
    print(f"\n{total - failed}/{total} tests passed")
    print(f"{PASSED}/{PASSED + len(FAILED)} checks passed")
    sys.exit(1 if failed else 0)
