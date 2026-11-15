// test_meshlet_builder -- self-contained executable, no engine, no window, no GPU.
//
// MeshletBuilder is pure CPU work over a ProceduralMesh, so MSBuild proves it
// compiles and this proves it is correct. The properties under test are the ones
// Task 2.2's mesh shader depends on:
//
//   1. the budget is respected (a meshlet that overflows 64 vertices or 124
//      triangles is a mesh shader that reads past its vertex buffer);
//   2. the union of the meshlet bounds contains the mesh bounds (a meshlet that
//      under-reports its bounds is a cluster the mesh shader wrongly culls);
//   3. every meshlet's vertices are within the remapped vertex array, and the
//      remap actually points at the mesh vertices the triangles reference;
//   4. every triangle appears in exactly one meshlet, in order, with no gaps --
//      a dropped or duplicated triangle is a hole or a double-drawn surface.
//
// A 24-triangle cube is built by hand because ProceduralMeshes has no Cube
// generator (the PrimitiveShape enum has Cube = 0, but the generator set starts
// at Plane). The cube is the plan's own example mesh. A Torus is added to force
// the multi-meshlet path: at 48x12 segments it has 600 triangles, well past the
// 124-triangle budget.

#include <cstdio>
#include <cstdint>
#include <vector>

#include "Wasteland/Renderer/MeshletBuilder.h"
#include "Wasteland/Renderer/ProceduralMeshes.h"

