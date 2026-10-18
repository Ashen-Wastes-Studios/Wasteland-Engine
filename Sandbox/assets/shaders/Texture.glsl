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

// The 32 texture units match the sandbox batcher. A per-primitive slot is not
// dynamically uniform across a draw, so use literal sampler indices in the
// switch rather than indexing this opaque array directly.
layout(binding = 0) uniform sampler2D u_Texture[32];

vec4 wl_sample_sandbox_texture(int slot, vec2 uv)
{
	switch (slot)
	{
		case 0:  return textureLod(u_Texture[0], uv, 0.0);
		case 1:  return textureLod(u_Texture[1], uv, 0.0);
		case 2:  return textureLod(u_Texture[2], uv, 0.0);
		case 3:  return textureLod(u_Texture[3], uv, 0.0);
		case 4:  return textureLod(u_Texture[4], uv, 0.0);
		case 5:  return textureLod(u_Texture[5], uv, 0.0);
		case 6:  return textureLod(u_Texture[6], uv, 0.0);
		case 7:  return textureLod(u_Texture[7], uv, 0.0);
		case 8:  return textureLod(u_Texture[8], uv, 0.0);
		case 9:  return textureLod(u_Texture[9], uv, 0.0);
		case 10: return textureLod(u_Texture[10], uv, 0.0);
		case 11: return textureLod(u_Texture[11], uv, 0.0);
		case 12: return textureLod(u_Texture[12], uv, 0.0);
		case 13: return textureLod(u_Texture[13], uv, 0.0);
		case 14: return textureLod(u_Texture[14], uv, 0.0);
		case 15: return textureLod(u_Texture[15], uv, 0.0);
		case 16: return textureLod(u_Texture[16], uv, 0.0);
		case 17: return textureLod(u_Texture[17], uv, 0.0);
		case 18: return textureLod(u_Texture[18], uv, 0.0);
		case 19: return textureLod(u_Texture[19], uv, 0.0);
		case 20: return textureLod(u_Texture[20], uv, 0.0);
		case 21: return textureLod(u_Texture[21], uv, 0.0);
		case 22: return textureLod(u_Texture[22], uv, 0.0);
		case 23: return textureLod(u_Texture[23], uv, 0.0);
		case 24: return textureLod(u_Texture[24], uv, 0.0);
		case 25: return textureLod(u_Texture[25], uv, 0.0);
		case 26: return textureLod(u_Texture[26], uv, 0.0);
		case 27: return textureLod(u_Texture[27], uv, 0.0);
		case 28: return textureLod(u_Texture[28], uv, 0.0);
		case 29: return textureLod(u_Texture[29], uv, 0.0);
		case 30: return textureLod(u_Texture[30], uv, 0.0);
		case 31: return textureLod(u_Texture[31], uv, 0.0);
		default: return vec4(1.0);
	}
}

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
	color = wl_sample_sandbox_texture(int(v_textureIndex), v_TexCoord * v_TilingFactor) * v_Color;
}
