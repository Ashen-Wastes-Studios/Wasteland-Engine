// The smallest shader in the renderer: one constant colour across every
        // pixel of a transformed triangle. Useful as a debug primitive and as
        // the baseline for measuring rasterisation cost with no texture or
        // lighting work.
// Flat Color Shader (HLSL for NVRHI)

#type vertex

// A bare comment, not a marker. The stage split above is done by the
        // engine preprocessor scans for the stage marker comment on
        // line 3; these labels are for the human reading the file.
// Vertex Shader
// One attribute: position, already in object space. No colour, no normal,
        // no uv -- everything this shader draws is flat, so nothing else
// needs to reach the GPU.
struct VSInput
{
    float3 Position : POSITION;
};

// The minimal inter-stage struct: SV_Position only. There is no varyings
// because the pixel stage does not read the interpolated position --
// it returns a constant. Contrast with Renderer2D_Circle.hlsl, which
// carries colour, local position and an entity id across the boundary.
struct VSOutput
{
    float4 Position : SV_Position;
};

// Three constant buffers rather than one, each explicitly bound to a fixed
// register slot. This is the portable spelling NVRHI uses: register(b0)
// means the same thing as a D3D binding slot, a Vulkan descriptor set
// binding and a GL uniform block, so one source compiles on all four
// backends.
cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

// The per-object transform, in a SEPARATE buffer from the camera. That
// split is the performance decision: the view-projection is identical
// for everything in a frame and is uploaded once, while this matrix
// changes per object.
cbuffer TransformBuffer : register(b1)
{
    float4x4 u_Transform;
};

// Entry point for the vertex stage. Named `main`, and the pixel stage
// below is also `main` -- the preprocessor gives each stage its own
// compilation, so there is no collision.
VSOutput main(VSInput input)
{
    VSOutput output;
    // Object to clip space, in that order: the model transform is applied
    // first, then the view-projection. The nesting order is load-
    // bearing -- with float4x4 columns, multiplying the other way applies
    // the transforms in reverse and produces a plausible-looking but
    // wrong projection.
    // The 1.0f supplies w for the perspective divide. Both matrices come
    // from constant buffers, so the GPU does the arithmetic; there is no
    // per-vertex matrix here.
    output.Position = mul(u_ViewProjection, mul(u_Transform, float4(input.Position, 1.0f)));
    return output;
}

#type pixel

// The stage boundary. Everything between here and the end is the pixel
// shader, and the preprocessor splits on the marker comment above it.
// Pixel Shader
// The colour is a UNIFORM, not a vertex attribute. So every vertex drawn
// with this shader is the same colour, and changing it means one
// constant-buffer write rather than a new vertex buffer.
cbuffer ColorBuffer : register(b2)
{
    float4 u_Color;
};

// Takes the whole VSOutput as its parameter even though it uses none of
// it. That is the HLSL convention for a pixel entry point -- the
// signature shape is required, and omitting the parameter is a
// compile error in some shader model versions.
//
// Note there is no clipping logic, no alpha test and no interpolation:
// the rasteriser has already decided which fragments are inside the
// triangle.
float4 main(VSOutput input) : SV_Target
{
    // A single unconditional return of the constant. No discard, no blend
    // state -- which is why this shader is the reference for measuring
    // pure rasterisation cost.
    return u_Color;
}
