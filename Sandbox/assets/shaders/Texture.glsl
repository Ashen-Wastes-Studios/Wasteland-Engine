// Sandbox Basic Texture Shader (OpenGL & Vulkan Compatible)

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in float a_textureIndex;
layout(location = 4) in float a_TilingFactor;

uniform mat4 u_ViewProjection;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;
layout(location = 2) out float v_textureIndex;
layout(location = 3) out float v_TilingFactor;

void main()
{
	v_Color = a_Color;
	v_TexCoord = a_TexCoord;
	v_textureIndex = a_textureIndex;
	v_TilingFactor = a_TilingFactor;
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 color;

layout(location = 0) in vec4 v_Color;
layout(location = 1) in vec2 v_TexCoord;
layout(location = 2) in float v_textureIndex;
layout(location = 3) in float v_TilingFactor;

layout(binding = 0) uniform sampler2D u_Texture[32];

void main()
{
	color = texture(u_Texture[int(v_textureIndex)], v_TexCoord * v_TilingFactor) * v_Color;
}
