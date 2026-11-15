#pragma once

#include "Wasteland/Mesh/EditableMesh.h"

#include <string>
#include <vector>

namespace Wasteland {

	// One glTF PBR material, mapped onto the engine's MaterialComponent.
	//
	// The six fields the plan names are the scalar/colour parameters. Textures
	// are NOT extracted in v1: a glTF image is either a URI (recorded in
	// AlbedoTexturePath, so the material can be pointed at the file) or an
	// embedded bufferView, which would need PNG encoding to become an asset --
	// and stb_image_write is vendored only inside GLFW's own deps, so using it
	// would drag a GLFW-internal include path into the engine. Recording the
	// URI is honest; silently importing a textured material as flat white is
	// not.
	struct GltfMaterial
	{
		std::string Name;

		// pbrMetallicRoughness.baseColorFactor. Defaults are the glTF
		// specification's: opaque white.
		float BaseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float MetallicFactor = 1.0f;   // glTF default is 1.0, NOT 0.0
		float RoughnessFactor = 1.0f;  // glTF default is 1.0

		// emissiveFactor. glTF's default is black (0,0,0), which is inert.
		float EmissiveFactor[3] = {0.0f, 0.0f, 0.0f};

		// normalTexture.scale. glTF's default is 1.0, meaning the map is
		// applied at full strength; the engine's NormalStrength defaults to 0
		// (disabled), so a material WITH a normal map must set this to 1.0 to
		// actually show it.
		float NormalScale = 1.0f;

		// Set when the material names a baseColorTexture whose image is a URI.
		// Empty for an embedded bufferView image or no texture at all.
		std::string AlbedoTexturePath;
		bool HasBaseColorTexture = false;
		bool HasNormalTexture = false;

		// alphaMode is "OPAQUE", "MASK" or "BLEND". The engine's
		// MaterialComponent has no alpha-mode field (opacity comes from
		// transmission/thickness), so a non-opaque material is recorded here
		// rather than silently flattened to opaque.
		std::string AlphaMode = "OPAQUE";
	};

	// A clean-room glTF 2.0 importer, written against the published Khronos
	// specification. No vendor SDK.
	//
	// Scope is deliberate and matches the plan:
	//   IN:  POSITION, NORMAL, TEXCOORD_0, JOINTS_0_0, WEIGHTS_0_0, indices,
	//        skins (inverseBindMatrices, joint hierarchy), animations
	//        (translation/rotation/scale, LINEAR and STEP).
	//   OUT: Draco/KTX2, morph targets, materials beyond
	//        baseColor/metallicRoughness/normal.
	// Anything out of scope is REJECTED with a named error rather than
	// silently dropped, because a mesh that imports with its skeleton missing
	// is worse than one that fails to import.
	//
	// The result carries an EditableMesh rather than a MeshData. glTF is an
	// indexed format, so welding it into the half-edge structure on the way in
	// means every downstream modelling operation works on imported assets for
	// free -- and `Deindex()` still gives the renderer its de-indexed upload.
	struct GltfImportResult
	{
		// Empty on success. On failure this names the JSON path that failed,
		// e.g. "meshes[0].primitives[0]: accessor 4 has componentType 5125 but
		// POSITION requires a float type". A caller can show this verbatim.
		std::string Error;

		EditableMesh Mesh;

		// De-indexed geometry: one vertex per triangle corner, exactly what the
		// OBJ loader produces and the renderer uploads.
		//
		// Kept alongside the welded EditableMesh because the two are NOT
		// interchangeable. Weld merges by POSITION and keeps one normal, so a
		// hard edge -- two triangle corners sharing a position but carrying
		// different normals, which is how a crisp bevel is authored -- does not
		// survive a Deindex() round trip: both corners come back with the same
		// normal and the edge is gone. A render-only caller must use this field,
		// never Mesh.Deindex().
		MeshData Deindexed;

		// Skeleton, in BONE ORDER (the order skins[].joints lists them).
		std::vector<std::string> BoneNames;
		// Parent index per bone, -1 for a root. Parallel to BoneNames.
		std::vector<int32_t> BoneParents;
		// Inverse bind matrices, row-major 4x4, parallel to BoneNames.
		std::vector<float> InverseBindMatrices;

		struct ClipChannel
		{
			int32_t Bone = -1;
			std::string Path; // "translation" | "rotation" | "scale"
		};

		struct Clip
		{
			std::string Name;
			double Duration = 0.0;
			std::vector<ClipChannel> Channels;
			// Key times, shared across the clip's channels. One timeline per
			// clip is the common case and keeps the structure small.
			std::vector<double> KeyTimes;
			// Values, [channel][key][component]. Rotation is a quaternion
			// (x,y,z,w) and the other two are vec3.
			std::vector<std::vector<std::vector<float>>> Values;
		};

		std::vector<Clip> Clips;

		// Materials, in file order. A glTF mesh primitive names a material by
		// index; the importer does not merge or reorder them, so index i here is
		// index i in the file's materials array.
		//
		// Empty when the file declares no materials -- which is legal glTF and
		// means every surface renders with the entity's own MaterialComponent.
		std::vector<GltfMaterial> Materials;

		bool Ok() const { return Error.empty(); }
	};

	// Imports a .gltf (JSON + external .bin) or a .glb (JSON chunk + BIN chunk).
	//
	// `path` is used both to read the file and to resolve the .bin URI, so a
	// relative "buffer.bin" lands next to the .gltf regardless of the current
	// working directory.
	//
	// `relativeToDirectory` exists for tests: a caller can hand in JSON text
	// and a directory without writing either to disk.
	GltfImportResult ImportGltf(const std::string &path);

	// Text-level entry point, for a caller that already has the JSON and the
	// binary chunk in memory (an archive, a download cache, a test).
	GltfImportResult ImportGltfFromText(const std::string &json,
										const std::string &binaryChunk,
										const std::string &baseDirectory);

} // namespace Wasteland
