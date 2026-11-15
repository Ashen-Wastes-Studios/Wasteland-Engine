// Nova mesh-shader path (Phase 2, Task 2.2 of the mesh/RT/raster hybrid plan).
//
// THE SAME VISIBILITY BUFFER, PRODUCED BY MESH SHADERS INSTEAD OF A VS.
// Renderer3D_NovaVisibility.slang rasterises the scene with a classic vertex +
// fragment pipeline. This file produces the IDENTICAL G-buffer (meshId, albedo,
// normal, depth) from a mesh pipeline, so the Nova compute schedule and
// TryRasterPrimary() cannot tell the two apart. That is the whole point of the
// hybrid path: primary visibility from rasterisation, everything else from
// compute.
//
// ONE SOURCE, TWO BACKENDS. The plan's first draft said "plain vector arrays, not
// structs", from a probe where a struct form aborted with
// `Slang::InternalError: unexpected: not a struct`. That conclusion was wrong --
// the deciding factor is the QUALIFIERS:
//
//   form                                        SPIR-V (slangc)   DXIL (SDK dxc ms_6_5)
//   plain arrays, no qualifiers                  works             FAILS: "Semantic must be defined for all outputs"
//   struct + vertices/primitives/indices        works             works
//   struct, no qualifiers                       FAILS              fails
//
// So the struct-with-qualifiers form below is the only one that compiles on both,
// which is why this is one file compiled twice rather than two sources:
//
//   slangc -target spirv -entry NovaMeshMS -stage mesh ...   -> Vulkan
//   SDK dxc -T ms_6_5 -E NovaMeshMS ...                       -> DX12
//
// Do NOT drop the vertices/primitives/indices qualifiers: without them Slang
// aborts the SPIR-V emit with "Slang::InternalError: unexpected: not a struct".
//
// ARRAY SIZES ARE THE MAXIMA, NOT THE MESHLET'S ACTUAL COUNTS. `verts[64]` and
// `prims[124]`/`idx[124]` must be sized at the budget (64 vertices / 124
// triangles, MeshletBuilder.h); SetMeshOutputCounts() reports the real counts.
// The `out indices` array size MUST equal the primitive array size -- a mismatch
// fails with "max primitive count mismatch".
//
// MESHLET-LOCAL INDICES. The mesh's own index buffer cannot be used directly: a
// mesh shader addresses its vertices by position in the MESHLET's vertex list.
// MeshletBuilder therefore emits a parallel local index array
// (BuildMeshlets' outMeshletIndices), and that is what this shader reads.
//
// THE BUDGET CONSTANTS ARE DUPLICATED FROM MeshletBuilder.h. Slang cannot see the
// C++ header, so the values are restated here and nothing checks that they agree.
// They must: a meshlet built with a different budget than the shader's array sizes
// would read past the array. Keep both in step.

struct RayTracingInstance
{
    column_major float4x4 InvTransform;
    column_major float4x4 WorldTransform;
    float4 Albedo;
    float4 MaterialParams;
    float4 Min;
    float4 Max;
    float4 Emission;
    float  MaxDistance;
    int    LODLevel;
    int    TextureID;
    int    PackedMaterialMapID;
    float4 TextureScale;
    float4 DisplacementParams;
    float4 WaveParams;  // x=amplitude, y=wavelength, z=speed, w=chop; water only
    int    TerrainVertexOffset;
    int    TerrainCellsPerSide;
    int    TerrainPad0;
    int    TerrainPad1;
};

// Duplicated from Renderer3D_NovaRenderer.slang for the same reason the visibility
// shader duplicates it: Slang compiles each source file standalone, with no module
// import. If these fields drift, the mesh path and the compute path disagree about
// where every instance is.
StructuredBuffer<RayTracingInstance> Instances : register(t1);

// The mesh's own vertex data. One element per mesh vertex, in the mesh's local
// space, matching ProceduralMesh's parallel Positions/Normals/TexCoords vectors.
struct MeshVertex
{
    float3 position;
    float3 normal;
    float2 texcoord;
};
StructuredBuffer<MeshVertex> MeshVertices : register(t2);

// The cluster table from MeshletBuilder. BoundsMin/BoundsMax are float4 so the
// struct is 16-byte aligned and the GPU reads it in one transaction.
struct MeshletData
{
    uint   VertexOffset;
    uint   VertexCount;
    uint   PrimitiveOffset;
    uint   PrimitiveCount;
    float4 BoundsMin;
    float4 BoundsMax;
    float  BoundingSphereRadius;
    float  Pad0;
};
StructuredBuffer<MeshletData> Meshlets : register(t3);

// Remapped mesh vertices: MeshletVertices[Meshlet.VertexOffset + local] is the
// mesh vertex index for meshlet-local vertex `local`.
StructuredBuffer<uint> MeshletVertices : register(t4);

