// Renderer2D_Quad.glsl -- the GLSL source for Renderer2D's batched quad pass.
//
// Compiled by the OpenGL backend (OpenGLShader::PreProcess splits it on the
// `#type` directives) and, on Vulkan, by NVRHIShader. On DX11/DX12 this file
// is never parsed: NVRHIShader's constructor swaps a .glsl path for the
// same-named .hlsl beside it, and Renderer2D_Quad.hlsl is what D3DCompile
// actually sees. The two must stay behaviourally identical -- same attribute
// names, same output slots, same texture-slot arithmetic -- or a quad renders
// differently depending on which backend is active.
//
// NOT a standalone .glsl: `#type` markers are consumed by the engine's
// pre-processor and never reach the compiler. The `#version` line must sit
// inside its stage body, because the splitter skips blank lines between a
// `#type` and its body but GLSL still requires `#version` to precede
// everything except comments and whitespace.
// Renderer2D Quad Shader (OpenGL & Vulkan Compatible)

// Stage marker, consumed by PreProcess and NOT passed to the compiler.
// The name must be exactly "vertex"/"pixel" (lowercase, one space after the
// token): the splitter takes the text up to the line end as the map key and the
// constructor looks up those literals. A tab or extra space after `#type` puts
// the whitespace into the key and the lookup misses.
//
// Everything between here and the next `#type` is the vertex stage source.
#type vertex
// GLSL 450 = Vulkan-era GLSL, which is also what desktop GL 4.5 exposes. The
// `core` profile removes the compatibility built-ins; nothing in this shader
// needs them.
//
// 4.5 is the floor the OpenGL context asserts at startup
// (OpenGLContext::Init checks GL_MAJOR/MINOR_VERSION), so this directive and
// that assert are two halves of the same requirement.
//
// Explicit locations everywhere below are what allow this to be one source for
// both GL and Vulkan: there is no glBindAttribLocation step, so the vertex
// layout baked into Renderer2D's SetLayout call must agree with these numbers.
#version 450 core

// Vertex attribute 0. Matches the first entry of Renderer2D's
// QuadVertexBuffer->SetLayout list. The a_ prefix and this exact name are
// load-bearing: the OpenGL path binds attributes by name, so renaming one
// without updating SetLayout makes it silently read as 0 -- geometry collapses
// to the origin rather than failing to compile.
//
// vec3, not vec4: the w component is supplied by the literal 1.0 in main(),
// so positions are implicitly treated as having homogeneous coordinate 1.
layout(location = 0) in vec3 a_Position;
// Attribute 1, the per-vertex tint. Multiplied into the sampled texture in the
// fragment stage, which is how Renderer2D tints a white-textured quad without
// touching the texture itself.
layout(location = 1) in vec4 a_Color;
// Attribute 2, UVs. Scaled by a_TilingFactor in the fragment stage, so tiling
// is a per-vertex value rather than a sampler state -- the white default is
// tiling factor 1.
layout(location = 2) in vec2 a_TexCoord;
// Attribute 3: WHICH texture slot, as a float because that is the
// ShaderDataType::Float the CPU-side QuadVertex declares.
//
// A float carrying an integer index is odd but necessary -- it keeps the
// vertex struct's alignment simple across backends. The fragment stage casts it
// back with int() before indexing the sampler array.
//
// Slot 0 is reserved for the engine's 1x1 white texture, which is what an
// untextured quad samples and what makes TexIndex 0 unambiguous.
layout(location = 3) in float a_TexIndex;
// Attribute 4, the UV multiplier. 1.0 = no tiling; larger values repeat the
// texture across the quad.
layout(location = 4) in float a_TilingFactor;
// Attribute 5, the editor-picking id. Declared LAST in both the GLSL and the
// CPU-side QuadVertex struct so it occupies the final 4 bytes of each vertex.
//
// int rather than float because it carries a handle, not a coordinate.
// Renderer2D writes -1 for quads that are not pickable, which is also the value
// ReadPixel's -1 failure return uses -- so a failed pick and an unpickable quad
// are indistinguishable to the caller (harmless, since both mean "no entity").
layout(location = 5) in int a_EntityID;

// The only uniform the vertex stage needs: the active camera's matrix.
// Uploaded per frame by Renderer2D; there is no per-object matrix here because
// the CPU has already baked each quad's transform into a_Position.
//
// Column-major, matching glm's default and GLSL's expectation -- no transpose
// flag is needed as long as the uniform is uploaded as a mat4.
uniform mat4 u_ViewProjection;

// Vertex outputs. Five of them for six inputs: position is consumed by
// gl_Position and never interpolated.
//
// These locations must match the `in` declarations in the fragment stage
// below -- GLSL links by location, not by name.
layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;
layout(location = 2) out float v_TexIndex;
layout(location = 3) out float v_TilingFactor;
// `flat` is mandatory on an int varying: integers cannot be interpolated, and
// GLSL requires the qualifier. The provoking vertex is the FIRST vertex of the
// triangle in OpenGL, so the whole primitive reports the entity id of vertex
// 0 -- which is why Renderer2D writes the same id to all four quad vertices.
//
// The HLSL twin spells this `nointerpolation`, the D3D equivalent.
layout(location = 4) out flat int v_EntityID;

