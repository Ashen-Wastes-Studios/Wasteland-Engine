// A dual-stage shader in ONE file. The stage split below is done by the
// engine's shader preprocessor scanning for the marker comments, not by the
// build system -- so the two entry points are both called `main` and the
// backend decides which one is which. The marker comment is a real
// token to the preprocessor, so it must stay at the start of a line.
// Renderer2D Circle Shader (HLSL for NVRHI)

#type vertex

// Vertex attributes, named to match the C++ vertex buffer layout one-for-one.
// Every semantic here has to agree with the CPU-side layout element or
// the data arrives in the wrong registers -- a mismatch that compiles
// cleanly and renders garbage.
struct VSInput
{
    // Already in world space: the transform is applied CPU-side by the caller,
    // so this shader multiplies only by the view-projection below. That
    // keeps one matrix upload per frame instead of one per vertex.
    float3 WorldPosition : POSITION;
    // Object-space position, and the reason this shader exists: an analytic
    // circle is evaluated in object space, so a pixel's LocalPosition
    // length is its distance from the circle's centre regardless of
    // where that circle sits on screen or how the camera is oriented.
    // Without it a screen-space circle would need a radius parameter per
    // instance.
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    // Ring thickness, in the same object-space units as LocalPosition, so the
    // ring scales correctly with the transform for free.
    float Thickness : THICKNESS;
    // The anti-aliasing width, also in object space. Reused as the edge soft-
    // ness in BOTH smoothstep calls below, which is why it is one value
    // rather than two: a single width keeps the outer and inner falloffs
    // symmetrical.
    float Fade : FADE;
    // Passed through to the pixel stage and written to SV_Target1. This is a
    // GPU-picking channel: the renderer reads the integer out of a second
    // render target to identify what was drawn at a pixel. It therefore
    // has to be an integer type end to end -- interpolated as a float and
    // rounded, entity ids above 2^24 would alias.
    int EntityID : ENTITYID;
};

// The inter-stage struct. In HLSL this is an ordinary struct whose members
// happen to carry the semantics the rasteriser assigns them, so the
// field names double as the interface between the two stages.
struct VSOutput
{
    // Clip space, written by the matrix multiply below. SV_Position is the one
    // semantic that is NOT passed to the pixel shader as written -- the
    // hardware derives the pixel coordinate from it.
    float4 Position : SV_Position;
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
    // nointerpolation is LOAD-BEARING here and is repeated on the pixel-side
    // declaration to match. Without it the hardware would interpolate the
    // id across the triangle, so a primitive spanning several pixels could
    // report a fractional or blended id that matches no entity -- and
    // picking would silently return garbage on any large shape.
    nointerpolation int EntityID : ENTITYID;
};

// One constant buffer, one 4x4 matrix, one fixed register slot. register(b0)
// is the explicit binding that makes this work across NVRHI's D3D11,
// D3D12, Vulkan and GL backends, each of which binds constant buffers
// differently; the register annotation is the portable spelling.
cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

// Entry point. Named `main` for the vertex stage, and `main` again for the
// pixel stage below -- legal because the preprocessor gives each
// stage its own compilation unit.
VSOutput main(VSInput input)
{
    VSOutput output;
    // The 1.0f supplies the w component for the perspective divide. The vector
    // is constructed with an explicit float4 rather than relying on an
    // implicit float3-to-float4 conversion, so the w is unambiguous.
    output.Position = mul(u_ViewProjection, float4(input.WorldPosition, 1.0f));
    output.LocalPosition = input.LocalPosition;
    output.Color = input.Color;
    output.Thickness = input.Thickness;
    output.Fade = input.Fade;
    // A pure pass-through: the vertex stage does not interpret the id, it
    // only carries it to the pixel stage where it is written to a render
    // target.
    output.EntityID = input.EntityID;
    return output;
}

#type pixel

// Declares SV_Position alongside the interpolated varyings. The engine's
// preprocessor pastes the stage bodies into separate translations, so
// the pixel stage cannot see the VSOutput type and must redeclare what
// it consumes. The two declarations must agree field for field -- a
// mismatch here is a link-time or silent-garbage failure, not a
// compile error.
struct PSInput
{
    float4 Position : SV_Position;
    float3 LocalPosition : LOCALPOSITION;
    float4 Color : COLOR;
    float Thickness : THICKNESS;
    float Fade : FADE;
    nointerpolation int EntityID : ENTITYID;
};

// Two targets, so this shader draws to a multiple-render-target pass:
// target 0 is the colour the user sees, target 1 is the integer id
// buffer. The pass must therefore have been created with at least two
// attachments.
struct PSOutput
{
    // Attachment 0 of the MRT. The assignment to it is unconditional, but the
    // early discard below means a fragment outside the ring never reaches it --
    // it simply leaves this attachment untouched for the background.
    float4 Color : SV_Target0;
    // An INTEGER render target. The format of the attachment must be an
    // integer format, not a float one, or this write is invalid. Writing
    // the un-discarded pixel's id unconditionally is what lets the
    // renderer map a screen pixel back to an entity.
    int EntityID : SV_Target1;
};

PSOutput main(PSInput input)
{
    PSOutput output;
    
    // Calculate distance and fill circle with white
    // `distance` shadows nothing in HLSL, and the inversion is deliberate:
    // closer to the centre means a LARGER value, so the smoothstep edges
    // below can be written with increasing limits.
    float distance = 1.0 - length(input.LocalPosition);
    // Outer edge: fades in from the centre over a width of Fade. smoothstep is
    // the anti-aliasing function -- it interpolates smoothly and clamps at
    // both ends, so no branch is needed to keep the rim from aliasing.
    float circle = smoothstep(0.0, input.Fade, distance);
    // Inner edge: an INVERTED smoothstep (low > high) that multiplies down to
    // zero once distance falls below the ring thickness. The +Fade matches
    // the outer edge's width, which is what keeps the ring's two borders
    // the same softness.
    // DEFECT (thin-ring degenerate case, documented not fixed): when
    // Thickness and Fade overlap -- Thickness smaller than Fade -- the two
    // windows invert and the whole ring can multiply to zero, erasing the
    // primitive entirely. Nothing clamps Thickness to be at least Fade.
    circle *= smoothstep(input.Thickness + input.Fade, input.Thickness, distance);

    // Exact float comparison against zero, rather than a threshold. It is
    // correct in practice because smoothstep returns exactly 0.0 outside
    // its range, so outside the ring the value is precisely zero. Inside,
    // smoothstep can return a value that rounds to zero very close to the
    // edge, which discards a sub-pixel sliver -- harmless, and much cheaper
    // than evaluating the colour path for it.
    if (circle == 0.0)
        // Kills the fragment. A discard still runs the shader for the pixel, so
        // this saves the colour write rather than the invocation.
        discard;

    // Set output color
    output.Color = input.Color;
    // Alpha is multiplied by the coverage rather than the colour being
    // premultiplied, so this composites as a transparent ring over
    // whatever the render target already holds.
    output.Color.a *= circle;

    // Reached only for surviving pixels, so the id buffer contains no entries
    // for discarded fragments. That is what makes the id buffer a usable
    // picking source: a background pixel simply was not written by this
    // shader at all.
    output.EntityID = input.EntityID;
    
    return output;
}
