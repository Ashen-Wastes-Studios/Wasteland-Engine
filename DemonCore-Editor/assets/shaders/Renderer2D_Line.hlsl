// Renderer2D Line Shader (HLSL for NVRHI)

#type vertex

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
    int EntityID : ENTITYID;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
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
    output.EntityID = input.EntityID;
    return output;
}

#type pixel

struct PSInput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    nointerpolation int EntityID : ENTITYID;
};

struct PSOutput
{
    float4 Color : SV_Target0;
    int EntityID : SV_Target1;
};

PSOutput main(PSInput input)
{
    PSOutput output;
    output.Color = input.Color;
    output.EntityID = input.EntityID;
    return output;
}
