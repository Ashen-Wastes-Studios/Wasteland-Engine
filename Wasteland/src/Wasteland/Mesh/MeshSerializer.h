// MeshSerializer.h -- the authored .wlmesh asset.
//
// WHY A THIRD MESH FORMAT
// -----------------------
// The engine already reads glTF (the DCC interchange format) and writes OBJ
// (the artist-facing export). Neither is a scene asset:
//   * glTF is a container -- accessors, bufferViews, a binary chunk, a skin.
//     Correct for interchange, and far more machinery than a mesh edit needs
//     in order to leave the engine.
//   * OBJ is de-indexed and carries no material assignment beyond a usemtl
//     name, so a mesh that was edited in the engine and sent to Blender loses
//     its slot list on the way back.
// A .wlmesh is what an EDITED mesh is saved as: indexed geometry, per-vertex
// normals and UVs, a material slot list with per-face assignment, and an
// optional undo history.
//
// WHY YAML
// --------
// The same substrate as .wlmaterial and the scene files, so there is one
// parser, one failure convention (a bad field is REPORTED, not skipped) and
// one set of on-disk spellings. A binary format would need its own reader, its
// own version dispatch and its own error strings, for no gain at this asset
// size -- and it could not be read by the artist to see what the editor saved.
//
// WHAT IT DOES NOT CARRY
// ----------------------
// No skeleton, no skin weights, no animation. Those are glTF's job, and a
// rigged mesh should be exported through it. A .wlmesh is geometry plus
// shading; a skin is not geometry.
//
// THE SCULPT HISTORY IS OPT-IN
// ----------------------------
// A history snapshot is a WHOLE MESH, so a 16-deep stack on a 20k-triangle
// sculpt is hundreds of megabytes. `includeHistory` defaults to false and the
// editor's save path passes false: a scene file carrying an undo stack nobody
// asked for is not something to ship.
//
// VERSION
// -------
// A version the reader does not recognise is an ERROR, not a best-effort parse.
// A v2 file loaded as v1 would silently misread its own fields, which is the
// failure mode this format exists to avoid.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	// One .wlmesh document.
	struct MeshAsset
	{
		EditableMesh Mesh;

		// Material slot paths. Empty means the mesh carries no material and
		// renders with whatever the renderer defaults to.
		std::vector<std::string> Materials;

		// One slot index per triangle. Empty means every triangle uses slot 0.
		// A value outside [0, Materials.size()) is a LOAD ERROR rather than a
		// clamp: a face assigned to a material that does not exist is authoring
		// damage, and silently drawing it with slot 0 hides the damage.
		std::vector<int32_t> FaceMaterial;

		// Whole-mesh snapshots, oldest first -- the same order MeshEditHistory
		// stores them in. Only ever written when the caller asks.
		std::vector<EditableMesh> SculptHistory;

		// The on-disk format version. Emit writes the version it was given;
		// Parse accepts only the version it implements.
		int32_t Version = 1;
	};

	// Reads and writes .wlmesh YAML. Static and stateless: a mesh file is a
	// pure function of a MeshAsset, so there is nothing for an instance to hold.
	class MeshSerializer
	{
	public:
		// The format version this build reads and writes.
		static constexpr int32_t kSupportedVersion = 1;

		// Writes the asset to `path`, creating parent directories. Returns false
		// and fills `error` rather than throwing: a save is a user-facing action
		// and a full disk is user data, not a bug.
		//
		// `includeHistory` is false by default. The history is written only when
		// it is true AND `asset.SculptHistory` is non-empty -- an empty section
		// would read back as "history was requested and there was none", which is
		// a different statement from "not requested".
		static bool Save(const std::filesystem::path &path,
						 const MeshAsset &asset,
						 bool includeHistory = false,
						 std::string *error = nullptr);

		static bool Load(const std::filesystem::path &path,
						 MeshAsset &asset,
						 std::string *error = nullptr);

		// The YAML text for an asset, without the filesystem. Exists so the
		// format can be unit-tested headlessly and so a tool can preview a save
		// without writing one.
		static std::string Emit(const MeshAsset &asset, bool includeHistory = false);

		// The inverse of Emit, for the same reason. On failure `asset` is left
		// untouched: a caller that has a mesh loaded must not lose it to a file
		// that turned out to be corrupt.
		static bool Parse(const std::string &text,
						  MeshAsset &asset,
						  std::string &error);
	};

} // namespace Wasteland
