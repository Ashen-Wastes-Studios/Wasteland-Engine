#pragma once

// ProceduralMesh is the renderer's mesh layout. MeshLoader needs it because
// BuildProceduralMesh converts its own MeshData into that layout, and
// MeshAssetCache stores the result. ProceduralMeshes.h includes only glm and
// the standard library, so this does not pull the renderer into the loader.
#include "ProceduralMeshes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wasteland {

	// CPU-side mesh data, produced by the OBJ loader and consumed by the GPU
	// upload path.
	//
	// Deliberately separate from the GPU vertex/index buffers. The loader
	// builds this, tests assert against it without a GL context, and the
	// renderer uploads it exactly once into a VertexArray. That split is what
	// makes mesh import verifiable in a headless process -- a test can check
	// vertex counts, normals, and triangle winding with no window at all.
	struct MeshVertex
	{
		float Position[3] = {0.0f, 0.0f, 0.0f};
		float Normal[3] = {0.0f, 1.0f, 0.0f};
		float TexCoord[2] = {0.0f, 0.0f};
	};

	struct MeshData
	{
		// De-indexed: one MeshVertex per triangle corner. OBJ faces are
		// arbitrary polygons, so expanding to triangles is what lets the whole
		// mesh share one index buffer that is a trivial 0,1,2,3,... sequence.
		// It does cost memory versus a shared-index mesh; for import-and-
		// render that is the right trade because it removes per-triangle
		// branching and lets the GPU cache vertices positionally.
		std::vector<MeshVertex> Vertices;

		// Always 0..Vertices.size()-1 in order. Kept explicit so the GPU
		// upload path does not have to reconstruct it, and so a future
		// de-indexed optimisation has somewhere to write.
		std::vector<uint32_t> Indices;

		// Source path, kept for error messages and for the editor's asset
		// browser to key off.
		std::string SourcePath;

		// AABB of the imported geometry. The editor uses it to frame the
		// model, and it is a cheap sanity check that an import produced
		// something (a mesh with an infinite bound is a parse bug, not a
		// modelling choice).
		float BoundsMin[3] = {0.0f, 0.0f, 0.0f};
		float BoundsMax[3] = {0.0f, 0.0f, 0.0f};

		bool IsValid() const { return !Vertices.empty(); }
		uint32_t GetTriangleCount() const { return (uint32_t)(Indices.size() / 3); }

		// Recomputes BoundsMin/Max from the vertex positions. Called by the
		// loader, but public so a test can rebuild the bounds after mutating
		// vertices.
		void RecomputeBounds();
	};

	// Parses a Wavefront OBJ file.
	//
	// Supported: v, vn, vt, f (triangles, quads, and n-gons via fan
	// triangulation), o/g (group names are collected but not split -- one OBJ
	// becomes one mesh, which is what the blockout level wants).
	//
	// Deliberately NOT supported: mtllib and usemtl (materials need a .mtl
	// parser and a material-assignment story that does not exist yet), negative
	// indices (rare, and silently getting them wrong is worse than rejecting
	// them), and smoothing groups.
	//
	// Returns false and fills `outError` rather than throwing: an import is a
	// user-facing action where a clear message beats an exception, and a
	// malformed asset must not take the editor down.
	//
	// Normals are generated when a face has none (summing the face normal of
	// every triangle sharing a position, then normalizing), because a model
	// with flat-shaded lighting looks broken and most exported OBJs omit them.
	bool LoadOBJ(const std::string &filepath, MeshData &outMesh, std::string &outError);

	// Same, from memory. Exists so the parser can be unit-tested without
	// touching the filesystem, and so future formats (glTF, which is JSON +
	// base64 buffers) can share the front half of the path.
	bool ParseOBJFromString(const std::string &text, const std::string &virtualPath,
						   MeshData &outMesh, std::string &outError);

	// Converts DE-INDEXED import data into the renderer's ProceduralMesh.
	//
	// MeshData stores one MeshVertex per triangle corner because that is what
	// a GPU upload wants. ProceduralMesh stores parallel glm vectors because
	// that is what the procedural generators and the raster batch want. The
	// two layouts are not interchangeable, so an imported mesh cannot be
	// drawn without this step.
	//
	// A mesh whose vertex and normal counts disagree is rejected rather than
	// padded: ProceduralMesh documents that mismatch as a defect because the
	// GPU reads both attributes positionally, so a padded normal would render
	// with plausible-looking garbage instead of failing.
	bool BuildProceduralMesh(const MeshData &data, ProceduralMesh &out,
							 std::string &outError);

	// Loads and caches the RUNTIME mesh for one asset path.
	//
	// A scene references meshes by path, and the same path is drawn every
	// frame. Parsing it per frame would be an unbounded cost, so the parsed
	// result is kept. The cache is keyed by the path AS WRITTEN IN THE SCENE,
	// which means two paths that resolve to the same file are loaded twice --
	// correct, and the duplication is bounded by how many ways an author
	// spelled the path.
	//
	// A failed load is cached too, as a nullptr with a reason. Without that,
	// a missing asset would re-parse (and re-fail) on every frame forever.
	class MeshAssetCache
	{
	public:
		// Returns the mesh for a path, loading it on first use. nullptr when
		// the asset does not exist or could not be parsed; `outError` then
		// carries the reason from the FIRST attempt (the cached failure), not
		// a fresh parse.
		const ProceduralMesh *Get(const std::string &path, std::string *outError = nullptr);

		// Drops every entry. Called when the project's asset root changes,
		// because every cached path is relative to it.
		void Clear();

		// Number of live entries, for tests and the editor's asset panel.
		size_t Count() const { return m_Meshes.size(); }

	private:
		// Meshes and their cached failures are stored side by side so a
		// missing asset is not re-attempted. entt is not involved; this is a
		// plain path -> index map plus a vector.
		std::unordered_map<std::string, size_t> m_Index;
		std::vector<ProceduralMesh> m_Meshes;
		std::vector<std::string> m_Errors;
	};

} // namespace Wasteland
