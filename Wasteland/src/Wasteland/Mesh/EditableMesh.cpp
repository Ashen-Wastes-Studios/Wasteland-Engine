#include "wlpch.h"

#include "EditableMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Wasteland {

	namespace
	{
		// The weld key. Position only -- see the comment on Weld.
		//
		// A tuple rather than a string: a position is already three floats,
		// and formatting it into a string would be slower and would depend on
		// printf precision to decide whether two identical positions are
		// equal. Exact float comparison is right here because the weld is
		// merging values that came from the SAME source -- a cube's shared
		// corners are bit-identical in the OBJ, so a tolerance would only
		// risk merging two vertices an author meant to keep apart.
		using PositionKey = std::tuple<float, float, float>;

		PositionKey KeyOf(const float position[3])
		{
			return std::make_tuple(position[0], position[1], position[2]);
		}

		// Cross product, written out rather than pulled from a vector library:
		// the three components are all this file needs, and a dependency for
		// one formula is not worth the include.
		void Cross(const float a[3], const float b[3], float out[3])
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		float Length(const float v[3])
		{
			return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		}

		void Normalise(float v[3])
		{
			const float length = Length(v);
			// A zero-length normal is a DEGENERATE triangle, not a direction.
			// Leaving it at the default +Y rather than dividing by zero is the
			// difference between a mesh that renders and a mesh that produces
			// NaNs which then propagate through every consumer.
			if (length <= 1e-20f)
			{
				v[0] = 0.0f;
				v[1] = 1.0f;
				v[2] = 0.0f;
				return;
			}
			const float inverse = 1.0f / length;
			v[0] *= inverse;
			v[1] *= inverse;
			v[2] *= inverse;
		}

		// Formats one defect line. A helper so every check reports in the same
		// shape and a caller can grep for the node id.
		void Append(std::string &out, const std::string &message)
		{
			if (!out.empty())
				out += "\n";
			out += message;
		}
	} // namespace

	EditableMesh EditableMesh::Weld(const MeshData &source)
	{
		EditableMesh mesh;

		// The position -> vertex index map. A std::map rather than an
		// unordered_map because the key is a tuple and the hash would need a
		// combine helper; the weld is an import-time cost, not a per-frame one,
		// so clarity wins.
		std::map<PositionKey, int32_t> byPosition;

		// Reserve so the vertex vector does not reallocate mid-build. The
		// upper bound is the corner count; the real count is lower.
		mesh.Vertices.reserve(source.Vertices.size());

		// First pass: one vertex per UNIQUE position, and a corner -> vertex
		// translation table. Building the table in the same pass as the
		// vertices is what keeps the two in agreement without a second walk.
		std::vector<int32_t> cornerToVertex;
		cornerToVertex.reserve(source.Vertices.size());
		for (const MeshVertex &corner : source.Vertices)
		{
			const PositionKey key = KeyOf(corner.Position);
			auto found = byPosition.find(key);
			int32_t index;
			if (found == byPosition.end())
			{
				index = (int32_t)mesh.Vertices.size();
				EditableMesh::Vertex vertex;
				vertex.Position[0] = corner.Position[0];
				vertex.Position[1] = corner.Position[1];
				vertex.Position[2] = corner.Position[2];
				vertex.TexCoord[0] = corner.TexCoord[0];
				vertex.TexCoord[1] = corner.TexCoord[1];
				vertex.Normal[0] = corner.Normal[0];
				vertex.Normal[1] = corner.Normal[1];
				vertex.Normal[2] = corner.Normal[2];
				mesh.Vertices.push_back(vertex);
				byPosition.emplace(key, index);
			}
			else
			{
				index = found->second;
			}
			cornerToVertex.push_back(index);
		}

		// The edge map, keyed by the ORDERED pair (start, end). An ordered
		// key is what makes the twin lookup a single search: the reverse edge
		// (b, a) is the twin of (a, b), so when the second triangle bordering
		// an edge is welded the reverse key is already present.
		//
		// A std::map of pairs for the same reason as above.
		std::map<std::pair<int32_t, int32_t>, int32_t> edgeByEndpoints;

		mesh.Edges.reserve((size_t)source.GetTriangleCount() * 3);
		mesh.Faces.reserve((size_t)source.GetTriangleCount());

		for (uint32_t triangle = 0; triangle < source.GetTriangleCount(); ++triangle)
		{
			const size_t base = (size_t)triangle * 3;
			if (base + 2 >= cornerToVertex.size())
				break; // a truncated mesh: stop rather than read past the end.

			const int32_t v0 = cornerToVertex[base];
			const int32_t v1 = cornerToVertex[base + 1];
			const int32_t v2 = cornerToVertex[base + 2];

			const int32_t faceIndex = (int32_t)mesh.Faces.size();
			const int32_t firstEdge = (int32_t)mesh.Edges.size();

			// Three half-edges, wound v0 -> v1 -> v2 -> v0. The winding is
			// taken from the source triangle rather than enforced, so a mesh
			// with flipped faces stays flipped -- Weld is not the place to
			// silently change an author's normals.
			const int32_t order[3] = {v0, v1, v2};
			for (int local = 0; local < 3; ++local)
			{
				const int32_t start = order[local];
				const int32_t end = order[(local + 1) % 3];

				EditableMesh::HalfEdge edge;
				edge.Vertex = start;
				edge.Next = firstEdge + ((local + 1) % 3);
				edge.Face = faceIndex;
				edge.Twin = -1;

				const int32_t edgeIndex = (int32_t)mesh.Edges.size();
				mesh.Edges.push_back(edge);

				// Link the vertex's outgoing-edge list. Only the FIRST edge
				// leaving a vertex is stored on the vertex; the rest are
				// reached by walking the face loops, which is what keeps the
				// structure to four ints per edge.
				if (mesh.Vertices[(size_t)start].FirstEdge < 0)
					mesh.Vertices[(size_t)start].FirstEdge = edgeIndex;

				const std::pair<int32_t, int32_t> key(start, end);
				const std::pair<int32_t, int32_t> reverse(end, start);

				auto reverseEdge = edgeByEndpoints.find(reverse);
				if (reverseEdge != edgeByEndpoints.end())
				{
					// The neighbouring triangle has already been welded.
					// Link both directions NOW, because the reverse edge may
					// belong to an earlier triangle and will never be visited
					// again.
					const int32_t twin = reverseEdge->second;
					mesh.Edges[(size_t)edgeIndex].Twin = twin;
					mesh.Edges[(size_t)twin].Twin = edgeIndex;
				}
				else
				{
					edgeByEndpoints.emplace(key, edgeIndex);
				}
			}

			EditableMesh::Face face;
			face.FirstEdge = firstEdge;
			mesh.Faces.push_back(face);
		}

		return mesh;
	}

	MeshData EditableMesh::Deindex() const
	{
		MeshData data;

		// One corner per triangle vertex, exactly as MeshLoader produces.
		data.Vertices.reserve((size_t)GetFaceCount() * 3);
		data.Indices.reserve((size_t)GetFaceCount() * 3);

		uint32_t nextIndex = 0;
		for (const Face &face : Faces)
		{
			if (face.FirstEdge < 0)
				continue;

			// Walk the face loop rather than assuming three edges. A triangle
			// is guaranteed by IsTriangulated, but walking is free and makes
			// the function correct for a mesh that has been triangulated by a
			// future n-gon path.
			const int32_t first = face.FirstEdge;
			int32_t edge = first;
			do
			{
				const HalfEdge &half = Edges[(size_t)edge];
				const Vertex &vertex = Vertices[(size_t)half.Vertex];

				MeshVertex corner;
				corner.Position[0] = vertex.Position[0];
				corner.Position[1] = vertex.Position[1];
				corner.Position[2] = vertex.Position[2];
				corner.Normal[0] = vertex.Normal[0];
				corner.Normal[1] = vertex.Normal[1];
				corner.Normal[2] = vertex.Normal[2];
				corner.TexCoord[0] = vertex.TexCoord[0];
				corner.TexCoord[1] = vertex.TexCoord[1];
				data.Vertices.push_back(corner);
				data.Indices.push_back(nextIndex++);

				edge = half.Next;
				// A guard against a malformed loop: a face whose Next chain
				// never returns to its first edge would otherwise spin here
				// forever. Stopping yields a partial triangle, which
				// Validate() reports, rather than an infinite loop in an
				// editor that is trying to show you the damage.
				if (edge < 0)
					break;
			} while (edge != first);
		}

		data.RecomputeBounds();
		return data;
	}

	void EditableMesh::RecomputeNormals()
	{
		// Zero first: a vertex no triangle touches must not keep whatever
		// normal it was welded with.
		for (Vertex &vertex : Vertices)
		{
			vertex.Normal[0] = 0.0f;
			vertex.Normal[1] = 0.0f;
			vertex.Normal[2] = 0.0f;
		}

		for (const Face &face : Faces)
		{
			if (face.FirstEdge < 0)
				continue;

			const int32_t e0 = face.FirstEdge;
			const int32_t e1 = Edges[(size_t)e0].Next;
			if (e1 < 0)
				continue;

			// Three corners of the face, taken from the EDGE's Vertex field.
			//
			// This was written as `Vertices[Edges[e1].Next >= 0 ? Edges[e1].Next
			// : e1]` -- the ternary yields an EDGE index, and the `.Vertex` was
			// missing, so the third corner was read at VERTEX index e2. For a
			// quad that is out of bounds (e2=5 with 4 vertices); for a cube it
			// silently picks a unrelated vertex whenever e2 < VertexCount.
			// The cross product then used a wrong `c`, and a face whose e2 fell
			// past the end read zeros -- making its normal exactly (0,0,0), so
			// the face contributed nothing and its exclusive corners kept the
			// zero-length fallback. Named locals make the two steps impossible
			// to confuse.
			const int32_t e2 = Edges[(size_t)e1].Next;
			if (e2 < 0)
				continue;

			const int32_t corner[3] = {Edges[(size_t)e0].Vertex,
									   Edges[(size_t)e1].Vertex,
									   Edges[(size_t)e2].Vertex};

			const Vertex &a = Vertices[(size_t)corner[0]];
			const Vertex &b = Vertices[(size_t)corner[1]];
			const Vertex &c = Vertices[(size_t)corner[2]];

			const float ab[3] = {b.Position[0] - a.Position[0],
								 b.Position[1] - a.Position[1],
								 b.Position[2] - a.Position[2]};
			const float ac[3] = {c.Position[0] - a.Position[0],
								 c.Position[1] - a.Position[1],
								 c.Position[2] - a.Position[2]};

			// |cross(ab, ac)| is twice the triangle area, which is the
			// weight. A sliver therefore contributes proportionally to its
			// real size instead of outweighing a large face on count.
			float faceNormal[3];
			Cross(ab, ac, faceNormal);

			// Accumulate into all three corners. The winding comes from the
			// cross product order, so a face wound backwards contributes a
			// normal pointing the other way -- which is exactly what the
			// author asked for.
			for (const int32_t vertexIndex : corner)
			{
				Vertex &vertex = Vertices[(size_t)vertexIndex];
				vertex.Normal[0] += faceNormal[0];
				vertex.Normal[1] += faceNormal[1];
				vertex.Normal[2] += faceNormal[2];
			}
		}

		for (Vertex &vertex : Vertices)
			Normalise(vertex.Normal);
	}

	std::string EditableMesh::Validate() const
	{
		std::string errors;

		// --- index ranges -------------------------------------------------
		for (size_t i = 0; i < Edges.size(); ++i)
		{
			const HalfEdge &edge = Edges[i];
			if (edge.Vertex < 0 || edge.Vertex >= (int32_t)Vertices.size())
				Append(errors, "edge " + std::to_string(i) + " has vertex " +
									std::to_string(edge.Vertex) +
									", out of range");
			if (edge.Twin >= (int32_t)Edges.size())
				Append(errors, "edge " + std::to_string(i) + " has twin " +
									std::to_string(edge.Twin) +
									", out of range");
			if (edge.Next >= (int32_t)Edges.size())
				Append(errors, "edge " + std::to_string(i) + " has next " +
									std::to_string(edge.Next) +
									", out of range");
			if (edge.Face >= (int32_t)Faces.size())
				Append(errors, "edge " + std::to_string(i) + " has face " +
									std::to_string(edge.Face) +
									", out of range");
		}

		// --- twin symmetry ------------------------------------------------
		// The invariant that makes the structure a half-edge mesh at all:
		// if A is B's twin then B is A's twin. A one-directional link would
		// let a face walk cross into a triangle and lose track of which side
		// it was on.
		for (size_t i = 0; i < Edges.size(); ++i)
		{
			const int32_t twin = Edges[i].Twin;
			if (twin < 0)
				continue; // boundary edge -- reported below, not an error here.
			if (twin >= (int32_t)Edges.size())
				continue; // already reported as out of range.
			if (Edges[(size_t)twin].Twin != (int32_t)i)
				Append(errors, "edge " + std::to_string(i) + " has twin " +
									std::to_string(twin) +
									", but that edge's twin is " +
									std::to_string(Edges[(size_t)twin].Twin));
		}

		// --- next/vertex consistency --------------------------------------
		// The next edge must start where this one ends. A violation means the
		// face loop is broken, and every adjacency query downstream would
		// return a vertex from a different triangle.
		for (size_t i = 0; i < Edges.size(); ++i)
		{
			const int32_t next = Edges[i].Next;
			if (next < 0 || next >= (int32_t)Edges.size())
				continue;
			const int32_t end = EdgeEndVertex((int32_t)i);
			if (Edges[(size_t)next].Vertex != end)
				Append(errors, "edge " + std::to_string(i) + " ends at vertex " +
									std::to_string(end) + " but its next edge " +
									std::to_string(next) + " starts at " +
									std::to_string(Edges[(size_t)next].Vertex));
		}

		// --- face completeness --------------------------------------------
		for (size_t i = 0; i < Faces.size(); ++i)
		{
			const Face &face = Faces[i];
			if (face.FirstEdge < 0 || face.FirstEdge >= (int32_t)Edges.size())
			{
				Append(errors, "face " + std::to_string(i) + " has first edge " +
									std::to_string(face.FirstEdge) +
									", out of range");
				continue;
			}
			// Walk the loop and count. A triangle must close after exactly
			// three steps; a longer loop means an n-gon, a shorter one means
			// a broken chain.
			int32_t edge = face.FirstEdge;
			int steps = 0;
			bool closed = false;
			for (; steps < 8; ++steps)
			{
				if (edge < 0 || edge >= (int32_t)Edges.size())
					break;
				if (steps > 0 && edge == face.FirstEdge)
				{
					closed = true;
					break;
				}
				edge = Edges[(size_t)edge].Next;
			}
			if (!closed)
				Append(errors, "face " + std::to_string(i) +
									" does not close back on its first edge");
			else if (steps != 3)
				Append(errors, "face " + std::to_string(i) + " is not a triangle (" +
									std::to_string(steps) + " edges)");
		}

		// --- orphan vertices ----------------------------------------------
		// A vertex with no outgoing edge contributes nothing and is the
		// signature of a delete that forgot to clean up. Not a hard error for
		// rendering (Deindex never visits it) but always an authoring bug.
		for (size_t i = 0; i < Vertices.size(); ++i)
		{
			if (Vertices[i].FirstEdge < 0)
				Append(errors, "vertex " + std::to_string(i) +
									" has no outgoing edge (orphan)");
			else if (Vertices[i].FirstEdge >= (int32_t)Edges.size())
				Append(errors, "vertex " + std::to_string(i) +
									" has first edge " +
									std::to_string(Vertices[i].FirstEdge) +
									", out of range");
		}

		return errors;
	}

	bool EditableMesh::IsTriangulated() const
	{
		for (const Face &face : Faces)
		{
			if (face.FirstEdge < 0)
				return false;
			const int32_t second = Edges[(size_t)face.FirstEdge].Next;
			if (second < 0)
				return false;
			const int32_t third = Edges[(size_t)second].Next;
			if (third < 0)
				return false;
			if (Edges[(size_t)third].Next != face.FirstEdge)
				return false;
		}
		return true;
	}

	int32_t EditableMesh::MergeByDistance(float epsilon)
	{
		// A non-positive epsilon would collapse every vertex within an unbounded
		// radius into one, destroying the mesh. Refused rather than clamped: a
		// caller that asked for 0 meant "weld exactly", which Weld already does,
		// so a silent clamp would hide the misunderstanding.
		if (!std::isfinite(epsilon) || epsilon <= 0.0f)
			return -1;

		const int32_t count = GetVertexCount();
		if (count <= 1)
			return 0;

		// Spatial hash on floor(position / epsilon), searching the 27
		// neighbouring cells. Without it this is O(n^2) -- 1.6 billion
		// comparisons for a 40k-vertex sculpt, in a modal the artist is waiting
		// on. With it the cost is one hash lookup per vertex plus a handful of
		// distance checks.
		struct CellKey
		{
			int32_t x = 0, y = 0, z = 0;
			bool operator==(const CellKey &other) const
			{
				return x == other.x && y == other.y && z == other.z;
			}
		};
		struct CellHash
		{
			size_t operator()(const CellKey &key) const
			{
				// FNV-1a over the three cell coordinates. Cell indices are
				// small integers clustered near the origin, so a plain sum or
				// XOR would pile the whole model into a few buckets.
				uint64_t hash = 1469598103934665603ull;
				const int32_t axes[3] = {key.x, key.y, key.z};
				for (const int32_t axis : axes)
				{
					hash ^= static_cast<uint32_t>(axis);
					hash *= 1099511628211ull;
				}
				return static_cast<size_t>(hash);
			}
		};

		const float inverse = 1.0f / epsilon;
		const float squared = epsilon * epsilon;
		std::unordered_map<CellKey, std::vector<int32_t>, CellHash> grid;
		std::vector<int32_t> remap(static_cast<size_t>(count));

		for (int32_t index = 0; index < count; ++index)
		{
			const float *position = Vertices[(size_t)index].Position;
			const CellKey base{
				static_cast<int32_t>(std::floor(position[0] * inverse)),
				static_cast<int32_t>(std::floor(position[1] * inverse)),
				static_cast<int32_t>(std::floor(position[2] * inverse)),
			};

			int32_t representative = -1;
			for (int dx = -1; dx <= 1 && representative < 0; ++dx)
			for (int dy = -1; dy <= 1 && representative < 0; ++dy)
			for (int dz = -1; dz <= 1 && representative < 0; ++dz)
			{
				const auto found = grid.find(
					CellKey{base.x + dx, base.y + dy, base.z + dz});
				if (found == grid.end())
					continue;
				for (const int32_t candidate : found->second)
				{
					const float *other = Vertices[(size_t)candidate].Position;
					const float ox = position[0] - other[0];
					const float oy = position[1] - other[1];
					const float oz = position[2] - other[2];
					if (ox * ox + oy * oy + oz * oz <= squared)
					{
						representative = candidate;
						break;
					}
				}
			}

			if (representative < 0)
			{
				representative = index;
				grid[base].push_back(index);
			}
			remap[(size_t)index] = representative;
		}

		// Rebuild through a MeshData of remapped corners. Every corner of a
		// representative carries the representative's exact position, so the
		// position-only weld in Weld merges them back to exactly the
		// representatives -- no second tolerance path to keep in step.
		MeshData data;
		data.Vertices.reserve(Faces.size() * 3);
		data.Indices.reserve(Faces.size() * 3);

		int32_t keptFaces = 0;
		for (const Face &face : Faces)
		{
			if (face.FirstEdge < 0)
				continue;
			const int32_t first = face.FirstEdge;
			const int32_t second = Edges[(size_t)first].Next;
			if (second < 0)
				continue;
			const int32_t third = Edges[(size_t)second].Next;
			if (third < 0 || Edges[(size_t)third].Next != first)
				continue;

			const int32_t corners[3] = {
				remap[(size_t)Edges[(size_t)first].Vertex],
				remap[(size_t)Edges[(size_t)second].Vertex],
				remap[(size_t)Edges[(size_t)third].Vertex],
			};

			// Two corners that merged to the same vertex make a zero-area
			// triangle. Dropping it is the only geometrically honest option --
			// keeping it would leave a triangle the rasteriser skips and the
			// editor counts.
			if (corners[0] == corners[1] || corners[1] == corners[2] ||
				corners[0] == corners[2])
				continue;

			for (const int32_t corner : corners)
			{
				const Vertex &source = Vertices[(size_t)corner];
				MeshVertex outVertex;
				outVertex.Position[0] = source.Position[0];
				outVertex.Position[1] = source.Position[1];
				outVertex.Position[2] = source.Position[2];
				outVertex.Normal[0] = source.Normal[0];
				outVertex.Normal[1] = source.Normal[1];
				outVertex.Normal[2] = source.Normal[2];
				outVertex.TexCoord[0] = source.TexCoord[0];
				outVertex.TexCoord[1] = source.TexCoord[1];
				data.Vertices.push_back(outVertex);
				data.Indices.push_back(
					static_cast<uint32_t>(data.Vertices.size() - 1));
			}
			++keptFaces;
		}

		data.RecomputeBounds();
		const int32_t before = GetVertexCount();
		*this = Weld(data);
		// The count is returned even when every face collapsed: a merge that
		// removed all N vertices and left an empty mesh is a SUCCESSFUL merge,
		// and conflating it with the invalid-epsilon refusal would report a
		// destroyed mesh as though the caller had passed a bad argument.
		return before - GetVertexCount();
	}

	void EditableMesh::FlipNormals()
	{
		MeshData data = Deindex();

		// Swap the SECOND AND THIRD CORNER of every triangle.
		//
		// It has to be the vertex records, not the index buffer. MeshData
		// documents its Indices as always the identity (MeshLoader.h:41), and
		// Weld builds each face from cornerToVertex[triangle*3 + k] -- it never
		// reads Indices. Swapping Indices[t+1] and Indices[t+2] therefore
		// reversed a buffer nobody reads: the winding came back unchanged and
		// the normals still pointed outward, with no error anywhere.
		//
		// De-indexed corners are safe to permute because each corner belongs to
		// exactly one triangle -- that is what de-indexed means.
		for (size_t triangle = 0; triangle + 2 < data.Vertices.size(); triangle += 3)
		{
			std::swap(data.Vertices[triangle + 1], data.Vertices[triangle + 2]);
		}

		*this = Weld(data);
		// Weld keeps whichever normal it saw first, so the normals must be
		// re-derived from the new winding rather than carried over.
		RecomputeNormals();
	}

	bool EditableMesh::DeleteFaces(const int32_t *faces, int32_t faceCount)
	{
		if (faces == nullptr || faceCount <= 0)
			return false;

		std::vector<bool> removeFace(Faces.size(), false);
		std::vector<bool> removeEdge(Edges.size(), false);

		for (int32_t index = 0; index < faceCount; ++index)
		{
			const int32_t face = faces[index];
			// An out-of-range index is a caller bug. Returning false without
			// modifying anything is better than deleting an arbitrary face.
			if (face < 0 || face >= static_cast<int32_t>(Faces.size()))
				return false;
			removeFace[(size_t)face] = true;

			const int32_t first = Faces[(size_t)face].FirstEdge;
			if (first < 0)
				continue;
			int32_t edge = first;
			do
			{
				removeEdge[(size_t)edge] = true;
				edge = Edges[(size_t)edge].Next;
				if (edge < 0)
					break;
			} while (edge != first);
		}

		// A surviving edge whose twin is being deleted becomes a boundary edge.
		// Recorded before the rebuild because the twin indices are about to be
		// remapped.
		std::vector<bool> twinRemoved(Edges.size(), false);
		for (size_t edge = 0; edge < Edges.size(); ++edge)
		{
			const int32_t twin = Edges[edge].Twin;
			if (twin >= 0 && removeEdge[(size_t)twin])
				twinRemoved[edge] = true;
		}

		std::vector<int32_t> edgeRemap(Edges.size(), -1);
		std::vector<HalfEdge> newEdges;
		newEdges.reserve(Edges.size());
		for (size_t edge = 0; edge < Edges.size(); ++edge)
		{
			if (removeEdge[edge])
				continue;
			edgeRemap[edge] = static_cast<int32_t>(newEdges.size());
			HalfEdge copy = Edges[edge];
			copy.Twin = twinRemoved[edge] ? -1 : copy.Twin;
			newEdges.push_back(copy);
		}

		std::vector<int32_t> faceRemap(Faces.size(), -1);
		std::vector<Face> newFaces;
		newFaces.reserve(Faces.size());
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			if (removeFace[face])
				continue;
			faceRemap[face] = static_cast<int32_t>(newFaces.size());
			Face copy = Faces[face];
			copy.FirstEdge = edgeRemap[(size_t)copy.FirstEdge];
			newFaces.push_back(copy);
		}

		// Remap the surviving twins, nexts and faces into the new arrays.
		//
		// Next and Face MUST be remapped too. Leaving them pointing at the old
		// indices is not a cosmetic defect: after any face is deleted, every
		// edge after it shifts down, so the loop chain and the face back-link
		// then describe a mesh that does not exist -- Validate() reports every
		// face as "does not close back on its first edge".
		for (HalfEdge &edge : newEdges)
		{
			if (edge.Twin >= 0)
				edge.Twin = edgeRemap[(size_t)edge.Twin];
			if (edge.Next >= 0)
				edge.Next = edgeRemap[(size_t)edge.Next];
			if (edge.Face >= 0)
				edge.Face = faceRemap[(size_t)edge.Face];
		}

		// Keep only vertices referenced by a surviving edge's start. A vertex
		// left with no edge at all is an orphan, and Validate() reports
		// orphans -- so dropping it here keeps the mesh valid rather than
		// producing one the editor would then have to explain.
		std::vector<bool> used(Vertices.size(), false);
		for (const HalfEdge &edge : newEdges)
			if (edge.Vertex >= 0 && edge.Vertex < static_cast<int32_t>(Vertices.size()))
				used[(size_t)edge.Vertex] = true;

		std::vector<int32_t> vertexRemap(Vertices.size(), -1);
		std::vector<Vertex> newVertices;
		newVertices.reserve(Vertices.size());
		for (size_t vertex = 0; vertex < Vertices.size(); ++vertex)
		{
			if (!used[vertex])
				continue;
			vertexRemap[vertex] = static_cast<int32_t>(newVertices.size());
			newVertices.push_back(Vertices[vertex]);
		}

		for (HalfEdge &edge : newEdges)
			edge.Vertex = vertexRemap[(size_t)edge.Vertex];

		Edges = std::move(newEdges);
		Faces = std::move(newFaces);
		Vertices = std::move(newVertices);
		return true;
	}