// spdlog is on the include path for these projects, but this harness deliberately
// avoids it: printf is enough and keeps the test free of a logging dependency.
namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char *what)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("  FAIL: %s\n", what);
        }
    }

    // A unit cube: 24 vertices (4 per face, so each face has its own normal) and
    // 36 indices (12 triangles). Wound counter-clockwise, matching the convention
    // ProceduralMeshes documents.
    Wasteland::ProceduralMesh MakeCube()
    {
        Wasteland::ProceduralMesh mesh;
        const float h = 0.5f;
        // +X, -X, +Y, -Y, +Z, -Z. Each face is 4 vertices, so the vertex index of
        // face f, corner c is f * 4 + c.
        const float faces[6][4][3] = {
            { {  h, -h, -h }, {  h, -h,  h }, {  h,  h,  h }, {  h,  h, -h } }, // +X
            { { -h, -h,  h }, { -h, -h, -h }, { -h,  h, -h }, { -h,  h,  h } }, // -X
            { { -h,  h, -h }, {  h,  h, -h }, {  h,  h,  h }, { -h,  h,  h } }, // +Y
            { { -h, -h,  h }, {  h, -h,  h }, {  h, -h, -h }, { -h, -h, -h } }, // -Y
            { { -h, -h,  h }, { -h,  h,  h }, {  h,  h,  h }, {  h, -h,  h } }, // +Z
            { {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h }, { -h, -h, -h } }, // -Z
        };
        const int quads[6][4] = {
            { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 8, 9, 10, 11 },
            { 12, 13, 14, 15 }, { 16, 17, 18, 19 }, { 20, 21, 22, 23 },
        };
        for (int f = 0; f < 6; ++f)
        {
            for (int c = 0; c < 4; ++c)
            {
                mesh.Positions.push_back(glm::vec3(faces[f][c][0], faces[f][c][1], faces[f][c][2]));
                mesh.Normals.emplace_back(0.0f, 0.0f, 0.0f);
                mesh.TexCoords.emplace_back(0.0f, 0.0f);
            }
        }
        for (int f = 0; f < 6; ++f)
        {
            const int *q = quads[f];
            // Two triangles per quad, counter-clockwise: (0,1,2) and (0,2,3).
            mesh.Indices.push_back(q[0]); mesh.Indices.push_back(q[1]); mesh.Indices.push_back(q[2]);
            mesh.Indices.push_back(q[0]); mesh.Indices.push_back(q[2]); mesh.Indices.push_back(q[3]);
        }
        return mesh;
    }

    void TestMesh(const char *name, const Wasteland::ProceduralMesh &mesh, std::uint32_t expectedMinMeshlets)
    {
        std::vector<Wasteland::Meshlet> meshlets;
        std::vector<std::uint32_t> meshletVertices;
        std::vector<std::uint32_t> meshletIndices;
        const bool built = Wasteland::BuildMeshlets(mesh, meshlets, meshletVertices, meshletIndices);

        std::printf("[%s] vertices=%zu triangles=%zu -> %zu meshlets (%zu remapped vertices, %zu local indices)\n",
                    name, mesh.Positions.size(), mesh.Indices.size() / 3,
                    meshlets.size(), meshletVertices.size(), meshletIndices.size());

        Check(built, "build succeeds for a non-empty mesh");
        Check(!meshlets.empty(), "a mesh with triangles yields at least one meshlet");
        Check(meshlets.size() >= expectedMinMeshlets, "meshlet count meets the minimum for this mesh");

        // --- budget ---
        for (size_t m = 0; m < meshlets.size(); ++m)
        {
            const Wasteland::Meshlet &ml = meshlets[m];
            Check(ml.VertexCount > 0, "meshlet has vertices");
            Check(ml.PrimitiveCount > 0, "meshlet has triangles");
            Check(ml.VertexCount <= Wasteland::kMeshletMaxVertices, "meshlet respects the 64-vertex budget");
            Check(ml.PrimitiveCount <= Wasteland::kMeshletMaxTriangles, "meshlet respects the 124-triangle budget");

            // The vertex range has to be inside the remapped array.
            Check(ml.VertexOffset + ml.VertexCount <= meshletVertices.size(),
                  "meshlet vertex range is inside the remapped vertex array");
            // And the remapped entries have to be real mesh vertices.
            bool remapInRange = true;
            for (std::uint32_t v = 0; v < ml.VertexCount; ++v)
            {
                if (meshletVertices[ml.VertexOffset + v] >= mesh.Positions.size())
                {
                    remapInRange = false;
                }
            }
            Check(remapInRange, "every remapped vertex is a real mesh vertex");

            // The bounds have to contain the meshlet's own vertices.
            bool boundsContainVertices = true;
            for (std::uint32_t v = 0; v < ml.VertexCount; ++v)
            {
                const glm::vec3 &p = mesh.Positions[meshletVertices[ml.VertexOffset + v]];
                if (p.x < ml.BoundsMin.x || p.x > ml.BoundsMax.x ||
                    p.y < ml.BoundsMin.y || p.y > ml.BoundsMax.y ||
                    p.z < ml.BoundsMin.z || p.z > ml.BoundsMax.z)
                {
                    boundsContainVertices = false;
                }
            }
            Check(boundsContainVertices, "meshlet bounds contain the meshlet's own vertices");

            // The bounding sphere has to contain the vertices too.
            const glm::vec3 centre = (ml.BoundsMin + ml.BoundsMax) * 0.5f;
            bool sphereContainsVertices = true;
            for (std::uint32_t v = 0; v < ml.VertexCount; ++v)
            {
                const glm::vec3 &p = mesh.Positions[meshletVertices[ml.VertexOffset + v]];
                if (glm::length(p - centre) > ml.BoundingSphereRadius + 1e-4f)
                {
                    sphereContainsVertices = false;
                }
            }
            Check(sphereContainsVertices, "bounding sphere contains the meshlet's vertices");
        }

        // --- union of bounds contains the mesh bounds ---
        glm::vec3 meshMin(FLT_MAX), meshMax(-FLT_MAX);
        for (const glm::vec3 &p : mesh.Positions)
        {
            meshMin = glm::min(meshMin, p);
            meshMax = glm::max(meshMax, p);
        }
        glm::vec3 unionMin(FLT_MAX), unionMax(-FLT_MAX);
        for (const Wasteland::Meshlet &ml : meshlets)
        {
            unionMin = glm::min(unionMin, ml.BoundsMin);
            unionMax = glm::max(unionMax, ml.BoundsMax);
        }
        Check(glm::all(glm::lessThanEqual(unionMin, meshMin + glm::vec3(1e-5f))),
              "union of meshlet bounds reaches the mesh minimum");
        Check(glm::all(glm::greaterThanEqual(unionMax, meshMax - glm::vec3(1e-5f))),
              "union of meshlet bounds reaches the mesh maximum");

        // --- every triangle appears in exactly one meshlet, in order ---
        std::vector<std::uint32_t> covered(mesh.Indices.size() / 3, 0);
        std::uint32_t totalTriangles = 0;
        std::uint32_t expectedOffset = 0;
        for (const Wasteland::Meshlet &ml : meshlets)
        {
            Check(ml.PrimitiveOffset == expectedOffset,
                  "meshlet primitive ranges are contiguous and in order");
            expectedOffset = ml.PrimitiveOffset + ml.PrimitiveCount;
            totalTriangles += ml.PrimitiveCount;
            for (std::uint32_t t = 0; t < ml.PrimitiveCount; ++t)
            {
                ++covered[ml.PrimitiveOffset + t];
            }
        }
        Check(totalTriangles == mesh.Indices.size() / 3, "every triangle is assigned to a meshlet");
        bool exactlyOnce = true;
        for (std::uint32_t c : covered)
        {
            if (c != 1) exactlyOnce = false;
        }
        Check(exactlyOnce, "no triangle is assigned twice or dropped");

        // --- local indices address the meshlet's own vertex list ---
        // The mesh shader indexes `verts[]` by these, so a local index at or above
        // the meshlet's vertex count would read a vertex belonging to another
        // meshlet.
        bool localIndicesInRange = true;
        bool localIndicesMatchMeshVertices = true;
        for (const Wasteland::Meshlet &ml : meshlets)
        {
            for (std::uint32_t t = 0; t < ml.PrimitiveCount; ++t)
            {
                const std::uint32_t base = (ml.PrimitiveOffset + t) * 3;
                if (base + 2 >= meshletIndices.size())
                {
                    localIndicesInRange = false;
                    continue;
                }
                for (std::uint32_t k = 0; k < 3; ++k)
                {
                    const std::uint32_t local = meshletIndices[base + k];
                    if (local >= ml.VertexCount)
                    {
                        localIndicesInRange = false;
                    }
                    else
                    {
                        // The local index must resolve back to the mesh vertex the
                        // mesh's own index buffer names for this corner.
                        const std::uint32_t meshIndex = mesh.Indices[(ml.PrimitiveOffset + t) * 3 + k];
                        if (meshletVertices[ml.VertexOffset + local] != meshIndex)
                        {
                            localIndicesMatchMeshVertices = false;
                        }
                    }
                }
            }
        }
        Check(localIndicesInRange, "every local index is within its meshlet's vertex count");
        Check(localIndicesMatchMeshVertices,
              "every local index resolves back to the mesh vertex the triangle names");
    }
}

