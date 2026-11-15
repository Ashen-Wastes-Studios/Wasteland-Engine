// Nova mesh shader -- DX12 HLSL source.
//
// WHY THIS FILE EXISTS SEPARATELY. Slang's DXIL emit for the mesh stage is broken:
// it lowers
//     out vertices MeshVert verts[64]
// to
//     out vertices inout MeshVert verts_0[int(64)]
// which dxc rejects with "duplicate HLSL parameter usages 'out'". The SPIR-V emit
// is fine, so this file is the DX12 half of a two-route build:
//
//   slangc -target spirv ... Renderer3D_NovaMesh.slang  -> Vulkan
//   SDK dxc -T ms_6_5 ...    Renderer3D_NovaMesh.hlsl   -> DX12
//
// The two files MUST stay semantically identical. They are not generated from one
// another, so nothing enforces that; the shared contract is the G-buffer they
// write (meshId / albedo / normal / depth), which is what the Nova compute
// schedule reads. If one path drifts, the two backends render the same scene
// differently and nothing downstream can detect it.
//
// The meshlet budget constants are duplicated from MeshletBuilder.h for the same
// reason the .slang file duplicates them: dxc cannot see the C++ header.

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
    float4 WaveParams;
    int    TerrainVertexOffset;
    int    TerrainCellsPerSide;
    int    TerrainPad0;
    int    TerrainPad1;
};

StructuredBuffer<RayTracingInstance> Instances : register(t1);

struct MeshVertex
{
    float3 position;
    float3 normal;
    float2 texcoord;
};
StructuredBuffer<MeshVertex> MeshVertices : register(t2);

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

StructuredBuffer<uint> MeshletVertices : register(t4);
StructuredBuffer<uint> MeshletIndices  : register(t5);

// Per-shape dispatch ranges: [meshletOffset, +meshletCount) into the
// concatenated Meshlets table. The mesh shader walks this to derive
// (shape, instance, meshlet) from its group index, which is what lets one
// DispatchMesh draw the whole scene with no per-dispatch CPU state.
struct DispatchEntry
{
    uint meshletOffset;
    uint meshletCount;
};
StructuredBuffer<DispatchEntry> DispatchTable : register(t6);

static const uint kShapeCount = 8;

// The payload the amplification shader passes. The MS MUST declare it: an AS that
// passes a payload to DispatchMesh while the MS declares none fails PSO creation
// with E_INVALIDARG (the DXIL validation requires the two to match). With no AS
// the payload is undefined, so the meshlet index comes from gid.x and the instance
// from u_UsePayload ? payload.instance : u_Instance.
struct MeshPayload
{
    uint instance;
    uint meshlet;
};

cbuffer MeshConstants : register(b1)
{
    float4x4 u_ViewProjection;
    // Instance count for the scene. The mesh shader derives the per-group
    // instance index from its group index and this count.
    int      u_InstanceCount;
    // 1 = read the instance from the AS payload, 0 = read u_Instance.
    int      u_UsePayload;
    // The shape this dispatch is for. The mesh shader discards a meshlet whose
    // instance does not own this shape (MaterialParams.z), which is the mesh-path
    // equivalent of the visibility VS's per-instance filter. Without it the mesh
    // path draws EVERY shape at EVERY instance, so the two producers disagree.
    int      u_Shape;
    int      u_Pad2;
};

struct MeshVert
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    nointerpolation uint meshId : MESHID;
};

struct MeshPrim
{
    uint prim : SV_PrimitiveID;
};

struct MeshPSOut
{
    int   vis    : SV_Target0;
    float4 albedo : SV_Target1;
    float4 normal : SV_Target2;
};

static const uint kMeshletMaxVertices  = 64;
static const uint kMeshletMaxTriangles = 124;

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void NovaMeshMS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                out vertices   MeshVert  verts[kMeshletMaxVertices],
                out primitives MeshPrim  prims[kMeshletMaxTriangles],
                out indices    uint3     idx[kMeshletMaxTriangles])
{
    // Derive (shape, instance, meshlet) from the global group index. Each
    // shape owns [base, base + meshletCount * u_InstanceCount) groups; within
    // a shape the instance is the high part of the local index and the meshlet
    // the low part. Everything comes from buffers, so the derivation is valid
    // per group -- no CB or root-binding value varies per dispatch.
    uint instance = 0;
    uint meshletIndex = gid.x;
    uint shape = 0;
    bool valid = false;
    uint base = 0;
    for (uint s = 0; s < kShapeCount; s++)
    {
        const DispatchEntry e = DispatchTable[s];
        const uint range = e.meshletCount * (uint)u_InstanceCount;
        if (gid.x < base + range)
        {
            const uint local = gid.x - base;
            instance = local / e.meshletCount;
            meshletIndex = e.meshletOffset + (local % e.meshletCount);
            shape = s;
            valid = true;
            break;
        }
        base += range;
    }

    RayTracingInstance inst = (RayTracingInstance)0;
    if (valid)
        inst = Instances[instance];

    // Per-instance shape filter, matching the visibility VS: an instance only
    // draws the meshlets of its own shape. Emitting zero vertices and primitives
    // is how a mesh shader discards a cluster.
    //
    // SetMeshOutputCounts is called EXACTLY ONCE: dxc rejects a second call in the
    // same entry point ("SetMeshOutputCounts cannot be called multiple times"), so
    // the counts are computed first and the early-out happens after.
    MeshletData meshlet = (MeshletData)0;
    uint vertexCount = 0;
    uint primitiveCount = 0;
    if (valid && (int)inst.MaterialParams.z == (int)shape)
    {
        meshlet = Meshlets[meshletIndex];
        vertexCount = meshlet.VertexCount;
        primitiveCount = meshlet.PrimitiveCount;
    }
    SetMeshOutputCounts(vertexCount, primitiveCount);
    if (vertexCount == 0) return;

    if (tid.x < vertexCount)
    {
        const uint meshVertexIndex = MeshletVertices[meshlet.VertexOffset + tid.x];
        const MeshVertex v = MeshVertices[meshVertexIndex];

        verts[tid.x].pos    = mul(u_ViewProjection, mul(inst.WorldTransform, float4(v.position, 1.0)));
        verts[tid.x].normal = mul((float3x3)inst.WorldTransform, v.normal);
        verts[tid.x].uv     = v.texcoord;
        verts[tid.x].meshId = instance;
    }

    for (uint t = tid.x; t < primitiveCount; t += 64)
    {
        const uint triBase = (meshlet.PrimitiveOffset + t) * 3;
        idx[t]   = uint3(MeshletIndices[triBase + 0], MeshletIndices[triBase + 1], MeshletIndices[triBase + 2]);
        prims[t].prim = t;
    }
}

[shader("pixel")]
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
