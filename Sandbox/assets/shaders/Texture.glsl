// Sandbox Basic Texture Shader (OpenGL & Vulkan Compatible)
//
// The sandbox's own textured-quad program. It is close to but distinct
// from the engine's Renderer2D_Quad.glsl -- notably it has no entity-id
// output, so nothing drawn with it can be picked.
// Sandbox Basic Texture Shader (OpenGL & Vulkan Compatible)

// Loader directive. Both stages live in this one file, so it appears twice.
#type vertex
// GLSL 450. Declared per stage because the two are compiled separately.
#version 450 core

// Position in world space already, so the vertex stage needs no transform
// uniform beyond the camera.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
// Texture slot index, as a FLOAT rather than an int. It arrives as a float
// because the vertex buffer is interleaved with floats; the fragment stage
// casts it back. See the note there.
layout(location = 3) in float a_textureIndex;
// Tiling multiplier, multiplied into the texcoords in the fragment stage
// rather than baked into the vertices -- so the same buffer can be drawn at
// several tiling factors with no re-upload.
layout(location = 4) in float a_TilingFactor;

// Camera only. Unlike FlatColor.glsl there is no per-object transform here,
// so the sandbox layer must transform its vertices itself.
uniform mat4 u_ViewProjection;

// Texcoords and colour are interpolated; the slot index and tiling factor
// are passed through unchanged. None of these are marked flat, which is
// harmless only because all four corners of a quad carry the same value --
// a per-vertex value would be averaged and give nonsense.
layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;
layout(location = 2) out float v_textureIndex;
layout(location = 3) out float v_TilingFactor;

// Vertex entry point.
void main()
{
		// Straight pass-through. No gamma or colour-space work in either stage of
		// this shader.
	v_Color = a_Color;
	v_TexCoord = a_TexCoord;
	v_textureIndex = a_textureIndex;
	v_TilingFactor = a_TilingFactor;
		// World position straight through the camera matrix.
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

// Fragment stage.
#type fragment
#version 450 core

layout(location = 0) out vec4 color;

layout(location = 0) in vec4 v_Color;
layout(location = 1) in vec2 v_TexCoord;
layout(location = 2) in float v_textureIndex;
layout(location = 3) in float v_TilingFactor;

// A 32-element array of samplers in one uniform array. This is the whole
// reason the texture index is a float: an array of samplers cannot be
// indexed by anything that varies per vertex, so the index travels as
// interpolation data and is converted here.
//
// The 32 must match MaxTextureSlots in the renderer, which allocates and
// fills exactly this many. There is no bounds check at index time, so an
// out-of-range value -- including the default 0.0 for a quad whose buffer
// was not filled in -- is undefined behaviour rather than a clamp.
layout(binding = 0) uniform sampler2D u_Texture[32];

// Fragment entry point.
void main()
{
		// Sample, then modulate. The multiplication is the tint: because it is
		// applied per channel including alpha, a white tint leaves the texture
		// untouched and a black tint produces a silhouette.
	//
		// The int() cast truncates the interpolated float. It is only safe because
		// every vertex of the quad supplied the same integer-valued float; if two
		// vertices disagreed, the interpolated value could land anywhere and
		// truncate to a neighbouring slot or out of range entirely.
	//
		// No clamping of v_TilingFactor either. A negative factor mirrors the
		// texture (repeats are well-defined, so it does not produce garbage), and
		// a very large one simply aliases, since there is no mip selection here.
	color = texture(u_Texture[int(v_textureIndex)], v_TexCoord * v_TilingFactor) * v_Color;
}
