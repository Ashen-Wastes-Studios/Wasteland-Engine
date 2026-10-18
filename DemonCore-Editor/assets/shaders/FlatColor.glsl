// Flat Color Shader (OpenGL & Vulkan Compatible)
//
// The simplest shader in the project: one constant colour per draw. It is
// what lets a caller set a vertex buffer's colour once and draw every quad
// in a batch from the same program.
//
// Paired with FlatColor.hlsl for the NVRHI backends; the two must stay in
// step, as with the other Renderer2D shaders.
// Flat Color Shader (OpenGL & Vulkan Compatible)

// Loader directive, stripped before compilation. It is what lets one file
// carry two `#version` directives without them conflicting.
#type vertex
// GLSL 450 for the Vulkan path, as with every other shader here.
#version 450 core

// Position only -- the vertex buffer for this shader needs no colour, no
// texcoords and no entity id, so it is a cheaper layout than the quad one.
layout(location = 0) in vec3 a_Position;

// The view-projection the renderer binds before the draw.
uniform mat4 u_ViewProjection;
// The per-object transform, bound separately from the camera. Keeping them
// as two uniforms rather than multiplying on the CPU is what lets one
// shader serve a whole batch of differently-transformed quads.
uniform mat4 u_Transform;

// Vertex entry point.
void main()
{
		// ViewProjection * Transform * position, in that order, and the order is
		// load-bearing: matrices applied right-to-left to a column vector mean the
		// object transform lands first and the camera second. Written the other way
		// round it would still compile and would transform the camera by the object
		// instead, which fails in a way that looks like a bad camera rather than a
		// bad multiply order.
	gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
}

// Fragment stage. No varyings cross the boundary at all: every input is a
// uniform, which is why there is no interpolation setup here.
#type fragment
#version 450 core

layout(location = 0) out vec4 color;

// The single constant colour. Bound per draw call, so changing it is a
// uniform upload rather than a vertex buffer rewrite.
uniform vec4 u_Color;

// Fragment entry point.
void main()
{
		// One assignment, no maths. This is the whole shader: alpha comes from
		// u_Color.a, which the caller sets directly, so there is no clamping or
		// premultiplication performed here -- whatever the caller writes is what
		// reaches the framebuffer.
	//
		// That also means this shader offers no sRGB/linear conversion. Whatever
		// space the caller's colour is authored in goes straight to attachment 0.
	color = u_Color;
}
