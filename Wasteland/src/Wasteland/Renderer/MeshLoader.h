#pragma once

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

} // namespace Wasteland