int main()
{
    std::printf("test_meshlet_builder\n");

    TestMesh("cube", MakeCube(), 1);
    TestMesh("torus", Wasteland::ProceduralMeshes::Torus(48, 12), 1);
    TestMesh("icosphere", Wasteland::ProceduralMeshes::IcoSphere(2), 1);
    TestMesh("grid", Wasteland::ProceduralMeshes::Grid(10, 10), 1);

    // A mesh with no triangles is a valid input that yields no meshlets.
    {
        Wasteland::ProceduralMesh empty;
        empty.Positions.emplace_back(0.0f, 0.0f, 0.0f);
        std::vector<Wasteland::Meshlet> meshlets;
        std::vector<std::uint32_t> meshletVertices;
        std::vector<std::uint32_t> meshletIndices;
        const bool built = Wasteland::BuildMeshlets(empty, meshlets, meshletVertices, meshletIndices);
        Check(built, "an index-less mesh builds without failing");
        Check(meshlets.empty(), "an index-less mesh yields no meshlets");
    }

    // A malformed index (out of range) must fail the build rather than emit
    // meshlets with garbage bounds.
    {
        Wasteland::ProceduralMesh bad = MakeCube();
        bad.Indices[0] = 9999u;
        std::vector<Wasteland::Meshlet> meshlets;
        std::vector<std::uint32_t> meshletVertices;
        std::vector<std::uint32_t> meshletIndices;
        const bool built = Wasteland::BuildMeshlets(bad, meshlets, meshletVertices, meshletIndices);
        Check(!built, "an out-of-range index fails the build");
        Check(meshlets.empty(), "a failed build leaves no meshlets");
    }

    std::printf("%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
