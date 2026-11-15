#include "wlpch.h"

#include "MeshRemesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Wasteland
{

namespace
{

	using Vec3 = std::array<float, 3>;

	inline float Dot3(const float a[3], const float b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	inline float DistanceSquared(const Vec3 &a, const Vec3 &b)
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return dx * dx + dy * dy + dz * dz;
	}

	// The three corners of a face, or false when the face is degenerate.
	// Duplicated from EditableMesh.cpp's anonymous namespace rather than
	// exposed: it is a modelling primitive, not part of the mesh's contract.
	bool FaceCorners(const EditableMesh &mesh, int32_t face, int32_t out[3])
	{
		if (face < 0 || face >= (int32_t)mesh.Faces.size())
			return false;
		const EditableMesh::Face &record = mesh.Faces[(size_t)face];
		if (record.FirstEdge < 0)
			return false;
		const int32_t first = record.FirstEdge;
		const int32_t second = mesh.Edges[(size_t)first].Next;
		if (second < 0)
			return false;
		const int32_t third = mesh.Edges[(size_t)second].Next;
		if (third < 0 || mesh.Edges[(size_t)third].Next != first)
			return false;
		out[0] = mesh.Edges[(size_t)first].Vertex;
		out[1] = mesh.Edges[(size_t)second].Vertex;
		out[2] = mesh.Edges[(size_t)third].Vertex;
		for (int corner = 0; corner < 3; ++corner)
		{
			if (out[corner] < 0 || out[corner] >= (int32_t)mesh.Vertices.size())
				return false;
		}
		return out[0] != out[1] && out[1] != out[2] && out[0] != out[2];
	}

	// Rebuilds an EditableMesh from a triangle soup. Weld merges by position,
	// so a degenerate triangle (two corners equal after the merge) is dropped
	// and the caller does not have to filter it.
	EditableMesh BuildFromTriangles(const std::vector<Vec3> &positions,
									const std::vector<int32_t> &indices)
	{
		MeshData data;
		data.Vertices.resize(indices.size());
		data.Indices.resize(indices.size());
		for (size_t i = 0; i < indices.size(); ++i)
		{
			const Vec3 &p = positions[(size_t)indices[i]];
			data.Vertices[i].Position[0] = p[0];
			data.Vertices[i].Position[1] = p[1];
			data.Vertices[i].Position[2] = p[2];
			// Placeholder; Weld keeps the first normal it saw, so a caller
			// that cares calls RecomputeNormals.
			data.Vertices[i].Normal[0] = 0.0f;
			data.Vertices[i].Normal[1] = 1.0f;
			data.Vertices[i].Normal[2] = 0.0f;
			data.Vertices[i].TexCoord[0] = 0.0f;
			data.Vertices[i].TexCoord[1] = 0.0f;
			data.Indices[i] = static_cast<uint32_t>(i);
		}
		data.RecomputeBounds();
		return EditableMesh::Weld(data);
	}

	// -----------------------------------------------------------------------
	// Point-triangle distance (Ericson, Real-Time Collision Detection 5.1.5).
	// Used by the voxel rasteriser to test each candidate voxel centre
	// against a triangle. The branchy region test is the standard one; the
	// alternative -- projecting onto the plane and testing barycentric
	// coordinates -- is shorter but reports a wrong distance for points
	// beyond a vertex, which is exactly the case a voxel grid produces.
	// -----------------------------------------------------------------------
	float PointTriangleDistanceSquared(const Vec3 &p, const Vec3 &a,
									   const Vec3 &b, const Vec3 &c)
	{
		const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		const float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};

		const float d1 = Dot3(ab, ap);
		const float d2 = Dot3(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f)
			return DistanceSquared(p, a);

		const float bp[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};
		const float d3 = Dot3(ab, bp);
		const float d4 = Dot3(ac, bp);
		if (d3 >= 0.0f && d4 <= d3)
			return DistanceSquared(p, b);

		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
		{
			const float v = d1 / (d1 - d3);
			const Vec3 q = {a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]};
			return DistanceSquared(p, q);
		}

		const float cp[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
		const float d5 = Dot3(ab, cp);
		const float d6 = Dot3(ac, cp);
		if (d6 >= 0.0f && d5 <= d6)
			return DistanceSquared(p, c);

		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
		{
			const float w = d2 / (d2 - d6);
			const Vec3 q = {a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]};
			return DistanceSquared(p, q);
		}

		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
		{
			const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			const Vec3 q = {b[0] + w * (c[0] - b[0]),
							b[1] + w * (c[1] - b[1]),
							b[2] + w * (c[2] - b[2])};
			return DistanceSquared(p, q);
		}

		const float denom = 1.0f / (va + vb + vc);
		const float v = vb * denom;
		const float w = vc * denom;
		const Vec3 q = {a[0] + ab[0] * v + ac[0] * w,
						a[1] + ab[1] * v + ac[1] * w,
						a[2] + ab[2] * v + ac[2] * w};
		return DistanceSquared(p, q);
	}

	int64_t GridDim(float minimum, float maximum, float voxelSize)
	{
		if (!(maximum > minimum))
			return 1;
		return static_cast<int64_t>(std::floor((maximum - minimum) / voxelSize)) + 1;
	}

} // namespace

