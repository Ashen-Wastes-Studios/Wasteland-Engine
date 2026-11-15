// MeshPicking.h -- per-face ray picking for the mesh editor.
//
// WHY THIS IS ITS OWN MODULE
// --------------------------
// The editor already picks ENTITIES by pixel readback (EditorLayer.cpp reads a
// pixel from the entity-ID attachment). That cannot answer "which FACE was
// clicked" -- the ID buffer has no sub-object resolution. So sub-object
// selection needs a geometric raycast against the mesh, and the raycast is the
// part where a mistake is invisible: a face that selects one ring off, or a
// miss that silently selects nothing, both look like a UI quirk rather than a
// bug.
//
// It lives in the engine rather than the panel for the same reason as the OBJ
// writer: it is a pure function of a mesh and a ray, so it can be tested
// headlessly against a triangle whose hit point is known by construction.
//
// ALGORITHM: Moller-Trumbore. It computes the barycentric coordinates and the
// ray parameter in one pass with no precomputed plane equation, and it is the
// standard published form. The triangle data is already in the half-edge
// structure the mesh keeps.
//
// NO BVH YET, BY CHOICE. The plan says "brute-force over the mesh's triangles
// is correct at editor scale, with a BVH if a measured mesh is slow. Measure,
// then optimize." A BVH that is wrong is worse than a loop that is slow: it
// would reject hits the loop finds, and the failure is mesh-dependent. The
// brute-force loop is what ships, and the panel reports the triangle count so
// the measurement can be made.
#pragma once

#include <cstdint>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	struct MeshPickResult
	{
		// Index into EditableMesh::Faces, or -1 for a miss.
		int32_t Face = -1;
		// Ray parameter: hit = Origin + Distance * Direction. Negative means
		// the hit is behind the ray origin.
		float Distance = 0.0f;
		// Barycentric coordinates of the hit within the face. U + V <= 1 on a
		// hit; the third weight is 1 - U - V. Exposed because "which part of the
		// face" is what an inset or a bevel needs, and recomputing it later
		// would mean intersecting twice.
		float U = 0.0f;
		float V = 0.0f;
		// World-space hit point, computed from the barycentrics rather than
		// Origin + Distance * Direction: both are the same point, but this one
		// is guaranteed to lie on the triangle plane even when the caller passed
		// an unnormalised direction.
		float Point[3] = {0.0f, 0.0f, 0.0f};
	};

	// Moller-Trumbore against one triangle. Returns false for a miss.
	//
	// `direction` need NOT be normalised -- the barycentrics are invariant under
	// scaling of the direction, and `outDistance` is then in units of the
	// direction's length. Callers comparing distances across rays must use
	// normalised directions or compare `Point` instead.
	bool IntersectTriangle(const float v0[3], const float v1[3], const float v2[3],
						   const float origin[3], const float direction[3],
						   float &outDistance, float &outU, float &outV);

	// Closest face hit against the whole mesh. Returns false when nothing is
	// hit. Ties (a ray exactly along an edge shared by two faces) resolve to
	// the FIRST face in index order, which is deterministic -- an editor that
	// picks a different face on the same click between frames is unusable.
	bool PickFace(const EditableMesh &mesh,
				  const float origin[3], const float direction[3],
				  MeshPickResult &out);

	// The same walk for an explicit face list, used by the panel's box-select
	// and lasso (which select many faces at once) and by tests that want to
	// intersect a subset.
	bool PickFaces(const EditableMesh &mesh, const int32_t *faces, int32_t faceCount,
				   const float origin[3], const float direction[3],
				   MeshPickResult &out);

} // namespace Wasteland
