// Ray Tracing Compute Shader for 3D Rendering (HLSL for NVRHI)

#type compute

struct RayTracingInstance
{
    matrix InvTransform;
    matrix WorldTransform;
    float4 Albedo;
    float4 MaterialParams;
    float4 Min;
    float4 Max;
    float4 Emission;
    float MaxDistance;
    int LODLevel;
    int TextureID;
    int PackedMaterialMapID;
    float4 TextureScale;
    float4 DisplacementParams;
};

struct BVHNode {
    float4 MinBounds;
    float4 MaxBounds;
};

StructuredBuffer<RayTracingInstance> SceneInstances : register(t1);
StructuredBuffer<BVHNode> BVHNodes : register(t2);
StructuredBuffer<int> LightIndices : register(t3);

RWTexture2D<float4> img_Output : register(u0);
RWTexture2D<float4> img_Accumulation : register(u1);
RWTexture2D<float4> img_Bloom : register(u2);
RWTexture2D<float4> img_Bloom_Temp : register(u7);
RWTexture2D<float2> img_Velocity : register(u5);
RWTexture2D<float4> img_FinalDisplay : register(u6);
RWTexture2D<unorm float4> img_MaterialPacked : register(u3);
RWTexture2D<float4> img_AlbedoHit : register(u4);

Texture2D s_Accumulation : register(t3);
Texture2D s_DepthBuffer : register(t4);
Texture2D s_Output : register(t8);
Texture2D s_Bloom : register(t9);
Texture2D s_Indirect : register(t2);
Texture2D s_NormalBuffer : register(t10);
Texture2D s_InputAlbedo : register(t11);
Texture2D s_PackedMaterialMap : register(t12);
SamplerState s_Sampler : register(s0);

cbuffer ComputeConstants : register(b0)
{
    float3 u_CameraPosition;
    matrix u_InverseViewProjection;
    matrix u_PrevViewProjection;
    int u_InstanceCount;
    int u_FrameIndex;
    int u_SamplesPerPixel;
    int u_PassID;
    int u_DepthBuffer;
    float2 u_Jitter;
    float2 u_PrevJitter;
    float u_CameraMoved;
    matrix u_ViewProjection;
    float3 u_SkyBottomColor;
    float3 u_SkyTopColor;
    float u_AccumulationAlpha;
    float u_NormalStrength;
    float u_RoughnessBias;
    float u_AOIntensity;
    int u_QualityLevel;
    int u_MaxBounces;
    int u_MaxLights;
    int u_IndirectRays;
    int u_StepSize;
    float u_RenderScale;
    int u_LightCount;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 st = dispatchThreadID.xy;
    
    img_Output[st] = float4(u_SkyTopColor, 1.0f);
    img_Accumulation[st] = float4(0.0f, 0.0f, 0.0f, 1.0f);
}
