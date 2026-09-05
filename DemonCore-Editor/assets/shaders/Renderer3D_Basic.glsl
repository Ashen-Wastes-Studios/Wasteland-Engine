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
};

layout (location = 0) out VertexOutput Output;
layout (location = 8) out flat int v_EntityID;

void main()
{
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    Output.WorldPos = a_Position;
    Output.Metallic = a_Metallic;
    Output.Roughness = a_Roughness;
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
    vec3 WorldPos;
    float Metallic;
    float Roughness;
};

layout (location = 0) in VertexOutput Input;
layout (location = 8) in flat int v_EntityID;

layout (binding = 0) uniform sampler2D u_Textures[32];

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
                      vec3 tint, vec4 d1, vec4 d2, float dither)
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
    vec3 sunCol = vec3(1.15, 1.05, 0.95);
    vec3 ambCol = mix(u_SkyBottomColor, u_SkyTopColor, 0.5);
    vec3 light = ambCol * 0.7 + sunCol * (0.25 + phase * 2.0);
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

vec3 wl_apply_volumetrics(vec3 ro, vec3 rd, float tHit, vec3 baseColor, float dither)
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
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither);
            col = col * r.a + r.rgb;
        }
    }
    return col;
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
    float metallic = clamp(Input.Metallic, 0.0, 1.0);
    float roughness = clamp(Input.Roughness, 0.04, 1.0);
    vec3 V = normalize(u_CameraPosition - Input.WorldPos);

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
                   * (1.0 - roughness * 0.7);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    float diffuseIntensity = NdotL;
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
        {
            vec3 vdir = Input.WorldPos - u_CameraPosition;
            float vdist = length(vdir);
            lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy));
        }
        color = vec4(lit, texColor.a);
        color2 = v_EntityID;
        return;
    }

    vec3 diffTerm = texColor.rgb * (1.0 - metallic) * (ambientIntensity + diffuseIntensity);
    vec3 lit = diffTerm + F * envColor * metallic + specTerm * F * NdotL;
    {
        vec3 vdir = Input.WorldPos - u_CameraPosition;
        float vdist = length(vdir);
        lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy));
    }

    color = vec4(lit, texColor.a);
    color2 = v_EntityID;
}
