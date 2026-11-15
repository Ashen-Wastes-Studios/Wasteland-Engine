#pragma once

#include "Wasteland/Renderer/MeshLoader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland {

	// A modelling-ready mesh: indexed geometry plus HALF-EDGE adjacency.
	//
	// Why not MeshData: MeshData is explicitly DE-INDEXED -- one vertex per
	// triangle corner (MeshLoader.h:27). That is the right shape for a GPU
	// upload, and the wrong shape for every modelling operation. Extrude,
	// bevel, inset, loop cut, select-by-adjacency and sculpt all need to ask
	// "what shares this edge?" and a triangle soup cannot answer that without
	// an O(n^2) rebuild every time.
	//
	// Half-edge rather than a winged-edge or quad-edge: a half-edge stores
	// exactly the four links a face walk needs (twin, next, vertex, face) and
	// nothing else, so a triangle mesh is 3 edges per face with no per-vertex
	// edge lists to keep in sync. The twin map is rebuilt on demand rather than
	// maintained, which keeps the structure a plain value type.
	//
	// The structure is NOT a general polygon mesh: faces are triangles, which
	// is what every DCC eventually exports and what the renderer rasterises.
	// Supporting n-gons would mean a variable-length face loop in the middle
	// of the hottest traversal path for no modelling benefit.
	struct EditableMesh
	{
		struct Vertex
		{
			float Position[3] = {0.0f, 0.0f, 0.0f};
			float Normal[3] = {0.0f, 1.0f, 0.0f};
			float TexCoord[2] = {0.0f, 0.0f};
			// First edge LEAVING this vertex, or -1 when the vertex is
			// isolated. Kept as an index rather than a pointer so the struct
			// stays copyable and a vector reallocation cannot dangle it.
			int32_t FirstEdge = -1;
		};

		// One directed edge of a triangle.
		//
		// Convention (the one the whole file relies on):
		//   Vertex = the vertex this edge STARTS at.
		//   Next   = the next edge around the same face, also starting where
		//            this one ends.
		//   Twin   = the same geometric edge in the opposite direction, so
		//            Edges[Twin].Vertex == the vertex this edge ENDS at.
		//   Face   = the triangle this edge borders, or -1 on a boundary.
		struct HalfEdge
		{
			int32_t Twin = -1;
			int32_t Next = -1;
			int32_t Vertex = -1;
			int32_t Face = -1;
		};

		struct Face
		{
			// First edge of the face loop. The other two are Next and
			// Next->Next; a triangle face never needs a longer walk, but the
			// loop form is what makes the half-edge algebra uniform.
			int32_t FirstEdge = -1;
		};

		std::vector<Vertex> Vertices;
		std::vector<HalfEdge> Edges;
		std::vector<Face> Faces;

		// Builds an indexed mesh from de-indexed MeshData, MERGING vertices
		// that share a position.
		//
		// Merging is the whole point of the function. An OBJ cube is 36
		// corners; a modelling mesh needs 8 vertices and 36 half-edges. The
		// weld is by POSITION ONLY -- not by normal or UV -- because a seam
		// where the UVs differ must stay a seam, and welding it would make a
		// texture atlas unwrap impossible to edit. Callers that want a
		// position+normal+uv weld ask for it explicitly.
		static EditableMesh Weld(const MeshData &source);

		// Back to the renderer's shape: one vertex per triangle corner.
		//
		// The inverse of Weld, but NOT a lossless round trip in general.
		// Weld merges by position; Deindex cannot know which merged vertex
		// came from which corner, so a welded cube becomes 36 corners again.
		// That is correct: the renderer wants de-indexed data, and the
		// triangle COUNT and winding are preserved exactly.
		MeshData Deindex() const;

		// Area-weighted vertex normals from the current triangles.
		//
		// Area weighting rather than a plain angle or count average: a tiny
		// triangle shares a vertex with a huge one, and an unweighted average
		// lets the sliver dominate the shading of the large face. The weight
		// is the cross product magnitude, which is twice the triangle area --
		// the factor cancels in the normalise, so it is left uncorrected.
		void RecomputeNormals();

		// Structural check. Returns an EMPTY string when the mesh is sound.
		//
		// A string rather than a bool or an exception: a mesh can have five
		// independent defects and a caller wants all of them, not the first.
		// Empty-on-success also makes the common `if (!Validate().empty())`
		// read the way the caller thinks.
		//
		// Checks, in order: index ranges, twin symmetry, next/vertex
		// consistency, face completeness, and orphan vertices.
		std::string Validate() const;

		// True when Validate() reports nothing. Convenience for assertions.
		bool IsValid() const { return Validate().empty(); }

		// Every face loop is a triangle. Kept as a named predicate because
		// the modelling operations assume it and a caller that has just run
		// Triangulate wants that assumption stated rather than implied.
		bool IsTriangulated() const;

		// --- modelling operations -------------------------------------------
			// These are the operations the mesh editor's toolbar exposes. They live
			// on the mesh rather than in the panel so a test can run them against a
			// known mesh and check the resulting vertex/edge/face counts -- which is
			// the only way to tell a working bevel from one that silently dropped
			// half the model.

			// Merge vertices closer than `epsilon` into one. Returns the number of
			// vertices REMOVED, or -1 for a non-positive epsilon (which would
			// collapse the whole model into one vertex, so it is refused rather
			// than clamped).
			//
			// A merge that collapses every face and leaves an empty mesh returns
			// the full vertex count, NOT -1: that is a successful merge of a
			// degenerate mesh, and reporting it as a refusal would tell the caller
			// its argument was bad when it was the mesh.
			//
			// A spatial hash on floor(position / epsilon) rather than an O(n^2)
			// pairwise scan: a merge-by-distance over a 40k-vertex sculpt is 1.6
			// billion comparisons, and the artist is waiting.
			int32_t MergeByDistance(float epsilon);

			// Reverse the winding of every triangle and re-derive the normals.
			//
			// Implemented as Deindex -> reverse each triangle -> Weld rather than by
			// swapping half-edge links in place: an edge is shared with its twin in
			// the adjacent face, so reversing one face's loop without reversing the
			// neighbours' leaves the twin map describing a mesh that no longer
			// exists. Rebuilding is correct by construction and costs one pass.
			void FlipNormals();

			// Delete the named faces. Edges belonging only to deleted faces go with
			// them; a surviving neighbour's half-edge loses its twin and becomes a
			// boundary edge, which is what "delete a face" means geometrically.
			// Orphaned vertices are compacted out. Returns false when a face index
			// is out of range.
			bool DeleteFaces(const int32_t *faces, int32_t faceCount);

			// --- extrude / inset ------------------------------------------------
			// Both REPLACE the selected region: the original faces are removed and
			// replaced by the new surface, which is what a DCC extrude does (the
			// original face becomes the interior).
			//
			// Both rebuild the mesh from a triangle soup and re-weld it. That is
			// deliberate: extrude and inset change vertex count, face count AND the
			// twin map at once, and maintaining a half-edge structure by hand
			// through that is where subtle non-manifold damage comes from. Weld
			// already builds a correct structure from triangles; the operation's
			// own job is to decide WHICH triangles.

			// Push the selected region along `delta` (a world-space vector, NOT a
			// distance -- an extrude direction is a direction). The region's cap is
			// duplicated at original+delta and the boundary is bridged with quads.
			// Returns false when an index is out of range or the selection is empty.
			bool ExtrudeFaces(const int32_t *faces, int32_t faceCount,
							  const float delta[3]);

			// Shrink each selected face toward its own centroid by `thickness` (in
			// the asset's units) and bridge the old boundary to the new one. A
			// per-face inset rather than a region inset: the plan's scope is a
			// per-face inset, and a region inset needs a boundary-offset solve that
			// a region of disjoint faces cannot have.
			bool InsetFaces(const int32_t *faces, int32_t faceCount, float thickness);

			// Chamfer (bevel) the named edges: each edge is replaced by a quad strip
			// ("looped edge offset" in the D.2 table) whose plane is `offset` of the
			// way from the edge toward the two adjacent faces' shared midpoint, and
			// the two faces either side are split so the chamfer reads as a visible
			// polish. `offset` must be in (0, 1). Returns false with `error`
			// populated when an index is out of range or the edge is on the mesh
			// BOUNDARY (a bevel needs two faces) or TWO beveled edges share a vertex
			// (that corner is where the offset solve lives, and v1 refuses rather
			// than guess it). Rebuild-from-triangles, like extrude/inset.
			bool BevelEdges(const int32_t *edges, int32_t edgeCount, float offset,
							std::string *error);

			// Loop cut: split every edge of the edge ring that `startEdge` opens,
			// inserting a new loop of midpoint vertices and splitting each face the
			// ring crosses in two. This is the D.2 "edge-ring split". The ring is
			// walked deterministically through quads (two adjacent triangles sharing
			// an interior, non-boundary edge); it is REQUIRED to be a clean quad
			// ring, so a face where the ring would need to cross at a pole or a
			// mixed triangulation is REFUSED with `error` rather than guessed, and
			// a boundary that ends the ring (an open loop) is a legal result. A
			// closed ring must return to `startEdge` (the cut joins itself);
			// otherwise it reports an error. Returns false and leaves the mesh
			// untouched on any refusal.
			bool LoopCut(int32_t startEdge, std::string *error);

		int32_t GetVertexCount() const { return (int32_t)Vertices.size(); }
		int32_t GetEdgeCount() const { return (int32_t)Edges.size(); }
		int32_t GetFaceCount() const { return (int32_t)Faces.size(); }

		// --- narrow mutators -------------------------------------------------
		// The Python binding exposes Edges as a COPY-OUT property (the same
		// trap MeshData.Vertices has), so `mesh.Edges[i].Twin = x` mutates a
		// temporary and silently no-ops. These exist so a test can construct a
		// KNOWN-broken mesh and prove Validate() catches it -- which is the
		// only way to test a validator at all.
		//
		// They are deliberately narrow (one field each) rather than a generic
		// element setter: a setter that takes a whole HalfEdge would let a
		// caller write a struct whose Next/Twin/Vertex disagree, and the
		// validator would then be reporting damage the API invited.

		// Move one vertex. This is the sculpt primitive every modelling
		// operation reduces to, and it is a POSITION-ONLY mutator on purpose:
		// it does not touch normals or UVs, so a caller that has just pushed a
		// surface must decide explicitly whether to RecomputeNormals. Moving a
		// vertex without recomputing leaves the shading stale, and that is a
		// choice the caller should make rather than a side effect the API hides.
		//
		// Returns false for an out-of-range index rather than clamping: a
		// vertex index that does not exist is a caller bug, and silently doing
		// nothing would leave the caller believing the sculpt landed.
		bool SetVertexPosition(int32_t vertex, float x, float y, float z)
		{
			if (vertex < 0 || vertex >= (int32_t)Vertices.size())
				return false;
			Vertices[(size_t)vertex].Position[0] = x;
			Vertices[(size_t)vertex].Position[1] = y;
			Vertices[(size_t)vertex].Position[2] = z;
			return true;
		}

		// Set one vertex's texture coordinate. The UV counterpart of
	// SetVertexPosition, and it exists for the same reason: the OBJ loader
	// parses the texture index and DISCARDS it, so the only engine path that
	// produces a mesh with UVs is the glTF importer -- and without a mutator a
	// headless test cannot construct one at all, which means it cannot test
	// that the .wlmesh format carries UVs. Independent of the position, like
	// every other narrow mutator here.
	bool SetVertexTexCoord(int32_t vertex, float u, float v)
	{
		if (vertex < 0 || vertex >= (int32_t)Vertices.size())
			return false;
		Vertices[(size_t)vertex].TexCoord[0] = u;
		Vertices[(size_t)vertex].TexCoord[1] = v;
		return true;
	}

	void SetEdgeTwin(int32_t edge, int32_t twin)
		{
			if (edge >= 0 && edge < (int32_t)Edges.size())
				Edges[(size_t)edge].Twin = twin;
		}

		void SetEdgeNext(int32_t edge, int32_t next)
		{
			if (edge >= 0 && edge < (int32_t)Edges.size())
				Edges[(size_t)edge].Next = next;
		}

		void SetEdgeVertex(int32_t edge, int32_t vertex)
		{
			if (edge >= 0 && edge < (int32_t)Edges.size())
				Edges[(size_t)edge].Vertex = vertex;
		}

		void SetEdgeFace(int32_t edge, int32_t face)
		{
			if (edge >= 0 && edge < (int32_t)Edges.size())
				Edges[(size_t)edge].Face = face;
		}

		// The three corner VERTEX indices of a face, in loop order.
		//
		// A face stores only its first edge, so the corners are a three-step walk.
		// Every serializer and every triangle-emitting operation needs exactly this
		// and there were three private copies of it in the tree; a fourth would be
		// the one that drifts. `face` out of range leaves `out` untouched and
		// returns false rather than reading `Edges[-1]`.
		bool GetFaceCorners(int32_t face, int32_t out[3]) const
		{
			if (face < 0 || face >= (int32_t)Faces.size())
				return false;
			const int32_t e0 = Faces[(size_t)face].FirstEdge;
			if (e0 < 0 || e0 >= (int32_t)Edges.size())
				return false;
			const int32_t e1 = Edges[(size_t)e0].Next;
			if (e1 < 0 || e1 >= (int32_t)Edges.size())
				return false;
			const int32_t e2 = Edges[(size_t)e1].Next;
			if (e2 < 0 || e2 >= (int32_t)Edges.size())
				return false;
			out[0] = Edges[(size_t)e0].Vertex;
			out[1] = Edges[(size_t)e1].Vertex;
			out[2] = Edges[(size_t)e2].Vertex;
			return true;
		}

		// The vertex an edge ENDS at. Derived from Next->Vertex rather than
		// stored, so it cannot disagree with the loop.
		int32_t EdgeEndVertex(int32_t edge) const
		{
			return Edges[(size_t)edge].Next >= 0
					   ? Edges[(size_t)Edges[(size_t)edge].Next].Vertex
					   : -1;
		}
	};

} // namespace Wasteland
