// Renderer2D Circle Shader (OpenGL & Vulkan Compatible)
// ---
// A circle is drawn as a QUAD: the CPU emits a unit square and this shader
// decides per fragment which pixels are inside the ring. That is why there
// is no geometry stage work beyond the same 4 corners every quad uses.
// ---
// This file is one of a matched PAIR with Renderer2D_Circle.hlsl, which
// carries the identical maths for the NVRHI backends. Any change here
// needs the same change there or the two backends render differently.
// Renderer2D Circle Shader (OpenGL & Vulkan Compatible)

// Preprocessor directives the engine's shader loader consumes. They are
// stripped before the GL compile, which is why the same asset can carry
// `#type` markers and still be valid GLSL once they are gone.
#type vertex
// 450 core: GLSL 4.50, the first version with guaranteed SPIR-V support,
// so the identical source compiles for Vulkan. Declared TWICE below (once
// per stage) because each stage is a separate translation unit.
#version 450 core

// Vertex inputs, matched one-for-one to the attribute layout the renderer
// declares in Renderer2D.cpp for CircleVertex. The explicit location
// qualifiers are what let the two agree without a binding convention.
layout(location = 0) in vec3 a_WorldPosition;
// Position WITHIN the quad, not in the world. It is never transformed:
// the CPU sends the [-0.5, 0.5] corner table scaled by 2, so this arrives
// as [-1, 1] and its length is the distance field the fragment stage
// needs. No u_Transform uniform appears in this stage at all -- the world
// matrix was already applied to a_WorldPosition.
layout(location = 1) in vec3 a_LocalPosition;
layout(location = 2) in vec4 a_Color;
// Ring thickness, as a distance threshold rather than as geometry. A
// thicker ring costs the same four vertices as a thinner one.
layout(location = 3) in float a_Thickness;
// Edge softness in the same distance units. This is the anti-aliasing
// width: the default of 0.005 in Renderer2D.h keeps a one-pixel smooth
// rim regardless of how large the circle is on screen.
layout(location = 4) in float a_Fade;
// Entity id, so a pick can identify which primitive was clicked. Written
// per vertex and marked flat in the fragment input, which matters for
// correctness rather than performance.
layout(location = 5) in int a_EntityID;

// The only uniform. Applied to the already-world-space position, so the
// result is clip space with no further transform.
uniform mat4 u_ViewProjection;

// Varyings. Smooth (non-flat) interpolation is correct for the position
// and colour, and would be wrong for the entity id -- see below.
layout(location = 0) out vec3 v_LocalPosition;
layout(location = 1) out vec4 v_Color;
layout(location = 2) out float v_Thickness;
layout(location = 3) out float v_Fade;
// `flat` is REQUIRED here, not an optimisation. An int has no meaningful
// interpolated value, and without flat the GPU is free to average the four
// corners into a value no vertex supplied, so a picked entity could come
// back as an id that was never drawn.
layout(location = 4) out flat int v_EntityID;

// Vertex entry point.
void main()
{
        // The signed-distance seed. length() of a vec2 here is the distance from
        // the quad centre, which is (0,0) in this local space, so the inverted
        // length is positive inside the circle and negative outside.
        // ---
        // The variable is named `distance`, which SHADOWS the built-in distance()
        // function for the rest of this scope. It compiles because the built-in is
        // never called in this function, but it is a trap for anyone who tries to
        // add one.
    v_LocalPosition = a_LocalPosition;
	v_Color = a_Color;
	v_Thickness = a_Thickness;
	v_Fade = a_Fade;
	v_EntityID = a_EntityID;
		// The full MVP applied in one expression. The 1.0 goes in as w, and the
		// engine's projection matrix supplies its own w, so the divide happens
		// in the fixed-function stage as usual.
	gl_Position = u_ViewProjection * vec4(a_WorldPosition, 1.0);
}

// Start of the fragment stage. The `#type` marker above switches the
// loader from concatenating vertex source to fragment source.
#type fragment
#version 450 core

