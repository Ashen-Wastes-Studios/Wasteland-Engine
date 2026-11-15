// MeshRemesh.h -- uniform voxel remesh and quadric decimation.
//
// WHY BOTH
// --------
// A sculpt brush needs vertices to move and a dense import has too many. These
// are the two ends of that problem: remesh to GET vertices where the surface
// has none, decimate to GET RID of them where it has too many.
//
// WHAT THE REMESH ACTUALLY IS
// ----------------------------
// The plan asks for a voxel-based remesh. This is a voxel occupancy remesh: the
// surface is rasterised into a voxel grid, and every occupied voxel with an
// empty neighbour emits a quad on that side. The result is WATERTIGHT by
// construction -- every emitted quad is shared by exactly two voxels' shells
// -- and its silhouette converges to the original as the voxel size shrinks.
//
// It is deliberately NOT marching cubes. Marching cubes produces a smooth
// surface but needs a 256-case table (or 15 base cases with symmetry), a
// sign grid, and an edge-interpolation table; a blocky shell is a few hundred
// lines and its error bound is exactly the voxel size, which is a number the
// caller already chose. Marching cubes is named in the plan as a P1 upgrade
// rather than being smuggled in here.
//
// A voxel joins the occupied set when its CENTRE is within half a voxel
// DIAGONAL of the surface. That single radius is both the thickening and the
// coverage guarantee: a voxel the surface passes through always has its
// centre within half a diagonal of the surface point, so no triangle can slip
// between two voxel centres unmarked. The exterior is then flood-filled in
// from the grid boundary and everything the fill cannot reach becomes solid,
// so the extracted shell is the boundary of a volume rather than the two
// surfaces of a hollow shell.
//
// The shell is closed by construction: every grid edge is shared by an even
// number of boundary faces, because the four voxels around it form a cycle.
//
// The cost is a systematic outward offset of up to half a diagonal, plus
// another half diagonal at the shell's corners -- the tolerance the tests
// state.
#pragma once

#include <cstdint>
#include <string>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	// Rasterises `source` into a voxel grid of `voxelSize` and extracts the
	// shell. Returns an empty mesh (and sets `error`) when the voxel size is not
	// positive, the source is empty, or the grid would exceed `maxVoxels`.
	//
	// The voxel size is in the source's own units, not normalised: a 20mm voxel
	// grid on a 2m prop is 0.02, and the caller has to know its asset's units.
	// That is the same rule the OBJ and glTF paths use.
	EditableMesh VoxelRemesh(const EditableMesh &source, float voxelSize,
							 int64_t maxVoxels, std::string *error);

	// Reduces `source` to at most `targetTriangles` using quadric error metrics:
	// every vertex accumulates the sum of its adjacent face planes' outer
	// products, and the cheapest edge is contracted first, with the surviving
	// endpoint's quadric updated. The optimal position of a contraction is the
	// minimiser of the summed quadric, which is what makes this a metric rather
	// than a "merge the nearest pair" heuristic.
	//
	// Returns an empty mesh (and sets `error`) when the target is not less than
	// the current count (nothing to do is an error here, not a no-op: a caller
	// asking for a decimation that does not decimate has usually mis-sized the
	// mesh), when the target is below 4, or when the source is empty.
	EditableMesh DecimateMesh(const EditableMesh &source, int32_t targetTriangles,
							  std::string *error);

	// The number of boundary edges in a mesh: an edge whose twin is absent. Zero
	// means watertight. Exposed because "watertight" is the plan's own
	// verification criterion and a caller should not have to walk the half-edge
	// structure to count it.
	int32_t CountBoundaryEdges(const EditableMesh &mesh);

} // namespace Wasteland
