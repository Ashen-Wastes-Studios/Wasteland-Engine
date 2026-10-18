// Header guard. Included by Renderer3D.h, so anything added here lands in
// every translation unit that pulls in the renderer interface.
#pragma once

// glm core only (vec3/vec2), NOT the matrix or geometry extensions: this
// header holds plain data and no transforms.
#include <glm/glm.hpp>

// uint32_t for the index buffer and the two count helpers below. Fixed
// width because these values go to the GPU as GL_UNSIGNED_INT.
#include <cstdint>
#include <vector>

namespace Wasteland
{
    // A triangle mesh in local space, with one entry per attribute. Positions
    // are centred on the origin and wound counter-clockwise so the face normal
    // (cross(v1-v0, v2-v0)) points away from the surface, matching the winding
    // CubeRendererComponent's 24 hardcoded vertices already use.
    // A value type holding four parallel vectors, not an indexed vertex
    // struct. That keeps it trivially copyable and lets each generator
    // append to positions/normals/texcoords without maintaining an index
    // remap.
    struct ProceduralMesh
    {
        // LOCAL space, centred on the origin, and every primitive is normalised
        // to fit a 1x1x1 box so a Scale of (1,1,1) makes them comparable. The
        // caller applies the transform; the mesh knows nothing about it.
        std::vector<glm::vec3> Positions;
        // DEFECT (no size invariant, documented not fixed): nothing enforces
        // Normals.size() == Positions.size(), yet the GPU needs matching
        // attributes. A generator that appended a position without a normal
        // would produce a mesh that renders with undefined data rather than
        // failing. A shared index across the three vectors would make the
        // invariant structural.
        std::vector<glm::vec3> Normals;
        // May legitimately be SHORTER than Positions -- the procedural primitives
        // (sphere, torus) generate UVs with a seam, and the common approach is
        // to leave seam-duplicated vertices without a second UV. So this one
        // is deliberately allowed to differ in length while Normals is not.
        std::vector<glm::vec2> TexCoords;
        // uint32_t, not uint16_t: a default IcoSphere at subdivision 2 already
        // exceeds the 65535 vertex limit a 16-bit index would impose, and
        // subdivision counts are caller-controlled.
        std::vector<uint32_t> Indices;

        // Emptiness is judged by INDICES, not positions: a mesh with vertices but
        // no triangles draws nothing, so that is the accurate question. Also
        // the cheap check -- a generator that failed early leaves both empty.
        bool Empty() const { return Indices.empty(); }
        // An explicit static_cast rather than an implicit narrowing: the count is
        // computed in size_t and handed to GPU-facing APIs. A mesh above 4
        // billion vertices is impossible in practice, so the cast is safe.
        uint32_t VertexCount() const { return static_cast<uint32_t>(Positions.size()); }
        // INTEGER division by 3, so a malformed index buffer with a length not
        // divisible by 3 silently truncates rather than rounding up or erroring.
        // All generators emit whole triangles, so this is a defensive floor.
        uint32_t TriangleCount() const { return static_cast<uint32_t>(Indices.size() / 3); }
    };

    // Blender's Add > Mesh operator set, generated procedurally.
    //
    // Segment/subdivision counts are Blender's own defaults. Dimensions are
    // scaled from Blender's units (size 2, radius 1) down to the engine's unit
    // convention: every primitive fits inside the same 1x1x1 box centred on
    // the origin that CubeRendererComponent's +/-0.5 cube occupies, so
    // Scale = (1,1,1) makes all of them visually comparable.
    //
    // Results are memoised on the parameter tuple -- the scene calls these
    // every frame per entity, and regeneration would otherwise re-walk the
    // whole index buffer at draw time. Not thread-safe, matching the rest of
    // the render path (draw submission is main-thread only).
    // A namespace of free functions, each returning a CONST REFERENCE. That
    // return type is the memoisation: callers must not hold the reference
    // across a call with different parameters, and there is no mechanism
    // here to enforce or document that per-call.
    namespace ProceduralMeshes
    {
        // 2x2 unit quad in the XZ plane facing +Y (Blender: Plane, size 2).
        // Default 0 subdivisions despite the "2x2 unit quad" comment above: the
        // zero means "minimal mesh, one quad", which is a different default from
        // every other function in the namespace. Worth knowing before relying on
        // the defaults being consistent.
        const ProceduralMesh &Plane(int subdivisions = 0);