namespace
{
	// One triangle of a rebuild, as three positions.
	struct BuildTriangle
	{
		float a[3];
		float b[3];
		float c[3];
	};

	BuildTriangle MakeTriangle(const std::array<float, 3> &a,
							   const std::array<float, 3> &b,
							   const std::array<float, 3> &c)
	{
		BuildTriangle out;
		std::memcpy(out.a, a.data(), sizeof(float) * 3);
		std::memcpy(out.b, b.data(), sizeof(float) * 3);
		std::memcpy(out.c, c.data(), sizeof(float) * 3);
		return out;
	}

	// Rebuilds an EditableMesh from a triangle soup. Shared by extrude and
	// inset, and the reason those operations can be written as "decide which
	// triangles" instead of "maintain half-edge links".
	//
	// Weld merges by position, so a triangle whose corners coincide after the
	// merge degenerates and is dropped -- the caller does not filter them.
	EditableMesh BuildFromTriangles(const std::vector<BuildTriangle> &triangles)
	{
		MeshData data;
		data.Vertices.reserve(triangles.size() * 3);
		data.Indices.reserve(triangles.size() * 3);
		for (const BuildTriangle &triangle : triangles)
		{
			const float *corners[3] = {triangle.a, triangle.b, triangle.c};
			for (const float *corner : corners)
			{
				MeshVertex vertex;
				vertex.Position[0] = corner[0];
				vertex.Position[1] = corner[1];
				vertex.Position[2] = corner[2];
				// Weld keeps whichever normal it saw first, so these are
				// placeholders; the caller recomputes when it needs them.
				vertex.Normal[0] = 0.0f;
				vertex.Normal[1] = 1.0f;
				vertex.Normal[2] = 0.0f;
				vertex.TexCoord[0] = 0.0f;
				vertex.TexCoord[1] = 0.0f;
				data.Vertices.push_back(vertex);
				data.Indices.push_back(
					static_cast<uint32_t>(data.Vertices.size() - 1));
			}
		}
		data.RecomputeBounds();
		return EditableMesh::Weld(data);
	}

