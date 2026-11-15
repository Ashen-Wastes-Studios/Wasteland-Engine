// PackedMesh.h -- the J.2 mesh cook format.
//
// A packed runtime mesh: the EditableMesh DE-INDEXED (one vertex per triangle
// corner -- the renderer's native layout, so a cook needs no per-triangle index
// work at load) plus a material index, written as a tight little-endian binary.
//
// Format, all little-endian, nothing else:
//   Offset  Size  Field
//   0       8     magic   "WLMESH01"
//   8       4     version (1)
//   12      4     material index (signed)
//   16      4     triangle count (zero means an empty/valid mesh)
//   20      ...   per-corner vertices, triangleCount * 3 of them, each 8 floats:
//                 Position[3] Normal[3] TexCoord[2], written bit-exactly (memcpy).
//
// Determinism is a loaded requirement, not a nicety: a cook that embeds a
// timestamp, a hash order, or a pointer address produces a different artifact
// every run, which defeats the incremental core -- the `.cook` sidecar would say
// "current" only for the identical bytes, so every cook would re-run. Floats are
// written as their in-memory IEEE-754 byte patterns and everything is positional
// and fixed-size, so the same input ALWAYS produces the same file.
//
// By convention the header + corners is the whole file: no trailing alignment, no
// padding to a power of two. A reader must reject anything shorter than the
// declared length and anything with a mismatched magic or version, never guess.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Wasteland
{
	struct EditableMesh;
	struct MeshData;

	// The 8 floats that make one de-indexed corner.
	// Exposed so a test can assert the stride without copying the layout.
	static constexpr std::size_t kPackedFloatsPerCorner = 8;
	static constexpr std::size_t kPackedFloatsPerTriangle = 3 * kPackedFloatsPerCorner;

	// Cooks `mesh` into the packed binary at `output`. `materialIndex` is stored
	// verbatim. Returns false with `error` set on any failure; the output is left
	// untouched on failure so a cook that dies halfway does not leave a
	// sector-corrupt artifact that a later run mistakes for valid.
	bool WritePackedMesh(const std::filesystem::path &output,
	                     const EditableMesh &mesh, int materialIndex,
	                     std::string *error);

	// Reads a packed mesh back into the renderer's de-indexed shape. `outIndices`
	// is set to a trivial 0..n-1 sequence and `outMaterialIndex` receives the
	// stored index. Returns false with `error` set on a missing file, bad magic,
	// unsupported version, or a truncated payload -- a truncated payload must be
	// rejected, not padded.
	bool ReadPackedMesh(const std::filesystem::path &path, MeshData &out,
	                    int *outMaterialIndex, std::string *error);
}