// ---------------------------------------------------------------------------
// VoxelRemesh
// ---------------------------------------------------------------------------

EditableMesh VoxelRemesh(const EditableMesh &source, float voxelSize,
						 int64_t maxVoxels, std::string *error)
{
	const auto fail = [error](const char *reason)
	{
		if (error != nullptr)
			*error = reason;
		return EditableMesh{};
	};

	if (!std::isfinite(voxelSize) || voxelSize <= 0.0f)
		return fail("voxel size must be positive and finite");
	if (source.Vertices.empty() || source.Faces.empty())
		return fail("the source mesh is empty");
	if (maxVoxels <= 0)
		return fail("maxVoxels must be positive");

	float boundsMin[3] = {std::numeric_limits<float>::max(),
						  std::numeric_limits<float>::max(),
						  std::numeric_limits<float>::max()};
	float boundsMax[3] = {std::numeric_limits<float>::lowest(),
						  std::numeric_limits<float>::lowest(),
						  std::numeric_limits<float>::lowest()};
	for (const EditableMesh::Vertex &vertex : source.Vertices)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			boundsMin[axis] = std::min(boundsMin[axis], vertex.Position[axis]);
			boundsMax[axis] = std::max(boundsMax[axis], vertex.Position[axis]);
		}
	}

	// One voxel of margin on every side. The exterior flood fill below seeds
	// from the grid's outer layer, and that layer is only guaranteed to be
	// outside the model if the model is inset from the grid by at least one
	// voxel -- a tight bounding box would otherwise put model geometry on the
	// boundary layer and leave the fill with no seed.
	for (int axis = 0; axis < 3; ++axis)
	{
		boundsMin[axis] -= voxelSize;
		boundsMax[axis] += voxelSize;
	}

	const int64_t nx = GridDim(boundsMin[0], boundsMax[0], voxelSize);
	const int64_t ny = GridDim(boundsMin[1], boundsMax[1], voxelSize);
	const int64_t nz = GridDim(boundsMin[2], boundsMax[2], voxelSize);

	// Overflow first, then the caller's cap. The cap exists because the
	// occupancy array is a flat byte per voxel and a 2m asset at a 1mm
	// voxel is 8 gigabytes -- the caller chose the grid resolution and is
	// the only one who can judge whether it is sane.
	const int64_t total = nx * ny * nz;
	if (total < 0 || total > maxVoxels)
	{
		return fail("the voxel grid exceeds maxVoxels (raise the voxel size or the cap)");
	}

	std::vector<uint8_t> occupied((size_t)total, 0);
	const auto index = [nx, ny](int64_t i, int64_t j, int64_t k)
	{
		return static_cast<size_t>(i + nx * (j + ny * k));
	};

	// The tube radius is half a voxel DIAGONAL, not half a voxel. That is the
	// coverage guarantee: a voxel the surface passes through has its centre
	// within half a diagonal of the surface point, so it is always marked.
	// A half-voxel radius leaves gaps wherever a triangle threads between two
	// voxel centres, and the exterior flood fill below leaks straight through
	// those gaps into the interior -- the solid is then lost silently, with
	// nothing in the output looking wrong.
	//
	// The cost is a systematic outward offset of up to half a diagonal, plus
	// another half diagonal at the shell's corners, which is the tolerance
	// the tests state.
	const float tubeRadius = voxelSize * 0.5f * std::sqrt(3.0f);
	const float tubeRadiusSquared = tubeRadius * tubeRadius;

	for (int32_t face = 0; face < (int32_t)source.Faces.size(); ++face)
	{
		int32_t corners[3];
		if (!FaceCorners(source, face, corners))
			continue;

		const Vec3 a = {source.Vertices[(size_t)corners[0]].Position[0],
						source.Vertices[(size_t)corners[0]].Position[1],
						source.Vertices[(size_t)corners[0]].Position[2]};
		const Vec3 b = {source.Vertices[(size_t)corners[1]].Position[0],
						source.Vertices[(size_t)corners[1]].Position[1],
						source.Vertices[(size_t)corners[1]].Position[2]};
		const Vec3 c = {source.Vertices[(size_t)corners[2]].Position[0],
						source.Vertices[(size_t)corners[2]].Position[1],
						source.Vertices[(size_t)corners[2]].Position[2]};

		float triangleMin[3];
		float triangleMax[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			triangleMin[axis] = std::min({a[axis], b[axis], c[axis]});
			triangleMax[axis] = std::max({a[axis], b[axis], c[axis]});
		}

		// Voxel indices covering the triangle's AABB expanded by the tube
		// radius: any voxel centre within that distance of the triangle lies
		// inside this box.
		int64_t range[3][2];
		bool empty = false;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float low = triangleMin[axis] - tubeRadius;
			const float high = triangleMax[axis] + tubeRadius;
			int64_t first = static_cast<int64_t>(
				std::floor((low - boundsMin[axis]) / voxelSize));
			int64_t last = static_cast<int64_t>(
				std::floor((high - boundsMin[axis]) / voxelSize));
			const int64_t extent = (axis == 0) ? nx : (axis == 1 ? ny : nz);
			first = std::max<int64_t>(first, 0);
			last = std::min<int64_t>(last, extent - 1);
			if (first > last)
			{
				empty = true;
				break;
			}
			range[axis][0] = first;
			range[axis][1] = last;
		}
		if (empty)
			continue;

		for (int64_t k = range[2][0]; k <= range[2][1]; ++k)
		{
			for (int64_t j = range[1][0]; j <= range[1][1]; ++j)
			{
				for (int64_t i = range[0][0]; i <= range[0][1]; ++i)
				{
					if (occupied[index(i, j, k)])
						continue;

					// Mark when the voxel's CENTRE is within the tube radius.
					// With the radius at half a diagonal this is also the
					// coverage guarantee: a voxel the surface passes through
					// always has its centre within half a diagonal of the
					// surface point, so no triangle can slip between two
					// voxel centres unmarked.
					//
					// An AABB-overlap test was here first and was wrong: a
					// triangle's AABB is much larger than the triangle, so it
					// marked voxels two and three voxels off the surface and
					// the remesh came back visibly inflated.
					const Vec3 centre = {boundsMin[0] + ((float)i + 0.5f) * voxelSize,
										 boundsMin[1] + ((float)j + 0.5f) * voxelSize,
										 boundsMin[2] + ((float)k + 0.5f) * voxelSize};
					if (PointTriangleDistanceSquared(centre, a, b, c) <= tubeRadiusSquared)
						occupied[index(i, j, k)] = 1;
				}
			}
		}
	}

	// Exterior flood fill. The tube above is a closed barrier around the
	// surface, so a 6-connected fill starting from the grid's outer layer
	// cannot reach the interior. Everything it cannot reach is inside the
	// model and joins the solid.
	//
	// Without this step the result is a hollow shell: the tube's own inner
	// boundary would be extracted as a second surface, and a closed asset
	// would come back as a thin-walled void with nothing but a radius
	// histogram to give it away.
	std::vector<uint8_t> exterior((size_t)total, 0);
	std::vector<int64_t> flood;
	flood.reserve((size_t)std::min<int64_t>(total, 1 << 20));

	const auto seed = [&](int64_t i, int64_t j, int64_t k)
	{
		const size_t at = index(i, j, k);
		if (occupied[at] || exterior[at])
			return;
		exterior[at] = 1;
		flood.push_back(at);
	};

	for (int64_t k = 0; k < nz; ++k)
	{
		for (int64_t j = 0; j < ny; ++j)
		{
			seed(0, j, k);
			seed(nx - 1, j, k);
		}
	}
	for (int64_t k = 0; k < nz; ++k)
	{
		for (int64_t i = 0; i < nx; ++i)
		{
			seed(i, 0, k);
			seed(i, ny - 1, k);
		}
	}
	for (int64_t j = 0; j < ny; ++j)
	{
		for (int64_t i = 0; i < nx; ++i)
		{
			seed(i, j, 0);
			seed(i, j, nz - 1);
		}
	}

	size_t cursor = 0;
	while (cursor < flood.size())
	{
		const int64_t at = flood[cursor++];
		const int64_t k = at / (nx * ny);
		const int64_t remainder = at - k * (nx * ny);
		const int64_t j = remainder / nx;
		const int64_t i = remainder - j * nx;
		if (i > 0)
			seed(i - 1, j, k);
		if (i + 1 < nx)
			seed(i + 1, j, k);
		if (j > 0)
			seed(i, j - 1, k);
		if (j + 1 < ny)
			seed(i, j + 1, k);
		if (k > 0)
			seed(i, j, k - 1);
		if (k + 1 < nz)
			seed(i, j, k + 1);
	}

	// Everything still unmarked is interior, and becomes part of the solid.
	for (size_t at = 0; at < (size_t)total; ++at)
	{
		if (!occupied[at] && !exterior[at])
			occupied[at] = 1;
	}

	// Shell extraction. Every occupied voxel emits one quad per EMPTY
	// neighbour. Internal faces are therefore emitted zero times (both
	// sides see an occupied neighbour) and boundary faces exactly once, so
	// the shell is closed by construction -- there is no stitching step
	// that could fail. This is the boundary of the SOLID, so there is one
	// closed surface, not the two a hollow shell would give.
	//
	// Winding is fixed per direction and verified against the cross product
	// of the first two edges: for +X the order (1,0,0),(1,1,0),(1,1,1),
	// (1,0,1) gives (0,1,0) x (0,0,1) = +X.
	struct Quad
	{
		Vec3 corner[4];
	};
	std::vector<Quad> quads;

	const auto occupiedAt = [&](int64_t i, int64_t j, int64_t k) -> bool
	{
		if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz)
			return false;
		return occupied[index(i, j, k)] != 0;
	};

	const auto emitQuad = [&](int64_t i, int64_t j, int64_t k,
							  const int (&offsets)[4][3])
	{
		Quad quad;
		for (int corner = 0; corner < 4; ++corner)
		{
			quad.corner[corner] = {
				boundsMin[0] + ((float)i + (float)offsets[corner][0]) * voxelSize,
				boundsMin[1] + ((float)j + (float)offsets[corner][1]) * voxelSize,
				boundsMin[2] + ((float)k + (float)offsets[corner][2]) * voxelSize};
		}
		quads.push_back(quad);
	};

	for (int64_t k = 0; k < nz; ++k)
	{
		for (int64_t j = 0; j < ny; ++j)
		{
			for (int64_t i = 0; i < nx; ++i)
			{
				if (!occupied[index(i, j, k)])
					continue;
				if (!occupiedAt(i + 1, j, k))
					emitQuad(i, j, k, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}});
				if (!occupiedAt(i - 1, j, k))
					emitQuad(i, j, k, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}});
				if (!occupiedAt(i, j + 1, k))
					emitQuad(i, j, k, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}});
				if (!occupiedAt(i, j - 1, k))
					emitQuad(i, j, k, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}});
				if (!occupiedAt(i, j, k + 1))
					emitQuad(i, j, k, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}});
				if (!occupiedAt(i, j, k - 1))
					emitQuad(i, j, k, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}});
			}
		}
	}

	if (quads.empty())
	{
		return fail("the voxel grid captured no surface (the voxel size may be "
					"larger than the mesh)");
	}

	std::vector<Vec3> positions;
	std::vector<int32_t> indices;
	positions.reserve(quads.size() * 4);
	indices.reserve(quads.size() * 6);
	for (const Quad &quad : quads)
	{
		const int32_t base = static_cast<int32_t>(positions.size());
		for (int corner = 0; corner < 4; ++corner)
			positions.push_back(quad.corner[corner]);
		// Two triangles per quad, both wound so the shared diagonal is
		// traversed in opposite directions.
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}

	EditableMesh result = BuildFromTriangles(positions, indices);
	result.RecomputeNormals();
	if (!result.IsValid())
	{
		return fail("the extracted shell failed validation");
	}
	return result;
}

