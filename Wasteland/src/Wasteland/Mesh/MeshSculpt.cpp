// MeshSculpt.cpp -- see MeshSculpt.h for the scope and the two invariants
// (the falloff shape and the radius clamp).

#include "wlpch.h"

#include "Wasteland/Mesh/MeshSculpt.h"

#include <array>
#include <cmath>
#include <vector>

namespace Wasteland
{
namespace
{
	bool IsFinite(float value)
	{
		return std::isfinite(value) != 0;
	}

	// The vertex indices reachable from a vertex in one edge step. Built once
	// per stroke because a smooth brush needs the neighbourhood of every
	// affected vertex, and rebuilding it per vertex is quadratic.
	std::vector<std::vector<int32_t>> BuildAdjacency(const EditableMesh &mesh)
	{
		std::vector<std::vector<int32_t>> neighbours(mesh.Vertices.size());
		for (size_t edge = 0; edge < mesh.Edges.size(); ++edge)
		{
			const int32_t from = mesh.Edges[edge].Vertex;
			const int32_t next = mesh.Edges[edge].Next;
			if (from < 0 || from >= (int32_t)mesh.Vertices.size() || next < 0)
				continue;
			const int32_t to = mesh.Edges[(size_t)next].Vertex;
			if (to < 0 || to >= (int32_t)mesh.Vertices.size() || to == from)
				continue;
			neighbours[(size_t)from].push_back(to);
			neighbours[(size_t)to].push_back(from);
		}
		return neighbours;
	}

	void AddScaled(float out[3], const float vector[3], float scale)
	{
		out[0] += vector[0] * scale;
		out[1] += vector[1] * scale;
		out[2] += vector[2] * scale;
	}

	// A plain in-place scale. NOT AddScaled(v, v, s), which is v*(1+s) -- that
	// off-by-one silently doubled the smooth, flatten and crease displacements.
	void Scale(float vector[3], float scale)
	{
		vector[0] *= scale;
		vector[1] *= scale;
		vector[2] *= scale;
	}