// Meshlet-local triangle indices: MeshletIndices[(Meshlet.PrimitiveOffset + t) * 3 + k]
// is the meshlet-local vertex for triangle t, corner k.
StructuredBuffer<uint> MeshletIndices : register(t5);

cbuffer MeshConstants : register(b1)
{
    float4x4 u_ViewProjection;
    // Instance index into Instances[] for THIS draw. A mesh draw is one instance
    // at a time, unlike the visibility shader's single instanced draw, because the
    // mesh pipeline's amplification stage dispatches per instance and the mesh
    // shader per meshlet.
    int      u_Instance;
    int      u_Pad0;
    int      u_Pad1;
    int      u_Pad2;
};

struct MeshVert
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    // Carried through to the fragment shader so it can look up the instance's
    // albedo and material params. The mesh shader cannot read a varying that is
    // not declared in its output struct.
    nointerpolation uint meshId : MESHID;
};

struct MeshPrim
{
    // SV_PrimitiveID lets the fragment shader identify the triangle; the mesh
    // shader's primitive payload is the only channel that reaches the fragment
    // stage without going through the rasteriser's interpolation.
    uint prim : SV_PrimitiveID;
};

struct MeshPSOut
{
    // Must match Renderer3D_NovaVisibility.slang's VisPSOut exactly: the Nova
    // compute schedule reads these four attachments and does not know or care
    // which pipeline wrote them.
    int   vis    : SV_Target0;   // meshId, or -1 where no geometry covers the pixel
    float4 albedo : SV_Target1;   // rgb = material albedo, a = metal
    float4 normal : SV_Target2;   // rgb = world normal * 0.5 + 0.5, a = roughness
};

// 64 threads: the meshlet vertex budget. Each thread emits at most one vertex,
// which is the mapping the budget was chosen for.
static const uint kMeshletMaxVertices  = 64;
static const uint kMeshletMaxTriangles = 124;

[shader("mesh")][numthreads(64, 1, 1)][outputtopology("triangle")]
void NovaMeshMS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                out vertices   MeshVert  verts[kMeshletMaxVertices],
                out primitives MeshPrim  prims[kMeshletMaxTriangles],
                out indices    uint3     idx[kMeshletMaxTriangles])
{
    const MeshletData meshlet = Meshlets[gid.x];

    // Report the REAL counts. The arrays are sized at the budget, so without this
    // the rasteriser would emit 64 vertices and 124 triangles regardless of what
    // the meshlet actually contains.
    SetMeshOutputCounts(meshlet.VertexCount, meshlet.PrimitiveCount);

    const RayTracingInstance inst = Instances[u_Instance];

    // One vertex per thread. Threads beyond VertexCount return without writing;
    // the unwritten slots are not emitted because SetMeshOutputCounts capped the
    // count.
    if (tid.x < meshlet.VertexCount)
    {
        const uint meshVertexIndex = MeshletVertices[meshlet.VertexOffset + tid.x];
        const MeshVertex v = MeshVertices[meshVertexIndex];

        // The instance transform is MANDATORY -- the mesh is authored in local
        // space, so omitting it puts every instance at the origin. Same rule, and
        // same warning, as the visibility VS: the upper 3x3 is only correct under
        // uniform scale, which is what the analytic primitives use.
        verts[tid.x].pos    = mul(u_ViewProjection, mul(inst.WorldTransform, float4(v.position, 1.0)));
        verts[tid.x].normal = mul(float3x3(inst.WorldTransform), v.normal);
        verts[tid.x].uv     = v.texcoord;
        verts[tid.x].meshId = (uint)u_Instance;
    }

    // Triangles are distributed round-robin: thread t takes t, t+64, t+128, ...
    // PrimitiveCount is at most 124, so no thread takes more than two.
    for (uint t = tid.x; t < meshlet.PrimitiveCount; t += 64)
    {
        const uint base = (meshlet.PrimitiveOffset + t) * 3;
        idx[t]   = uint3(MeshletIndices[base + 0], MeshletIndices[base + 1], MeshletIndices[base + 2]);
        prims[t].prim = t;
    }
}

// The fragment stage is shared by both producers. It is duplicated here rather
// than imported because Slang has no module import; the two copies must stay
// byte-identical in their output semantics.
[shader("fragment")]
MeshPSOut NovaMeshFS(MeshVert i)
{
    MeshPSOut o;
    RayTracingInstance inst = Instances[i.meshId];
    o.vis    = (int)i.meshId;
    float metal = clamp(inst.MaterialParams.x, 0.0, 1.0);
    float rough = inst.MaterialParams.y > 0.0 ? inst.MaterialParams.y : 0.5;
    o.albedo = float4(inst.Albedo.rgb, metal);
    o.normal = float4(normalize(i.normal) * 0.5 + 0.5, rough);
    return o;
}
