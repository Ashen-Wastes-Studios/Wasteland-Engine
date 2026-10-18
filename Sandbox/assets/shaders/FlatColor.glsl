// Sandbox Flat Color Shader (OpenGL & Vulkan Compatible)

// "#type" is NOT GLSL: it is a marker the project shader loader scans to
// split this file into a vertex-stage source and a fragment-stage source.
// The same file text can therefore feed both OpenGL and Vulkan.
#type vertex
// GLSL 4.50 core = the OpenGL 4.6 / Vulkan feature level. "core" rejects
// the deprecated compatibility profile, so no gl_FragColor-style
// builtins are available; everything is done via explicit in/out.
#version 450 core

// Fixed vertex-attribute slot 0. The location must match the layout used
// when the VAO/descriptor was set up, otherwise the shader reads
// whatever attribute happens to live there.
// vec3 (not vec4): this stage never needs w, the 1.0 is supplied below.
layout(location = 0) in vec3 a_Position;

// Camera matrix supplied by the host each draw -- shared by every batch
// that uses this shader, so it is set once per frame, not per quad.
uniform mat4 u_ViewProjection;
// Per-object matrix (model). Because it is a uniform rather than an
// instanced input, one draw call = one transform: batching N sprites
// with different transforms would need N draws or a rewrite.
uniform mat4 u_Transform;

// Vertex entry point. No varyings are declared, so nothing is passed on
// to the fragment stage beyond what uniforms already carry.
void main()
{
	// Multiply right-to-left: a_Position is put into object space by
	// u_Transform, then the combined result is placed in clip space by
	// u_ViewProjection. The trailing 1.0 promotes vec3 -> vec4 with w=1,
	// which is the "point", not a direction.
	gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
}

// Second stage begins. The loader emits this half as a separate shader
// module sharing the same vertex-side uniform block.
#type fragment
// Repeated per stage: a single GLSL source cannot contain two #version
// directives, so the preprocessor line has to be re-declared here.
#version 450 core

// Location 0 is the first colour attachment of the framebuffer.
// An explicit location is mandatory in core profile, and it is what
// makes the stage portable to Vulkan-style output binding.
layout(location = 0) out vec4 color;

// Flat tint: every fragment of every triangle this draw emits takes this
// exact colour, which is the whole point of a "FlatColor" shader -- no
// texture sampling, no lighting, no interpolation.
uniform vec4 u_Color;

// Fragment entry point, second definition of the name in this file.
// That is legal only because the loader splits the file before the
// compiler ever sees both halves at once.
void main()
{
	// Straight assignment: no alpha blending, gamma conversion or sRGB
	// encode happens here. Whatever blending state the host pipeline object
	// sets is the only thing acting on this value.
	color = u_Color;
}