// ---------------------------------------------------------------------------
// DecimateMesh
// ---------------------------------------------------------------------------

namespace
{

	struct Quadric
	{
		// Row-major 4x4, stored flat. A symmetric matrix only needs ten
		// entries, but the full sixteen keep the indexing obvious and the
		// memory cost is 64 bytes per vertex.
		std::array<float, 16> m{};

		Quadric &operator+=(const Quadric &other)
		{
			for (size_t i = 0; i < m.size(); ++i)
				m[i] += other.m[i];
			return *this;
		}
	};

	// The outer product n*n^T for a plane (a,b,c,d). This is the fundamental
	// quadric: the squared distance from a point to the plane is
	// (p . n)^2 = p^T (n n^T) p with p homogeneous.
	Quadric PlaneQuadric(const float plane[4])
	{
		Quadric out;
		for (int row = 0; row < 4; ++row)
		{
			for (int column = 0; column < 4; ++column)
				out.m[(size_t)row * 4 + column] = plane[row] * plane[column];
		}
		return out;
	}

	// Solves the 3x3 system for the position minimising the summed quadric.
	// Returns false when the system is singular, which happens on flat
	// regions (all planes parallel) and on a mesh whose faces are coplanar --
	// the caller then falls back to the midpoint, which is the right answer
	// there anyway.
	bool SolveOptimalPosition(const Quadric &quadric, Vec3 &out)
	{
		const float a11 = quadric.m[0], a12 = quadric.m[1], a13 = quadric.m[2];
		const float a22 = quadric.m[5], a23 = quadric.m[6];
		const float a33 = quadric.m[10];
		const float b1 = -quadric.m[3], b2 = -quadric.m[7], b3 = -quadric.m[11];

		const float det = a11 * (a22 * a33 - a23 * a23)
						  - a12 * (a12 * a33 - a23 * a13)
						  + a13 * (a12 * a23 - a22 * a13);
		if (!std::isfinite(det) || std::fabs(det) < 1e-12f)
			return false;

		const float inv = 1.0f / det;
		out[0] = inv * ((a22 * a33 - a23 * a23) * b1
						+ (a13 * a23 - a12 * a33) * b2
						+ (a12 * a23 - a13 * a22) * b3);
		out[1] = inv * ((a13 * a23 - a12 * a33) * b1
						+ (a11 * a33 - a13 * a13) * b2
						+ (a12 * a13 - a11 * a23) * b3);
		out[2] = inv * ((a12 * a23 - a13 * a22) * b1
						+ (a12 * a13 - a11 * a23) * b2
						+ (a11 * a22 - a12 * a12) * b3);
		return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
	}

