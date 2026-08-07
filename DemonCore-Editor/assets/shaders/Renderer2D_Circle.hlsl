// Renderer2D Circle Shader (HLSL for NVRHI)

#type vertex

struct VSInput
{
    float3 WorldPosition : POSITION;
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
    int EntityID : ENTITYID;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
    nointerpolation int EntityID : ENTITYID;
};

cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(u_ViewProjection, float4(input.WorldPosition, 1.0f));
    output.LocalPosition = input.LocalPosition;
    output.Color = input.Color;
    output.Thickness = input.Thickness;
    output.Fade = input.Fade;
    output.EntityID = input.EntityID;
    return output;
}

#type pixel

struct PSInput
{
    float4 Position : SV_Position;
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
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
    
    // Calculate distance and fill circle with white
    float distance = 1.0 - length(input.LocalPosition);
    float circle = smoothstep(0.0, input.Fade, distance);
    circle *= smoothstep(input.Thickness + input.Fade, input.Thickness, distance);

    if (circle == 0.0)
        discard;

    // Set output color
    output.Color = input.Color;
    output.Color.a *= circle;

    output.EntityID = input.EntityID;
    
    return output;
}
