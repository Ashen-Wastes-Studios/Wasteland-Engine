#type vertex

// ============================================================================
// SHARED DECLARATIONS (vertex + pixel stages)
// ============================================================================

Texture2D    u_Textures[32] : register(t0);
SamplerState u_Sampler      : register(s0);

// ---- GLSL-semantics helpers ----
float  fract2(float x)  { return x - floor(x); }
float2 fract2(float2 x) { return x - floor(x); }
float3 fract2(float3 x) { return x - floor(x); }
float4 fract2(float4 x) { return x - floor(x); }
float  lerp_safe(float a, float b, float t)    { return lerp(a, b, t); }
float2 lerp_safe(float2 a, float2 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float t)  { return lerp(a, b, t); }
float4 lerp_safe(float4 a, float4 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float3 t) { return lerp(a, b, t); }

// TEX/TEX_LOD macros are defined per-stage below (Sample vs SampleLevel).

#define TEX(tex, uv)        (tex).SampleLevel(u_Sampler, (uv), 0.0f)
#define TEX_LOD(tex, uv, l) (tex).SampleLevel(u_Sampler, (uv), (l))

// ============================================================================
// Renderer3D_Basic.hlsl -- HLSL (SM 5.0) port of Renderer3D_Basic.glsl
// (vertex + pixel) for the NVRHI D3D11/D3D12 raster path.
//
// Functional parity notes:
//  - Full port of the GLSL: neural relief POM (uv march + gradient normal +
//    self-shadow), per-vertex neural displacement, water ripples + mirror,
//    volumetric fog/clouds, analytic component lights, neural material/
//    ambient MLPs, weathering, EntityID picking.
//  - Vertex inputs use the BufferLayout attribute names as D3D semantics
//    (NVRHI passes them through verbatim); float/int formats match
//    ShaderDataTypeToNVRHIFormat.
//  - u_ViewProjection lives in the VS cbuffer (b0). The NVRHIShader
//    reflection keeps ONE uniform-name -> stage mapping, so every other
//    uniform lives in the PS cbuffer (b1) where SetX writes land. The VS
//    forwards the VP rows as nointerpolation interpolators, letting the PS
//    reconstruct the matrix and commit the per-fragment relief push to
//    SV_Depth exactly (gl_FragDepth parity, with [earlydepthstencil]).
//  - Known limitation: the neural micro term in the VERTEX height field is
//    omitted (u_Neural* are PS-only; the uniform map cannot feed the VS).
//    The fragment shading keeps the full neural stack.
//  - Implicit-LOD texture() in the PS maps to Sample() (auto-mip + driver
//    anisotropy, like GL); in the VS it maps to SampleLevel(0) (GL VS
//    behavior).
// ============================================================================

struct VSOutput
{
    float4 Position  : SV_Position;
    float4 Color     : COLOR0;
    float3 Normal    : NORMAL0;
    float2 TexCoord  : TEXCOORD0;
    float  TexIndex  : TEXCOORD1;
    float  TilingFactor : TEXCOORD2;
    float3 WorldPos  : TEXCOORD3;
    float  Metallic  : TEXCOORD4;
    float  Roughness : TEXCOORD5;
    float  Displacement : TEXCOORD6;
    float  BumpStrength : TEXCOORD7;
    nointerpolation int    v_EntityID : TEXCOORD8;
    nointerpolation float4 u_VPRow0 : TEXCOORD9;
    nointerpolation float4 u_VPRow1 : TEXCOORD10;
    nointerpolation float4 u_VPRow2 : TEXCOORD11;
    nointerpolation float4 u_VPRow3 : TEXCOORD12;
};

cbuffer TransformConstants : register(b0)
{
    float4x4 u_ViewProjection;
};

// ---- Neural geometric displacement (vertex stage) ----
// Same height field the fragment relief shades (luminance macro + craggy
// meso/strata + neural micro), evaluated per-vertex so silhouettes and
// intersections become real geometry. v-prefixed: the fragment stage defines
// same-named helpers and one program cannot hold duplicate symbols.



