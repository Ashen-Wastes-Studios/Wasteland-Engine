// Flat Color Shader (HLSL for NVRHI)

#type vertex

// Vertex Shader
struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 Position : SV_Position;
};

cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

cbuffer TransformBuffer : register(b1)
{
    float4x4 u_Transform;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(u_ViewProjection, mul(u_Transform, float4(input.Position, 1.0f)));
    return output;
}

#type pixel

// Pixel Shader
cbuffer ColorBuffer : register(b2)
{
    float4 u_Color;
};

float4 main(VSOutput input) : SV_Target
{
    return u_Color;
}