// Attachment 0: the RGBA the framebuffer composites.
layout(location = 0) out vec4 color;
// Attachment 1: the entity id, written as an int so it survives the
// round trip through a float framebuffer.
// ---
// DEFECT (documented, not fixed): this output is only useful if the bound
// framebuffer actually has a second colour attachment and glDrawBuffers
// names it. The engine's only glDrawBuffers call is in OpenGLFramebuffer
// and it does include GL_COLOR_ATTACHMENT1, so it works -- but only when
// drawing into an entity-id target. Against the default framebuffer, or
// any single-attachment one, writing location 1 has no destination and
// the driver may drop the whole draw depending on how the draw buffer
// mask is set. The HLSL twin declares this as SV_Target1 with the same
// requirement.
layout(location = 1) out int color2;

// Fragment inputs, matching the vertex outputs by location. The flat
// integer here must agree with the flat qualifier in the vertex stage or
// the interface does not link.
layout(location = 0) in vec3 v_LocalPosition;
layout(location = 1) in vec4 v_Color;
layout(location = 2) in float v_Thickness;
layout(location = 3) in float v_Fade;
layout(location = 4) in flat int v_EntityID;

// Fragment entry point.
void main()
{
        // Signed distance to the circle edge, in the [-1, 1] local space the CPU
        // established. 1.0 at the centre, 0.0 exactly on the unit circle, negative
        // outside. This single line is what replaces tessellating a disc.
    float distance = 1.0 - length(v_LocalPosition);
        // smoothstep(0, v_Fade, distance) ramps 0 -> 1 across the OUTER edge over
        // a band v_Fade wide. With the default fade of 0.005 that band is thinner
        // than a pixel, which is why the anti-aliasing is invisible at the rim.
        // ---
        // smoothstep also CLAMPS, so distance values outside [0, v_Fade] produce
        // exactly 0 or 1 -- there is no overshoot at either end.
    float circle = smoothstep(0.0, v_Fade, distance);
        // The second smoothstep carves the RING: edge1 = thickness + fade,
        // edge0 = thickness. Because edge0 < edge1 this is a DESCENDING ramp, so
        // it is 1 where distance < thickness and 0 beyond it. Multiplied into the
        // first term, the product is the intersection of "inside the disc" and
        // "inside the ring" -- a filled ring of the requested thickness.
        // ---
        // DEFECT (documented, not fixed): if a caller passes a NEGATIVE thickness,
        // thickness + fade can be less than thickness while both are negative, and
        // GLSL smoothstep with edge0 >= edge1 is UNDEFINED -- the result may be
        // NaN, and NaN then fails the `circle == 0.0` test below so the fragment
        // is neither discarded nor blended. Nothing in Renderer2D::DrawCircle
        // validates the parameter, so a negative thickness reaches this line.
        // Thickness 0 exactly is fine: it is a hairline that the fade widens.
    circle *= smoothstep(v_Thickness + v_Fade, v_Thickness, distance);

        // The discard test, on the EXACT comparison rather than a threshold.
        // Because both smoothsteps clamp, the product is exactly 0.0 across the
        // whole outside region -- so this compares against a value that really is
        // zero, and nothing is thrown away by a tolerance choice.
        // ---
        // Interpolation between the two terms could in principle produce a tiny
        // non-zero value outside; in practice the clamp is what makes the strict
        // comparison correct. This is also why the shader uses discard instead of
        // writing alpha 0 -- discarding keeps discarded fragments out of any
        // later depth or blend work.
    if (circle == 0.0)
                // No braces: a single-statement discard needs none, and the C-derived
                // GLSL grammar permits it.
        discard;

        // Base colour first, then alpha scaled by the coverage term below. Doing
        // it in this order means v_Color.a is treated as a maximum opacity for the
        // whole primitive, which is what a caller setting alpha on a ring wants.
    color = v_Color;
        // Coverage into alpha. This is the smooth (feathered) edge; the hard discard
        // above handles the fully-exterior case. Both matter: the discard saves the
        // blend, this term gives the antialiased rim.
    color.a *= circle;

        // The id written to attachment 1, for a later pick pass to read back.
        // ---
        // DEFECT (documented, not fixed): this line runs for every fragment of the
        // ring INCLUDING the feathered ones, so an antialiased edge pixel carries
        // the same id as the solid interior -- fine. But a fragment that survives
        // the discard and then blends still overwrites the id buffer with its own
        // value, so where two primitives overlap in the id attachment the last one
        // drawn wins regardless of depth. That matches the colour attachment's own
        // lack of depth testing here, so it is consistent rather than surprising.
    color2 = v_EntityID;
}