	// The three corners of a face, walked through the half-edge links.
	bool FaceCorners(const EditableMesh &mesh, int32_t face, int32_t out[3])
	{
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
} // namespace

	bool EditableMesh::ExtrudeFaces(const int32_t *faces, int32_t faceCount,
								   const float delta[3])
	{
		if (faces == nullptr || faceCount <= 0 || delta == nullptr)
			return false;

		std::vector<bool> inRegion(Faces.size(), false);
		for (int32_t index = 0; index < faceCount; ++index)
		{
			const int32_t face = faces[index];
			if (face < 0 || face >= static_cast<int32_t>(Faces.size()))
				return false;
			inRegion[(size_t)face] = true;
		}

		std::vector<BuildTriangle> triangles;

		// A position with or without the extrude offset. Returning a value
		// rather than writing into an out-parameter keeps the call sites
		// readable.
		auto cornerAt = [&](int32_t vertex, bool offset) {
			const float *source = Vertices[(size_t)vertex].Position;
			return std::array<float, 3>{
				source[0] + (offset ? delta[0] : 0.0f),
				source[1] + (offset ? delta[1] : 0.0f),
				source[2] + (offset ? delta[2] : 0.0f),
			};
		};

		// 1. The cap: the region's own triangles, offset by delta. The winding
		//    is the region's, so the cap faces the same way the original did.
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			if (!inRegion[face])
				continue;
			int32_t corners[3];
			if (!FaceCorners(*this, (int32_t)face, corners))
				continue;
			triangles.push_back(MakeTriangle(cornerAt(corners[0], true),
											 cornerAt(corners[1], true),
											 cornerAt(corners[2], true)));
		}

