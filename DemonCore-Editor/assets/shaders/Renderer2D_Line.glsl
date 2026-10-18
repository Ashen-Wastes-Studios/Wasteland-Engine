// Existing header. Note the "Line" in the name, and the body below:
// this shader draws filled quads with per-vertex colour, so nothing
// here actually rasterises GL_LINES. The line/quad naming belongs to
// the C++ side that picks the primitive.
// Renderer2D Line Shader (OpenGL & Vulkan Compatible)

// Not GLSL: a marker the shader loader scans to split this file into
// two stages. That is what lets one file serve OpenGL and Vulkan.
#type vertex
#version 450 core

// Fixed attribute slot 0. A vec3 because the batched 2D pipeline packs
// position with an implied w=1; z carries the draw order for 2D
// layering rather than a real depth.
layout(location = 0) in vec3 a_Position;
// Per-vertex colour at slot 1, interpolated across the primitive -- that
// is what gives a quad a gradient rather than one flat tint.
// Slots 0/1/2 are contiguous, which is what a tightly-packed
// interleaved vertex buffer on the CPU side would expect.
layout(location = 1) in vec4 a_Color;
// A per-vertex integer identifier, carried through to the fragment
// stage and written to the second colour attachment (line 26). This is
// a G-buffer / deferred-style write: the ID goes to an integer render
// target so a later pass can look up which entity owns these pixels
// (picking, deferred lighting).
// Being an int attribute, it must be declared with an appropriate
// integer vertex format on the GL side -- the name does not say which.
layout(location = 2) in int a_EntityID;

// Camera matrix. There is no per-object u_Transform here, unlike
// FlatColor.glsl: this shader is drawn as ONE batch, with per-object
// transform already baked into a_Position on the CPU. That is the whole
// point of the batched path.
uniform mat4 u_ViewProjection;

// Colour varying to the fragment stage. Smoothly interpolated unless
// marked flat -- here it is smooth, which is what produces the gradient
// across a quad.
layout(location = 0) out vec4 v_Color;
// DEFECT-adjacent (likely a bug): `flat` disables interpolation, so the
// fragment stage receives ONE provoking-vertex value for the whole
// primitive. That is the correct qualifier for an entity ID and almost
// certainly a deliberate choice.
// The inconsistency is on the fragment side: line 29 declares it flat
// too (matching), but line 26 writes it to `color2` at location 1 --
// see the note there.
layout(location = 1) out flat int v_EntityID;

// Vertex entry. Three assignments, all unconditional -- no branches,
// no discarding, so this is a cheap pass-through shader.
void main()
{
	// Straight copy. The interpolation is implicit in the varying; there
	// is no barycentric or perspective-correct division written here.
	v_Color = a_Color;
	// Copy to the varying. Because that varying is declared `flat` at
	// line 13, only the provoking vertex's value actually reaches the
	// fragment stage -- so writing it per-vertex is required by the GLSL
	// spec but only one of the values is ever used.
	v_EntityID = a_EntityID;
	// Only the camera matrix: the per-object transform was already applied
	// on the CPU when the batch was built.
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

// Colour attachment 0. Note the name `color` collides conceptually with
// the varying `v_Color` on line 28 -- different identifiers, easy to
// misread.
layout(location = 0) out vec4 color;
// DEFECT (naming, and a real portability problem): an integer colour
// attachment at location 1. `color2` is a placeholder name with no
// meaning, and worse, an int output is not guaranteed to be a supported
// render target format on every backend -- a plain RGBA8 framebuffer
// has nowhere to put it. This implies an integer or dual-source
// framebuffer must be bound; nothing in this file establishes that,
// and the file is the only place that would.
// It is also written unconditionally, so if the bound framebuffer has
// only one attachment this write goes nowhere (or is an error).
layout(location = 1) out int color2;

// Input varying, location-matched to line 12.
layout(location = 0) in vec4 v_Color;
// `flat` on the input matches the `flat` on the matching output, which
// is required by GLSL -- the qualifiers must agree or the link fails.
layout(location = 1) in flat int v_EntityID;

// DEFECT (dead declaration): a 32-element sampler array that main()
// never reads. The fragment stage writes v_Color straight through to
// color, so this uniform is entirely unused.
// Two further problems if it were used: (a) `binding = 0` is a Vulkan
// descriptor-set layout qualifier and is IGNORED by desktop GL, so the
// file is not really OpenGL-compatible despite the header comment;
// (b) sampling a sampler array element requires a dynamically uniform
// index, which this shader has no way to produce.
// The likely intent was a texture-atlas or bindless path that was
// dropped when the batcher stopped needing textures for lines.
layout(binding = 0) uniform sampler2D u_Texture[32];

// Fragment entry. Two writes, no texture fetch, no maths.
void main()
{
	// Straight copy of the interpolated colour. No blending is done in the
	// shader: alpha in v_Color is written to the target and the pipeline
	// blend state decides what happens with it.
	color = v_Color;
	// The ID write. Because the varying is flat, this is a constant write
	// across the primitive rather than a per-pixel one.
	// As noted on line 26, this output only has somewhere to go if an
	// integer-capable second attachment is bound.
	color2 = v_EntityID;
}