        // Subdivided plane for terrain/GI probes (Blender: Grid, size 2, 10 cuts).
        // Blender's own default of 10 cuts, and both axes default independently so
        // a caller can request an anisotropic grid (a road strip, a long wall).
        const ProceduralMesh &Grid(int xSubdivisions = 10, int ySubdivisions = 10);

        // Filled disc in the XZ plane facing +Y (Blender: Circle, 32 verts, NGON fill).
        // NGON fill rather than a triangle fan, per the comment. The parameter
        // name says "vertexCount" while the concept is a SEGMENT count, so a
        // caller asking for 32 gets 32 segments and 32 rim vertices -- the two
        // coincide for a circle, which is why the name is not actively wrong.
        const ProceduralMesh &Circle(int vertexCount = 32);

        // Equal-handed icosahedral sphere (Blender: Ico Sphere, 2 subdivisions).
        // Preferred over UV Sphere for even triangle distribution under
        // displacement -- no pole pinching.
        // Preferred over a UV sphere for displacement, per the comment: an ico
        // sphere has no pole vertices, so there is no pinching artefact where
        // the UV sphere's triangles converge.
        const ProceduralMesh &IcoSphere(int subdivisions = 2);

        // Y-axis cylinder with capped ends (Blender: Cylinder, 32 verts, depth 2).
        const ProceduralMesh &Cylinder(int vertexCount = 32);

        // Y-axis cone with a capped base (Blender: Cone, 32 verts, depth 2).
        const ProceduralMesh &Cone(int vertexCount = 32);

        // Y-axis torus (Blender: Torus, major 1, minor 0.25, 48x12 segments).
        // Two independent segment counts, matching Blender's defaults, and the
        // minor radius is fixed at 0.25 by the mesh rather than being a
        // parameter -- the DrawTorus entry point in Renderer3D takes both
        // radii, so the ratio is decided by the caller, not here.
        const ProceduralMesh &Torus(int majorSegments = 48, int minorSegments = 12);
    }

    // The block above this enum is a CONTRACT with a GPU shader, not an
    // internal detail: these ordinals are written into
    // RayTracingInstance::MaterialParams.z and branched on by the analytic
    // intersection routine in the .glsl/.hlsl.
    // Shape discriminator shared with the Nova path tracer: this value is
    // written to RayTracingInstance::MaterialParams.z and branched on to pick
    // the analytic intersection routine. Keep the numbering in sync with the
    // SHAPE_* constants in Renderer3D_NovaRenderer.glsl / .hlsl.
    //
    // Water also uses this struct but pins Cube with a separate marker in
    // DisplacementParams.z, so these values must stay stable.
    // Explicit : int because the value crosses into shader-side data as a
    // float uniform. Every entry must match a SHAPE_* constant in
    // Renderer3D_NovaRenderer.glsl and .hlsl -- and nothing checks that, so
    // adding a shape here without editing both shaders compiles cleanly and
    // renders nothing.
    enum class PrimitiveShape : int
    {
        // Zero because water reuses this struct with Cube and pins it with a
        // SEPARATE marker in DisplacementParams.z, per the comment above.
        // Reordering any value here breaks the water path as well as the ray
        // tracer.
        Cube = 0,
        Sphere = 1,
        Plane = 2,
        Cylinder = 3,
        Cone = 4,
        Torus = 5,
        Circle = 6,
        // The highest value, added after the original Blender set. A safe
        // addition at the end of the enum -- which is why inserting a shape in
        // the middle would be a silent regression for water.
        Grid = 7,
    };
}