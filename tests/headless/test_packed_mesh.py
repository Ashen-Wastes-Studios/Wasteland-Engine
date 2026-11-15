"""The J.2 packed mesh format: deterministic, byte-stable, and a strict reader.

Format (little-endian): magic "WLMESH01", version u32, material index i32,
triangle count u32, then triangleCount*3 corners of {Position[3] Normal[3]
TexCoord[2]} -- the de-indexed renderer layout, hence "EditableMesh de-index
plus a material index" in the plan's J.2 wording.

The discriminator is determinism: the same mesh MUST produce the same bytes every
time, or the incremental cook core (.cook sidecar) would see every run as stale.
And the reader must REJECT a truncated or corrupt file, never pad it.
"""
import atexit
import json
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

ImportGltfFromText = Wasteland.Mesh.ImportGltfFromText
WritePackedMesh = Wasteland.WritePackedMesh
ReadPackedMesh = Wasteland.ReadPackedMesh

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_packed_")
    _DIRS.append(path)
    return path


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


class _GlbBuilder:
    """A minimal glTF binary builder, enough for a triangle. Mirrors the helper
    in tests/test_gltf_import.py (same alignment, same accessor shape)."""

    def __init__(self):
        self.binary = bytearray()
        self.buffer_views = []
        self.accessors = []

    def _pack(self, values, component_type):
        fmt = {5126: "<f", 5123: "<H"}[component_type]
        return b"".join(struct.pack(fmt, value) for value in values)

    def add_accessor(self, values, component_type, type_name):
        raw = self._pack(values, component_type)
        offset = len(self.binary)
        self.binary.extend(raw)
        while len(self.binary) % 4 != 0:
            self.binary.append(0)
        self.buffer_views.append({"buffer": 0, "byteOffset": offset,
                                  "byteLength": len(raw)})
        self.accessors.append({"bufferView": len(self.buffer_views) - 1,
                               "componentType": component_type,
                               "count": len(values) // {"SCALAR": 1, "VEC3": 3}[type_name],
                               "type": type_name})
        return len(self.accessors) - 1

    def build(self, gltf):
        gltf["buffers"] = [{"byteLength": len(self.binary)}]
        gltf["bufferViews"] = self.buffer_views
        gltf["accessors"] = self.accessors
        return json.dumps(gltf, separators=(",", ":")), bytes(self.binary)


def _triangle_mesh():
    """Imports a single triangle glTF and returns its EditableMesh via Weld."""
    builder = _GlbBuilder()
    positions = [0.0, 0.0, 0.0,   1.0, 0.0, 0.0,   0.0, 1.0, 0.0]
    normals = [0.0, 0.0, 1.0] * 3
    indices = [0, 1, 2]
    p = builder.add_accessor(positions, 5126, "VEC3")
    n = builder.add_accessor(normals, 5126, "VEC3")
    i = builder.add_accessor(indices, 5123, "SCALAR")
    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": p, "NORMAL": n},
                                    "indices": i, "mode": 4}]}],
    }
    text, binary = builder.build(gltf)
    result = ImportGltfFromText(text, binary, ".")
    check(result.Ok(), "triangle glTF import failed: %s" % result.Error)
    return result.Mesh


def test_a_triangle_round_trips_through_the_packed_format():
    mesh = _triangle_mesh()
    check_eq(mesh.FaceCount, 1, "the fixture is not a single triangle")

    path = os.path.join(_root(), "tri.wlmesh.bin")
    check(WritePackedMesh(path, mesh, 3), "the write failed")

    loaded, materialIndex = ReadPackedMesh(path)
    check_eq(loaded.TriangleCount, 1, "cook did not preserve the triangle count")
    check_eq(materialIndex, 3, "the material index did not round-trip")


def test_the_pack_is_byte_stable_across_two_writes():
    """Two cooks of the same mesh MUST be byte-identical, or the incremental core
    re-cooks every run (the .cook sidecar would never match)."""
    mesh = _triangle_mesh()
    first = os.path.join(_root(), "a.bin")
    second = os.path.join(_root(), "b.bin")
    check(WritePackedMesh(first, mesh, 7), "first write failed")
    check(WritePackedMesh(second, mesh, 7), "second write failed")
    check_eq(_read(first), _read(second),
             "two cooks of the same mesh produced different bytes")


def test_a_negative_material_index_survives():
    mesh = _triangle_mesh()
    path = os.path.join(_root(), "negative.bin")
    WritePackedMesh(path, mesh, -1)
    loaded, materialIndex = ReadPackedMesh(path)
    check_eq(materialIndex, -1, "a negative material index was not preserved")


def test_the_material_index_affects_the_bytes_but_not_the_geometry():
    mesh = _triangle_mesh()
    a = os.path.join(_root(), "m0.bin")
    b = os.path.join(_root(), "m1.bin")
    WritePackedMesh(a, mesh, 0)
    WritePackedMesh(b, mesh, 1)
    check(_read(a) != _read(b), "a different material index changed nothing")
    loaded_a, ia = ReadPackedMesh(a)
    loaded_b, ib = ReadPackedMesh(b)
    check_eq(ia, 0, "mesh a loaded the wrong material")
    check_eq(ib, 1, "mesh b loaded the wrong material")
    check_eq(loaded_a.VertexCount, loaded_b.VertexCount,
             "two materials changed the geometry")


def test_the_header_magic_is_rejected():
    path = os.path.join(_root(), "bad.bin")
    with open(path, "wb") as handle:
        handle.write(b"NOTAMESH" + b"\x00" * 40)
    raised = None
    try:
        ReadPackedMesh(path)
    except RuntimeError as error:
        raised = error
    check(raised is not None and "magic" in str(raised),
          "a bad magic was not rejected: %s" % raised)


def test_a_truncated_payload_is_rejected():
    mesh = _triangle_mesh()
    full = os.path.join(_root(), "full.bin")
    WritePackedMesh(full, mesh, 0)
    data = _read(full)
    truncated = os.path.join(_root(), "trunc.bin")
    with open(truncated, "wb") as handle:
        handle.write(data[:20 + 10])  # header plus a few corner bytes

    raised = None
    try:
        ReadPackedMesh(truncated)
    except RuntimeError as error:
        raised = error
    check(raised is not None and "trunc" in str(raised).lower(),
          "a truncated payload was not rejected: %s" % raised)


def test_trailing_bytes_are_rejected():
    mesh = _triangle_mesh()
    full = os.path.join(_root(), "full.bin")
    WritePackedMesh(full, mesh, 0)
    padded = os.path.join(_root(), "padded.bin")
    with open(padded, "wb") as handle:
        handle.write(_read(full) + b"junk")

    raised = None
    try:
        ReadPackedMesh(padded)
    except RuntimeError as error:
        raised = error
    check(raised is not None and "trailing" in str(raised).lower(),
          "trailing bytes were not rejected: %s" % raised)


def test_the_written_size_is_header_plus_corners():
    mesh = _triangle_mesh()
    path = os.path.join(_root(), "size.bin")
    WritePackedMesh(path, mesh, 0)
    data = _read(path)
    # 20-byte header + 3 corners * 8 floats * 4 bytes.
    check_eq(len(data), 20 + 3 * 8 * 4,
             "the packed size is not header plus corners")


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

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

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{len(tests) - failed}/{len(tests)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
