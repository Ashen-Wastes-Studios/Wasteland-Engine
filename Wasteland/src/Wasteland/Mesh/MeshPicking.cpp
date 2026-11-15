// MeshPicking.cpp -- Moller-Trumbore triangle picking. See MeshPicking.h for
// why this is a separate tested module rather than part of the panel.

#include "wlpch.h"

#include "Wasteland/Mesh/MeshPicking.h"

namespace Wasteland
{
namespace
{
	// Below this, a ray is treated as parallel to the triangle. The threshold
	// is on `a = dot(edge1, cross(dir, edge2))`, which is twice the signed area
	// of the projected triangle scaled by the direction's length -- so a small
	// or degenerate triangle and a long direction both shrink it. A fixed
	// epsilon is therefore only correct for unit directions and reasonable
	// triangles; the caller's `direction` is normalised by the panel before the
	// call, and a triangle smaller than ~1e-7 in a unit-epsilon test is
	// degenerate by any standard.
	constexpr float kParallelEpsilon = 1e-9f;

	void Cross(const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	}

	float Dot(const float a[3], const float b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}
} // namespace

bool IntersectTriangle(const float v0[3], const float v1[3], const float v2[3],
					   const float origin[3], const float direction[3],
					   float &outDistance, float &outU, float &outV)
{
	float edge1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
	float edge2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};

	float h[3];
	Cross(direction, edge2, h);
	const float a = Dot(edge1, h);

	// Parallel (or back-facing, which this routine does NOT cull -- a mesh
	// editor must pick back faces too, because a user orbits around a model and
	// the far side is often the side they clicked).
	if (a > -kParallelEpsilon && a < kParallelEpsilon)
		return false;

	const float f = 1.0f / a;
	float s[3] = {origin[0] - v0[0], origin[1] - v0[1], origin[2] - v0[2]};
	const float u = f * Dot(s, h);
	if (u < 0.0f || u > 1.0f)
		return false;

	float q[3];
	Cross(s, edge1, q);
	const float v = f * Dot(direction, q);
	if (v < 0.0f || u + v > 1.0f)
		return false;

	const float t = f * Dot(edge2, q);
	outDistance = t;
	outU = u;
	outV = v;
	return true;
}

namespace
{
	// Walks one face's three corners through the half-edge links. Shared with
	// the writer's own walk; a separate copy here keeps MeshPicking free of a
	// dependency on MeshWriter, and the two agree because both follow the
	// documented convention (Vertex = the vertex the edge STARTS at).
	bool FaceVertices(const EditableMesh &mesh, int32_t face, int32_t out[3])
	{
		const EditableMesh::Face &record = mesh.Faces[(size_t)face];
		if (record.FirstEdge < 0)
			return false;
		const int32_t first = record.FirstEdge;
		const int32_t second = mesh.Edges[(size_t)first].Next;
		if (second < 0)
			return false;
		const int32_t third = mesh.Edges[(size_t)second].Next;
		if (third < 0)
			return false;
		if (mesh.Edges[(size_t)third].Next != first)
			return false;

		out[0] = mesh.Edges[(size_t)first].Vertex;
		out[1] = mesh.Edges[(size_t)second].Vertex;
		out[2] = mesh.Edges[(size_t)third].Vertex;
		for (int corner = 0; corner < 3; ++corner)
		{
			if (out[corner] < 0 ||
				out[corner] >= (int32_t)mesh.Vertices.size())
				return false;
		}
		return out[0] != out[1] && out[1] != out[2] && out[0] != out[2];
	}
} // namespace

bool PickFaces(const EditableMesh &mesh, const int32_t *faces, int32_t faceCount,
			   const float origin[3], const float direction[3],
			   MeshPickResult &out)
{
	bool hit = false;
	float bestDistance = 0.0f;
	MeshPickResult best;

	for (int32_t index = 0; index < faceCount; ++index)
	{
		const int32_t face = faces[index];
		if (face < 0 || face >= (int32_t)mesh.Faces.size())
			continue;

		int32_t corners[3];
		if (!FaceVertices(mesh, face, corners))
			continue;

		const EditableMesh::Vertex &v0 = mesh.Vertices[(size_t)corners[0]];
		const EditableMesh::Vertex &v1 = mesh.Vertices[(size_t)corners[1]];
		const EditableMesh::Vertex &v2 = mesh.Vertices[(size_t)corners[2]];

		float distance = 0.0f, u = 0.0f, v = 0.0f;
		if (!IntersectTriangle(v0.Position, v1.Position, v2.Position,
							   origin, direction, distance, u, v))
			continue;

		// A hit BEHIND the origin is not a hit for a screen-space pick: the ray
		// starts at the camera and travels into the scene. Culling here rather
		// than trusting IntersectTriangle, which deliberately does not cull.
		if (distance < 0.0f)
			continue;

		if (!hit || distance < bestDistance)
		{
			hit = true;
			bestDistance = distance;
			best.Face = face;
			best.Distance = distance;
			best.U = u;
			best.V = v;
		}
		// Ties keep the FIRST face encountered (strict <), which is
		// deterministic across frames.
	}

	if (!hit)
	{
		out = MeshPickResult{};
		return false;
	}

	// The hit point is reconstructed from the barycentrics on the winning face.
	const int32_t corners[3] = {
		mesh.Edges[(size_t)mesh.Faces[(size_t)best.Face].FirstEdge].Vertex,
		mesh.Edges[(size_t)mesh.Edges[(size_t)mesh.Faces[(size_t)best.Face].FirstEdge].Next].Vertex,
		mesh.Edges[(size_t)mesh.Edges[(size_t)mesh.Edges[(size_t)mesh.Faces[(size_t)best.Face].FirstEdge].Next].Next].Vertex,
	};
	const EditableMesh::Vertex &v0 = mesh.Vertices[(size_t)corners[0]];
	const EditableMesh::Vertex &v1 = mesh.Vertices[(size_t)corners[1]];
	const EditableMesh::Vertex &v2 = mesh.Vertices[(size_t)corners[2]];
	const float w = 1.0f - best.U - best.V;
	for (int axis = 0; axis < 3; ++axis)
	{
		best.Point[axis] = w * v0.Position[axis] +
						   best.U * v1.Position[axis] +
						   best.V * v2.Position[axis];
	}

	out = best;
	return true;
}

bool PickFace(const EditableMesh &mesh,
			  const float origin[3], const float direction[3],
			  MeshPickResult &out)
{
	// A face list of 0..Faces-1 built on the stack for the common case. A mesh
	// with more faces than the buffer uses the heap; the threshold is far above
	// anything a level designer selects at once and small enough to stay on the
	// stack in the panel's per-frame call.
	const int32_t stackCapacity = 4096;
	int32_t stack[stackCapacity];
	int32_t *faces = stack;
	std::vector<int32_t> heap;

	if ((int32_t)mesh.Faces.size() > stackCapacity)
	{
		heap.resize(mesh.Faces.size());
		faces = heap.data();
	}
	for (size_t face = 0; face < mesh.Faces.size(); ++face)
		faces[face] = (int32_t)face;

	return PickFaces(mesh, faces, (int32_t)mesh.Faces.size(), origin, direction, out);
}

} // namespace Wasteland