// Entry point. Note there is no separate position output assignment until the
// last line: every other varying is a straight copy of its attribute, so the
// only real work in this stage is the matrix multiply.
void main()
{
	// Straight pass-through of the per-vertex tint. Copied before the position
	// multiply purely for readability; order among these writes does not matter.
	v_Color = a_Color;
	v_TexCoord = a_TexCoord;
	v_TexIndex = a_TexIndex;
	// Also a pass-through. Carried all the way to the fragment stage so tiling is
	// interpolated across the quad, which is what keeps the UV scale continuous
	// when a batched quad is stretched.
	v_TilingFactor = a_TilingFactor;
	v_EntityID = a_EntityID;
	// The whole point of the vertex stage. a_Position is vec3, so it is widened
	// with an explicit w of 1.0 -- the CPU-side QuadVertex likewise carries a w
	// component and Renderer2D's positions are written with 1.0 in that slot.
	//
	// Order matters: view-projection on the LEFT means the vertex is in world
	// space and the matrix takes it to clip space. Writing it the other way round
	// would be equally valid arithmetic but not what the uniform's name implies.
	//
	// No perspective divide is performed here -- the rasteriser does that, along
	// with the near/far clipping this shader never mentions.
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

// Second stage. Spelling matters, and it differs by backend:
//   * OpenGLShader::ShaderTypeFromString accepts BOTH "fragment" and
//     "pixel" as aliases, so either works under OpenGL.
//   * NVRHIShader::PreProcess keys its map by the literal text after
//     `#type`, and the NVRHIShader constructor then looks up exactly
//     "vertex", "pixel" and "compute".
// So `pixel` is the name that works everywhere.
//
// DEFECT: this file says `fragment`, and that is correct for OpenGL and for
// DX11/DX12 (where NVRHIShader swaps a .glsl for the same-named .hlsl before
// reading, so this text is never parsed). It is WRONG on NVRHI_Vulkan: that
// substitution is explicitly gated to non-Vulkan because the .glsl is the real
// source there, so this file IS read, PreProcess produces a "fragment" key,
// the find("pixel") test fails, and the constructor logs "does not contain
// valid shader types". Renderer2D.cpp:342 loads this exact path
// unconditionally, so a Vulkan run fails to create the quad shader.
//
// `#version` is repeated because the splitter hands each stage body to the
// compiler separately; a continuation from the vertex stage would not parse.
#type fragment
#version 450 core

// Two render targets. Slot 0 is the colour the user sees; slot 1 is the integer
// entity id, which is what the editor's picking pass reads back through
// Framebuffer::ReadPixel.
//
// Writing a value to BOTH is what forces the editor's framebuffer to have two
// colour attachments (RGBA8 then RED_INTEGER) -- slot numbering follows
// declaration order in this shader and the attachment order in the spec, so
// these two lines and the editor's FramebufferSpecification must agree.
layout(location = 0) out vec4 color;
// Named color2 because `color` is taken. Deliberately never blended or
// post-processed: an integer attachment must hold exact values, so any
// smoothing or sRGB conversion would corrupt the id.
//
// Always written, unconditionally. A fragment that skipped this would leave
// undefined data in the target and ReadPixel could return a stale entity id.
layout(location = 1) out int color2;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;
layout(location = 2) in float v_TexIndex;
layout(location = 3) in float v_TilingFactor;
layout(location = 4) in flat int v_EntityID;

// Thirty-two texture units for this batch. The slot varies between primitives
// in a draw, so indexing this sampler array directly from v_TexIndex is not
// portable GLSL (opaque-array indices must be dynamically uniform). The
// literal-index switch below selects each sampler legally and matches the
// HLSL twin's fixed-slot sampling path.
layout(binding = 0) uniform sampler2D u_Texture[32];

vec4 wl_sample_quad_texture(int slot, vec2 uv)
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

// Fragment entry point. One statement of real work: sample, tint, and emit the
// id alongside.
void main()
{
	// The literal-index switch avoids non-uniform sampler-array indexing. LOD 0
	// matches the HLSL sprite path, which also samples the authored base level.
	//
	// v_TexCoord * v_TilingFactor: tiling is a UV multiply, so it costs nothing
	// and needs no sampler wrap-mode change (which this header cannot express --
	// see the TODO on FramebufferTextureSpecification).
	//
	// * v_Color: per-vertex tint in straight alpha. The blend state set in
	// OpenGLRendererAPI::Init (GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA) then
	// composites the result, so the shader itself does no blending.
	//
	color = wl_sample_quad_texture(int(v_TexIndex), v_TexCoord * v_TilingFactor) * v_Color;
	// The picking pass. `flat` on the varying means this is the provoking vertex's
	// id verbatim, not an average -- averaging two entity ids would produce an id
	// belonging to neither.
	//
	// Written to a separate integer attachment so it survives the blend that the
	// colour output goes through, and so the readback can memcpy an int directly
	// (NVRHI's R32_SINT path) rather than reassembling bytes.
	color2 = v_EntityID;
}
