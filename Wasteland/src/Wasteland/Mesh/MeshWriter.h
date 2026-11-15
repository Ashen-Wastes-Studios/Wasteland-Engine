// MeshWriter.h -- the export half of the mesh round trip.
//
// WHY OBJ AND NOT glTF
// --------------------
// The engine already PARSES OBJ (`MeshLoader::ParseOBJFromString`), so writing
// it makes the round trip close through code this repository owns and tests,
// with no second container format to get subtly wrong. glTF export would mean
// writing a binary container, accessors, bufferViews and a skin -- a large
// amount of code to produce a file whose only purpose here is to be read back.
//
// OBJ is also what an artist expects a mesh edit to be able to leave the engine
// with: every DCC imports it, so a sculpted prop can go engine -> Blender ->
// back without a conversion step in the middle.
//
// WHAT IT DOES NOT CARRY
// ----------------------
// OBJ has no skeleton, no animation and no material assignment beyond a
// `usemtl`/`mtllib` name. That is a real limitation and the round-trip test
// states it: the mesh comes back, the rig does not travel through OBJ. A rigged
// character's mesh should be exported through the glTF path instead; OBJ is for
// props and for the sculpt/edit gate.
#pragma once

#include <filesystem>
#include <string>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	// Writes meshes to Wavefront OBJ. Static and stateless: a mesh is a pure
	// function of its file, so there is nothing for an instance to hold.
	class MeshWriter
	{
	public:
		// An indexed mesh: vertices are SHARED between faces, so `f` records
		// vertex indices and the file carries one `v` per unique vertex. This is
		// the shape an editor produces and the smaller file.
		static bool SaveOBJ(const std::filesystem::path &path,
							const EditableMesh &mesh,
							std::string *error = nullptr);
		static std::string EmitOBJ(const EditableMesh &mesh);

		// A de-indexed MeshData: one vertex per triangle corner, which is the
		// renderer's shape. Every corner becomes its own `v` and `vn`, so the
		// file is larger and no vertex sharing survives -- correct, because a
		// MeshData has already been welded by position and its sharing is not
		// recoverable information.
		static bool SaveOBJFromMeshData(const std::filesystem::path &path,
										const MeshData &data,
										std::string *error = nullptr);
		static std::string EmitOBJFromMeshData(const MeshData &data);
	};

} // namespace Wasteland