	float Length(const float vector[3])
	{
		return std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] +
						 vector[2] * vector[2]);
	}

	float Length(const std::array<float, 3> &vector)
	{
		return Length(vector.data());
	}

	// Clamps a displacement to the brush radius. The clamp is a MAGNITUDE clamp
	// on the total stroke, not per axis -- per-axis clamping would shrink a
	// diagonal drag by 1/sqrt(3), which is the kind of thing nobody notices
	// until the brush feels weak in corners.
	void ClampToRadius(float displacement[3], float radius)
	{
		const float magnitude = Length(displacement);
		if (magnitude > radius && magnitude > 0.0f)
		{
			const float scale = radius / magnitude;
			displacement[0] *= scale;
			displacement[1] *= scale;
			displacement[2] *= scale;
		}
	}
} // namespace

	float SculptFalloff(float distance, float radius)
	{
		if (radius <= 0.0f)
			return 0.0f;
		if (distance >= radius)
			return 0.0f;
		const float t = distance / radius;
		const float inner = 1.0f - t * t;
		return inner * inner;
	}

	void SculptCommit(EditableMesh &mesh)
	{
		mesh.RecomputeNormals();
	}

	float SculptStrokeApply(EditableMesh &mesh, const SculptStroke &stroke)
	{
		if (stroke.Radius <= 0.0f || !IsFinite(stroke.Radius))
			return -1.0f;
		if (!IsFinite(stroke.Strength))
			return -1.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!IsFinite(stroke.Center[axis]))
				return -1.0f;
		}

		const std::vector<std::vector<int32_t>> neighbours = BuildAdjacency(mesh);
		const float radius = stroke.Radius;
		const float strength = stroke.Strength;

		// Accumulate the displacement first and apply it afterwards. Applying
		// as we go would let one vertex's new position influence its neighbour's
		// Laplacian in the same stroke, which makes the result depend on vertex
		// order -- a stroke that gives different answers on the same mesh is
		// the worst possible property for a brush.
		std::vector<std::array<float, 3>> displacement(mesh.Vertices.size(),
													   std::array<float, 3>{0.0f,
																			0.0f,
																			0.0f});
		std::vector<float> affectedWeight(mesh.Vertices.size(), 0.0f);
		int32_t affectedCount = 0;
		double totalDisplacement = 0.0;

		for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
		{
			const float *position = mesh.Vertices[vertex].Position;
			const float dx = position[0] - stroke.Center[0];
			const float dy = position[1] - stroke.Center[1];
			const float dz = position[2] - stroke.Center[2];
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

			const float falloff = SculptFalloff(distance, radius);
			if (falloff <= 0.0f)
				continue;

			float weight = falloff * strength;
			// The mask is indexed by vertex. A mask SHORTER than the vertex
			// array is treated as unmasked for the missing entries rather than
			// as an error: the mask is authored data, and a brush must not
			// refuse to run because a mask is one vertex short. An empty mask
			// means no mask at all.
			if (stroke.Mask.size() > vertex)
				weight *= stroke.Mask[vertex];
			if (weight <= 0.0f)
				continue;

			float move[3] = {0.0f, 0.0f, 0.0f};

			switch (stroke.Brush)
			{
			case SculptBrush::Grab:
				AddScaled(move, stroke.Direction, weight);
				break;

			case SculptBrush::Smooth:
			{
				// Laplacian: toward the centroid of the one-ring. The step is
				// scaled by `weight` rather than by a fixed 0.5, so the brush's
				// strength and falloff both act on it.
				const std::vector<int32_t> &ring = neighbours[vertex];
				if (ring.empty())
					continue;
				double average[3] = {0.0, 0.0, 0.0};
				for (const int32_t other : ring)
				{
					const float *neighbour = mesh.Vertices[(size_t)other].Position;
					average[0] += neighbour[0];
					average[1] += neighbour[1];
					average[2] += neighbour[2];
				}
				const float inverse = 1.0f / (float)ring.size();
				move[0] = (float)average[0] * inverse - position[0];
				move[1] = (float)average[1] * inverse - position[1];
				move[2] = (float)average[2] * inverse - position[2];
				Scale(move, weight);
				break;
			}

			case SculptBrush::Inflate:
			{
				// Along the vertex's own normal, not the brush centre's. A
				// per-vertex normal is what makes a sphere inflate into a
				// larger sphere; a single centre normal would flatten one side.
				const float *normal = mesh.Vertices[vertex].Normal;
				AddScaled(move, normal, weight * radius);
				break;
			}

			case SculptBrush::Flatten:
			{
				// The signed distance to the plane, moved toward zero. Using
				// the plane's own point (not the brush centre) keeps the
				// flatten surface stable as the brush is dragged -- otherwise
				// the plane would jump every time the cursor moved.
				const float nx = stroke.PlaneNormal[0];
				const float ny = stroke.PlaneNormal[1];
				const float nz = stroke.PlaneNormal[2];
				const float signedDistance =
					(position[0] - stroke.PlanePoint[0]) * nx +
					(position[1] - stroke.PlanePoint[1]) * ny +
					(position[2] - stroke.PlanePoint[2]) * nz;
				move[0] = -signedDistance * nx;
				move[1] = -signedDistance * ny;
				move[2] = -signedDistance * nz;
				Scale(move, weight);
				break;
			}

			case SculptBrush::Crease:
			{
				// Toward the hit point. The vector is (Center - position), so
				// the crease is deepest at the brush centre and fades with the
				// falloff -- pushing toward the centre pinches the surface.
				move[0] = stroke.Center[0] - position[0];
				move[1] = stroke.Center[1] - position[1];
				move[2] = stroke.Center[2] - position[2];
				Scale(move, weight);
				break;
			}
			}

			ClampToRadius(move, radius);
			displacement[vertex] = {move[0], move[1], move[2]};
			affectedWeight[vertex] = weight;
		}

		for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
		{
			const std::array<float, 3> &move = displacement[vertex];
			if (move[0] == 0.0f && move[1] == 0.0f && move[2] == 0.0f)
				continue;
			const float *position = mesh.Vertices[vertex].Position;
			mesh.SetVertexPosition(static_cast<int32_t>(vertex),
								   position[0] + move[0],
								   position[1] + move[1],
								   position[2] + move[2]);
			++affectedCount;
			totalDisplacement += Length(move);
		}

		SculptCommit(mesh);

		// The mean over the AFFECTED vertices, not over every vertex in the
		// mesh. Averaging over the whole mesh would let a large mesh report a
		// near-zero mean for a stroke that moved its target a great deal, which
		// is the opposite of what the number is for.
		return affectedCount > 0
				   ? static_cast<float>(totalDisplacement / affectedCount)
				   : 0.0f;
	}

} // namespace Wasteland
