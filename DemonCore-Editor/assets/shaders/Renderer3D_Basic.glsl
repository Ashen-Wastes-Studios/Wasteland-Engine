// Renderer3D Unified Basic Shader
// Handles textures, coloring, custom tiling, normals, and hardware selection passes.

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec3 a_Normal;
layout(location = 3) in vec2 a_TexCoord;
layout(location = 4) in float a_TexIndex;
layout(location = 5) in float a_TilingFactor;
layout(location = 6) in int a_EntityID;

uniform mat4 u_ViewProjection;

struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
};

layout (location = 0) out VertexOutput Output;
layout (location = 5) out flat int v_EntityID;

void main()
{
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    v_EntityID = a_EntityID;

    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 color;
layout(location = 1) out int color2; // Entity ID attachment texture slot

struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
};

layout (location = 0) in VertexOutput Input;
layout (location = 5) in flat int v_EntityID;

layout (binding = 0) uniform sampler2D u_Textures[32];

void main()
{
    vec4 texColor = Input.Color;

    // Evaluate hardware texture slots using an integer cast selector
    switch(int(Input.TexIndex))
    {
        case 0:  texColor *= texture(u_Textures[0],  Input.TexCoord * Input.TilingFactor); break;
        case 1:  texColor *= texture(u_Textures[1],  Input.TexCoord * Input.TilingFactor); break;
        case 2:  texColor *= texture(u_Textures[2],  Input.TexCoord * Input.TilingFactor); break;
        case 3:  texColor *= texture(u_Textures[3],  Input.TexCoord * Input.TilingFactor); break;
        case 4:  texColor *= texture(u_Textures[4],  Input.TexCoord * Input.TilingFactor); break;
        case 5:  texColor *= texture(u_Textures[5],  Input.TexCoord * Input.TilingFactor); break;
        case 6:  texColor *= texture(u_Textures[6],  Input.TexCoord * Input.TilingFactor); break;
        case 7:  texColor *= texture(u_Textures[7],  Input.TexCoord * Input.TilingFactor); break;
        case 8:  texColor *= texture(u_Textures[8],  Input.TexCoord * Input.TilingFactor); break;
        case 9:  texColor *= texture(u_Textures[9],  Input.TexCoord * Input.TilingFactor); break;
        case 10: texColor *= texture(u_Textures[10], Input.TexCoord * Input.TilingFactor); break;
        case 11: texColor *= texture(u_Textures[11], Input.TexCoord * Input.TilingFactor); break;
        case 12: texColor *= texture(u_Textures[12], Input.TexCoord * Input.TilingFactor); break;
        case 13: texColor *= texture(u_Textures[13], Input.TexCoord * Input.TilingFactor); break;
        case 14: texColor *= texture(u_Textures[14], Input.TexCoord * Input.TilingFactor); break;
        case 15: texColor *= texture(u_Textures[15], Input.TexCoord * Input.TilingFactor); break;
        case 16: texColor *= texture(u_Textures[16], Input.TexCoord * Input.TilingFactor); break;
        case 17: texColor *= texture(u_Textures[17], Input.TexCoord * Input.TilingFactor); break;
        case 18: texColor *= texture(u_Textures[18], Input.TexCoord * Input.TilingFactor); break;
        case 19: texColor *= texture(u_Textures[19], Input.TexCoord * Input.TilingFactor); break;
        case 20: texColor *= texture(u_Textures[20], Input.TexCoord * Input.TilingFactor); break;
        case 21: texColor *= texture(u_Textures[21], Input.TexCoord * Input.TilingFactor); break;
        case 22: texColor *= texture(u_Textures[22], Input.TexCoord * Input.TilingFactor); break;
        case 23: texColor *= texture(u_Textures[23], Input.TexCoord * Input.TilingFactor); break;
        case 24: texColor *= texture(u_Textures[24], Input.TexCoord * Input.TilingFactor); break;
        case 25: texColor *= texture(u_Textures[25], Input.TexCoord * Input.TilingFactor); break;
        case 26: texColor *= texture(u_Textures[26], Input.TexCoord * Input.TilingFactor); break;
        case 27: texColor *= texture(u_Textures[27], Input.TexCoord * Input.TilingFactor); break;
        case 28: texColor *= texture(u_Textures[28], Input.TexCoord * Input.TilingFactor); break;
        case 29: texColor *= texture(u_Textures[29], Input.TexCoord * Input.TilingFactor); break;
        case 30: texColor *= texture(u_Textures[30], Input.TexCoord * Input.TilingFactor); break;
        case 31: texColor *= texture(u_Textures[31], Input.TexCoord * Input.TilingFactor); break;
    }

    // Simple default light calculation setup (directional downward-angled light source)
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4)); 
    vec3 norm = normalize(Input.Normal);
    
    // Combine diffuse lighting element with ambient base factor
    float diffuseIntensity = max(dot(norm, lightDir), 0.0f);
    float ambientIntensity = 0.2f;
    float lightFactor = ambientIntensity + diffuseIntensity;

    color = vec4(texColor.rgb * lightFactor, texColor.a);
    color2 = v_EntityID;
}