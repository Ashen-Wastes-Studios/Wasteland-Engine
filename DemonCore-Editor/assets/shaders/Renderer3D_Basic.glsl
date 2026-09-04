// Renderer3D Unified Basic Shader with Global Illumination (OpenGL & Vulkan Compatible)
// Handles textures, coloring, custom tiling, normals, ambient occlusion, and global illumination.

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

// Neural Rendering (OpenGL): tiny in-shader MLP for texture detail + learned ambient.
// Driven by Renderer3D neural uniforms; defaults off (0) until Flush() uploads them.
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;
uniform float u_NeuralLightStrength;
// Neural material blend. Defaults to 0 (classic response) until Renderer3D uploads it.
uniform float u_NeuralMatStrength;

float wl_softsign(float x) { return x / (1.0 + abs(x)); }

float wl_hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 4-feature sinusoidal encoding -> 4-unit hidden layer -> detail multiplier + tint.
// Output ~= 1.0 (neutral); keeps energy stable when strength is 0.
vec3 wl_neural_tex_detail(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    float grain = wl_hash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    vec3 tint = vec3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// Learned hemisphere ambient: sky gradient + albedo-tinted bounce lobe.
vec3 wl_neural_ambient(vec3 n, vec3 albedo)
{
    float up = n.y * 0.5 + 0.5;
    vec3 skyRef = mix(vec3(0.32, 0.30, 0.34), vec3(0.55, 0.62, 0.78), up);
    float h = wl_softsign(dot(n.xz, vec2(0.8, -0.6)) * 1.5 + n.y * 1.2);
    vec3 bounce = albedo * (0.35 + 0.25 * h) * skyRef;
    return skyRef * 0.35 + bounce;
}

// Neural material (raster): same learned PBR idea as the ray-trace path, minus
// world position — porosity/patina modulate albedo, the MLP gates sparse
// metallic-flake glints, and wrap width softens the diffuse response.
void wl_neural_mat_raster(vec2 uv, vec3 n, inout vec3 albedo, out float glint, out float wrapW)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    vec4 feat = vec4(sin(s1 + n.x * 2.0),
                     sin(s1 * 1.6 + n.y * 1.7),
                     sin(s1 * 0.7 + n.z * 2.3),
                     sin((s1 * 0.6 + n.x - n.y) * 1.3));
    float h0 = wl_softsign( 0.8 * feat.x - 0.5 * feat.y + 0.6 * feat.z + 0.2 * feat.w + 0.05);
    float h1 = wl_softsign(-0.4 * feat.x + 0.9 * feat.y - 0.2 * feat.z + 0.5 * feat.w - 0.10);
    float h2 = wl_softsign( 0.3 * feat.x + 0.2 * feat.y + 0.7 * feat.z - 0.6 * feat.w + 0.35);
    float h3 = wl_softsign(-0.5 * feat.x - 0.4 * feat.y + 0.4 * feat.z + 0.8 * feat.w + 0.00);
    float por = clamp(-h0, 0.0, 1.0);
    albedo *= (1.0 - 0.25 * por);
    albedo *= (vec3(1.0) + vec3(-0.06, 0.02, 0.05) * h1);
    float flakeCell = step(0.75, wl_hash12(floor(uv * 48.0)));
    glint = flakeCell * clamp(h2 * 1.5, 0.0, 1.0);
    wrapW = 0.35 + 0.65 * clamp(h3 * 0.5 + 0.5, 0.0, 1.0);
}

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

    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4));
    vec3 norm = normalize(Input.Normal);

    float diffuseIntensity = max(dot(norm, lightDir), 0.0);
    float ambientIntensity = 0.2;

    if (u_NeuralEnabled == 1)
    {
        vec3 detail = wl_neural_tex_detail(Input.TexCoord * Input.TilingFactor, norm);
        texColor.rgb *= mix(vec3(1.0), detail, clamp(u_NeuralTexStrength, 0.0, 1.0));

        vec3 namb = wl_neural_ambient(norm, texColor.rgb);
        float k = clamp(u_NeuralLightStrength, 0.0, 1.0);
        float km = clamp(u_NeuralMatStrength, 0.0, 1.0);
        // Neural material: porosity/patina albedo, wrap diffuse, flake glints
        float matGlint = 0.0;
        float matWrap = 0.5;
        vec3 matAlbedo = texColor.rgb;
        {
            float g;
            float w;
            wl_neural_mat_raster(Input.TexCoord * Input.TilingFactor, norm, matAlbedo, g, w);
            matGlint = g;
            matWrap = w;
        }
        texColor.rgb = mix(texColor.rgb, matAlbedo, km);
        float wrapDiffuse = clamp((dot(norm, lightDir) + matWrap * km) / (1.0 + matWrap * km), 0.0, 1.0);
        float diffTerm = mix(diffuseIntensity, wrapDiffuse, km * 0.7);
        vec3 warmLight = vec3(1.0, 0.98, 0.94);
        vec3 lit = texColor.rgb * (diffTerm * warmLight + mix(vec3(ambientIntensity), namb * 2.2, k));
        lit += matGlint * km * diffuseIntensity * vec3(1.3, 1.25, 1.15);
        color = vec4(lit, texColor.a);
        color2 = v_EntityID;
        return;
    }

    float lightFactor = ambientIntensity + diffuseIntensity;

    color = vec4(texColor.rgb * lightFactor, texColor.a);
    color2 = v_EntityID;
}
