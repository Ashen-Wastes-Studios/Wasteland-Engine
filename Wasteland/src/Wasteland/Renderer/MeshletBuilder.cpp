// MeshletBuilder implementation. See MeshletBuilder.h for the algorithm and the
// VertexOffset semantics.
//
// The build is a single forward pass with a per-meshlet remap table. The remap
// table is a flat vector of int32 sized to the mesh's vertex count, holding the
// meshlet-local vertex slot for each mesh vertex, or -1 when the vertex is not in
// the current meshlet. A flat vector rather than a hash map because the vertex
// count is known up front and the access pattern is dense: every triangle touches
// three entries, and the table is rebuilt per meshlet (not per mesh), so the cost
// is O(vertices in meshlet) to clear rather than O(mesh vertices).
#include "wlpch.h"

#include "MeshletBuilder.h"

#include "ProceduralMeshes.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <vector>

namespace Wasteland
{
    bool BuildMeshlets(const ProceduralMesh &mesh,
                       std::vector<Meshlet> &outMeshlets,
                       std::vector<std::uint32_t> &outMeshletVertices,
                       std::vector<std::uint32_t> &outMeshletIndices)
    {
        outMeshlets.clear();
        outMeshletVertices.clear();
        outMeshletIndices.clear();

        const std::uint32_t triangleCount = mesh.TriangleCount();
        if (triangleCount == 0)
        {
            // A mesh with no triangles draws nothing, so there is nothing to
            // cluster. Reporting success with an empty table is the accurate
            // answer: the caller asked for meshlets and there are none.
            return true;
        }

        if (mesh.Positions.empty())
        {
            // Indices that reference missing positions would produce meshlets
            // with NaN bounds, which the mesh shader would cull incorrectly.
            return false;
        }

        outMeshlets.reserve(triangleCount / kMeshletMaxTriangles + 1);

        // Per-meshlet remap: mesh vertex index -> meshlet-local slot, -1 if the
        // vertex is not part of the meshlet being built.
        std::vector<std::int32_t> remap(mesh.Positions.size(), -1);

        Meshlet current{};
        // The local triangle list is rebuilt per meshlet and appended to the
        // meshlet's primitive range in the mesh's own index buffer, so the
        // primitive offsets are into the mesh index buffer, not a per-meshlet
        // index array. See the header: PrimitiveOffset * 3 is the triangle's
        // first index.
        std::uint32_t currentTriangleStart = 0;

        auto seal = [&]()
        {
            // The bounds are computed from the meshlet's vertices, not from its
            // triangles: a triangle's vertices are the only things a mesh shader
            // transforms, so the bounds have to contain exactly those.
            glm::vec3 minimum(FLT_MAX);
            glm::vec3 maximum(-FLT_MAX);
            for (std::uint32_t v = 0; v < current.VertexCount; ++v)
            {
                const glm::vec3 &p = mesh.Positions[outMeshletVertices[current.VertexOffset + v]];
                minimum = glm::min(minimum, p);
                maximum = glm::max(maximum, p);
            }
            current.BoundsMin = minimum;
            current.BoundsMax = maximum;

            // Bounding sphere radius from the AABB centre. The centre is not
            // stored because the mesh shader can recompute it from the bounds,
            // and storing it would add a field the plan's struct does not have.
            const glm::vec3 centre = (minimum + maximum) * 0.5f;
            float radiusSquared = 0.0f;
            for (std::uint32_t v = 0; v < current.VertexCount; ++v)
            {
                const glm::vec3 &p = mesh.Positions[outMeshletVertices[current.VertexOffset + v]];
                radiusSquared = glm::max(radiusSquared, glm::dot(p - centre, p - centre));
            }
            current.BoundingSphereRadius = glm::sqrt(radiusSquared);

            outMeshlets.push_back(current);
        };

        for (std::uint32_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const std::uint32_t i0 = mesh.Indices[triangle * 3 + 0];
            const std::uint32_t i1 = mesh.Indices[triangle * 3 + 1];
            const std::uint32_t i2 = mesh.Indices[triangle * 3 + 2];

            // How many NEW vertices this triangle would add? Only vertices not
            // already in the meshlet count against the budget.
            std::uint32_t newVertices = 0;
            if (i0 >= remap.size() || i1 >= remap.size() || i2 >= remap.size())
            {
                // A malformed index buffer. Rather than silently dropping the
                // triangle, fail the whole build: a meshlet table that quietly
                // omits triangles produces a mesh that renders with holes.
                outMeshlets.clear();
                outMeshletVertices.clear();
                outMeshletIndices.clear();
                return false;
            }
            if (remap[i0] < 0) ++newVertices;
            if (remap[i1] < 0) ++newVertices;
            if (remap[i2] < 0) ++newVertices;

            const bool vertexBudgetExceeded =
                (current.VertexCount + newVertices) > kMeshletMaxVertices;
            const bool triangleBudgetExceeded =
                current.PrimitiveCount >= kMeshletMaxTriangles;

            if ((vertexBudgetExceeded || triangleBudgetExceeded) && current.PrimitiveCount > 0)
            {
                seal();

                // Reset the meshlet state. The remap table has to be cleared only
                // for the vertices the previous meshlet used; clearing the whole
                // table would be O(mesh vertices) per meshlet.
                for (std::uint32_t v = 0; v < current.VertexCount; ++v)
                {
                    remap[outMeshletVertices[current.VertexOffset + v]] = -1;
                }
                // The new meshlet's vertices start where the previous one's end,
                // so VertexOffset has to be re-established: `Meshlet{}` zeroes it,
                // which would make every meshlet after the first read from the
                // start of the array.
                current = Meshlet{};
                current.VertexOffset = static_cast<std::uint32_t>(outMeshletVertices.size());
                currentTriangleStart = triangle;
            }

            // Add the triangle's vertices to the meshlet, remapping as needed.
            const std::uint32_t indices[3] = { i0, i1, i2 };
            std::uint32_t local[3];
            for (std::uint32_t k = 0; k < 3; ++k)
            {
                const std::uint32_t meshIndex = indices[k];
                if (remap[meshIndex] < 0)
                {
                    remap[meshIndex] = static_cast<std::int32_t>(outMeshletVertices.size() - current.VertexOffset);
                    outMeshletVertices.push_back(meshIndex);
                    ++current.VertexCount;
                }
                local[k] = static_cast<std::uint32_t>(remap[meshIndex]);
            }

            // The triangle is recorded with MESHLET-LOCAL indices, which is what
            // the mesh shader's `out indices` array addresses. Appending here keeps
            // the array parallel to the mesh's own index buffer, so
            // PrimitiveOffset * 3 indexes both.
            outMeshletIndices.push_back(local[0]);
            outMeshletIndices.push_back(local[1]);
            outMeshletIndices.push_back(local[2]);

            ++current.PrimitiveCount;
            current.PrimitiveOffset = currentTriangleStart;
        }

        // The last meshlet is sealed outside the loop: the loop only seals when it
        // sees a triangle that does not fit, so a mesh whose final meshlet is
        // exactly full would otherwise be dropped.
        if (current.PrimitiveCount > 0)
        {
            seal();
        }

        return true;
    }

    std::vector<Meshlet> BuildMeshlets(const ProceduralMesh &mesh)
    {
        std::vector<Meshlet> meshlets;
        std::vector<std::uint32_t> meshletVertices;
        std::vector<std::uint32_t> meshletIndices;
        BuildMeshlets(mesh, meshlets, meshletVertices, meshletIndices);
        return meshlets;
    }
}
