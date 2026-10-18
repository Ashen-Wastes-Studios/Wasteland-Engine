// NVRHI-sourced line shader for Renderer2D's line pass, in two stages
// selected by the #type markers below (a convention NVRHI's shader
// preprocessor understands, not an HLSL keyword -- the file is otherwise
// plain HLSL).
//
// It is the simplest shader in the set: position transform, colour
// passthrough. What makes it interesting is the ENTITYID field, which is
// passed through both stages untouched so a render target can be read
// back and queried for which entity a line came from.
// Renderer2D Line Shader (HLSL for NVRHI)

// Selects the vertex-shader compilation of the half-file that follows.
// Everything from here to the #type pixel marker is the VS; everything
// after it is the PS.
#type vertex

// Vertex attributes. Semantics (POSITION, COLOR) are what NVRHI names the
// engine's vertex-buffer elements with, so the two must be kept in step
// with the vertex layout the CPU side builds.
struct VSInput
{
    // Three components, no w. The implicit 1.0f is supplied by the mul below,
    // so the CPU side never uploads a 4-component position for this pass.
    float3 Position : POSITION;
    float4 Color : COLOR;
    // The source entity, carried as an int rather than a float.
    //
    // Present in the VS INPUT even though it needs no transform -- it is read
    // straight from the vertex buffer and passed along so the pixel stage can
    // write it out.
    int EntityID : ENTITYID;
};

// What the VS hands the PS. Split structs rather than reusing VSInput
// because SV_Position has already been transformed into clip space by the
// time it reaches the PS.
struct VSOutput
{
    // The system-value semantic: after the VS writes this, the hardware
    // performs the perspective divide and viewport transform, so the PS
    // receives screen-space coordinates.
    float4 Position : SV_Position;
    float4 Color : COLOR;
    // nointerpolation means 'do not interpolate': every fragment under a
    // triangle reads the value from the provoking vertex instead of a blend
    // across the face. Without it a rasterised line would return a smoothly
    // varying, meaningless entity id, and the readback below would name an
    // entity that was never there.
    //
    // It has to be nointerpolation here AND on PSInput for the attribute to
    // survive the stage boundary.
    nointerpolation int EntityID : ENTITYID;
};

// The one constant buffer, bound at b0. Register naming matters: the CPU
// side sets that binding by hand, so a change here without a matching
// change there samples whatever is in b0 instead.
cbuffer CameraBuffer : register(b0)
{
    // A column-major 4x4 by HLSL default, so it pairs with a glm::mat4 from
    // the camera without a transpose on upload. Named u_ prefixed like every
    // other uniform in the codebase.
    float4x4 u_ViewProjection;
};

// The VS entry point. One function, no branching -- lines are submitted as
// already-expanded geometry, so the rasteriser does the work.
VSOutput main(VSInput input)
{
    // Uninitialised, then fully assigned on the next three lines. Legal here
    // because every field of VSOutput is written before the return; a field
    // added to the struct without a matching write would leak indeterminate
    // data into the interpolators.
    VSOutput output;
    // World to clip. mul() takes the matrix on the LEFT, so the operand order
    // is matrix-then-vector as HLSL requires for this direction; reversed, it
    // would be a compile error rather than a silent transpose.
    //
    // The float4 with the trailing 1.0f supplies the homogeneous w that the
    // 3-component attribute omits. Lines carry no perspective effect of their
    // own, so w stays 1 through the divide.
    output.Position = mul(u_ViewProjection, float4(input.Position, 1.0f));
    output.Color = input.Color;
    output.EntityID = input.EntityID;
    return output;
}

// Switches compilation to the fragment stage for the rest of the file.
#type pixel

// Interpolator struct. Mirrors VSOutput field for field -- standard
// practice, and required here since SV_Position is written by the VS but
// declared again as an input to the PS.
struct PSInput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    // The repeat of the VS-side modifier. Both halves must say it: the VS
    // stops the value being interpolated across a primitive, and the PS
    // declares it expects that flat value.
    //
    // Cost: every covered fragment redundantly shades the same id, which is
    // fine -- the alternative would be averaging ids into nonsense.
    nointerpolation int EntityID : ENTITYID;
};

// Two render targets, so the struct has two SV_Target slots. This is a
// multi-target pass: colour to target 0, entity id to target 1.
struct PSOutput
{
    // The colour attachment. RGBA even though line vertices may arrive with
    // unused components -- the rasteriser blends across the line using this.
    float4 Color : SV_Target0;
    // The id attachment, an integer target. This is the reason the whole
    // shader exists: a render target holding which entity each pixel came
    // from, which something like Framebuffer::ReadPixel can then query to
    // implement picking.
    //
    // Writing an int to an integer target requires the matching attachment be
    // a RED_INTEGER format -- the same assumption behind
    // OpenGLFramebuffer::ReadPixel, which hardcodes a RED_INTEGER read.
    int EntityID : SV_Target1;
};

// The PS entry point, again a straight passthrough: both outputs are the
// inputs unchanged, so the stage exists purely to route values to the two
// attachments.
PSOutput main(PSInput input)
{
    // Uninitialised but fully written below, same reasoning as the VS's local.
    PSOutput output;
    output.Color = input.Color;
    output.EntityID = input.EntityID;
    // Returning a struct is how both stages' outputs are delivered. Everything
    // the hardware needed was written to SV_-tagged fields, so no register
    // hand-off is involved.
    return output;
}