float wl_vhash12(float2 p)
{
    float3 p3 = fract2(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract2((p3.x + p3.y) * p3.z);
}

float wl_vsoftsign(float x) { return x / (1.0 + abs(x)); }

float3 wl_vneural_tex_detail(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    float grain = wl_vhash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    float3 tint = float3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

float wl_vneural_micro(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

float wl_vvol_hash(float3 p)
{
    p = fract2(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract2((p.x + p.y) * p.z);
}

float wl_vvol_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = fract2(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    return lerp_safe(lerp_safe(lerp_safe(wl_vvol_hash(i), wl_vvol_hash(i + float3(1.0, 0.0, 0.0)), u.x),
                   lerp_safe(wl_vvol_hash(i + float3(0.0, 1.0, 0.0)), wl_vvol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y),
               lerp_safe(lerp_safe(wl_vvol_hash(i + float3(0.0, 0.0, 1.0)), wl_vvol_hash(i + float3(1.0, 0.0, 1.0)), u.x),
                   lerp_safe(wl_vvol_hash(i + float3(0.0, 1.0, 1.0)), wl_vvol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float4 wl_vtex_slot(int slot, float2 uv)
{
    switch (slot)
    {
        case 0:  return TEX(u_Textures[0], uv);
        case 1:  return TEX(u_Textures[1], uv);
        case 2:  return TEX(u_Textures[2], uv);
        case 3:  return TEX(u_Textures[3], uv);
        case 4:  return TEX(u_Textures[4], uv);
        case 5:  return TEX(u_Textures[5], uv);
        case 6:  return TEX(u_Textures[6], uv);
        case 7:  return TEX(u_Textures[7], uv);
        case 8:  return TEX(u_Textures[8], uv);
        case 9:  return TEX(u_Textures[9], uv);
        case 10: return TEX(u_Textures[10], uv);
        case 11: return TEX(u_Textures[11], uv);
        case 12: return TEX(u_Textures[12], uv);
        case 13: return TEX(u_Textures[13], uv);
        case 14: return TEX(u_Textures[14], uv);
        case 15: return TEX(u_Textures[15], uv);
        case 16: return TEX(u_Textures[16], uv);
        case 17: return TEX(u_Textures[17], uv);
        case 18: return TEX(u_Textures[18], uv);
        case 19: return TEX(u_Textures[19], uv);
        case 20: return TEX(u_Textures[20], uv);
        case 21: return TEX(u_Textures[21], uv);
        case 22: return TEX(u_Textures[22], uv);
        case 23: return TEX(u_Textures[23], uv);
        case 24: return TEX(u_Textures[24], uv);
        case 25: return TEX(u_Textures[25], uv);
        case 26: return TEX(u_Textures[26], uv);
        case 27: return TEX(u_Textures[27], uv);
        case 28: return TEX(u_Textures[28], uv);
        case 29: return TEX(u_Textures[29], uv);
        case 30: return TEX(u_Textures[30], uv);
        case 31: return TEX(u_Textures[31], uv);
        default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

float wl_vheight(int slot, float2 uv, float3 wp, float3 n, float disp)
{
    float h = dot(wl_vtex_slot(slot, uv).rgb, float3(0.299, 0.587, 0.114));
    // HLSL port: neural micro term omitted in the VS (see header note).
    float ma = 0.25 * clamp(disp, 0.0, 1.5);
    if (ma > 0.001)
    {
        float2 muv = uv * 6.0;
        float n1 = wl_vvol_noise(float3(muv, 3.7));
        float n2 = wl_vvol_noise(float3(muv * 2.3 + 5.0, 9.1));
        float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
        float sw = clamp(1.0 - abs(n.y), 0.15, 1.0);
        h += ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * ma
           + (st - 0.5) * ma * 0.8 * sw;
    }
    return clamp(h, 0.0, 1.5);
}

VSOutput main(float3 a_Position  : POSITION,
              float4 a_Color     : COLOR,
              float3 a_Normal    : NORMAL,
              float2 a_TexCoord  : TEXCOORD,
              float  a_TexIndex  : TEXINDEX,
              float  a_TilingFactor : TILINGFACTOR,
              int    a_EntityID  : ENTITYID,
              float  a_Metallic  : METALLIC,
              float  a_Roughness : ROUGHNESS,
              float  a_Displacement : DISPLACEMENT,
              float  a_BumpStrength : BUMPSTRENGTH)
{
    VSOutput Output;
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    // Neural geometric displacement: real vertices move along the (world)
    // normal by the neural height field. Unfaded by design — geometry must
    // not swim while fragment shading detail fades with distance.
    float3 vPos = a_Position;
    if (a_Displacement > 0.001)
    {
        float3 vN = normalize(a_Normal);
        float vh = wl_vheight((int)(a_TexIndex), a_TexCoord * a_TilingFactor, a_Position, vN, a_Displacement);
        vPos = a_Position + vN * (vh - 0.5) * a_Displacement;
    }
    Output.WorldPos = vPos;
    Output.Metallic = a_Metallic;
    Output.Roughness = a_Roughness;
    Output.Displacement = a_Displacement;
    Output.BumpStrength = a_BumpStrength;
    Output.v_EntityID = a_EntityID;
    Output.Position = mul(u_ViewProjection, float4(vPos, 1.0f));
    // Pass the VP matrix rows so the PS can reproject the per-fragment
    // relief push exactly (gl_FragDepth parity).
    Output.u_VPRow0 = u_ViewProjection[0];
    Output.u_VPRow1 = u_ViewProjection[1];
    Output.u_VPRow2 = u_ViewProjection[2];
    Output.u_VPRow3 = u_ViewProjection[3];
    return Output;
}

#type pixel

// ============================================================================
// SHARED DECLARATIONS (vertex + pixel stages)
// ============================================================================

Texture2D    u_Textures[32] : register(t0);
SamplerState u_Sampler      : register(s0);

// ---- GLSL-semantics helpers ----
float  fract2(float x)  { return x - floor(x); }
float2 fract2(float2 x) { return x - floor(x); }
float3 fract2(float3 x) { return x - floor(x); }
float4 fract2(float4 x) { return x - floor(x); }
float  lerp_safe(float a, float b, float t)    { return lerp(a, b, t); }
float2 lerp_safe(float2 a, float2 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float t)  { return lerp(a, b, t); }
float4 lerp_safe(float4 a, float4 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float3 t) { return lerp(a, b, t); }

// TEX/TEX_LOD macros are defined per-stage below (Sample vs SampleLevel).

#define TEX(tex, uv)        (tex).Sample(u_Sampler, (uv))
#define TEX_LOD(tex, uv, l) (tex).SampleLevel(u_Sampler, (uv), (l))

struct PSOutput
{
    float4 Color   : SV_Target0;
    int EntityID   : SV_Target1;
    float Depth    : SV_Depth;
};

struct PSInput
{
    float4 Position  : SV_Position;
    float4 Color     : COLOR0;
    float3 Normal    : NORMAL0;
    float2 TexCoord  : TEXCOORD0;
    float  TexIndex  : TEXCOORD1;
    float  TilingFactor : TEXCOORD2;
    float3 WorldPos  : TEXCOORD3;
    float  Metallic  : TEXCOORD4;
    float  Roughness : TEXCOORD5;
    float  Displacement : TEXCOORD6;
    float  BumpStrength : TEXCOORD7;
    nointerpolation int    v_EntityID : TEXCOORD8;
    nointerpolation float4 u_VPRow0 : TEXCOORD9;
    nointerpolation float4 u_VPRow1 : TEXCOORD10;
    nointerpolation float4 u_VPRow2 : TEXCOORD11;
    nointerpolation float4 u_VPRow3 : TEXCOORD12;
};

cbuffer BasicConstants : register(b1)
{
    // Camera + sky (view-dependent metal reflection)
    float3  u_CameraPosition;
    float3  u_SkyBottomColor;
    float3  u_SkyTopColor;
    // Neural rendering
    int     u_NeuralEnabled;
    float   u_NeuralTexStrength;
    float   u_NeuralLightStrength;
    float   u_NeuralMatStrength;
    // Volumetric atmosphere
    float3  u_SunDirection;
    float   u_Time;
    int     u_FogEnabled;
    float3  u_SunLightColor;
    int     u_FogCount;
    float3  u_FogMin[4];
    float3  u_FogMax[4];
    float3  u_FogColor[4];
    float4  u_FogData[4];
    float4  u_FogData2[4];
    int     u_CloudEnabled;
    int     u_CloudCount;
    float3  u_CloudMin[2];
    float3  u_CloudMax[2];
    float3  u_CloudColor[2];
    float3  u_CloudAmbient[2];
    float4  u_CloudData[2];
    float4  u_CloudData2[2];
    float4  u_CloudData3[2];
    int     u_VolFast;
    // Analytic component lights
    int     u_ALightCount;
    float3  u_ALightPos[8];
    float3  u_ALightDir[8];
    float3  u_ALightColor[8];
    float4  u_ALightData[8];
    float4  u_ALightData2[8];
};
// Entity ID attachment texture slot

// Relief writes gl_FragDepth only for displaced fragments; this keeps the
// early-z cull for everything else instead of forcing late-z program-wide.


// Shared with the vertex stage (uploaded once per frame by Renderer3D):
// reprojects the relief-pushed position for the gl_FragDepth write.


// Camera + sky (uploaded by Renderer3D): view-dependent metal reflection.




// Neural Rendering (OpenGL): tiny in-shader MLP for texture detail + learned ambient.
// Driven by Renderer3D neural uniforms; defaults off (0) until Flush() uploads them.



// Neural material blend. Defaults to 0 (classic response) until Renderer3D uploads it.


// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)



// God-ray sun color (directional light color x intensity, or warm default).
















// 1 = reduced noise octaves (Low/Medium quality presets).


// Analytic component lights (Renderer3D uploads; -1 locations skipped).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)







float wl_softsign(float x) { return x / (1.0 + abs(x)); }

float wl_hash12(float2 p)
{
    float3 p3 = fract2(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract2((p3.x + p3.y) * p3.z);
}

// 4-feature sinusoidal encoding -> 4-unit hidden layer -> detail multiplier + tint.
// Output ~= 1.0 (neutral); keeps energy stable when strength is 0.
float3 wl_neural_tex_detail(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    float grain = wl_hash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    float3 tint = float3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain).
float wl_neural_micro(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
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
float3 wl_neural_ambient(float3 n, float3 albedo)
{
    float up = n.y * 0.5 + 0.5;
    float3 skyRef = lerp_safe(float3(0.32, 0.30, 0.34), float3(0.55, 0.62, 0.78), up);
    float h = wl_softsign(dot(n.xz, float2(0.8, -0.6)) * 1.5 + n.y * 1.2);
    float3 bounce = albedo * (0.35 + 0.25 * h) * skyRef;
    return skyRef * 0.35 + bounce;
}

// Neural material (raster): same learned PBR idea as the ray-trace path, minus
// world position — porosity/patina modulate albedo, the MLP gates sparse
// metallic-flake glints, and wrap width softens the diffuse response.
void wl_neural_mat_raster(float2 uv, float3 n, inout float3 albedo, out float glint, out float wrapW)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float4 feat = float4(sin(s1 + n.x * 2.0),
                     sin(s1 * 1.6 + n.y * 1.7),
                     sin(s1 * 0.7 + n.z * 2.3),
                     sin((s1 * 0.6 + n.x - n.y) * 1.3));
    float h0 = wl_softsign( 0.8 * feat.x - 0.5 * feat.y + 0.6 * feat.z + 0.2 * feat.w + 0.05);
    float h1 = wl_softsign(-0.4 * feat.x + 0.9 * feat.y - 0.2 * feat.z + 0.5 * feat.w - 0.10);
    float h2 = wl_softsign( 0.3 * feat.x + 0.2 * feat.y + 0.7 * feat.z - 0.6 * feat.w + 0.35);
    float h3 = wl_softsign(-0.5 * feat.x - 0.4 * feat.y + 0.4 * feat.z + 0.8 * feat.w + 0.00);
    float por = clamp(-h0, 0.0, 1.0);
    albedo *= (1.0 - 0.25 * por);
    albedo *= (float3(1.0, 1.0, 1.0) + float3(-0.06, 0.02, 0.05) * h1);
    float flakeCell = step(0.75, wl_hash12(floor(uv * 48.0)));
    glint = flakeCell * clamp(h2 * 1.5, 0.0, 1.0);
    wrapW = 0.35 + 0.65 * clamp(h3 * 0.5 + 0.5, 0.0, 1.0);
}

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
float wl_vol_hash(float3 p)
{
    p = fract2(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract2((p.x + p.y) * p.z);
}

float wl_vol_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = fract2(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    return lerp_safe(lerp_safe(lerp_safe(wl_vol_hash(i), wl_vol_hash(i + float3(1.0, 0.0, 0.0)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0, 1.0, 0.0)), wl_vol_hash(i + float3(1.0, 1.0, 0.0)), u.x), u.y),
               lerp_safe(lerp_safe(wl_vol_hash(i + float3(0.0, 0.0, 1.0)), wl_vol_hash(i + float3(1.0, 0.0, 1.0)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0, 1.0, 1.0)), wl_vol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float wl_vol_fbm(float3 p)
{
    float v = 0.0;
    float a = 0.5;
    for (int o = 0; o < 4; o++)
    {
        v += a * wl_vol_noise(p);
        p = p * 2.03 + float3(17.3, 17.3, 17.3);
        a *= 0.5;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
float wl_vol_fbm2(float3 p)
{
    return (0.5 * wl_vol_noise(p) + 0.25 * wl_vol_noise(p * 2.03 + float3(17.3, 17.3, 17.3))) * 1.25;
}

float2 wl_box_range(float3 ro, float3 rd, float3 bmin, float3 bmax)
{
    float3 safe = rd + (float3(1.0, 1.0, 1.0) - step(float3(0.00001, 0.00001, 0.00001), abs(rd))) * 0.00001;
    float3 inv = 1.0 / safe;
    float3 t0 = (bmin - ro) * inv;
    float3 t1 = (bmax - ro) * inv;
    float3 tmin = min(t0, t1);
    float3 tmax = max(t0, t1);
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    return 0.07957747 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosT, 0.001), 1.5);
}

float4 wl_march_fog_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                      float3 tint, float4 d1, float4 d2, float dither, float sunVis)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    // Depth-adaptive steps: thin intersections cost proportionally less while
    // keeping an approximately constant world-space step size.
    float fogLen = max(t1 - t0, 0.0);
    int steps = (int)(clamp(d2.z * clamp(fogLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / (float)(steps);
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9, 0.9));
    float3 ambCol = lerp_safe(u_SkyBottomColor, u_SkyTopColor, 0.5);
    float3 light = ambCol * 0.7 + u_SunLightColor * sunVis * (0.25 + phase * 2.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    float3 acc = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        float d = max(d1.x, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        d *= lerp_safe(1.0, exp(-h * 4.0), clamp(d2.y, 0.0, 1.0));
        if (d1.z > 0.001)
        {
            float3 np = p * max(d1.w, 0.001) + float3(u_Time * d2.x, 0.0, u_Time * d2.x * 0.6);
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
    return float4(acc, trans);
}

float4 wl_march_cloud_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                        float3 tint, float3 amb, float4 d1, float4 d2, float4 d3, float dither)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0);
    int steps = (int)(clamp(d2.w * clamp(clLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / (float)(steps);
    float2 wnd = float2(d2.y, d2.z);
    if (dot(wnd, wnd) < 0.00000001)
        wnd = float2(1.0, 0.0);
    wnd = normalize(wnd) * d2.x * u_Time;
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0, 1.0), 6.0);
    float3 sunCol = float3(1.2, 1.1, 1.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    float nScale = max(d1.z, 0.001);
    float3 acc = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        float3 q = p * nScale + float3(wnd.x, 0.0, wnd.y);
        float base = (u_VolFast == 1) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0, 1.0);
        float d = smoothstep(1.0 - cov, 1.0 - cov + 0.35, base);
        if (d > 0.001 && d1.w > 0.001)
        {
            float det = wl_vol_noise(q * 3.7 + float3(0.0, u_Time * d2.x * 0.5, 0.0));
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
                float3 sp = p + u_SunDirection * dt * 2.0;
                float sd = wl_vol_noise(sp * nScale + float3(wnd.x, 0.0, wnd.y)) * max(d1.y, 0.0);
                sh = exp(-sd * dt * 4.0 * clamp(d3.y, 0.0, 1.0));
            }
            float3 scol = tint * (amb * (0.35 + 0.65 * sh) + sunCol * sh * 0.9)
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
    return float4(acc, trans);
}

float3 wl_apply_volumetrics(float3 ro, float3 rd, float tHit, float3 baseColor, float dither, float sunVis)
{
    float3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5)
                continue;
            float4 r = wl_march_cloud_vol(ro, rd, tHit, u_CloudMin[i], u_CloudMax[i],
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
            float4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
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
float4 wl_tex_slot(int slot, float2 uv)
{
    // SampleLevel(0) instead of Sample() to avoid implicit-gradient (ddx/ddy)
    // instructions inside the relief POM march loop, whose iteration count
    // varies per-pixel — that triggers X3570 on the gradient Sample path.
    switch (slot)
    {
        case 0:  return u_Textures[0].SampleLevel(u_Sampler, uv, 0.0f);
        case 1:  return u_Textures[1].SampleLevel(u_Sampler, uv, 0.0f);
        case 2:  return u_Textures[2].SampleLevel(u_Sampler, uv, 0.0f);
        case 3:  return u_Textures[3].SampleLevel(u_Sampler, uv, 0.0f);
        case 4:  return u_Textures[4].SampleLevel(u_Sampler, uv, 0.0f);
        case 5:  return u_Textures[5].SampleLevel(u_Sampler, uv, 0.0f);
        case 6:  return u_Textures[6].SampleLevel(u_Sampler, uv, 0.0f);
        case 7:  return u_Textures[7].SampleLevel(u_Sampler, uv, 0.0f);
        case 8:  return u_Textures[8].SampleLevel(u_Sampler, uv, 0.0f);
        case 9:  return u_Textures[9].SampleLevel(u_Sampler, uv, 0.0f);
        case 10: return u_Textures[10].SampleLevel(u_Sampler, uv, 0.0f);
        case 11: return u_Textures[11].SampleLevel(u_Sampler, uv, 0.0f);
        case 12: return u_Textures[12].SampleLevel(u_Sampler, uv, 0.0f);
        case 13: return u_Textures[13].SampleLevel(u_Sampler, uv, 0.0f);
        case 14: return u_Textures[14].SampleLevel(u_Sampler, uv, 0.0f);
        case 15: return u_Textures[15].SampleLevel(u_Sampler, uv, 0.0f);
        case 16: return u_Textures[16].SampleLevel(u_Sampler, uv, 0.0f);
        case 17: return u_Textures[17].SampleLevel(u_Sampler, uv, 0.0f);
        case 18: return u_Textures[18].SampleLevel(u_Sampler, uv, 0.0f);
        case 19: return u_Textures[19].SampleLevel(u_Sampler, uv, 0.0f);
        case 20: return u_Textures[20].SampleLevel(u_Sampler, uv, 0.0f);
        case 21: return u_Textures[21].SampleLevel(u_Sampler, uv, 0.0f);
        case 22: return u_Textures[22].SampleLevel(u_Sampler, uv, 0.0f);
        case 23: return u_Textures[23].SampleLevel(u_Sampler, uv, 0.0f);
        case 24: return u_Textures[24].SampleLevel(u_Sampler, uv, 0.0f);
        case 25: return u_Textures[25].SampleLevel(u_Sampler, uv, 0.0f);
        case 26: return u_Textures[26].SampleLevel(u_Sampler, uv, 0.0f);
        case 27: return u_Textures[27].SampleLevel(u_Sampler, uv, 0.0f);
        case 28: return u_Textures[28].SampleLevel(u_Sampler, uv, 0.0f);
        case 29: return u_Textures[29].SampleLevel(u_Sampler, uv, 0.0f);
        case 30: return u_Textures[30].SampleLevel(u_Sampler, uv, 0.0f);
        case 31: return u_Textures[31].SampleLevel(u_Sampler, uv, 0.0f);
        default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

// Procedural craggy breakup + strata banding. Added to final/gradient/shadow
// taps only (never the march), so detail rides on the macro silhouette.
float wl_relief_meso(float2 uv, float3 wp, float amp, float strataW)
{
    if (amp <= 0.001)
        return 0.0;
    float2 muv = uv * 6.0;
    float n1 = wl_vol_noise(float3(muv, 3.7));
    float n2 = wl_vol_noise(float3(muv * 2.3 + 5.0, 9.1));
    float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
    return ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * amp
         + (st - 0.5) * amp * 0.8 * strataW;
}

float wl_relief_height(int slot, float2 uv, float3 n, float neuralK)
{
    float h = dot(wl_tex_slot(slot, uv).rgb, float3(0.299, 0.587, 0.114));
    if (neuralK > 0.001)
        h += wl_neural_micro(uv, n) * 2.0 * neuralK;
    return clamp(h, 0.0, 1.5);
}

// Analytic component lights for the raster preview path. Mirrors the Nova
// evaluation (same falloff/cone/area model) but WITHOUT shadow rays — the
// raster path has no shadow maps; true shadows live in Nova.
float3 wl_analytic_lights(float3 albedo, float metallic, float roughness, float3 N, float3 wp)
{
    float3 V = normalize(u_CameraPosition - wp);
    float3 F0 = lerp_safe(float3(0.04, 0.04, 0.04), albedo, clamp(metallic, 0.0, 1.0));
    float rough = clamp(roughness, 0.04, 1.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    for (int a = 0; a < 8; a++)
    {
        if (a >= u_ALightCount) break;
        float atype = u_ALightData[a].x;
        float3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0);
        float afall = max(u_ALightData[a].w, 0.5);
        float3 L;
        float att = 1.0;
        if (atype < 0.5) {
            L = -u_ALightDir[a];
        } else {
            float3 toL = u_ALightPos[a] - wp;
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
        float3 H = normalize(V + L);
        float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float spec = pow(max(dot(N, H), 0.0), lerp_safe(128.0, 8.0, rough));
        acc += (albedo * (1.0 - metallic) * NdotL + F * spec * NdotL) * lcol * att;
    }
    return acc;
}

[earlydepthstencil]
PSOutput main(PSInput Input)
{
    PSOutput o;
    o.Depth = Input.Position.z; // fragments that never write depth keep it
    float4x4 u_ViewProjection_PS = float4x4(Input.u_VPRow0, Input.u_VPRow1,
                                            Input.u_VPRow2, Input.u_VPRow3);
    float3 lightDir = normalize(float3(0.3, 1.0, 0.4));
    float3 norm0 = normalize(Input.Normal);
    float3 V = normalize(u_CameraPosition - Input.WorldPos);
    // Water marker: water verts arrive with BumpStrength = -1 (the relief
    // path above already skipped them: its gate needs values > 0.001).

    // ---- Neural relief: UV-space height march (parallax) + gradient normal
    // + light self-shadow. Distance- and grazing-faded, zero taps when the
    // material opts out (Displacement and BumpStrength both 0).
    float2 reliefUV = Input.TexCoord * Input.TilingFactor;
    float3 norm = norm0;
    float reliefShadow = 1.0;
    float reliefPush = 0.0;
    float reliefH = -1.0;
    float mesoAmp = 0.0;
    float strataW = clamp(1.0 - abs(norm0.y), 0.15, 1.0);
    {
        float disp = Input.Displacement;
        float bump = Input.BumpStrength;
        int slot = (int)(Input.TexIndex);
        float dist = length(u_CameraPosition - Input.WorldPos);
        float fade = clamp(1.0 - (dist - 18.0) / 30.0, 0.0, 1.0);
        float ndv = abs(dot(V, norm0));
        // Same angular collapse as Nova: kill POM smear streaks edge-on.
        float marchK = smoothstep(0.10, 0.45, ndv);
        // Tangent frame from screen-space derivatives: uniform control flow,
        // so no undefined-derivative issues inside the gated march below.
        float3 dpx = ddx(Input.WorldPos);
        float3 dpy = ddy(Input.WorldPos);
        float2 dtx = ddx(reliefUV);
        float2 dty = ddy(reliefUV);
        float3 T = dpx * dty.y - dpy * dtx.y;
        T = normalize(T - norm0 * dot(norm0, T) + float3(1e-6, 1e-6, 1e-6));
        float3 B = normalize(cross(norm0, T));
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
                float3 viewTS = float3(dot(V, T), dot(V, B), dot(V, norm0));
                int layers = (int)(lerp_safe(14.0, 6.0, ndv));
                float2 stepUV = (-viewTS.xy / max(viewTS.z, 0.08)) * (pScale / (float)(layers));
                float dh = 1.0 / (float)(layers);
                float2 cuv = reliefUV;
                float clH = 1.0;
                float cTexH = finalH;
                float2 puv = cuv;
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
                    float2 midUV = (puv + cuv) * 0.5;
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
                float2 delta = (lerp_safe(puv, cuv, rw) - reliefUV) * marchK;
                if (length(delta) > 0.08)
                    delta = normalize(delta) * 0.08;
                reliefUV += delta;
                finalH = lerp_safe(pTexH, cTexH, rw);
                finalH = clamp(finalH + wl_relief_meso(reliefUV, Input.WorldPos, mesoAmp, strataW), 0.0, 1.5);
                reliefH = finalH;
                // Shape-preserving push (bilateral around the mid-level, like
                // Nova): the base shape survives, features rise and recess.
                reliefPush = (finalH - 0.5) * ds;
            }
            if (bs > 0.001)
            {
                float e = 0.004;
                float hU = wl_relief_height(slot, reliefUV + float2(e, 0.0), norm0, neuralK)
                         + wl_relief_meso(reliefUV + float2(e, 0.0), Input.WorldPos, mesoAmp, strataW);
                float hV = wl_relief_height(slot, reliefUV + float2(0.0, e), norm0, neuralK)
                         + wl_relief_meso(reliefUV + float2(0.0, e), Input.WorldPos, mesoAmp, strataW);
                float dhdu = (hU - finalH) / e;
                float dhdv = (hV - finalH) / e;
                // UV-per-world measured per pixel: raster faces span 0-1 UV
                // over arbitrary world sizes, so Nova's ~1 UV/m assumption
                // does not hold here.
                float worldPerUV = clamp(length(dpx) / max(length(dtx), 1e-6), 0.05, 200.0);
                float3 grad = (T * dhdu + B * dhdv) * (bs / worldPerUV);
                float gl = length(grad);
                if (gl > 1.5)
                    grad /= gl / 1.5;
                norm = normalize(norm0 - grad);
            }
            if (ds > 0.001)
            {
                // Cheap heightfield self-shadow: 4 taps toward the light.
                float3 lts = float3(dot(lightDir, T), dot(lightDir, B), dot(lightDir, norm0));
                float2 sStep = (lts.xy / max(lts.z, 0.1)) * (pScale / 4.0);
                float rayH = finalH + 0.03;
                float occ = 1.0;
                float2 suv = reliefUV;
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
        float4 dispClip = mul(u_ViewProjection_PS, float4(Input.WorldPos + norm0 * reliefPush, 1.0));
        o.Depth = (dispClip.z / dispClip.w) * 0.5 + 0.5;
    }

    float4 texColor = Input.Color;

    // Evaluate hardware texture slots using an integer cast selector
    switch((int)(Input.TexIndex))
    {
        case 0:  texColor *= TEX(u_Textures[0],  reliefUV); break;
        case 1:  texColor *= TEX(u_Textures[1],  reliefUV); break;
        case 2:  texColor *= TEX(u_Textures[2],  reliefUV); break;
        case 3:  texColor *= TEX(u_Textures[3],  reliefUV); break;
        case 4:  texColor *= TEX(u_Textures[4],  reliefUV); break;
        case 5:  texColor *= TEX(u_Textures[5],  reliefUV); break;
        case 6:  texColor *= TEX(u_Textures[6],  reliefUV); break;
        case 7:  texColor *= TEX(u_Textures[7],  reliefUV); break;
        case 8:  texColor *= TEX(u_Textures[8],  reliefUV); break;
        case 9:  texColor *= TEX(u_Textures[9],  reliefUV); break;
        case 10: texColor *= TEX(u_Textures[10], reliefUV); break;
        case 11: texColor *= TEX(u_Textures[11], reliefUV); break;
        case 12: texColor *= TEX(u_Textures[12], reliefUV); break;
        case 13: texColor *= TEX(u_Textures[13], reliefUV); break;
        case 14: texColor *= TEX(u_Textures[14], reliefUV); break;
        case 15: texColor *= TEX(u_Textures[15], reliefUV); break;
        case 16: texColor *= TEX(u_Textures[16], reliefUV); break;
        case 17: texColor *= TEX(u_Textures[17], reliefUV); break;
        case 18: texColor *= TEX(u_Textures[18], reliefUV); break;
        case 19: texColor *= TEX(u_Textures[19], reliefUV); break;
        case 20: texColor *= TEX(u_Textures[20], reliefUV); break;
        case 21: texColor *= TEX(u_Textures[21], reliefUV); break;
        case 22: texColor *= TEX(u_Textures[22], reliefUV); break;
        case 23: texColor *= TEX(u_Textures[23], reliefUV); break;
        case 24: texColor *= TEX(u_Textures[24], reliefUV); break;
        case 25: texColor *= TEX(u_Textures[25], reliefUV); break;
        case 26: texColor *= TEX(u_Textures[26], reliefUV); break;
        case 27: texColor *= TEX(u_Textures[27], reliefUV); break;
        case 28: texColor *= TEX(u_Textures[28], reliefUV); break;
        case 29: texColor *= TEX(u_Textures[29], reliefUV); break;
        case 30: texColor *= TEX(u_Textures[30], reliefUV); break;
        case 31: texColor *= TEX(u_Textures[31], reliefUV); break;
    }

    float metallic = clamp(Input.Metallic, 0.0, 1.0);
    float roughness = clamp(Input.Roughness, 0.04, 1.0);

    // Relief weathering: dust settles in pits, sun bleaches ridge tops.
    if (reliefH >= 0.0)
    {
        float cav = smoothstep(0.45, 0.05, reliefH);
        float ridge = smoothstep(0.55, 0.95, reliefH);
        texColor.rgb *= (1.0 - 0.30 * cav);
        texColor.rgb = lerp_safe(texColor.rgb, texColor.rgb * float3(1.10, 1.03, 0.92), ridge * 0.65);
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
        float2 wpuv = Input.WorldPos.xz;
        float wdist = length(u_CameraPosition - Input.WorldPos);
        float wfade = clamp(1.0 - (wdist - 25.0) / 60.0, 0.0, 1.0);
        if (wfade > 0.001)
        {
            float wt = u_Time;
            float2 wg = float2(0.0, 0.0);
            wg += float2(0.96, 0.28) * (cos(dot(float2(0.96, 0.28), wpuv) * 2.1 + wt * 1.6) * 0.22);
            wg += float2(-0.42, 0.91) * (cos(dot(float2(-0.42, 0.91), wpuv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
            wg += float2(0.66, -0.75) * (cos(dot(float2(0.66, -0.75), wpuv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
            norm = normalize(norm + float3(-wg.x, 0.0, -wg.y) * (0.55 * wfade));
        }
        metallic = 1.0;
    }

    // Metallic PBR terms (shared by neural + classic paths). Diffuse dies
    // out on metals; reflection + specular take over. No shadow maps on the
    // raster path — true shadows live in the Nova ray-trace pipeline.
    float3 F0 = lerp_safe(float3(0.04, 0.04, 0.04), texColor.rgb, metallic);
    float3 R = reflect(-V, norm);
    float3 specSky = lerp_safe(u_SkyBottomColor, u_SkyTopColor, R.y * 0.5 + 0.5);
    float3 diffSky = lerp_safe(u_SkyBottomColor, u_SkyTopColor, norm.y * 0.5 + 0.5);
    float3 envColor = lerp_safe(specSky, (specSky + diffSky) * 0.5, roughness * roughness);
    float NdotL = max(dot(norm, lightDir), 0.0);
    float3 H = normalize(lightDir + V);
    float specTerm = pow(max(dot(norm, H), 0.0), lerp_safe(128.0, 8.0, roughness))
                   * (1.0 - roughness * 0.7) * reliefShadow;
    float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    float diffuseIntensity = NdotL * reliefShadow;
    float ambientIntensity = 0.2;

    if (u_NeuralEnabled == 1)
    {
        if (!wlWater)
        {
            float3 detail = wl_neural_tex_detail(reliefUV, norm);
            texColor.rgb *= lerp_safe(float3(1.0, 1.0, 1.0), detail, clamp(u_NeuralTexStrength, 0.0, 1.0));
        }

        float3 namb = wl_neural_ambient(norm, texColor.rgb);
        float k = clamp(u_NeuralLightStrength, 0.0, 1.0);
        float km = clamp(u_NeuralMatStrength, 0.0, 1.0);
        // Neural material: porosity/patina albedo, wrap diffuse, flake glints
        float matGlint = 0.0;
        float matWrap = 0.5;
        float3 matAlbedo = texColor.rgb;
        {
            float g;
            float w;
            wl_neural_mat_raster(reliefUV, norm, matAlbedo, g, w);
            matGlint = g;
            matWrap = w;
        }
        if (!wlWater)
            texColor.rgb = lerp_safe(texColor.rgb, matAlbedo, km);
        // Re-derive F0 from the neural albedo so flakes tint reflections.
        float3 nF0 = lerp_safe(float3(0.04, 0.04, 0.04), texColor.rgb, metallic);
        float3 nF = nF0 + (1.0 - nF0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float wrapDiffuse = clamp((dot(norm, lightDir) + matWrap * km) / (1.0 + matWrap * km), 0.0, 1.0);
        float diffTerm = lerp_safe(diffuseIntensity, wrapDiffuse, km * 0.7) * (1.0 - metallic);
        float3 warmLight = float3(1.0, 0.98, 0.94);
        float3 lit = texColor.rgb * (diffTerm * warmLight + lerp_safe(float3(ambientIntensity, ambientIntensity, ambientIntensity), namb * 2.2, k) * (1.0 - metallic * 0.85));
        lit += nF * envColor * metallic;
        lit += specTerm * nF * NdotL;
        lit += matGlint * km * (0.25 + 0.75 * metallic) * diffuseIntensity * float3(1.3, 1.25, 1.15);
        lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
        {
            float3 vdir = Input.WorldPos - u_CameraPosition;
            float vdist = length(vdir);
            lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(Input.Position.xy), 1.0);
        }
        o.Color = float4(lit, texColor.a);
        o.EntityID = Input.v_EntityID;
        return o;
    }

    float3 diffTerm = texColor.rgb * (1.0 - metallic) * (ambientIntensity + diffuseIntensity);
    float3 lit = diffTerm + F * envColor * metallic + specTerm * F * NdotL;
    lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
    {
        float3 vdir = Input.WorldPos - u_CameraPosition;
        float vdist = length(vdir);
        lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(Input.Position.xy), 1.0);
    }

    o.Color = float4(lit, texColor.a);
    o.EntityID = Input.v_EntityID;
    return o;
}