		// 2. The walls: one quad per BOUNDARY edge -- an edge whose twin is
		//    missing or belongs to a face outside the region. An interior edge
		//    is shared by two region faces and must produce no wall.
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			if (!inRegion[face])
				continue;
			const int32_t first = Faces[face].FirstEdge;
			if (first < 0)
				continue;
			int32_t edge = first;
			do
			{
				const int32_t twin = Edges[(size_t)edge].Twin;
				const bool boundary =
					twin < 0 || !inRegion[(size_t)Edges[(size_t)twin].Face];
				if (boundary)
				{
					const int32_t start = Edges[(size_t)edge].Vertex;
					const int32_t nextEdge = Edges[(size_t)edge].Next;
					if (nextEdge >= 0)
					{
						const int32_t end = Edges[(size_t)nextEdge].Vertex;
						// The boundary edge runs a->b in the region face's own
						// direction, with the region's interior on its left.
						// (a, b, b', a') therefore has the exterior on its
						// right, i.e. it faces outward.
						triangles.push_back(MakeTriangle(cornerAt(start, false),
														 cornerAt(end, false),
														 cornerAt(end, true)));
						triangles.push_back(MakeTriangle(cornerAt(start, false),
														 cornerAt(end, true),
														 cornerAt(start, true)));
					}
				}
				edge = Edges[(size_t)edge].Next;
				if (edge < 0)
					break;
			} while (edge != first);
		}

		// 3. The untouched faces, verbatim.
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			if (inRegion[face])
				continue;
			int32_t corners[3];
			if (!FaceCorners(*this, (int32_t)face, corners))
				continue;
			triangles.push_back(MakeTriangle(cornerAt(corners[0], false),
											 cornerAt(corners[1], false),
											 cornerAt(corners[2], false)));
		}

		*this = BuildFromTriangles(triangles);
		RecomputeNormals();
		return true;
	}

	bool EditableMesh::InsetFaces(const int32_t *faces, int32_t faceCount,
								  float thickness)
	{
		if (faces == nullptr || faceCount <= 0)
			return false;
		// A negative inset grows the face. That is a legitimate operation (an
		// outset) and the arithmetic handles it, so it is not refused -- but a
		// zero inset produces a degenerate ring and is refused, because the
		// result would be a face welded against itself.
		if (thickness == 0.0f)
			return false;

		std::vector<BuildTriangle> triangles;

		for (int32_t index = 0; index < faceCount; ++index)
		{
			const int32_t faceIndex = faces[index];
			if (faceIndex < 0 || faceIndex >= static_cast<int32_t>(Faces.size()))
				return false;

			int32_t corners[3];
			if (!FaceCorners(*this, faceIndex, corners))
				continue;

			std::array<float, 3> original[3];
			std::array<float, 3> shrunk[3];
			std::array<float, 3> centroid{0.0f, 0.0f, 0.0f};
			for (int corner = 0; corner < 3; ++corner)
			{
				const float *source = Vertices[(size_t)corners[corner]].Position;
				original[corner] = {source[0], source[1], source[2]};
				centroid[0] += source[0] / 3.0f;
				centroid[1] += source[1] / 3.0f;
				centroid[2] += source[2] / 3.0f;
			}
			for (int corner = 0; corner < 3; ++corner)
			{
				for (int axis = 0; axis < 3; ++axis)
				{
					shrunk[corner][axis] =
						original[corner][axis] +
						(centroid[axis] - original[corner][axis]) * thickness;
				}
			}

			// The inset face, keeping the original winding.
			triangles.push_back(MakeTriangle(shrunk[0], shrunk[1], shrunk[2]));

			// The ring: one quad per edge, (a, b, b', a'). The original edge
			// runs a->b with the face's interior on its left, so this quad
			// faces away from the face -- the outward direction for the ring.
			for (int edge = 0; edge < 3; ++edge)
			{
				const int next = (edge + 1) % 3;
				triangles.push_back(
					MakeTriangle(original[edge], original[next], shrunk[next]));
				triangles.push_back(
					MakeTriangle(original[edge], shrunk[next], shrunk[edge]));
			}
		}

		*this = BuildFromTriangles(triangles);
		RecomputeNormals();
		return true;
	}

	bool EditableMesh::BevelEdges(const int32_t *edges, int32_t edgeCount,
								  float offset, std::string *error)
	{
		if (edges == nullptr || edgeCount <= 0)
		{
			if (error)
				*error = "bevel: no edges";
			return false;
		}
		if (!(offset > 0.0f && offset < 1.0f))
		{
			if (error)
				*error = "bevel: offset must be in (0, 1)";
			return false;
		}

		// Validate every edge before touching anything: an out-of-range index or
		// a boundary edge (no twin -> no second face to hold the chamfer) refuses
		// the whole call, so a bad selection is never half-applied.
		for (int32_t i = 0; i < edgeCount; ++i)
		{
			const int32_t edge = edges[i];
			if (edge < 0 || edge >= (int32_t)Edges.size())
			{
				if (error)
					*error = "bevel: edge out of range";
				return false;
			}
			if (Edges[(size_t)edge].Twin < 0)
			{
				if (error)
					*error = "bevel: cannot chamfer a boundary edge";
				return false;
			}
		}
		// Two beveled edges that share a vertex meet at a corner whose offset is
		// where the D.2 "solve" lives. v1 refuses rather than emit a mangled
		// corner; a disjoint selection (the common "chamfer these quads" case)
		// is exactly handled.
		for (int32_t i = 0; i < edgeCount; ++i)
			for (int32_t j = i + 1; j < edgeCount; ++j)
			{
				const int32_t a = Edges[(size_t)edges[i]].Vertex;
				const int32_t b = EdgeEndVertex(edges[i]);
				const int32_t c = Edges[(size_t)edges[j]].Vertex;
				const int32_t d = EdgeEndVertex(edges[j]);
				if (a == c || a == d || b == c || b == d)
				{
					if (error)
						*error = "bevel: adjacent edges need a corner offset solve (v1 refuses)";
					return false;
				}
			}

		const auto at = [&](int32_t vi, float out[3])
		{
			const float *p = Vertices[(size_t)vi].Position;
			out[0] = p[0];
			out[1] = p[1];
			out[2] = p[2];
		};
		const auto Pos = [&](int32_t vi)
		{
			const float *p = Vertices[(size_t)vi].Position;
			return std::array<float, 3>{p[0], p[1], p[2]};
		};

		std::vector<bool> removed(Faces.size(), false);
		std::vector<BuildTriangle> triangles;

		struct Item
		{
			int32_t A, B, C, D;
			std::array<float, 3> Ap, Bp;
		};
		std::vector<Item> items;

		for (int32_t i = 0; i < edgeCount; ++i)
		{
			const int32_t edge = edges[i];
			const int32_t A = Edges[(size_t)edge].Vertex;
			const int32_t B = EdgeEndVertex(edge);
			const int32_t f = Edges[(size_t)edge].Face;
			const int32_t g = Edges[(size_t)Edges[(size_t)edge].Twin].Face;
			int32_t cf[3], cg[3];
			if (!FaceCorners(*this, f, cf) || !FaceCorners(*this, g, cg))
			{
				if (error)
					*error = "bevel: face walk failed";
				return false;
			}
			int32_t C = -1, D = -1;
			for (int k = 0; k < 3; ++k)
			{
				if (cf[k] != A && cf[k] != B)
					C = cf[k];
				if (cg[k] != A && cg[k] != B)
					D = cg[k];
			}
			if (C < 0 || D < 0)
			{
				if (error)
					*error = "bevel: endpoints not present on both adjacent faces";
				return false;
			}

			Item it;
			it.A = A;
			it.B = B;
			it.C = C;
			it.D = D;
			float aPos[3], bPos[3];
			at(A, aPos);
			at(B, bPos);
			for (int k = 0; k < 3; ++k)
			{
				const float mid = (aPos[k] + bPos[k]) * 0.5f;
				it.Ap[k] = aPos[k] + (mid - aPos[k]) * offset;
				it.Bp[k] = bPos[k] + (mid - bPos[k]) * offset;
			}
			items.push_back(it);
			removed[(size_t)f] = true;
			removed[(size_t)g] = true;
		}

		// Untouched faces, verbatim.
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			if (removed[face])
				continue;
			int32_t c[3];
			if (!FaceCorners(*this, (int32_t)face, c))
				continue;
			triangles.push_back(MakeTriangle(Pos(c[0]), Pos(c[1]), Pos(c[2])));
		}

		// Object centroid, so the chamfer winding points AWAY from the model.
		std::array<float, 3> obj{0.0f, 0.0f, 0.0f};
		if (!Vertices.empty())
		{
			for (const Vertex &v : Vertices)
				for (int k = 0; k < 3; ++k)
					obj[k] += v.Position[k] / (float)Vertices.size();
		}

		for (const Item &it : items)
		{
			std::array<float, 3> cham[4] = {
				Pos(it.A), Pos(it.B), it.Bp, it.Ap};

			// Chamfer normal: (B - A) x (B' - A).
			std::array<float, 3> normal;
			{
				const float u0x = cham[1][0] - cham[0][0], u0y = cham[1][1] - cham[0][1], u0z = cham[1][2] - cham[0][2];
				const float u1x = cham[2][0] - cham[0][0], u1y = cham[2][1] - cham[0][1], u1z = cham[2][2] - cham[0][2];
				normal[0] = u0y * u1z - u0z * u1y;
				normal[1] = u0z * u1x - u0x * u1z;
				normal[2] = u0x * u1y - u0y * u1x;
			}
			// Outward ray from the object centre to the chamfer centre; flip the
			// chamfer winding if it faces inward, so it reads as polish not a dent.
			std::array<float, 3> chamCenter{0, 0, 0}, ray{0, 0, 0};
			for (int k = 0; k < 3; ++k)
			{
				for (int q = 0; q < 4; ++q)
					chamCenter[k] += cham[q][k] / 4.0f;
				ray[k] = chamCenter[k] - obj[k];
			}
			float ndot = 0.0f;
			for (int k = 0; k < 3; ++k)
				ndot += normal[k] * ray[k];
			if (ndot < 0.0f && (ray[0] != 0.0f || ray[1] != 0.0f || ray[2] != 0.0f))
				std::swap(cham[1], cham[3]);
			// Chamfer: (A, B, B', A') as two triangles.
			triangles.push_back(MakeTriangle(cham[0], cham[1], cham[2]));
			triangles.push_back(MakeTriangle(cham[0], cham[2], cham[3]));
			// Face f shrinks to (A', B', C); face g to (B', A', D).
			std::array<float, 3> ap = it.Ap, bp = it.Bp;
			triangles.push_back(MakeTriangle(ap, bp, Pos(it.C)));
			triangles.push_back(MakeTriangle(bp, ap, Pos(it.D)));
		}

		*this = BuildFromTriangles(triangles);
		RecomputeNormals();
		return true;
	}

	bool EditableMesh::LoopCut(int32_t startEdge, std::string *error)
	{
		if (startEdge < 0 || startEdge >= (int32_t)Edges.size())
		{
			if (error)
				*error = "loop cut: edge out of range";
			return false;
		}
		const auto Pos = [&](int32_t vi)
		{
			const float *p = Vertices[(size_t)vi].Position;
			return std::array<float, 3>{p[0], p[1], p[2]};
		};

		// ---- walk the edge ring ----------------------------------------------
		// Starting at `startEdge`, advance across each quad: leave the current
		// face through its Next edge, cross that edge's twin to the neighbour,
		// and continue from the twin's Next. This is the D.2 "edge-ring
		// traversal" -- the part the plan says neither extrude nor inset implies.
		// The ring stops when it returns to the start (a closed loop) or when a
		// boundary edge ends it (an open loop). A walk that neither closes nor
		// terminates on a boundary after a full pass is a pole / mixed
		// triangulation and is REFUSED rather than guessed.
		std::vector<int32_t> ring;
		int32_t e = startEdge;
		const int32_t e0 = startEdge;
		bool closed = false, hitBoundary = false;
		const int capacity = (int32_t)Faces.size() * 3 + 1;
		while ((int32_t)ring.size() < capacity)
		{
			ring.push_back(e);
			const int32_t nextInFace = Edges[(size_t)e].Next;
			if (nextInFace < 0)
			{
				hitBoundary = true;
				break;
			}
			const int32_t twinOfNext = Edges[(size_t)nextInFace].Twin;
			if (twinOfNext < 0)
			{
				hitBoundary = true;
				break;
			}
			const int32_t next = Edges[(size_t)twinOfNext].Next;
			if (next == e0)
			{
				closed = true;
				break;
			}
			e = next;
		}
		if (!closed && !hitBoundary)
		{
			if (error)
				*error = "loop cut: edge ring neither closed nor ended on a boundary (pole or mixed triangulation)";
			return false;
		}
		if (ring.empty())
		{
			if (error)
				*error = "loop cut: empty ring";
			return false;
		}

		// ---- split ----------------------------------------------
		// Gather the ring edges; a face ring edge maps to a midpoint position.
		std::vector<bool> isRing(Edges.size(), false);
		for (const int32_t edge : ring)
			if (edge >= 0)
				isRing[(size_t)edge] = true;

		// Midpoint per ring edge.
		std::map<int32_t, std::array<float, 3>> midMap;
		for (const int32_t edge : ring)
		{
			const int32_t a = Edges[(size_t)edge].Vertex;
			const int32_t b = EdgeEndVertex(edge);
			if (a < 0 || b < 0)
				continue;
			std::array<float, 3> m;
			for (int k = 0; k < 3; ++k)
				m[k] = (Vertices[(size_t)a].Position[k] +
						Vertices[(size_t)b].Position[k]) * 0.5f;
			midMap[edge] = m;
		}

		std::vector<BuildTriangle> triangles;
		for (size_t face = 0; face < Faces.size(); ++face)
		{
			int32_t corners[3];
			if (!FaceCorners(*this, (int32_t)face, corners))
				continue;
			// Which edges (by their start vertex -> end vertex pair) are ring.
			int ringEdgeIndex[3] = {-1, -1, -1};
			int ringCount = 0;
			for (int k = 0; k < 3; ++k)
			{
				const int32_t v0 = corners[k];
				const int32_t v1 = corners[(k + 1) % 3];
				// find the half-edge (v0->v1) that belongs to this face
				int32_t he = Faces[face].FirstEdge;
				for (int s = 0; s < 3; ++s)
				{
					if (Edges[(size_t)he].Vertex == v0 &&
						EdgeEndVertex(he) == v1)
						break;
					he = Edges[(size_t)he].Next;
				}
				if (he >= 0 && isRing[(size_t)he])
				{
					ringEdgeIndex[k] = he;
					++ringCount;
				}
			}
			if (ringCount == 0)
			{
				triangles.push_back(MakeTriangle(Pos(corners[0]), Pos(corners[1]),
												 Pos(corners[2])));
				continue;
			}
			// One ring edge: split that edge at its midpoint into two triangles.
			if (ringCount == 1)
			{
				int k0 = ringEdgeIndex[0] >= 0 ? 0 : (ringEdgeIndex[1] >= 0 ? 1 : 2);
				const int a = corners[k0], b = corners[(k0 + 1) % 3], c = corners[(k0 + 2) % 3];
				const std::array<float, 3> &m = midMap[ringEdgeIndex[k0]];
				triangles.push_back(MakeTriangle(Pos(a), m, Pos(c)));
				triangles.push_back(MakeTriangle(m, Pos(b), Pos(c)));
				continue;
			}
			// Two ring edges: split at the corner BOTH edges share. p and q are
			// the midpoints on the two ring edges; x,y are the other two corners.
			if (ringCount == 2)
			{
				int k0 = -1, k1 = -1, count = 0;
				for (int k = 0; k < 3; ++k)
					if (ringEdgeIndex[k] >= 0)
					{
						if (count == 0)
							k0 = k;
						else
							k1 = k;
						++count;
					}
				const int c0 = corners[k0], c1 = corners[k1];
				const int c0n = corners[(k0 + 1) % 3], c1n = corners[(k1 + 1) % 3];
				// shared corner s = the corner common to both ring edges.
				int s = -1;
				if (c0 == c1 || c0 == c1n)
					s = c0;
				else if (c0n == c1 || c0n == c1n)
					s = c0n;
				if (s < 0)
				{
					if (error)
						*error = "loop cut: could not find the shared corner of two ring edges";
					return false;
				}
				// p = midpoint of edge k0 (spanning c0..c0n), q = of k1.
				const std::array<float, 3> &m0 = midMap[ringEdgeIndex[k0]];
				const std::array<float, 3> &m1 = midMap[ringEdgeIndex[k1]];
				// corner tri (s, m0, m1), then the region m0 - x - y - m1 split
				// into two triangles (x,y = the two non-s corners).
				triangles.push_back(MakeTriangle(Pos(s), m0, m1));
				int nonS[2], ns = 0;
				for (int k = 0; k < 3 && ns < 2; ++k)
					if (corners[k] != s)
						nonS[ns++] = corners[k];
				triangles.push_back(MakeTriangle(m0, Pos(nonS[0]), Pos(nonS[1])));
				triangles.push_back(MakeTriangle(m0, Pos(nonS[1]), m1));
				continue;
			}
			// Three ring edges: the face is fully split into a central triangle
			// plus three corner triangles.
			const int a = corners[0], b = corners[1], c = corners[2];
			const std::array<float, 3> &m1 = midMap[ringEdgeIndex[0]];
			const std::array<float, 3> &m2 = midMap[ringEdgeIndex[1]];
			const std::array<float, 3> &m3 = midMap[ringEdgeIndex[2]];
			triangles.push_back(MakeTriangle(m1, Pos(b), m2));
			triangles.push_back(MakeTriangle(m2, Pos(c), m3));
			triangles.push_back(MakeTriangle(m3, Pos(a), m1));
			triangles.push_back(MakeTriangle(m1, m2, m3));
		}

		*this = BuildFromTriangles(triangles);
		RecomputeNormals();
		return true;
	}

} // namespace Wasteland
