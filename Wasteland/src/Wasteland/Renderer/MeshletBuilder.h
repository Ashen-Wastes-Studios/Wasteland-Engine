// MeshletBuilder -- per-mesh cluster generation for the mesh-shader path.
//
// WHY THIS EXISTS: a mesh shader draws a whole cluster (meshlet) per threadgroup,
// so the mesh has to be cut into clusters that fit the hardware's vertex and
// primitive budget. The GPU does not do this; it has to be done on the CPU once
// per mesh and uploaded as a side table.
//
// THE PLAN THIS FOLLOWS: .hermes/plans/2026-11-07_163941-nova-mesh-rt-raster-hybrid-pipeline.md
// Task 2.1. The struct layout there is reproduced exactly, because Task 2.2's
// mesh shader will read it.
//
// THE ALGORITHM: greedy forward growth over the index buffer. Each meshlet
// accumulates triangles until adding the next one would exceed either the vertex
// budget or the triangle budget, then the meshlet is sealed and a new one starts.
// Greedy forward growth is not optimal -- it can leave a small tail meshlet, and
// it does not try to maximise vertex reuse across meshlet boundaries -- but it is
// the standard approach for procedural meshes whose triangle order is already
// locality-friendly (Blender's generators emit triangles in surface order).
//
// VERTEX REMAPPING: a mesh shader's meshlet needs its own vertex list, because
// the vertices are shared WITHIN the meshlet but not between meshlets. The
// builder therefore emits a second array, MeshletVertices, which maps a meshlet's
// local vertex slot back to the mesh's vertex buffer. Meshlet::VertexOffset is an
// index into THAT array, not into the mesh's own vertex buffer -- the plan's
// comment says "index into the index buffer" for PrimitiveOffset, and this is the
// same idea one level up. Reading VertexOffset as a mesh-vertex index would give
// wrong vertices for every meshlet after the first.
#pragma once

// glm core only, matching ProceduralMeshes.h: this header holds plain data and
// performs no transforms.
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Wasteland
{
    // One cluster. The layout is a contract with the mesh shader in Task 2.2, so
    // the field order and types are deliberate.
    struct Meshlet
    {
        // Index into the MeshletVertices array produced by the same build.
        std::uint32_t VertexOffset;
        std::uint32_t VertexCount;
        // Index into the mesh's own index buffer, in triangles (so the byte offset
        // is PrimitiveOffset * 3 * sizeof(uint32_t)).
        std::uint32_t PrimitiveOffset;
        std::uint32_t PrimitiveCount;
        // Axis-aligned bounds of the meshlet's vertices, in the mesh's local space.
        glm::vec3     BoundsMin;
        glm::vec3     BoundsMax;
        // Radius of the bounding sphere centred on the AABB centre. Used by the
        // mesh shader for cluster culling against the frustum and the hi-Z.
        float         BoundingSphereRadius;
    };

    // The meshlet budget. 64 vertices and 124 triangles are the NVIDIA-recommended
    // meshlet sizes: 64 vertices keep the meshlet's vertex buffer in shared memory
    // next to the mesh shader's payload, and 124 triangles is the largest multiple
    // of the hardware's 128-thread wave that leaves room for the primitive index.
    inline constexpr std::uint32_t kMeshletMaxVertices = 64;
    inline constexpr std::uint32_t kMeshletMaxTriangles = 124;

    // Build the meshlets for one mesh.
    //
    // `outMeshlets` receives the cluster table. `outMeshletVertices` receives the
    // remapped vertex indices: for meshlet M, its vertices are
    // outMeshletVertices[M.VertexOffset .. M.VertexOffset + M.VertexCount].
    //
    // `outMeshletIndices` receives the triangles with MESHLET-LOCAL vertex indices:
    // for meshlet M, triangle T's three indices are
    // outMeshletIndices[(M.PrimitiveOffset + T) * 3 .. +2]. A mesh shader cannot use
    // the mesh's own index buffer directly, because its vertices are addressed by
    // position in the meshlet's vertex list, not by mesh vertex index. This array is
    // that translation, precomputed.
    //
    // Returns false for a mesh with no triangles (nothing to cluster) and true
    // otherwise, including for a mesh whose triangle count is under one meshlet.
    // An empty `outMeshlets` with a true return is not possible.
    //
    // NOT THREAD-SAFE in the same way the rest of the render path is not: it
    // allocates and walks the mesh, and is expected to run once at load time.
    bool BuildMeshlets(const struct ProceduralMesh &mesh,
                       std::vector<Meshlet> &outMeshlets,
                       std::vector<std::uint32_t> &outMeshletVertices,
                       std::vector<std::uint32_t> &outMeshletIndices);

    // Convenience for the common case where only the cluster table is wanted.
    // Returns an empty vector for a mesh with no triangles.
    std::vector<Meshlet> BuildMeshlets(const struct ProceduralMesh &mesh);
}
