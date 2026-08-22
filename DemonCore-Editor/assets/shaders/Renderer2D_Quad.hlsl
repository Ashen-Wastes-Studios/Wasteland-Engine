// Renderer2D Quad Shader (HLSL for NVRHI)

#type vertex

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    int EntityID : ENTITYID;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
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

    int texIndex = clamp(int(input.TexIndex), 0, 31);
    output.Color = u_Textures[texIndex].Sample(u_Sampler, input.TexCoord * input.TilingFactor) * input.Color;
    output.EntityID = input.EntityID;

    return output;
}