	float EvaluateQuadric(const Quadric &quadric, const Vec3 &p)
	{
		const float v[4] = {p[0], p[1], p[2], 1.0f};
		float row[4];
		for (int r = 0; r < 4; ++r)
		{
			row[r] = quadric.m[(size_t)r * 4 + 0] * v[0]
					 + quadric.m[(size_t)r * 4 + 1] * v[1]
					 + quadric.m[(size_t)r * 4 + 2] * v[2]
					 + quadric.m[(size_t)r * 4 + 3] * v[3];
		}
		return v[0] * row[0] + v[1] * row[1] + v[2] * row[2] + v[3] * row[3];
	}

	struct HeapEntry
	{
		float Cost;
		int32_t U;
		int32_t V;
		uint32_t VersionU;
		uint32_t VersionV;
	};

	struct HeapCompare
	{
		bool operator()(const HeapEntry &a, const HeapEntry &b) const
		{
			return a.Cost > b.Cost;
		}
	};

} // namespace

EditableMesh DecimateMesh(const EditableMesh &source, int32_t targetTriangles,
						  std::string *error)
{
	const auto fail = [error](const char *reason)
	{
		if (error != nullptr)
			*error = reason;
		return EditableMesh{};
	};

	if (source.Vertices.empty() || source.Faces.empty())
		return fail("the source mesh is empty");
	if (targetTriangles < 4)
		return fail("targetTriangles must be at least 4 (a mesh cannot have fewer)");

	std::vector<Vec3> positions;
	positions.reserve(source.Vertices.size());
	for (const EditableMesh::Vertex &vertex : source.Vertices)
		positions.push_back({vertex.Position[0], vertex.Position[1], vertex.Position[2]});

	std::vector<std::array<int32_t, 3>> triangles;
	for (int32_t face = 0; face < (int32_t)source.Faces.size(); ++face)
	{
		int32_t corners[3];
		if (!FaceCorners(source, face, corners))
			continue;
		triangles.push_back({corners[0], corners[1], corners[2]});
	}

	if (targetTriangles >= (int32_t)triangles.size())
	{
		return fail("targetTriangles is not below the current triangle count "
					"(a decimation that does not decimate is a caller bug)");
	}

	const int32_t vertexCount = (int32_t)positions.size();
	const size_t triangleCount = triangles.size();

	std::vector<uint8_t> aliveVertex((size_t)vertexCount, 1);
	std::vector<uint32_t> version((size_t)vertexCount, 0);
	std::vector<Quadric> quadrics((size_t)vertexCount);
	std::vector<std::unordered_set<int32_t>> adjacency((size_t)vertexCount);
	std::vector<std::vector<int32_t>> vertexTriangles((size_t)vertexCount);
	std::vector<uint8_t> aliveTriangle(triangleCount, 1);

	auto addEdge = [&](int32_t a, int32_t b)
	{
		if (a == b)
			return;
		adjacency[(size_t)a].insert(b);
		adjacency[(size_t)b].insert(a);
	};

	for (size_t t = 0; t < triangleCount; ++t)
	{
		const std::array<int32_t, 3> &triangle = triangles[t];
		vertexTriangles[(size_t)triangle[0]].push_back((int32_t)t);
		vertexTriangles[(size_t)triangle[1]].push_back((int32_t)t);
		vertexTriangles[(size_t)triangle[2]].push_back((int32_t)t);
		addEdge(triangle[0], triangle[1]);
		addEdge(triangle[1], triangle[2]);
		addEdge(triangle[0], triangle[2]);

		// Face plane, area-weighted by the cross product magnitude being left
		// in the normal: a large face contributes a proportionally larger
		// quadric, so the solver protects it.
		const Vec3 &a = positions[(size_t)triangle[0]];
		const Vec3 &b = positions[(size_t)triangle[1]];
		const Vec3 &c = positions[(size_t)triangle[2]];
		const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		const float normal[3] = {ab[1] * ac[2] - ab[2] * ac[1],
								 ab[2] * ac[0] - ab[0] * ac[2],
								 ab[0] * ac[1] - ab[1] * ac[0]};
		const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]
									   + normal[2] * normal[2]);
		if (length <= 0.0f)
			continue;
		const float invLength = 1.0f / length;
		const float plane[4] = {normal[0] * invLength, normal[1] * invLength,
								normal[2] * invLength,
								-(normal[0] * a[0] + normal[1] * a[1]
								  + normal[2] * a[2]) * invLength};
		const Quadric faceQuadric = PlaneQuadric(plane);
		quadrics[(size_t)triangle[0]] += faceQuadric;
		quadrics[(size_t)triangle[1]] += faceQuadric;
		quadrics[(size_t)triangle[2]] += faceQuadric;
	}

	auto edgeCost = [&](int32_t u, int32_t v, Vec3 &optimal) -> float
	{
		Quadric sum = quadrics[(size_t)u];
		sum += quadrics[(size_t)v];
		if (!SolveOptimalPosition(sum, optimal))
		{
			optimal = {(positions[(size_t)u][0] + positions[(size_t)v][0]) * 0.5f,
					   (positions[(size_t)u][1] + positions[(size_t)v][1]) * 0.5f,
					   (positions[(size_t)u][2] + positions[(size_t)v][2]) * 0.5f};
		}
		return EvaluateQuadric(sum, optimal);
	};

	std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapCompare> heap;
	for (int32_t u = 0; u < vertexCount; ++u)
	{
		for (int32_t v : adjacency[(size_t)u])
		{
			if (v <= u)
				continue; // each undirected edge once
			Vec3 optimal;
			const float cost = edgeCost(u, v, optimal);
			heap.push({cost, u, v, version[(size_t)u], version[(size_t)v]});
		}
	}

	int32_t aliveTriangleCount = (int32_t)triangleCount;
	while (aliveTriangleCount > targetTriangles && !heap.empty())
	{
		const HeapEntry entry = heap.top();
		heap.pop();
		if (!aliveVertex[(size_t)entry.U] || !aliveVertex[(size_t)entry.V])
			continue;
		if (version[(size_t)entry.U] != entry.VersionU
			|| version[(size_t)entry.V] != entry.VersionV)
			continue;

		Vec3 optimal;
		edgeCost(entry.U, entry.V, optimal);

		positions[(size_t)entry.U] = optimal;
		quadrics[(size_t)entry.U] += quadrics[(size_t)entry.V];
		aliveVertex[(size_t)entry.V] = 0;
		++version[(size_t)entry.U];

		// Retriangulate around the removed endpoint. A triangle that contains
		// both endpoints becomes degenerate and dies; every other triangle
		// containing V is rewritten to U.
		for (int32_t t : vertexTriangles[(size_t)entry.V])
		{
			if (!aliveTriangle[(size_t)t])
				continue;
			std::array<int32_t, 3> &triangle = triangles[(size_t)t];
			for (int32_t &corner : triangle)
			{
				if (corner == entry.V)
					corner = entry.U;
			}
			if (triangle[0] == triangle[1] || triangle[1] == triangle[2]
				|| triangle[0] == triangle[2])
			{
				aliveTriangle[(size_t)t] = 0;
				--aliveTriangleCount;
				continue;
			}
			vertexTriangles[(size_t)entry.U].push_back(t);
		}
		vertexTriangles[(size_t)entry.V].clear();

		for (int32_t w : adjacency[(size_t)entry.V])
		{
			if (w == entry.U || !aliveVertex[(size_t)w])
				continue;
			adjacency[(size_t)w].erase(entry.V);
			adjacency[(size_t)w].insert(entry.U);
			adjacency[(size_t)entry.U].insert(w);
		}
		adjacency[(size_t)entry.V].clear();
		adjacency[(size_t)entry.U].erase(entry.V);

		// Every edge around U has a new cost, so the old heap entries for
		// them are stale and the version check discards them.
		for (int32_t w : adjacency[(size_t)entry.U])
		{
			if (!aliveVertex[(size_t)w])
				continue;
			Vec3 optimalPosition;
			const float cost = edgeCost(entry.U, w, optimalPosition);
			heap.push({cost, entry.U, w, version[(size_t)entry.U],
					   version[(size_t)w]});
		}
	}

	std::vector<int32_t> indices;
	indices.reserve((size_t)aliveTriangleCount * 3);
	for (size_t t = 0; t < triangleCount; ++t)
	{
		if (!aliveTriangle[t])
			continue;
		for (int corner = 0; corner < 3; ++corner)
			indices.push_back(triangles[t][(size_t)corner]);
	}

	if (indices.empty())
		return fail("decimation removed every triangle");

	EditableMesh result = BuildFromTriangles(positions, indices);
	result.RecomputeNormals();
	return result;
}

int32_t CountBoundaryEdges(const EditableMesh &mesh)
{
	int32_t count = 0;
	for (const EditableMesh::HalfEdge &edge : mesh.Edges)
	{
		if (edge.Twin < 0)
			++count;
	}
	return count;
}

} // namespace Wasteland
