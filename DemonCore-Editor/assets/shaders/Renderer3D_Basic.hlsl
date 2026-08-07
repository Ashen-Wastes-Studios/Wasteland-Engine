// Renderer3D Unified Basic Shader (HLSL for NVRHI)
// Handles textures, coloring, custom tiling, normals, ambient occlusion, and global illumination.

#type vertex

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    int EntityID : ENTITYID;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    nointerpolation int EntityID : ENTITYID;
};

cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(u_ViewProjection, float4(input.Position, 1.0f));
    output.Color = input.Color;
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    output.TexIndex = input.TexIndex;
    output.TilingFactor = input.TilingFactor;
    output.EntityID = input.EntityID;
    return output;
}

#type pixel

struct PSInput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    nointerpolation int EntityID : ENTITYID;
};

struct PSOutput
{
    float4 Color : SV_Target0;
    int EntityID : SV_Target1;
};

Texture2D u_Textures[32] : register(t0);
SamplerState u_Sampler : register(s0);

PSOutput main(PSInput input)
{
    PSOutput output;
    
    float4 texColor = input.Color;

    // Dynamic texture array indexing
    int texIndex = int(input.TexIndex);
    texColor *= u_Textures[texIndex].Sample(u_Sampler, input.TexCoord * input.TilingFactor);

    // Simple default light calculation (directional downward-angled light source)
    float3 lightDir = normalize(float3(0.3, 1.0, 0.4));
    float3 norm = normalize(input.Normal);

    // Combine diffuse lighting element with ambient base factor
    float diffuseIntensity = max(dot(norm, lightDir), 0.0);
    float ambientIntensity = 0.2;
    float lightFactor = ambientIntensity + diffuseIntensity;

    output.Color = float4(texColor.rgb * lightFactor, texColor.a);
    output.EntityID = input.EntityID;
    
    return output;
}
