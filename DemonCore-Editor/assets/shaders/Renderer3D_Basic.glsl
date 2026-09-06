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
layout(location = 7) in float a_Metallic;
layout(location = 8) in float a_Roughness;
layout(location = 9) in float a_Displacement;
layout(location = 10) in float a_BumpStrength;

uniform mat4 u_ViewProjection;

struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
    vec3 WorldPos;
    float Metallic;
    float Roughness;
    float Displacement;
    float BumpStrength;
};

layout (location = 0) out VertexOutput Output;
layout (location = 10) out flat int v_EntityID;

// ---- Neural geometric displacement (vertex stage) ----
// Same height field the fragment relief shades (luminance macro + craggy
// meso/strata + neural micro), evaluated per-vertex so silhouettes and
// intersections become real geometry. v-prefixed: the fragment stage defines
// same-named helpers and one program cannot hold duplicate symbols.
layout(binding = 0) uniform sampler2D u_Textures[32];
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;

float wl_vhash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float wl_vsoftsign(float x) { return x / (1.0 + abs(x)); }

vec3 wl_vneural_tex_detail(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    float grain = wl_vhash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    vec3 tint = vec3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

float wl_vneural_micro(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

float wl_vvol_hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float wl_vvol_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(wl_vvol_hash(i), wl_vvol_hash(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(wl_vvol_hash(i + vec3(0.0, 1.0, 0.0)), wl_vvol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y),
               mix(mix(wl_vvol_hash(i + vec3(0.0, 0.0, 1.0)), wl_vvol_hash(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(wl_vvol_hash(i + vec3(0.0, 1.0, 1.0)), wl_vvol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

vec4 wl_vtex_slot(int slot, vec2 uv)
{
    switch (slot)
    {
        case 0:  return texture(u_Textures[0], uv);
        case 1:  return texture(u_Textures[1], uv);
        case 2:  return texture(u_Textures[2], uv);
        case 3:  return texture(u_Textures[3], uv);
        case 4:  return texture(u_Textures[4], uv);
        case 5:  return texture(u_Textures[5], uv);
        case 6:  return texture(u_Textures[6], uv);
        case 7:  return texture(u_Textures[7], uv);
        case 8:  return texture(u_Textures[8], uv);
        case 9:  return texture(u_Textures[9], uv);
        case 10: return texture(u_Textures[10], uv);
        case 11: return texture(u_Textures[11], uv);
        case 12: return texture(u_Textures[12], uv);
        case 13: return texture(u_Textures[13], uv);
        case 14: return texture(u_Textures[14], uv);
        case 15: return texture(u_Textures[15], uv);
        case 16: return texture(u_Textures[16], uv);
        case 17: return texture(u_Textures[17], uv);
        case 18: return texture(u_Textures[18], uv);
        case 19: return texture(u_Textures[19], uv);
        case 20: return texture(u_Textures[20], uv);
        case 21: return texture(u_Textures[21], uv);
        case 22: return texture(u_Textures[22], uv);
        case 23: return texture(u_Textures[23], uv);
        case 24: return texture(u_Textures[24], uv);
        case 25: return texture(u_Textures[25], uv);
        case 26: return texture(u_Textures[26], uv);
        case 27: return texture(u_Textures[27], uv);
        case 28: return texture(u_Textures[28], uv);
        case 29: return texture(u_Textures[29], uv);
        case 30: return texture(u_Textures[30], uv);
        case 31: return texture(u_Textures[31], uv);
        default: return vec4(1.0);
    }
}

float wl_vheight(int slot, vec2 uv, vec3 wp, vec3 n, float disp)
{
    float h = dot(wl_vtex_slot(slot, uv).rgb, vec3(0.299, 0.587, 0.114));
    if (u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001)
        h += wl_vneural_micro(uv, n) * 2.0 * clamp(u_NeuralTexStrength, 0.0, 1.0);
    float ma = 0.25 * clamp(disp, 0.0, 1.5);
    if (ma > 0.001)
    {
        vec2 muv = uv * 6.0;
        float n1 = wl_vvol_noise(vec3(muv, 3.7));
        float n2 = wl_vvol_noise(vec3(muv * 2.3 + 5.0, 9.1));
        float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
        float sw = clamp(1.0 - abs(n.y), 0.15, 1.0);
        h += ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * ma
           + (st - 0.5) * ma * 0.8 * sw;
    }
    return clamp(h, 0.0, 1.5);
}

void main()
{
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    // Neural geometric displacement: real vertices move along the (world)
    // normal by the neural height field. Unfaded by design — geometry must
    // not swim while fragment shading detail fades with distance.
    vec3 vPos = a_Position;
    if (a_Displacement > 0.001)
    {
        vec3 vN = normalize(a_Normal);
        float vh = wl_vheight(int(a_TexIndex), a_TexCoord * a_TilingFactor, a_Position, vN, a_Displacement);
        vPos = a_Position + vN * (vh - 0.5) * a_Displacement;
    }
    Output.WorldPos = vPos;
    Output.Metallic = a_Metallic;
    Output.Roughness = a_Roughness;
    Output.Displacement = a_Displacement;
    Output.BumpStrength = a_BumpStrength;
    v_EntityID = a_EntityID;

    gl_Position = u_ViewProjection * vec4(vPos, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 color;
layout(location = 1) out int color2; // Entity ID attachment texture slot

// Relief writes gl_FragDepth only for displaced fragments; this keeps the
// early-z cull for everything else instead of forcing late-z program-wide.
layout(early_fragment_tests) in;

struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
    vec3 WorldPos;
    float Metallic;
    float Roughness;
    float Displacement;
    float BumpStrength;
};

layout (location = 0) in VertexOutput Input;
layout (location = 10) in flat int v_EntityID;

layout (binding = 0) uniform sampler2D u_Textures[32];

// Shared with the vertex stage (uploaded once per frame by Renderer3D):
// reprojects the relief-pushed position for the gl_FragDepth write.
uniform mat4 u_ViewProjection;

// Camera + sky (uploaded by Renderer3D): view-dependent metal reflection.
uniform vec3 u_CameraPosition;
uniform vec3 u_SkyBottomColor;
uniform vec3 u_SkyTopColor;

// Neural Rendering (OpenGL): tiny in-shader MLP for texture detail + learned ambient.
// Driven by Renderer3D neural uniforms; defaults off (0) until Flush() uploads them.
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;
uniform float u_NeuralLightStrength;
// Neural material blend. Defaults to 0 (classic response) until Renderer3D uploads it.
uniform float u_NeuralMatStrength;

// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)
uniform vec3 u_SunDirection;
uniform float u_Time;
uniform int u_FogEnabled;
// God-ray sun color (directional light color x intensity, or warm default).
uniform vec3 u_SunLightColor;
uniform int u_FogCount;
uniform vec3 u_FogMin[4];
uniform vec3 u_FogMax[4];
uniform vec3 u_FogColor[4];
uniform vec4 u_FogData[4];
uniform vec4 u_FogData2[4];
uniform int u_CloudEnabled;
uniform int u_CloudCount;
uniform vec3 u_CloudMin[2];
uniform vec3 u_CloudMax[2];
uniform vec3 u_CloudColor[2];
uniform vec3 u_CloudAmbient[2];
uniform vec4 u_CloudData[2];
uniform vec4 u_CloudData2[2];
uniform vec4 u_CloudData3[2];
// 1 = reduced noise octaves (Low/Medium quality presets).
uniform int u_VolFast;

// Analytic component lights (Renderer3D uploads; -1 locations skipped).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)
uniform int u_ALightCount;
uniform vec3 u_ALightPos[8];
uniform vec3 u_ALightDir[8];
uniform vec3 u_ALightColor[8];
uniform vec4 u_ALightData[8];
uniform vec4 u_ALightData2[8];

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

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain).
float wl_neural_micro(vec2 uv, vec3 n)
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
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
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

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
float wl_vol_hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float wl_vol_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(wl_vol_hash(i), wl_vol_hash(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0, 1.0, 0.0)), wl_vol_hash(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
               mix(mix(wl_vol_hash(i + vec3(0.0, 0.0, 1.0)), wl_vol_hash(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0, 1.0, 1.0)), wl_vol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float wl_vol_fbm(vec3 p)
{
    float v = 0.0;
    float a = 0.5;
    for (int o = 0; o < 4; o++)
    {
        v += a * wl_vol_noise(p);
        p = p * 2.03 + vec3(17.3);
        a *= 0.5;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
float wl_vol_fbm2(vec3 p)
{
    return (0.5 * wl_vol_noise(p) + 0.25 * wl_vol_noise(p * 2.03 + vec3(17.3))) * 1.25;
}

vec2 wl_box_range(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax)
{
    vec3 safe = rd + (vec3(1.0) - step(vec3(0.00001), abs(rd))) * 0.00001;
    vec3 inv = 1.0 / safe;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    return 0.07957747 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosT, 0.001), 1.5);
}

vec4 wl_march_fog_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                      vec3 tint, vec4 d1, vec4 d2, float dither, float sunVis)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    // Depth-adaptive steps: thin intersections cost proportionally less while
    // keeping an approximately constant world-space step size.
    float fogLen = max(t1 - t0, 0.0);
    int steps = int(clamp(d2.z * clamp(fogLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / float(steps);
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9, 0.9));
    vec3 ambCol = mix(u_SkyBottomColor, u_SkyTopColor, 0.5);
    vec3 light = ambCol * 0.7 + u_SunLightColor * sunVis * (0.25 + phase * 2.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        float d = max(d1.x, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        d *= mix(1.0, exp(-h * 4.0), clamp(d2.y, 0.0, 1.0));
        if (d1.z > 0.001)
        {
            vec3 np = p * max(d1.w, 0.001) + vec3(u_Time * d2.x, 0.0, u_Time * d2.x * 0.6);
            float n = (u_VolFast == 1) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            d *= (1.0 - d1.z * 0.5) + d1.z * n;
        }
        float a = 1.0 - exp(-max(d, 0.0) * dt);
        acc += trans * a * tint * light;
        trans *= (1.0 - a);
        if (trans < 0.02)
        {
            trans = 0.0;
            break;
        }
        t += dt;
    }
    return vec4(acc, trans);
}

vec4 wl_march_cloud_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                        vec3 tint, vec3 amb, vec4 d1, vec4 d2, vec4 d3, float dither)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0);
    int steps = int(clamp(d2.w * clamp(clLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / float(steps);
    vec2 wnd = vec2(d2.y, d2.z);
    if (dot(wnd, wnd) < 0.00000001)
        wnd = vec2(1.0, 0.0);
    wnd = normalize(wnd) * d2.x * u_Time;
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0, 1.0), 6.0);
    vec3 sunCol = vec3(1.2, 1.1, 1.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    float nScale = max(d1.z, 0.001);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        vec3 q = p * nScale + vec3(wnd.x, 0.0, wnd.y);
        float base = (u_VolFast == 1) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0, 1.0);
        float d = smoothstep(1.0 - cov, 1.0 - cov + 0.35, base);
        if (d > 0.001 && d1.w > 0.001)
        {
            float det = wl_vol_noise(q * 3.7 + vec3(0.0, u_Time * d2.x * 0.5, 0.0));
            d = max(d - det * d1.w * d, 0.0);
        }
        d *= max(d1.y, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        d *= smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.6, 1.0, h) * 0.7);
        float a = 1.0 - exp(-d * dt);
        if (a > 0.001)
        {
            float sh = 1.0;
            if (d3.y > 0.01)
            {
                // Cheap self-shadow: single-octave density tap toward the sun
                // (a full second march is overkill for a soft darkening term).
                vec3 sp = p + u_SunDirection * dt * 2.0;
                float sd = wl_vol_noise(sp * nScale + vec3(wnd.x, 0.0, wnd.y)) * max(d1.y, 0.0);
                sh = exp(-sd * dt * 4.0 * clamp(d3.y, 0.0, 1.0));
            }
            vec3 scol = tint * (amb * (0.35 + 0.65 * sh) + sunCol * sh * 0.9)
                      + sunCol * silverPow * clamp(d3.x, 0.0, 1.0) * 0.6;
            acc += trans * a * scol;
            trans *= (1.0 - a);
            if (trans < 0.02)
            {
                trans = 0.0;
                break;
            }
        }
        t += dt;
    }
    return vec4(acc, trans);
}

vec3 wl_apply_volumetrics(vec3 ro, vec3 rd, float tHit, vec3 baseColor, float dither, float sunVis)
{
    vec3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5)
                continue;
            vec4 r = wl_march_cloud_vol(ro, rd, tHit, u_CloudMin[i], u_CloudMax[i],
                                        u_CloudColor[i], u_CloudAmbient[i],
                                        u_CloudData[i], u_CloudData2[i], u_CloudData3[i], dither);
            col = col * r.a + r.rgb;
        }
    }
    if (u_FogEnabled == 1)
    {
        for (int i = 0; i < 4; i++)
        {
            if (i >= u_FogCount)
                break;
            if (u_FogData2[i].w < 0.5)
                continue;
            vec4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    return col;
}

// ---- Neural relief mapping (fragment-space POM) ----
// Height source: texture luminance + neural micro-relief from the existing
// detail MLP, so even flat albedo carries sub-texel geometry. Two triangles
// read as millions of polys: a UV-space height march (parallax), a
// derivative-scaled gradient normal, and a short light march for
// self-shadowing. Opt-in per material via Displacement / BumpStrength.
vec4 wl_tex_slot(int slot, vec2 uv)
{
    switch (slot)
    {
        case 0:  return texture(u_Textures[0], uv);
        case 1:  return texture(u_Textures[1], uv);
        case 2:  return texture(u_Textures[2], uv);
        case 3:  return texture(u_Textures[3], uv);
        case 4:  return texture(u_Textures[4], uv);
        case 5:  return texture(u_Textures[5], uv);
        case 6:  return texture(u_Textures[6], uv);
        case 7:  return texture(u_Textures[7], uv);
        case 8:  return texture(u_Textures[8], uv);
        case 9:  return texture(u_Textures[9], uv);
        case 10: return texture(u_Textures[10], uv);
        case 11: return texture(u_Textures[11], uv);
        case 12: return texture(u_Textures[12], uv);
        case 13: return texture(u_Textures[13], uv);
        case 14: return texture(u_Textures[14], uv);
        case 15: return texture(u_Textures[15], uv);
        case 16: return texture(u_Textures[16], uv);
        case 17: return texture(u_Textures[17], uv);
        case 18: return texture(u_Textures[18], uv);
        case 19: return texture(u_Textures[19], uv);
        case 20: return texture(u_Textures[20], uv);
        case 21: return texture(u_Textures[21], uv);
        case 22: return texture(u_Textures[22], uv);
        case 23: return texture(u_Textures[23], uv);
        case 24: return texture(u_Textures[24], uv);
        case 25: return texture(u_Textures[25], uv);
        case 26: return texture(u_Textures[26], uv);
        case 27: return texture(u_Textures[27], uv);
        case 28: return texture(u_Textures[28], uv);
        case 29: return texture(u_Textures[29], uv);
        case 30: return texture(u_Textures[30], uv);
        case 31: return texture(u_Textures[31], uv);
        default: return vec4(1.0);
    }
}

// Procedural craggy breakup + strata banding. Added to final/gradient/shadow
// taps only (never the march), so detail rides on the macro silhouette.
float wl_relief_meso(vec2 uv, vec3 wp, float amp, float strataW)
{
    if (amp <= 0.001)
        return 0.0;
    vec2 muv = uv * 6.0;
    float n1 = wl_vol_noise(vec3(muv, 3.7));
    float n2 = wl_vol_noise(vec3(muv * 2.3 + 5.0, 9.1));
    float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
    return ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * amp
         + (st - 0.5) * amp * 0.8 * strataW;
}

float wl_relief_height(int slot, vec2 uv, vec3 n, float neuralK)
{
    float h = dot(wl_tex_slot(slot, uv).rgb, vec3(0.299, 0.587, 0.114));
    if (neuralK > 0.001)
        h += wl_neural_micro(uv, n) * 2.0 * neuralK;
    return clamp(h, 0.0, 1.5);
}

// Analytic component lights for the raster preview path. Mirrors the Nova
// evaluation (same falloff/cone/area model) but WITHOUT shadow rays — the
// raster path has no shadow maps; true shadows live in Nova.
vec3 wl_analytic_lights(vec3 albedo, float metallic, float roughness, vec3 N, vec3 wp)
{
    vec3 V = normalize(u_CameraPosition - wp);
    vec3 F0 = mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0));
    float rough = clamp(roughness, 0.04, 1.0);
    vec3 acc = vec3(0.0);
    for (int a = 0; a < 8; a++)
    {
        if (a >= u_ALightCount) break;
        float atype = u_ALightData[a].x;
        vec3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0);
        float afall = max(u_ALightData[a].w, 0.5);
        vec3 L;
        float att = 1.0;
        if (atype < 0.5) {
            L = -u_ALightDir[a];
        } else {
            vec3 toL = u_ALightPos[a] - wp;
            float d = length(toL);
            L = toL / max(d, 0.00001);
            if (arange > 0.0) {
                if (d >= arange) continue;
                att *= pow(clamp(1.0 - d / arange, 0.0, 1.0), afall);
            } else {
                att *= 1.0 / (1.0 + 0.1 * afall * d * d);
            }
            if (atype > 1.5 && atype < 2.5) {
                float c = dot(-L, u_ALightDir[a]);
                att *= smoothstep(u_ALightData2[a].y, u_ALightData2[a].x, c);
                if (att <= 0.0) continue;
            } else if (atype > 2.5) {
                float front = dot(-L, u_ALightDir[a]);
                if (u_ALightData2[a].w < 0.5) {
                    if (front <= 0.0) continue;
                    att *= clamp(front * 3.0, 0.0, 1.0);
                }
                float asize = max(u_ALightData2[a].z, 0.01);
                att *= asize / (d + asize);
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        vec3 H = normalize(V + L);
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float spec = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough));
        acc += (albedo * (1.0 - metallic) * NdotL + F * spec * NdotL) * lcol * att;
    }
    return acc;
}

void main()
{
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4));
    vec3 norm0 = normalize(Input.Normal);
    vec3 V = normalize(u_CameraPosition - Input.WorldPos);
    // Water marker: water verts arrive with BumpStrength = -1 (the relief
    // path above already skipped them: its gate needs values > 0.001).

    // ---- Neural relief: UV-space height march (parallax) + gradient normal
    // + light self-shadow. Distance- and grazing-faded, zero taps when the
    // material opts out (Displacement and BumpStrength both 0).
    vec2 reliefUV = Input.TexCoord * Input.TilingFactor;
    vec3 norm = norm0;
    float reliefShadow = 1.0;
    float reliefPush = 0.0;
    float reliefH = -1.0;
    float mesoAmp = 0.0;
    float strataW = clamp(1.0 - abs(norm0.y), 0.15, 1.0);
    {
        float disp = Input.Displacement;
        float bump = Input.BumpStrength;
        int slot = int(Input.TexIndex);
        float dist = length(u_CameraPosition - Input.WorldPos);
        float fade = clamp(1.0 - (dist - 18.0) / 30.0, 0.0, 1.0);
        float ndv = abs(dot(V, norm0));
        // Same angular collapse as Nova: kill POM smear streaks edge-on.
        float marchK = smoothstep(0.10, 0.45, ndv);
        // Tangent frame from screen-space derivatives: uniform control flow,
        // so no undefined-derivative issues inside the gated march below.
        vec3 dpx = dFdx(Input.WorldPos);
        vec3 dpy = dFdy(Input.WorldPos);
        vec2 dtx = dFdx(reliefUV);
        vec2 dty = dFdy(reliefUV);
        vec3 T = dpx * dty.y - dpy * dtx.y;
        T = normalize(T - norm0 * dot(norm0, T) + vec3(1e-6));
        vec3 B = normalize(cross(norm0, T));
        float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0, 1.0) : 0.0;
        if ((disp > 0.001 || bump > 0.001) && fade > 0.001 && ndv > 0.12)
        {
            float ds = disp * fade;
            float bs = bump * fade;
            mesoAmp = 0.25 * clamp(ds, 0.0, 1.5);
            float pScale = clamp(ds * 0.05, 0.0, 0.12);
            float finalH = wl_relief_height(slot, reliefUV, norm0, neuralK);
            if (ds > 0.001)
            {
                vec3 viewTS = vec3(dot(V, T), dot(V, B), dot(V, norm0));
                int layers = int(mix(14.0, 6.0, ndv));
                vec2 stepUV = (-viewTS.xy / max(viewTS.z, 0.08)) * (pScale / float(layers));
                float dh = 1.0 / float(layers);
                vec2 cuv = reliefUV;
                float clH = 1.0;
                float cTexH = finalH;
                vec2 puv = cuv;
                float plH = clH;
                float pTexH = cTexH;
                for (int i = 0; i < 14; i++)
                {
                    if (i >= layers)
                        break;
                    puv = cuv;
                    plH = clH;
                    pTexH = cTexH;
                    cuv += stepUV;
                    clH -= dh;
                    cTexH = wl_relief_height(slot, cuv, norm0, neuralK);
                    if (cTexH > clH)
                        break;
                }
                // Binary refine: crisp intersections for deep relief (4 taps).
                for (int b = 0; b < 4; b++)
                {
                    vec2 midUV = (puv + cuv) * 0.5;
                    float midH = wl_relief_height(slot, midUV, norm0, neuralK);
                    float midL = (plH + clH) * 0.5;
                    if (midH > midL)
                    {
                        cuv = midUV;
                        clH = midL;
                        cTexH = midH;
                    }
                    else
                    {
                        puv = midUV;
                        plH = midL;
                        pTexH = midH;
                    }
                }
                float rd1 = plH - pTexH;
                float rd2 = cTexH - clH;
                float rw = rd1 / max(rd1 + rd2, 0.0001);
                vec2 delta = (mix(puv, cuv, rw) - reliefUV) * marchK;
                if (length(delta) > 0.08)
                    delta = normalize(delta) * 0.08;
                reliefUV += delta;
                finalH = mix(pTexH, cTexH, rw);
                finalH = clamp(finalH + wl_relief_meso(reliefUV, Input.WorldPos, mesoAmp, strataW), 0.0, 1.5);
                reliefH = finalH;
                // Shape-preserving push (bilateral around the mid-level, like
                // Nova): the base shape survives, features rise and recess.
                reliefPush = (finalH - 0.5) * ds;
            }
            if (bs > 0.001)
            {
                float e = 0.004;
                float hU = wl_relief_height(slot, reliefUV + vec2(e, 0.0), norm0, neuralK)
                         + wl_relief_meso(reliefUV + vec2(e, 0.0), Input.WorldPos, mesoAmp, strataW);
                float hV = wl_relief_height(slot, reliefUV + vec2(0.0, e), norm0, neuralK)
                         + wl_relief_meso(reliefUV + vec2(0.0, e), Input.WorldPos, mesoAmp, strataW);
                float dhdu = (hU - finalH) / e;
                float dhdv = (hV - finalH) / e;
                // UV-per-world measured per pixel: raster faces span 0-1 UV
                // over arbitrary world sizes, so Nova's ~1 UV/m assumption
                // does not hold here.
                float worldPerUV = clamp(length(dpx) / max(length(dtx), 1e-6), 0.05, 200.0);
                vec3 grad = (T * dhdu + B * dhdv) * (bs / worldPerUV);
                float gl = length(grad);
                if (gl > 1.5)
                    grad /= gl / 1.5;
                norm = normalize(norm0 - grad);
            }
            if (ds > 0.001)
            {
                // Cheap heightfield self-shadow: 4 taps toward the light.
                vec3 lts = vec3(dot(lightDir, T), dot(lightDir, B), dot(lightDir, norm0));
                vec2 sStep = (lts.xy / max(lts.z, 0.1)) * (pScale / 4.0);
                float rayH = finalH + 0.03;
                float occ = 1.0;
                vec2 suv = reliefUV;
                for (int i = 0; i < 4; i++)
                {
                    suv += sStep;
                    rayH += 0.06;
                    float hh = wl_relief_height(slot, suv, norm0, neuralK)
                             + wl_relief_meso(suv, Input.WorldPos, mesoAmp, strataW);
                    occ = min(occ, clamp((rayH - hh) * 8.0, 0.0, 1.0));
                }
                reliefShadow = clamp(occ, 0.3, 1.0);
            }
        }
    }

    // Screen-space displacement: commit the relief to the depth buffer so it
    // self-occludes and intersects other geometry like real polygons. Untouched
    // (== interpolated triangle depth) for every non-relief fragment.
    if (abs(reliefPush) > 0.00001)
    {
        vec4 dispClip = u_ViewProjection * vec4(Input.WorldPos + norm0 * reliefPush, 1.0);
        gl_FragDepth = (dispClip.z / dispClip.w) * 0.5 + 0.5;
    }

    vec4 texColor = Input.Color;

    // Evaluate hardware texture slots using an integer cast selector
    switch(int(Input.TexIndex))
    {
        case 0:  texColor *= texture(u_Textures[0],  reliefUV); break;
        case 1:  texColor *= texture(u_Textures[1],  reliefUV); break;
        case 2:  texColor *= texture(u_Textures[2],  reliefUV); break;
        case 3:  texColor *= texture(u_Textures[3],  reliefUV); break;
        case 4:  texColor *= texture(u_Textures[4],  reliefUV); break;
        case 5:  texColor *= texture(u_Textures[5],  reliefUV); break;
        case 6:  texColor *= texture(u_Textures[6],  reliefUV); break;
        case 7:  texColor *= texture(u_Textures[7],  reliefUV); break;
        case 8:  texColor *= texture(u_Textures[8],  reliefUV); break;
        case 9:  texColor *= texture(u_Textures[9],  reliefUV); break;
        case 10: texColor *= texture(u_Textures[10], reliefUV); break;
        case 11: texColor *= texture(u_Textures[11], reliefUV); break;
        case 12: texColor *= texture(u_Textures[12], reliefUV); break;
        case 13: texColor *= texture(u_Textures[13], reliefUV); break;
        case 14: texColor *= texture(u_Textures[14], reliefUV); break;
        case 15: texColor *= texture(u_Textures[15], reliefUV); break;
        case 16: texColor *= texture(u_Textures[16], reliefUV); break;
        case 17: texColor *= texture(u_Textures[17], reliefUV); break;
        case 18: texColor *= texture(u_Textures[18], reliefUV); break;
        case 19: texColor *= texture(u_Textures[19], reliefUV); break;
        case 20: texColor *= texture(u_Textures[20], reliefUV); break;
        case 21: texColor *= texture(u_Textures[21], reliefUV); break;
        case 22: texColor *= texture(u_Textures[22], reliefUV); break;
        case 23: texColor *= texture(u_Textures[23], reliefUV); break;
        case 24: texColor *= texture(u_Textures[24], reliefUV); break;
        case 25: texColor *= texture(u_Textures[25], reliefUV); break;
        case 26: texColor *= texture(u_Textures[26], reliefUV); break;
        case 27: texColor *= texture(u_Textures[27], reliefUV); break;
        case 28: texColor *= texture(u_Textures[28], reliefUV); break;
        case 29: texColor *= texture(u_Textures[29], reliefUV); break;
        case 30: texColor *= texture(u_Textures[30], reliefUV); break;
        case 31: texColor *= texture(u_Textures[31], reliefUV); break;
    }

    float metallic = clamp(Input.Metallic, 0.0, 1.0);
    float roughness = clamp(Input.Roughness, 0.04, 1.0);

    // Relief weathering: dust settles in pits, sun bleaches ridge tops.
    if (reliefH >= 0.0)
    {
        float cav = smoothstep(0.45, 0.05, reliefH);
        float ridge = smoothstep(0.55, 0.95, reliefH);
        texColor.rgb *= (1.0 - 0.30 * cav);
        texColor.rgb = mix(texColor.rgb, texColor.rgb * vec3(1.10, 1.03, 0.92), ridge * 0.65);
    }

    // ---- Water: mirror + pixel ripple detail (Unreal-style) ----
    // Dielectric water would get no sky reflection from the metal-gated env
    // term below, so route water through the mirror path instead: diffuse
    // dies (water has none), F0 picks up the water tint, and the sky mirror
    // + sun glints take over. norm (Gerstner mesh normal) is augmented with
    // animated pixel-level ripple octaves so chop survives at any mesh
    // density; the existing fresnel/spec/analytic-light terms then respond
    // to the perturbed normal with zero further changes.
    bool wlWater = (Input.BumpStrength < -0.5);
    if (wlWater)
    {
        vec2 wpuv = Input.WorldPos.xz;
        float wdist = length(u_CameraPosition - Input.WorldPos);
        float wfade = clamp(1.0 - (wdist - 25.0) / 60.0, 0.0, 1.0);
        if (wfade > 0.001)
        {
            float wt = u_Time;
            vec2 wg = vec2(0.0);
            wg += vec2(0.96, 0.28) * (cos(dot(vec2(0.96, 0.28), wpuv) * 2.1 + wt * 1.6) * 0.22);
            wg += vec2(-0.42, 0.91) * (cos(dot(vec2(-0.42, 0.91), wpuv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
            wg += vec2(0.66, -0.75) * (cos(dot(vec2(0.66, -0.75), wpuv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
            norm = normalize(norm + vec3(-wg.x, 0.0, -wg.y) * (0.55 * wfade));
        }
        metallic = 1.0;
    }

    // Metallic PBR terms (shared by neural + classic paths). Diffuse dies
    // out on metals; reflection + specular take over. No shadow maps on the
    // raster path — true shadows live in the Nova ray-trace pipeline.
    vec3 F0 = mix(vec3(0.04), texColor.rgb, metallic);
    vec3 R = reflect(-V, norm);
    vec3 specSky = mix(u_SkyBottomColor, u_SkyTopColor, R.y * 0.5 + 0.5);
    vec3 diffSky = mix(u_SkyBottomColor, u_SkyTopColor, norm.y * 0.5 + 0.5);
    vec3 envColor = mix(specSky, (specSky + diffSky) * 0.5, roughness * roughness);
    float NdotL = max(dot(norm, lightDir), 0.0);
    vec3 H = normalize(lightDir + V);
    float specTerm = pow(max(dot(norm, H), 0.0), mix(128.0, 8.0, roughness))
                   * (1.0 - roughness * 0.7) * reliefShadow;
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    float diffuseIntensity = NdotL * reliefShadow;
    float ambientIntensity = 0.2;

    if (u_NeuralEnabled == 1)
    {
        if (!wlWater)
        {
            vec3 detail = wl_neural_tex_detail(reliefUV, norm);
            texColor.rgb *= mix(vec3(1.0), detail, clamp(u_NeuralTexStrength, 0.0, 1.0));
        }

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
            wl_neural_mat_raster(reliefUV, norm, matAlbedo, g, w);
            matGlint = g;
            matWrap = w;
        }
        if (!wlWater)
            texColor.rgb = mix(texColor.rgb, matAlbedo, km);
        // Re-derive F0 from the neural albedo so flakes tint reflections.
        vec3 nF0 = mix(vec3(0.04), texColor.rgb, metallic);
        vec3 nF = nF0 + (1.0 - nF0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float wrapDiffuse = clamp((dot(norm, lightDir) + matWrap * km) / (1.0 + matWrap * km), 0.0, 1.0);
        float diffTerm = mix(diffuseIntensity, wrapDiffuse, km * 0.7) * (1.0 - metallic);
        vec3 warmLight = vec3(1.0, 0.98, 0.94);
        vec3 lit = texColor.rgb * (diffTerm * warmLight + mix(vec3(ambientIntensity), namb * 2.2, k) * (1.0 - metallic * 0.85));
        lit += nF * envColor * metallic;
        lit += specTerm * nF * NdotL;
        lit += matGlint * km * (0.25 + 0.75 * metallic) * diffuseIntensity * vec3(1.3, 1.25, 1.15);
        lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
        {
            vec3 vdir = Input.WorldPos - u_CameraPosition;
            float vdist = length(vdir);
            lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy), 1.0);
        }
        color = vec4(lit, texColor.a);
        color2 = v_EntityID;
        return;
    }

    vec3 diffTerm = texColor.rgb * (1.0 - metallic) * (ambientIntensity + diffuseIntensity);
    vec3 lit = diffTerm + F * envColor * metallic + specTerm * F * NdotL;
    lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
    {
        vec3 vdir = Input.WorldPos - u_CameraPosition;
        float vdist = length(vdir);
        lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy), 1.0);
    }

    color = vec4(lit, texColor.a);
    color2 = v_EntityID;
}
