// Nova visibility-buffer producer (Phase 1, Task 1.1 of the mesh/RT/raster hybrid plan).
//
// ONE INSTANCED DRAW for the whole scene. Task 1.0 added instanceCount/firstInstance to
// RenderCommand::DrawIndexed; without them SV_InstanceID is always 0 and every pixel would
// carry meshId 0, making the buffer worthless.
//
// Per-instance data comes from the SAME buffer the compute path already binds:
//   StructuredBuffer<RayTracingInstance> Instances : register(t1)
// RayTracingInstance is declared in Renderer3D_NovaRenderer.slang. It is repeated here
// because Slang compiles each source file standalone -- there is no module import, and
// duplicating the struct is the only way to keep the two stages reading the same layout.
// If the fields here drift from the compute shader's copy, the visibility buffer and the
// tracer disagree about where every instance is.

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

StructuredBuffer<RayTracingInstance> Instances : register(t1);

// The visibility shader is a separate pipeline from the compute one, so it owns its own
// cbuffer. Slot b1 is chosen to match the compute shader's NovaConstants (b1) so the CPU
// can upload both from the same per-frame block. THE CPU MUST ACTUALLY UPLOAD TO b1 --
// nothing does yet; Task 1.2/1.3 wires it. Until then the VS transforms with an identity
// matrix and every instance lands at the origin.
cbuffer VisConstants : register(b1)
{
    float4x4 u_ViewProjection;
    // PrimitiveShape ordinal (ProceduralMeshes.h) of the mesh bound for THIS draw.
    // The whole scene is drawn once per shape and every instance that belongs to a
    // different shape is collapsed to a clipped position, so the meshId written to
    // the G-buffer stays SV_InstanceID -- an index into the SAME reordered instance
    // buffer the compute path binds at t1. Compacting the buffer per shape would
    // renumber the instances and silently break that contract.
    int      u_Shape;
};

struct VisVSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VisVSOut
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    nointerpolation uint meshId : MESHID;
};

struct VisPSOut
{
    // R32_SINT matches the engine's RED_INTEGER attachment.
    //
    // The "nothing here" sentinel is -1, matching NVRHIFramebuffer's RED_INTEGER
    // mapping: the engine uses -1 for "no entity" in its picking attachment, and
    // clearing to 0 would make every empty pixel look like instance 0.
    int   vis    : SV_Target0;   // meshId, or -1 where no instance covers the pixel
    float4 albedo : SV_Target1;
    float4 normal : SV_Target2;
};

[shader("vertex")]
VisVSOut NovaVisVS(VisVSIn i, uint instanceID : SV_InstanceID)
{
    VisVSOut o;
    RayTracingInstance inst = Instances[instanceID];

    // Per-shape draw filter. Instances of a different shape are moved behind the
    // camera so the triangle is clipped away and rasterises to nothing. Doing this
    // in the VS rather than by compacting the instance buffer is what keeps
    // meshId == SV_InstanceID == the compute path's Instances[] index.
    if (u_Shape >= 0 && (int)(inst.MaterialParams.z + 0.5f) != u_Shape)
    {
        o.pos    = float4(0.0f, 0.0f, -2.0f, 1.0f);
        o.normal = float3(0.0f, 0.0f, 1.0f);
        o.uv     = float2(0.0f, 0.0f);
        o.meshId = instanceID;
        return o;
    }

    // The instance transform is MANDATORY. The analytic primitives are authored in LOCAL
    // space, so a VS that only applies u_ViewProjection puts every instance at the
    // origin -- the first draft of this sketch made exactly that mistake, and it would
    // have rendered the whole scene as one blob at the world origin.
    o.pos    = mul(u_ViewProjection, mul(inst.WorldTransform, float4(i.pos, 1.0)));

    // World normal. Uses the upper 3x3 of WorldTransform directly, which is only correct
    // under uniform scale; non-uniform scale needs the inverse-transpose. The analytic
    // primitives are uniformly scaled, so this is an acceptable approximation here, but
    // it is an approximation and should not be copied into a general mesh path.
    o.normal = mul(float3x3(inst.WorldTransform), i.normal);
    o.uv     = i.uv;
    o.meshId = instanceID;
    return o;
}

[shader("fragment")]
VisPSOut NovaVisFS(VisVSOut i)
{
    VisPSOut o;
    RayTracingInstance inst = Instances[i.meshId];
    o.vis    = (int)i.meshId;
    // Metal and roughness ride in the two alpha channels, which nothing else
    // reads: MaterialComponent::Albedo's alpha is documented as unused (opacity
    // comes from transmission/thickness) and the normal's alpha was a constant 0.
    // Without them the raster-primary path has to guess metal=0, rough=1 and every
    // metal in the scene shades as a dielectric.
    float metal = clamp(inst.MaterialParams.x, 0.0, 1.0);
    float rough = inst.MaterialParams.y > 0.0 ? inst.MaterialParams.y : 0.5;
    o.albedo = float4(inst.Albedo.rgb, metal);
    o.normal = float4(normalize(i.normal) * 0.5 + 0.5, rough);
    return o;
}
