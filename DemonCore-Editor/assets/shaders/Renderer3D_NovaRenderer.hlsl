// ============================================================================
// Renderer3D_NovaRenderer.hlsl -- HLSL (SM 5.0) port of
// Renderer3D_NovaRenderer.glsl for the NVRHI D3D11/D3D12 paths.
//
// Functional parity notes (vs the GLSL compute original):
//  - Identical pass structure (u_PassID 0/1/2/4/5/6/7/8/9/10), identical math:
//    BVH traversal, PBR direct lighting, neural MLP blocks, Gerstner water,
//    volumetric fog/clouds, temporal accumulation, bilateral blur, bloom,
//    composite/upscale, god rays, Sobel material maps.
//  - Matrices are column_major, so GLSL M*v == HLSL mul(M, v) exactly.
//  - GLSL mod/fract/mix/textureLod/image* are remapped to GLSL-semantics
//    helpers below (mod keeps GLSL wrap semantics: x - y*floor(x/y)).
//  - SM5 has no dynamic texture-array indexing: u_SceneTextures[i] goes
//    through TEX_SLOT_IDX (32-way switch), matching the raster shader.
//  - The POM UV-space march is not portable (needs dynamically-indexed
//    texture reads per march tap); ApplyPOM keeps push, bump-normal,
//    self-occlusion and sun-shadow evaluated at the base uv, and the
//    screen-space ray anchor keeps displaced hits ray-correct.
//  - Resources keep the GL binding numbers as register slots so the C++
//    wiring maps 1:1 (StructuredBuffer t1/t2/t3 = SSBOs 1/2/3,
//    RWTexture u# = image units, Texture t# = sampler units).
// Native DX11 dispatch uses immutable SRV snapshots at t64..t71.
// Never bind an image as both SRV and UAV; DX11Nova owns snapshots.
// ============================================================================

// ---- GPU structs (std430 layouts, column-major matrices) ----
struct RayTracingInstance
{
    column_major float4x4 InvTransform;
    column_major float4x4 WorldTransform;
    float4 Albedo;
    float4 MaterialParams;
    float4 Min;
    float4 Max;
    float4 Emission;
    float  MaxDistance;
    int    LODLevel;
    int    TextureID;
    int    PackedMaterialMapID;
    float4 TextureScale;
    float4 DisplacementParams;
    float4 WaveParams;  // x=amplitude, y=wavelength, z=speed, w=chop; water only
};

struct BVHNode
{
    float4 MinBounds; // xyz = AABB min, w = leftChild (inner) or -(firstInstance+1) (leaf)
    float4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
};

struct Ray { float3 Origin; float3 Direction; };
Ray MakeRay(float3 o, float3 d) { Ray r; r.Origin = o; r.Direction = d; return r; }

// ---- images (GL image units -> u registers) ----
RWTexture2D<float4> img_Output        : register(u0);
RWTexture2D<float4> img_Accumulation  : register(u1);
RWTexture2D<float4> img_Bloom         : register(u2);
RWTexture2D<float2> img_Velocity      : register(u5);
RWTexture2D<float4> img_TraceGBuffer  : register(u6);
RWTexture2D<float4> img_Bloom_Temp    : register(u7);
RWTexture2D<unorm float4> img_MaterialPacked : register(u3);
RWTexture2D<float4> img_AlbedoHit     : register(u4);

// ---- scene data (GL SSBO bindings 1/2/3 -> t registers) ----
StructuredBuffer<RayTracingInstance> Instances    : register(t1);
StructuredBuffer<BVHNode>            BVHNodes     : register(t2);
StructuredBuffer<int>                LightIndices : register(t3);

// ---- sampled buffers (GL sampler units shifted to t48+ to avoid
// colliding with the StructuredBuffer registers t1/t2/t3) ----
Texture2D s_Accumulation  : register(t48);
Texture2D s_DepthBuffer   : register(t49);
Texture2D s_Output        : register(t50);
Texture2D s_NormalBuffer  : register(t51);
Texture2D s_InputAlbedo   : register(t52);
// Declared in GLSL but unread (dead bindings): s_Bloom, s_Indirect,
// s_PackedMaterialMap -- omitted here; the future NVRHI compute wiring
// must skip them.
SamplerState s_Sampler : register(s0);

// ---- scene texture array (GL units 15..46 -> t15..) ----
Texture2D u_SceneTextures[32] : register(t15);

// ---- per-frame constants (GL individual uniforms -> one cbuffer) ----
// Nothing external reads this buffer, so HLSL's internal packing is
// self-consistent regardless of GL uniform layout.
cbuffer NovaConstants : register(b1)
{
    float3  u_CameraPosition;
    float4x4 u_InverseViewProjection;
    float4x4 u_PrevViewProjection;
    int     u_InstanceCount;
    int     u_FrameIndex;
    // DEAD UNIFORM (D3): declared and uploaded every frame, but never READ by any
    // pass. There is no per-pixel sample loop and no bounce-depth loop that
    // consults it; the real controls are u_IndirectRays (ray count, read at the
    // indirect-bounce loop) and the preset's own indirect-ray tiering.
    //
    // u_MaxBounces = 1 at EVERY preset, so wiring it to a loop would hardcode one
    // bounce and silently disable indirect lighting.
    //
    // KEPT IN PLACE deliberately: this is a packed constant buffer where every
    // declaration occupies a fixed slot. Removing it would shift the offset of
    // every uniform after it and require re-deriving the CPU-side
    // D3D12_SHADER_VARIABLE_DESC offsets. The upload is cheap; a wrong offset is
    // not. See references/dx12-compute-submission-safety.md and the Phase -1
    // notes in the AAA upgrade plan.
    int     u_SamplesPerPixel;   // UNUSED -- see note above
    int     u_PassID;
    int     u_DepthBuffer;
    float2  u_Jitter;
    float2  u_PrevJitter;
    float   u_CameraMoved;
    float4x4 u_ViewProjection;
    float3  u_SkyBottomColor;
    float3  u_SkyTopColor;
    float   u_AccumulationAlpha;
    float   u_NormalStrength;
    float   u_RoughnessBias;
    float   u_AOIntensity;
    int     u_QualityLevel;

    // POST-PROCESSING UNIFORMS (reconstructed; see Phase -1 D3). These were
    // declared and reachable from editor sliders but read by no pass: vignette,
    // chromatic aberration and colour grading had no implementation here at all,
    // and the bloom threshold was hardcoded (0.8 in RunComposite, 0.5 in
    // RunBloomThreshold). RunComposite below now reads them.
    int      u_BloomEnabled;
    float    u_BloomIntensity;
    float    u_BloomThreshold;
    int      u_VignetteEnabled;
    float    u_VignetteIntensity;
    float    u_VignetteRadius;
    float    u_VignetteSoftness;
    int      u_ChromaticAberrationEnabled;
    float    u_ChromaticAberrationIntensity;
    int      u_ColorGradingEnabled;
    float    u_ColorTemperature;   // -1..1, negative shifts cool
    float    u_ColorTint;          // -1..1, negative shifts green
    float    u_ColorGamma;
    int      u_FilmGrainEnabled;
    float    u_FilmGrainIntensity;
    int     u_MaxBounces;   // UNUSED -- see the note at u_SamplesPerPixel
    int     u_MaxLights;
    int     u_IndirectRays;
    int     u_StepSize;
    float   u_RenderScale;
    int     u_LightCount;
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
    // Global atmospheric fog
    int     u_GlobalFogEnabled;
    float   u_GlobalFogDensity;
    float   u_GlobalFogHeightFalloff;
    float3  u_GlobalFogColor;
    float   u_GlobalFogBaseHeight;
    float   u_GlobalFogDistAtten;
    // Screen-space god rays
    int     u_GodRaysEnabled;
    float2  u_GodSunScreenPos;
    int     u_GodSunValid;
    float   u_GodRayIntensity;
    float   u_GodRayDecay;
    int     u_GodRaySamples;
    float   u_GodRayDensity;
    float3  u_GodRayColor;
    // Analytic component lights
    int     u_ALightCount;
    float3  u_ALightPos[8];
    float3  u_ALightDir[8];
    float3  u_ALightColor[8];
    float4  u_ALightData[8];
    float4  u_ALightData2[8];
};

// ---- GLSL-semantics helpers ----
float  fract2(float x)                { return x - floor(x); }
float2 fract2(float2 x)               { return x - floor(x); }
float3 fract2(float3 x)               { return x - floor(x); }
float4 fract2(float4 x)               { return x - floor(x); }
float  fmod_safe(float x, float y)    { return x - y * floor(x / y); }  // GLSL mod wrap
float  lerp_safe(float a, float b, float t)    { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float t)  { return lerp(a, b, t); }
float2 lerp_safe(float2 a, float2 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
float4 lerp_safe(float4 a, float4 b, float t)  { return lerp(a, b, t); }

// image helpers: GLSL imageLoad/imageStore/imageSize on RWTexture2D
#define IMG_STORE(tex, pos, val) (tex)[(pos)] = (val)
Texture2D<float4> read_img_Output : register(t64);
Texture2D<float4> read_img_Accumulation : register(t65);
Texture2D<float4> read_img_Bloom : register(t66);
Texture2D<float4> read_img_MaterialPacked : register(t67);
Texture2D<float4> read_img_AlbedoHit : register(t68);
Texture2D<float2> read_img_Velocity : register(t69);
Texture2D<float4> read_img_TraceGBuffer : register(t70);
Texture2D<float4> read_img_Bloom_Temp : register(t71);
#define IMG_LOAD(tex, pos) read_##tex.Load(int3(clamp(int2(pos), int2(0,0), int2(g_dims)-1),0))
#define IMG_SIZE(tex)            (g_dims)

// texture helpers: compute shaders have no implicit LOD -> SampleLevel
#define TEX(tex, uv)        (tex).SampleLevel(s_Sampler, (uv), 0.0f)
#define TEX_LOD(tex, uv, l) (tex).SampleLevel(s_Sampler, (uv), (l))

// SM5 forbids dynamically indexing a texture array; dispatch through a
// switch of constant indices (same approach as the raster shader).
float4 TEX_SLOT_IDX(int idx, float2 uv, float lod)
{
    switch (idx)
    {
        case 0:  return u_SceneTextures[0].SampleLevel(s_Sampler, uv, lod);
        case 1:  return u_SceneTextures[1].SampleLevel(s_Sampler, uv, lod);
        case 2:  return u_SceneTextures[2].SampleLevel(s_Sampler, uv, lod);
        case 3:  return u_SceneTextures[3].SampleLevel(s_Sampler, uv, lod);
        case 4:  return u_SceneTextures[4].SampleLevel(s_Sampler, uv, lod);
        case 5:  return u_SceneTextures[5].SampleLevel(s_Sampler, uv, lod);
        case 6:  return u_SceneTextures[6].SampleLevel(s_Sampler, uv, lod);
        case 7:  return u_SceneTextures[7].SampleLevel(s_Sampler, uv, lod);
        case 8:  return u_SceneTextures[8].SampleLevel(s_Sampler, uv, lod);
        case 9:  return u_SceneTextures[9].SampleLevel(s_Sampler, uv, lod);
        case 10: return u_SceneTextures[10].SampleLevel(s_Sampler, uv, lod);
        case 11: return u_SceneTextures[11].SampleLevel(s_Sampler, uv, lod);
        case 12: return u_SceneTextures[12].SampleLevel(s_Sampler, uv, lod);
        case 13: return u_SceneTextures[13].SampleLevel(s_Sampler, uv, lod);
        case 14: return u_SceneTextures[14].SampleLevel(s_Sampler, uv, lod);
        case 15: return u_SceneTextures[15].SampleLevel(s_Sampler, uv, lod);
        case 16: return u_SceneTextures[16].SampleLevel(s_Sampler, uv, lod);
        case 17: return u_SceneTextures[17].SampleLevel(s_Sampler, uv, lod);
        case 18: return u_SceneTextures[18].SampleLevel(s_Sampler, uv, lod);
        case 19: return u_SceneTextures[19].SampleLevel(s_Sampler, uv, lod);
        case 20: return u_SceneTextures[20].SampleLevel(s_Sampler, uv, lod);
        case 21: return u_SceneTextures[21].SampleLevel(s_Sampler, uv, lod);
        case 22: return u_SceneTextures[22].SampleLevel(s_Sampler, uv, lod);
        case 23: return u_SceneTextures[23].SampleLevel(s_Sampler, uv, lod);
        case 24: return u_SceneTextures[24].SampleLevel(s_Sampler, uv, lod);
        case 25: return u_SceneTextures[25].SampleLevel(s_Sampler, uv, lod);
        case 26: return u_SceneTextures[26].SampleLevel(s_Sampler, uv, lod);
        case 27: return u_SceneTextures[27].SampleLevel(s_Sampler, uv, lod);
        case 28: return u_SceneTextures[28].SampleLevel(s_Sampler, uv, lod);
        case 29: return u_SceneTextures[29].SampleLevel(s_Sampler, uv, lod);
        case 30: return u_SceneTextures[30].SampleLevel(s_Sampler, uv, lod);
        case 31: return u_SceneTextures[31].SampleLevel(s_Sampler, uv, lod);
        default: return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

// Screen-space displacement anchor (moved out of TestInstanceHit so the
// port compiles under SM5's stricter inlining/flow rules).
#define WL_RAYANCHOR(ray, info, pomPush) \
    do { \
        float pomPushAbs = abs(pomPush); \
        if (pomPushAbs > 0.00001f) { \
            float pomDenom = dot(ray.Direction, info.normal); \
            if (abs(pomDenom) > 0.0001f) { \
                float tAnchor = dot(info.worldPos - ray.Origin, info.normal) / pomDenom; \
                if (tAnchor > 0.0f && tAnchor < info.t + pomPushAbs + 0.001f) { \
                    info.t = tAnchor; \
                    info.worldPos = ray.Origin + ray.Direction * tAnchor; \
                } \
            } \
        } \
    } while (0)

// ---- per-thread mutable state (GLSL file-scope mutables) ----
static uint3 g_dt   = uint3(0, 0, 0);
static uint2 g_dims = uint2(0, 0);
static uint  seed   = 0; // re-seeded per pixel in RunTraceAndDenoise

// Per-thread BVH traversal stacks (groupshared: fxc cannot compile a
// dynamically-indexed local array inside the traversal call graph).
// 64 threads/group * 32 slots; each thread touches only its own 32 slots,
// so no GroupMemoryBarrier is required for correctness.
groupshared int gs_trav_stack[64 * 32];
static uint g_tid = 0; // flattened thread index within the group, set in main

// r=NDC depth (1=sky), gba=packed normal
// RGB = albedo, A = hitMask



























// Neural Rendering (OpenGL): tiny in-shader MLP radiance cache + texture detail.
// u_NeuralEnabled gates both; strengths blend 0 (classic path) -> 1 (full neural).



// Neural material blend. Defaults to 0 (classic PBR) until Renderer3D uploads it.


// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)



// God-ray sun color (directional light color x intensity, or warm default).
















// 1 = reduced noise octaves (Low/Medium quality presets).


// Global atmospheric fog (distance haze everywhere)







// Screen-space god rays (radial blur from sun)









// Analytic component lights (Renderer3D uploads; locations skipped if absent).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)
// u_ALightDir = travel direction (dir/spot) or rect normal facing scene (area).







float wl_softsign(float x) { return x / (1.0f + abs(x)); }

// Reduced-resolution tracing: passes 0/1/2/7 dispatch at ComputeWidth/Height
// (= full size * u_RenderScale) into the corner subregion, which RunComposite
// upscales. UV math must use the dispatched extent, not the full image.
// Identical to IMG_SIZE() at scale 1, so lower presets are unaffected.
int2 wl_trace_extent(int2 fullSize)
{
    int2 t = int2(float2(fullSize) * u_RenderScale);
    return int2(max(t.x, 1), max(t.y, 1));
}

// Trace G-buffers (depth/normal) live full-size with valid data in the trace
// subregion; trace-space UVs span [0,1] across dispatched pixels, so scale
// them to buffer space when sampling. Identity at scale 1.
float2 wl_buf_uv(float2 traceUV) { return traceUV * u_RenderScale; }

float wl_hash12(float2 p)
{
    float3 p3 = fract2(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return fract2((p3.x + p3.y) * p3.z);
}

// Neural texture detail: sinusoidal UV encoding x normal -> 4-unit MLP -> ~1.0 multiplier.
float3 wl_neural_tex_detail(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    float4 feat = float4(sin(s1 + n.x * 3.1f),
                     sin(s1 * 1.7f + n.y * 2.3f),
                     sin(s2 + n.z * 3.7f),
                     sin(s2 * 1.3f + s1 * 0.7f));
    float h0 = wl_softsign( 0.9f * feat.x - 0.7f * feat.y + 0.5f * feat.z - 0.3f * feat.w + 0.15f);
    float h1 = wl_softsign(-0.6f * feat.x + 0.8f * feat.y - 0.4f * feat.z + 0.6f * feat.w - 0.10f);
    float h2 = wl_softsign( 0.4f * feat.x + 0.5f * feat.y + 0.9f * feat.z + 0.2f * feat.w + 0.05f);
    float h3 = wl_softsign(-0.3f * feat.x - 0.5f * feat.y + 0.6f * feat.z - 0.8f * feat.w + 0.20f);
    float grain = wl_hash12(floor(uv * 64.0f)) - 0.5f;
    float d = 1.0f + 0.10f * h0 + 0.07f * h1 - 0.06f * h2 + 0.05f * h3 + 0.06f * grain;
    float3 tint = float3(1.0f + 0.03f * h1, 1.0f + 0.02f * h2, 1.0f - 0.03f * h0);
    return tint * d;
}

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain): the grain
// stays in the albedo tint where it sparkles, but must not imprint voxel
// cells into displaced height.
float wl_neural_micro(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    float4 feat = float4(sin(s1 + n.x * 3.1f),
                     sin(s1 * 1.7f + n.y * 2.3f),
                     sin(s2 + n.z * 3.7f),
                     sin(s2 * 1.3f + s1 * 0.7f));
    float h0 = wl_softsign( 0.9f * feat.x - 0.7f * feat.y + 0.5f * feat.z - 0.3f * feat.w + 0.15f);
    float h1 = wl_softsign(-0.6f * feat.x + 0.8f * feat.y - 0.4f * feat.z + 0.6f * feat.w - 0.10f);
    float h2 = wl_softsign( 0.4f * feat.x + 0.5f * feat.y + 0.9f * feat.z + 0.2f * feat.w + 0.05f);
    float h3 = wl_softsign(-0.3f * feat.x - 0.5f * feat.y + 0.6f * feat.z - 0.8f * feat.w + 0.20f);
    return 0.10f * h0 + 0.07f * h1 - 0.06f * h2 + 0.05f * h3;
}

// Neural radiance cache: predicts indirect bounce (reflectance * light) from
// position/normal/albedo/roughness/metal + artist sky colors. One eval
// replaces N traced rays. Diffuse lobe is suppressed on metals (they have
// ~no diffuse response); the specular lobe stays for all materials.
float3 wl_neural_indirect(float3 wp, float3 n, float3 albedo, float rough, float metal, float3 v)
{
    float3 p = wp * 0.35f;
    float4 f = float4(sin(p.x * 2.1f + n.x * 2.0f),
                  sin(p.y * 1.7f + n.y * 2.0f),
                  sin(p.z * 2.3f + n.z * 2.0f),
                  sin((p.x + p.y + p.z) * 1.3f + rough * 3.0f));
    float h0 = wl_softsign( 0.8f * f.x - 0.6f * f.y + 0.4f * f.z + 0.5f * f.w + 0.10f);
    float h1 = wl_softsign(-0.5f * f.x + 0.9f * f.y - 0.3f * f.z + 0.4f * f.w - 0.05f);
    float h2 = wl_softsign( 0.3f * f.x + 0.4f * f.y + 0.8f * f.z - 0.6f * f.w + 0.00f);
    float h3 = wl_softsign(-0.4f * f.x - 0.3f * f.y + 0.5f * f.z + 0.7f * f.w + 0.15f);
    float up = n.y * 0.5f + 0.5f;
    float3 sky = lerp_safe(u_SkyBottomColor, u_SkyTopColor, up);
    float3 diffLobe = albedo * (1.0f - metal) * sky * (0.45f + 0.30f * h0 + 0.15f * h1);
    float ndv = clamp(dot(n, v), 0.0f, 1.0f);
    float3 F0 = lerp_safe(float3(0.04f, 0.04f, 0.04f), max(albedo, float3(0.04f, 0.04f, 0.04f)), clamp(1.0f - rough, 0.0f, 1.0f) * 0.5f);
    float3 spec = F0 * sky * (0.25f + 0.35f * h2) * (0.3f + 0.7f * ndv) * (1.0f - rough * 0.7f);
    float occ = 0.85f + 0.15f * h3;
    return (diffLobe + spec) * occ;
}

// Neural material: learned spatially-varying PBR params. Encodes UV + world
// position + normal into a 4-unit MLP whose outputs drive porosity (darken +
// roughen), patina hue shift, sparse metallic flakes, and a dielectric
// clearcoat-like F0 lift. Feeds the analytic GGX BRDF, so glints stay
// view/light-correct instead of painted on.
void wl_neural_material(float2 uv, float3 wp, float3 n,
                        inout float3 albedo, inout float metal, inout float rough,
                        inout float3 f0Tint)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float3 q = wp * 0.9f;
    float4 f = float4(sin(s1 + q.x),
                  sin(s1 * 1.6f + q.y * 1.3f),
                  sin(s1 * 0.7f - q.z * 1.7f + n.x * 2.0f),
                  sin((q.x - q.y + q.z) * 1.1f + n.y * 2.0f));
    float h0 = wl_softsign( 0.8f * f.x - 0.5f * f.y + 0.6f * f.z + 0.2f * f.w + 0.05f);
    float h1 = wl_softsign(-0.4f * f.x + 0.9f * f.y - 0.2f * f.z + 0.5f * f.w - 0.10f);
    float h2 = wl_softsign( 0.3f * f.x + 0.2f * f.y + 0.7f * f.z - 0.6f * f.w + 0.35f);
    float h3 = wl_softsign(-0.5f * f.x - 0.4f * f.y + 0.4f * f.z + 0.8f * f.w + 0.00f);
    float flakeCell = step(0.75f, wl_hash12(floor(uv * 48.0f) + floor(wp.xy * 8.0f)));
    // Porosity: darken + roughen crevices
    float por = clamp(-h0, 0.0f, 1.0f);
    albedo *= (1.0f - 0.25f * por);
    rough = clamp(rough + 0.30f * por, 0.03f, 1.0f);
    // Patina: subtle hue shift
    albedo *= (float3(1.0f, 1.0f, 1.0f) + float3(-0.06f, 0.02f, 0.05f) * h1);
    // Metallic flakes: sparse cells go mirror-smooth, tinted by albedo
    float fl = flakeCell * clamp(h2 * 1.5f, 0.0f, 1.0f);
    metal = clamp(metal + fl * 0.9f, 0.0f, 1.0f);
    rough = clamp(lerp_safe(rough, 0.08f, fl), 0.03f, 1.0f);
    // Dielectric coating: warm F0 lift (clearcoat-like), suppressed on metals
    float coat = clamp(h3, 0.0f, 1.0f) * (1.0f - metal);
    f0Tint = float3(1.0f, 1.0f, 1.0f) + float3(1.2f, 0.9f, 0.6f) * coat;
}

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
float wl_vol_hash(float3 p)
{
    p = fract2(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract2((p.x + p.y) * p.z);
}

float wl_vol_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = fract2(p);
    float3 u = f * f * (3.0f - 2.0f * f);
    return lerp_safe(lerp_safe(lerp_safe(wl_vol_hash(i), wl_vol_hash(i + float3(1.0f, 0.0f, 0.0f)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0f, 1.0f, 0.0f)), wl_vol_hash(i + float3(1.0f, 1.0f, 0.0f)), u.x), u.y),
               lerp_safe(lerp_safe(wl_vol_hash(i + float3(0.0f, 0.0f, 1.0f)), wl_vol_hash(i + float3(1.0f, 0.0f, 1.0f)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0f, 1.0f, 1.0f)), wl_vol_hash(i + float3(1.0f, 1.0f, 1.0f)), u.x), u.y), u.z);
}

float wl_vol_fbm(float3 p)
{
    float v = 0.0f;
    float a = 0.5f;
    for (int o = 0; o < 4; o++)
    {
        v += a * wl_vol_noise(p);
        p = p * 2.03f + float3(17.3f, 17.3f, 17.3f);
        a *= 0.5f;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
float wl_vol_fbm2(float3 p)
{
    return (0.5f * wl_vol_noise(p) + 0.25f * wl_vol_noise(p * 2.03f + float3(17.3f, 17.3f, 17.3f))) * 1.25f;
}

// Ultra-cheap single-hash noise: 1 hash call instead of 24.
float wl_vol_cheap(float3 p)
{
    p = fract2(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract2((p.x + p.y) * p.z);
}

// Slab test: returns (entry, exit) distances, negative entry range when missed.
float2 wl_box_range(float3 ro, float3 rd, float3 bmin, float3 bmax)
{
    float3 safe = rd + (float3(1.0f, 1.0f, 1.0f) - step(float3(0.00001f, 0.00001f, 0.00001f), abs(rd))) * 0.00001f;
    float3 inv = 1.0f / safe;
    float3 t0 = (bmin - ro) * inv;
    float3 t1 = (bmax - ro) * inv;
    float3 tmin = min(t0, t1);
    float3 tmax = max(t0, t1);
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    return 0.07957747f * (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosT, 0.001f), 1.5f);
}

// Front-to-back march through one fog box. Returns (scatteredLight, transmittance).
// sunVis gates the sun in-scatter (god-ray shafts): 1 = fully sunlit fog.


bool RayAABB(Ray r, float3 invDir, float3 minB, float3 maxB) 
{
    float3 t0 = (minB - r.Origin) * invDir;
    float3 t1 = (maxB - r.Origin) * invDir;
    float3 tMin = min(t0, t1);
    float3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);
    return tNear <= tFar && tFar > 0.0f;
}


float hash() 
{
    seed = seed * 747796405u + 2891336453u;
    uint result = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    result = (result >> 22u) ^ result;
    return result / 4294967295.0f;
}

float3 random_in_hemisphere(float3 normal)
{
    float u = hash();
    float v = hash();
    float z = 2.0f * v - 1.0f;
    float r = sqrt(max(1.0f - z * z, 0.0f));
    float theta = 2.0f * 3.14159265359f * u;
    float3 dir = float3(r * cos(theta), r * sin(theta), z);
    return dot(dir, normal) > 0.0f ? dir : -dir;
}

static const float PI = 3.14159265359f;

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 0.0001f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    return F0 + (1.0f - F0) * t2 * t2 * t;
}

float3 GetSkyColor(float3 dir);

float HitCube(Ray localRay, out float3 outNormal) 
{
    float3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), float3(0.00001f, 0.00001f, 0.00001f));
    float3 invDir = 1.0f / safeDir;

    float3 tMin = (float3(-0.5f, -0.5f, -0.5f) - localRay.Origin) * invDir;
    float3 tMax = (float3(0.5f, 0.5f, 0.5f) - localRay.Origin) * invDir;
    
    float3 t1 = min(tMin, tMax);
    float3 t2 = max(tMin, tMax);
    
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar  = min(min(t2.x, t2.y), t2.z);
    
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    
    if (tNear == t1.x)      outNormal = float3(-sign(localRay.Direction.x), 0.0f, 0.0f);
    else if (tNear == t1.y) outNormal = float3(0.0f, -sign(localRay.Direction.y), 0.0f);
    else                    outNormal = float3(0.0f, 0.0f, -sign(localRay.Direction.z));

    return tNear;
}

float HitSphere(Ray localRay, float radius, out float3 outNormal) 
{
    float a = dot(localRay.Direction, localRay.Direction);
    float b = 2.0f * dot(localRay.Origin, localRay.Direction);
    float c = dot(localRay.Origin, localRay.Origin) - (radius * radius);
    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return -1.0f;
    float sqrtD = sqrt(discriminant);
    float t = (-b - sqrtD) / (2.0f * a);
    if (t < 0.0f) t = (-b + sqrtD) / (2.0f * a);
    if (t > 0.0f) 
    {
        outNormal = normalize(localRay.Origin + t * localRay.Direction);
        return t;
    }
    return -1.0f;
}

// --- Blender "Add > Mesh" primitives -------------------------------------
// Port of the GLSL in Renderer3D_NovaRenderer.glsl. Shape constants must stay
// in sync with Wasteland::PrimitiveShape (Renderer/ProceduralMeshes.h), which
// is what the CPU writes into inst.MaterialParams.z.

// Axis-aligned rectangular slab in the XZ plane facing +Y. Covers both Plane
// and Grid: a grid is a plane, just tessellated.
float HitPlaneXZ(Ray localRay, float3 halfExtent, out float3 outNormal)
{
    if (abs(localRay.Direction.y) < 1e-8f) return -1.0f;

    float t = -localRay.Origin.y / localRay.Direction.y;
    if (t <= 0.0f) return -1.0f;

    float3 p = localRay.Origin + t * localRay.Direction;
    if (abs(p.x) > halfExtent.x || abs(p.z) > halfExtent.z) return -1.0f;

    outNormal = float3(0.0f, 1.0f, 0.0f);
    return t;
}

// Flat filled disc in the XZ plane facing +Y (Blender: Circle).
float HitDisc(Ray localRay, float radius, out float3 outNormal)
{
    if (abs(localRay.Direction.y) < 1e-8f) return -1.0f;

    float t = -localRay.Origin.y / localRay.Direction.y;
    if (t <= 0.0f) return -1.0f;

    float3 p = localRay.Origin + t * localRay.Direction;
    if (dot(p.xz, p.xz) > radius * radius) return -1.0f;

    outNormal = float3(0.0f, 1.0f, 0.0f);
    return t;
}

// Y-axis cylinder with capped ends (Blender: Cylinder).
float HitCylinder(Ray localRay, float radius, float halfHeight, out float3 outNormal)
{
    float best = -1.0f;
    float3 bestNormal = float3(0.0f, 0.0f, 0.0f);

    float2 o = localRay.Origin.xz;
    float2 d = localRay.Direction.xz;
    float a = dot(d, d);
    float b = 2.0f * dot(o, d);
    float c = dot(o, o) - radius * radius;

    if (a > 1e-12f)
    {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f)
        {
            float sq = sqrt(disc);
            for (int k = 0; k < 2; k++)
            {
                float t = (k == 0) ? (-b - sq) / (2.0f * a) : (-b + sq) / (2.0f * a);
                if (t <= 0.0f) continue;
                float y = localRay.Origin.y + t * localRay.Direction.y;
                if (y < -halfHeight || y > halfHeight) continue;
                if (best < 0.0f || t < best)
                {
                    best = t;
                    float3 p = localRay.Origin + t * localRay.Direction;
                    bestNormal = normalize(float3(p.x, 0.0f, p.z));
                }
            }
        }
    }

    if (abs(localRay.Direction.y) > 1e-8f)
    {
        for (int k = 0; k < 2; k++)
        {
            float planeY = (k == 0) ? halfHeight : -halfHeight;
            float t = (planeY - localRay.Origin.y) / localRay.Direction.y;
            if (t <= 0.0f) continue;
            if (best >= 0.0f && t >= best) continue;
            float3 p = localRay.Origin + t * localRay.Direction;
            if (dot(p.xz, p.xz) > radius * radius) continue;
            best = t;
            bestNormal = float3(0.0f, (k == 0) ? 1.0f : -1.0f, 0.0f);
        }
    }

    if (best <= 0.0f) return -1.0f;
    outNormal = bestNormal;
    return best;
}

// Y-axis cone with a capped base (Blender: Cone).
float HitCone(Ray localRay, float radius, float halfHeight, out float3 outNormal)
{
    float best = -1.0f;
    float3 bestNormal = float3(0.0f, 0.0f, 0.0f);

    float2 o = localRay.Origin.xz;
    float2 d = localRay.Direction.xz;

    float slope = radius / (2.0f * halfHeight);
    float k = slope * slope;

    float a = dot(d, d) - k * localRay.Direction.y * localRay.Direction.y;
    float b = 2.0f * (dot(o, d) + k * localRay.Direction.y * (halfHeight - localRay.Origin.y));
    float c = dot(o, o) - k * (halfHeight - localRay.Origin.y) * (halfHeight - localRay.Origin.y);

    if (abs(a) > 1e-12f)
    {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f)
        {
            float sq = sqrt(disc);
            for (int i = 0; i < 2; i++)
            {
                float t = (i == 0) ? (-b - sq) / (2.0f * a) : (-b + sq) / (2.0f * a);
                if (t <= 0.0f) continue;
                float3 p = localRay.Origin + t * localRay.Direction;
                if (p.y < -halfHeight || p.y > halfHeight) continue;
                if (best < 0.0f || t < best)
                {
                    best = t;
                    float2 radial = p.xz;
                    float rl = length(radial);
                    radial = (rl > 1e-6f) ? radial / rl : float2(1.0f, 0.0f);
                    bestNormal = normalize(float3(radial.x, slope, radial.y));
                }
            }
        }
    }

    if (abs(localRay.Direction.y) > 1e-8f)
    {
        float t = (-halfHeight - localRay.Origin.y) / localRay.Direction.y;
        if (t > 0.0f && (best < 0.0f || t < best))
        {
            float3 p = localRay.Origin + t * localRay.Direction;
            if (dot(p.xz, p.xz) <= radius * radius)
            {
                best = t;
                bestNormal = float3(0.0f, -1.0f, 0.0f);
            }
        }
    }

    if (best <= 0.0f) return -1.0f;
    outNormal = bestNormal;
    return best;
}

// Distance from a point along the ray to the torus tube surface (negative
// inside).
float TorusSDF(Ray localRay, float t, float majorR, float minorR)
{
    float3 p = localRay.Origin + t * localRay.Direction;
    float rxz = length(p.xz);
    float ring = (rxz > 1e-6f) ? (rxz - majorR) : majorR;
    return sqrt(ring * ring + p.y * p.y) - minorR;
}

// Torus about the Y axis (Blender: Torus).
//
// Bracket the SDF over the ray interval already bounded by the caller's
// RayAABB test, then bisect each sign change. A seeded-Newton solve was tried
// and rejected: the radial-distance quadratic's two roots often converge to the
// same crossing, so it both missed hits and returned the far root.
float HitTorus(Ray localRay, float majorR, float minorR, float3 boxMin, float3 boxMax, out float3 outNormal)
{
    float3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), 0.00001f);
    float3 invDir = 1.0f / safeDir;

    float3 ta = (boxMin - localRay.Origin) * invDir;
    float3 tb = (boxMax - localRay.Origin) * invDir;
    float3 tNear3 = min(ta, tb);
    float3 tFar3 = max(ta, tb);

    float tEnter = max(max(tNear3.x, tNear3.y), tNear3.z);
    float tExit = min(min(tFar3.x, tFar3.y), tFar3.z);
    tEnter = max(tEnter, 0.0f);
    if (tExit <= tEnter) return -1.0f;

    const int kSamples = 32;
    float dt = (tExit - tEnter) / (float)kSamples;

    float best = -1.0f;
    float prevT = tEnter;
    float prevF = TorusSDF(localRay, prevT, majorR, minorR);
    // A ray starting inside the tube exits through a crossing, but that is an
    // exit rather than an entry; only accept outside->inside transitions.
    bool prevInside = prevF < 0.0f;

    for (int i = 1; i <= kSamples; i++)
    {
        float curT = tEnter + dt * (float)i;
        float curF = TorusSDF(localRay, curT, majorR, minorR);
        bool curInside = curF < 0.0f;

        if (curInside != prevInside)
        {
            float lo = prevT, hi = curT;
            float fLo = prevF;
            for (int iter = 0; iter < 24; iter++)
            {
                float mid = 0.5f * (lo + hi);
                float fMid = TorusSDF(localRay, mid, majorR, minorR);
                if ((fMid < 0.0f) == (fLo < 0.0f)) { lo = mid; fLo = fMid; }
                else { hi = mid; }
            }
            float hitT = 0.5f * (lo + hi);
            if (!prevInside && (best < 0.0f || hitT < best))
                best = hitT;
        }

        prevT = curT;
        prevF = curF;
        prevInside = curInside;
    }

    if (best <= 0.0f) return -1.0f;

    float3 p = localRay.Origin + best * localRay.Direction;
    float rxz = length(p.xz);
    float2 ring = (rxz > 1e-6f) ? p.xz * (majorR / rxz) : float2(majorR, 0.0f);
    outNormal = normalize(float3(p.x - ring.x, p.y, p.z - ring.y));
    return best;
}

// Single entry point for every analytic primitive.
float HitPrimitive(Ray localRay, RayTracingInstance inst, out float3 outNormal)
{
    int shape = (int)(inst.MaterialParams.z);
    float3 halfExtent = (inst.Max.xyz - inst.Min.xyz) * 0.5f;

    if (shape == 0) return HitCube(localRay, outNormal);              // Cube
    if (shape == 1) return HitSphere(localRay, inst.MaterialParams.w, outNormal); // Sphere

    if (shape == 2 || shape == 7) return HitPlaneXZ(localRay, halfExtent, outNormal); // Plane / Grid
    if (shape == 6) return HitDisc(localRay, inst.MaterialParams.w, outNormal);        // Circle

    // Cylinder / Cone / Torus carry radius in MaterialParams.w; the second
    // dimension (height, or minor radius) rides in Min.w, previously unused.
    if (shape == 3) return HitCylinder(localRay, inst.MaterialParams.w, inst.Min.w, outNormal);
    if (shape == 4) return HitCone(localRay, inst.MaterialParams.w, inst.Min.w, outNormal);
    if (shape == 5) return HitTorus(localRay, inst.MaterialParams.w, inst.Min.w, inst.Min.xyz, inst.Max.xyz, outNormal);

    // Unknown/newer shape: fall back to the bounding box rather than
    // silently dropping the surface.
    return HitCube(localRay, outNormal);
}


bool TestInstanceOcclusion(Ray r, float maxDist, int instIdx) {
    RayTracingInstance inst = Instances[instIdx];

    float3 diff = u_CameraPosition - float3(inst.WorldTransform[0][3], inst.WorldTransform[1][3], inst.WorldTransform[2][3]);
    if (dot(diff, diff) > inst.MaxDistance * inst.MaxDistance) return false;

    Ray localRay;
    localRay.Origin = (mul(inst.InvTransform, float4(r.Origin, 1.0f))).xyz;
    localRay.Direction = normalize((mul(inst.InvTransform, float4(r.Direction, 0.0f))).xyz);
    float3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), float3(0.00001f, 0.00001f, 0.00001f));
    float3 localInvDir = 1.0f / safeDir;

    if (RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) {
        float3 localNormal;
        float t = HitPrimitive(localRay, inst, localNormal);
        if (t > 0.0f) {
            float3 localHit = localRay.Origin + t * localRay.Direction;
            float3 worldHit = (mul(inst.WorldTransform, float4(localHit, 1.0f))).xyz;
            if (distance(r.Origin, worldHit) < maxDist) return true;
        }
    }
    return false;
}

bool IsOccluded(Ray r, float maxDist, int skipIdx) {
    if (u_InstanceCount == 0) return false;

    float3 safeDir = sign(r.Direction) * max(abs(r.Direction), float3(0.00001f, 0.00001f, 0.00001f));
    float3 invDir = 1.0f / safeDir;
    int sBase = g_tid * 32;
    int stackPtr = 0;
    gs_trav_stack[sBase + stackPtr] = 0; stackPtr++;

    while (stackPtr > 0) {
        int nodeIdx = (int)gs_trav_stack[sBase + --stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        float3 t0 = (node.MinBounds.xyz - r.Origin) * invDir;
        float3 t1 = (node.MaxBounds.xyz - r.Origin) * invDir;
        float3 tMin = min(t0, t1);
        float3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        if (tNear > tFar || tFar < 0.0f || tNear > maxDist) continue;

        int encoded = (int)(node.MinBounds.w);
        if (encoded < 0) {
            int first = -encoded - 1;
            int count = (int)(node.MaxBounds.w);
            for (int i = first; i < first + count; i++) {
                // Never let a light shadow itself: shadow rays aim at the
                // light's center, so without this the light's own front
                // surface always occludes and kills all direct light.
                if (i == skipIdx) continue;
                if (TestInstanceOcclusion(r, maxDist, i)) return true;
            }
        } else {
            if (stackPtr < 30) {
                gs_trav_stack[sBase + stackPtr] = (int)(node.MaxBounds.w); stackPtr++;
                gs_trav_stack[sBase + stackPtr] = encoded; stackPtr++;
            }
        }
    }
    return false;
}


float4 wl_march_fog_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                      float3 tint, float4 d1, float4 d2, float dither, float sunVis)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float fogLen = max(t1 - t0, 0.0f);
    float fogPerfK = lerp_safe(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    int steps = (int)(clamp(d2.z * fogPerfK * clamp(fogLen / 16.0f, 0.15f, 1.0f), 2.0f, 32.0f));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / (float)(steps);
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9f, 0.9f));
    float3 ambCol = lerp_safe(u_SkyBottomColor, u_SkyTopColor, 0.5f);
    float3 sunLight = ambCol * 0.7f + u_SunLightColor * sunVis * (0.25f + phase * 2.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float trans = 1.0f;
    float t = t0 + dt * dither;
    
    // Quality gate for multi-light scattering: Low preset uses sun only,
    // Medium+ adds point/spot lights with shadow rays.
    int lightStart = 0;
    int lightEnd = 0;
    if (u_QualityLevel >= 1 && u_ALightCount > 0)
        lightEnd = min(u_ALightCount, 8);
    
    for (int s = 0; s < 32; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        float d = max(d1.x, 0.0f);
        float h = clamp((p.y - bmin.y) / hgt, 0.0f, 1.0f);
        d *= lerp_safe(1.0f, exp(-h * 4.0f), clamp(d2.y, 0.0f, 1.0f));
        if (d1.z > 0.001f && d > 0.001f)
        {
            float3 np = p * max(d1.w, 0.001f) + float3(u_Time * d2.x, 0.0f, u_Time * d2.x * 0.6f);
            float n = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            d *= (1.0f - d1.z * 0.5f) + d1.z * n;
        }
        float a = 1.0f - exp(-max(d, 0.0f) * dt);
        if (a > 0.001f)
        {
            float3 sampleLight = sunLight;
            for (int li = lightStart; li < lightEnd; li++)
            {
                float atype = u_ALightData[li].x;
                if (atype < 0.5f) continue;
                float3 lpos = u_ALightPos[li];
                float3 lcol = u_ALightColor[li] * u_ALightData[li].y;
                float arange = max(u_ALightData[li].z, 0.0f);
                float3 toL = lpos - p;
                float dL = length(toL);
                if (arange > 0.0f && dL >= arange) continue;
                float3 L = toL / max(dL, 0.0001f);
                float att;
                if (arange > 0.0f)
                    att = pow(clamp(1.0f - dL / arange, 0.0f, 1.0f), max(u_ALightData[li].w, 0.5f));
                else
                    att = 1.0f / (1.0f + 0.1f * max(u_ALightData[li].w, 0.5f) * dL * dL);
                if (atype > 1.5f && atype < 2.5f)
                {
                    float c = dot(-L, u_ALightDir[li]);
                    att *= smoothstep(u_ALightData2[li].y, u_ALightData2[li].x, c);
                    if (att <= 0.001f) continue;
                }
                Ray lRay = MakeRay(p + L * 0.01f, L);
                if (IsOccluded(lRay, dL, -1)) continue;
                float lPhase = wl_hg(dot(rd, L), clamp(d1.y, -0.9f, 0.9f));
                sampleLight += lcol * att * (0.3f + lPhase * 2.0f);
            }
            acc += trans * a * tint * sampleLight;
            trans *= (1.0f - a);
            if (trans < 0.02f)
            {
                trans = 0.0f;
                break;
            }
        }
        t += dt;
    }
    return float4(acc, trans);
}

// Front-to-back march through one cloud layer box. Returns (scatteredLight, transmittance).
float4 wl_march_cloud_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                        float3 tint, float3 amb, float4 d1, float4 d2, float4 d3, float dither)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0f);
    // Perf tier: same render-scale step scaling as wl_march_fog_vol.
    float cloudPerfK = lerp_safe(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    int steps = (int)(clamp(d2.w * cloudPerfK * clamp(clLen / 8.0f, 0.25f, 1.0f), 2.0f, 32.0f));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / (float)(steps);
    float2 wnd = float2(d2.y, d2.z);
    if (dot(wnd, wnd) < 0.00000001f)
        wnd = float2(1.0f, 0.0f);
    wnd = normalize(wnd) * d2.x * u_Time;
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0f, 1.0f), 6.0f);
    float3 sunCol = float3(1.2f, 1.1f, 1.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    float nScale = max(d1.z, 0.001f);
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float trans = 1.0f;
    float t = t0 + dt * dither;
    for (int s = 0; s < 32; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        float3 q = p * nScale + float3(wnd.x, 0.0f, wnd.y);
        float base = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0f, 1.0f);
        float d = smoothstep(1.0f - cov, 1.0f - cov + 0.35f, base);
        if (d > 0.001f && d1.w > 0.001f)
        {
            float det = wl_vol_noise(q * 3.7f + float3(0.0f, u_Time * d2.x * 0.5f, 0.0f));
            d = max(d - det * d1.w * d, 0.0f);
        }
        d *= max(d1.y, 0.0f);
        float h = clamp((p.y - bmin.y) / hgt, 0.0f, 1.0f);
        d *= smoothstep(0.0f, 0.25f, h) * (1.0f - smoothstep(0.6f, 1.0f, h) * 0.7f);
        float a = 1.0f - exp(-d * dt);
        if (a > 0.001f)
        {
            float sh = 1.0f;
            if (d3.y > 0.01f)
            {
                // Cheap self-shadow: single-octave density tap toward the sun
                // (a full second march is overkill for a soft darkening term).
                float3 sp = p + u_SunDirection * dt * 2.0f;
                float sd = wl_vol_noise(sp * nScale + float3(wnd.x, 0.0f, wnd.y)) * max(d1.y, 0.0f);
                sh = exp(-sd * dt * 4.0f * clamp(d3.y, 0.0f, 1.0f));
            }
            float3 scol = tint * (amb * (0.35f + 0.65f * sh) + sunCol * sh * 0.9f)
                      + sunCol * silverPow * clamp(d3.x, 0.0f, 1.0f) * 0.6f;
            acc += trans * a * scol;
            trans *= (1.0f - a);
            if (trans < 0.02f)
            {
                trans = 0.0f;
                break;
            }
        }
        t += dt;
    }
    return float4(acc, trans);
}

// Applies all submitted cloud + fog volumes to a primary-ray sample.
// sunVis scales fog sun in-scatter (god rays); clouds keep self-shadowing.
float3 wl_apply_volumetrics(float3 ro, float3 rd, float tHit, float3 baseColor, float dither, float sunVis)
{
    float3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5f)
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
            if (u_FogData2[i].w < 0.5f)
                continue;
            float4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    return col;
}
// Global atmospheric fog: distance + height haze that applies everywhere
float3 wl_apply_global_fog(float3 col, float dist, float3 viewDir, float dither)
{
    if (u_GlobalFogEnabled != 1)
        return col;
    if (u_GlobalFogDensity <= 0.00001f)
        return col;
    float h = u_CameraPosition.y;
    float hBelow = max(u_GlobalFogBaseHeight - h, 0.0f);
    float hAbove = max(h - u_GlobalFogBaseHeight, 0.0f);
    float heightF = exp(-hBelow * u_GlobalFogHeightFalloff * 0.1f) * 
                    exp(-hAbove * u_GlobalFogHeightFalloff * 0.01f);
    float ext = 1.0f - exp(-dist * u_GlobalFogDensity * heightF * u_GlobalFogDistAtten);
    ext = clamp(ext, 0.0f, 1.0f);
    float3 fc = u_GlobalFogColor;
    float luma = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    float3 target = lerp_safe(col, fc, 0.5f + 0.3f * dither);
    target = lerp_safe(col, target, smoothstep(0.0f, 0.85f, luma));
    return lerp_safe(col, target, ext);
}

// Screen-space god rays: radial blur from sun screen position
void RunGodRays()
{
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    if (u_GodRaysEnabled != 1 || u_GodSunValid != 1 || u_GodRaySamples <= 0)
        return;

    float2 sunPos = u_GodSunScreenPos;
    float2 uv = (float2(pos) + 0.5f) / float2(fullSize);

    float2 delta = (sunPos - uv) * float2(fullSize.x / max((float)(fullSize.y), 1.0f), 1.0f);
    float len = length(delta);
    if (len < 0.001f) return;

    float2 stepDir = delta / len / (float)(u_GodRaySamples);
    float2 currentUV = uv;

    float3 accum = float3(0.0f, 0.0f, 0.0f);
    float decay = u_GodRayDecay;
    float weight = u_GodRayDensity;
    float illumDecay = 1.0f;

    int samples = clamp((int)(u_GodRaySamples), 1, 32);

    for (int i = 0; i < samples; i++)
    {
        currentUV += stepDir;
        if (currentUV.x < 0.0f || currentUV.x > 1.0f || currentUV.y < 0.0f || currentUV.y > 1.0f)
            break;

        float3 s = TEX(s_Output, currentUV).rgb;
        s *= illumDecay * weight;
        accum += s;
        illumDecay *= decay;
    }

    accum *= u_GodRayIntensity / (float)(samples);
    accum *= u_GodRayColor;

    float3 existing = IMG_LOAD(img_Output, pos).rgb;
    IMG_STORE(img_Output, pos, float4(existing + accum, 1.0f));
}



float Halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= (float)(base);
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

float2 GetJitter(int frameIndex, float cameraMoved) {
    int effectiveIndex = (cameraMoved > 0.5f) ? 0 : frameIndex;
    float x = Halton(effectiveIndex % 16 + 1, 2);
    float y = Halton(effectiveIndex % 16 + 1, 3);
    return float2(x, y) - 0.5f;
}

static const float3x3 Gx = float3x3(-1, -2, -1, 0, 0, 0, 1, 2, 1);
static const float3x3 Gy = float3x3(-1, 0, 1, -2, 0, 2, -1, 0, 1);

float GetLuminance(float3 col) {
    return dot(col, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 RGB2YCoCg(float3 rgb) {
    float Y  = 0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co = 0.5f  * rgb.r - 0.5f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCg2RGB(float3 ycocg) {
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(
        Y + Co - Cg,
        Y + Cg,
        Y - Co - Cg
    );
}

float2 CalculateUV(float3 localPos, RayTracingInstance inst) {
    int shape = (int)(inst.MaterialParams.z);

    // Plane / Grid: flat XZ quad, tiled by world scale so texel density stays
    // constant under the object's transform.
    if (shape == 2 || shape == 7) {
        return float2(localPos.x, localPos.z) * inst.TextureScale.xz;
    }

    // Cylinder / Cone: unwrap the wall by angle around Y, height along Y --
    // the same UVs ProceduralMeshes.cpp writes for the raster path.
    if (shape == 3 || shape == 4) {
        float phi = atan2(localPos.z, localPos.x);
        float u = phi / (2.0f * PI) + 0.5f;
        float halfH = max(inst.Min.w, 0.0001f);
        float v = clamp((localPos.y / (2.0f * halfH)) + 0.5f, 0.0f, 1.0f);
        float circumference = 2.0f * PI * max(inst.MaterialParams.w, 0.0001f);
        return float2(u, v) * float2(circumference, 2.0f * halfH);
    }

    // Torus: (major angle, minor angle) -- the natural unwrap of a tube.
    if (shape == 5) {
        float majorR = max(inst.MaterialParams.w, 0.0001f);
        float minorR = max(inst.Min.w, 0.0001f);
        float u = atan2(localPos.z, localPos.x) / (2.0f * PI) + 0.5f;
        float ringR = max(length(localPos.xz), 0.0001f);
        float v = atan2(localPos.y, ringR - majorR) / (2.0f * PI) + 0.5f;
        return float2(u, v) * float2(2.0f * PI * majorR, 2.0f * PI * minorR);
    }

    // Circle: radial disc mapping from the centre.
    if (shape == 6) {
        return localPos.xz + float2(0.5f, 0.5f);
    }

    if (shape == 0) {
        float3 absLocal = abs(localPos);
        float maxAxis = max(max(absLocal.x, absLocal.y), absLocal.z);

        float2 uv;
        float2 uvScale;
        if (maxAxis == absLocal.x) {
            float signX = sign(localPos.x);
            uv = float2(localPos.z * signX + 0.5f, localPos.y + 0.5f);
            uvScale = float2(inst.TextureScale.z, inst.TextureScale.y);
        } else if (maxAxis == absLocal.y) {
            float signY = sign(localPos.y);
            uv = float2(localPos.x * signY + 0.5f, localPos.z + 0.5f);
            uvScale = float2(inst.TextureScale.x, inst.TextureScale.w);
        } else {
            float signZ = sign(localPos.z);
            uv = float2(localPos.x * signZ + 0.5f, localPos.y + 0.5f);
            uvScale = float2(inst.TextureScale.x, inst.TextureScale.y);
        }

        return uv * uvScale;
    } else {
        // Sphere (and any future shape without an explicit case).
        float phi = atan2(localPos.z, localPos.x);
        float theta = acos(clamp(localPos.y, -1.0f, 1.0f));

        float u = phi / (2.0f * PI) + 0.5f;
        float v = theta / PI;

        return float2(u, v) * inst.TextureScale.xy;
    }
}

void RunVisibilityAndVelocity() {
    int2 pixelCoords = int2(g_dt.xy);
    float2 uv = (float2(pixelCoords) + 0.5f) / float2(wl_trace_extent(IMG_SIZE(img_Output)));
    
    float depth = TEX(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (depth >= 1.0f) { 
        IMG_STORE(img_Velocity, pixelCoords, float2(0.0f, 0.0f));
        return;
    }

    float4 clipPos = float4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
    float4 worldPos = mul(u_InverseViewProjection, clipPos);
    worldPos /= worldPos.w;

    float4 prevClipPos = mul(u_PrevViewProjection, worldPos);
    prevClipPos /= prevClipPos.w;
    float2 prevUV = prevClipPos.xy * 0.5f + 0.5f;

    // NOTE: intentionally the UNjittered grid UV. History is stored per-pixel
    // on that grid, so velocity = uv - prevUV lands the lookup on the texel
    // that saw this surface point. Biasing by the frame jitter (tempting,
    // tried) offsets the lookup by a per-frame-varying amount and smears.
    float2 velocity = uv - prevUV;
    IMG_STORE(img_Velocity, pixelCoords, float2(velocity));
}

struct HitInfo {
    bool hit;
    float t;
    float3 worldPos;
    float3 normal;
    float3 albedo;
    float metal;
    float rough;
    float3 emission;
    float occlusion;
    float3 f0Tint; // neural material specular tint, 1.0f = classic F0
};

// ---- Gerstner wave spectrum (matches WaterComponent::EvaluateSurface) ----
// 6 directional components, Phillips-style falloff, deep-water dispersion.
// Returns world-space height and normal perturbation at a world XZ position.
// baseDir = normalized primary swell direction; flowVel advects the sample
// (matches the CPU AdvectedPos) so rivers carry their waves downstream.
void EvaluateGerstnerWaves(float2 worldXZ, float time, float amplitude, float wavelength,
                           float speed, float chop, float steepness, float minWavelength,
                           float2 baseDir, float2 flowVel,
                           out float outHeight, out float3 outNormal)
{
    outHeight = 0.0f;
    outNormal = float3(0.0f, 1.0f, 0.0f);
    if (amplitude <= 0.00001f)
        return;

    // Spectrum tables (length ratios, amplitude shares, fan angles, phase offsets)
    // Mirrors CPU-side WaterComponent::SpectrumLengthRatio/Share/FanAngle/PhaseOffset
    float baseLen = max(wavelength, 0.1f);
    float t = time * speed;
    float steep = clamp(steepness, 0.0f, 1.0f);
    float chopK = clamp(chop, 0.0f, 1.0f);
    float2 d1 = baseDir;
    if (dot(d1, d1) < 1e-8f)
        d1 = float2(1.0f, 0.3f);
    d1 = normalize(d1);
    float2 p = worldXZ - flowVel * time;

    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    for (int i = 0; i < 6; i++)
    {
        // Length ratio per component
        float lenRatio = 1.0f;
        if (i == 1) lenRatio = 0.55f;
        else if (i == 2) lenRatio = 0.32f;
        else if (i == 3) lenRatio = 0.19f;
        else if (i == 4) lenRatio = 0.11f;
        else if (i == 5) lenRatio = 0.06f;

        float li = baseLen * lenRatio;

        // Spectral LOD: fade components shorter than minWavelength
        float fade = 1.0f;
        if (minWavelength > 0.00001f && li < minWavelength * 2.0f)
        {
            if (li <= minWavelength)
                continue;
            fade = (li - minWavelength) / minWavelength;
        }

        // Amplitude share per component
        float share = 0.55f;
        if (i == 1) share = 0.22f;
        else if (i == 2) share = 0.11f;
        else if (i == 3) share = 0.06f;
        else if (i == 4) share = 0.035f;
        else if (i == 5) share = 0.025f;

        float amp = amplitude * share * (i == 0 ? 1.0f : chopK) * fade;
        if (amp <= 0.0000001f)
            continue;

        float k = 6.2831853f / li;
        if (k > 40.0f) k = 40.0f;
        float omega = sqrt(9.81f * k);
        if (omega > 25.0f) omega = 25.0f;

        // Fan angle per component
        float angle = 0.0f;
        if (i == 1) angle = 0.40f;
        else if (i == 2) angle = -0.54f;
        else if (i == 3) angle = 0.82f;
        else if (i == 4) angle = -1.01f;
        else if (i == 5) angle = 1.24f;

        // Phase offset per component
        float phaseOff = 0.0f;
        if (i == 1) phaseOff = 1.3f;
        else if (i == 2) phaseOff = 4.1f;
        else if (i == 3) phaseOff = 2.2f;
        else if (i == 4) phaseOff = 5.0f;
        else if (i == 5) phaseOff = 0.7f;

        // Direction: fan from primary wave direction
        float ca = cos(angle);
        float sa = sin(angle);
        float2 dir = float2(d1.x * ca - d1.y * sa, d1.x * sa + d1.y * ca);

        float sharp = (i == 0 ? 0.6f : 1.0f) * chopK;
        float norm = 1.0f + 0.5f * sharp;
        float phi = fmod_safe(k * dot(dir, p) - omega * t, 6.2831853f);
        float s1 = sin(phi);
        float c1 = cos(phi);
        float harmonic = 2.0f * phi + phaseOff;
        float s2 = sin(harmonic);
        float c2 = cos(harmonic);

        float wa = k * amp;
        float qi = steep / (wa * 6.0f + 1e-6f);
        if (qi > 1.0f) qi = 1.0f;
        if (qi < 0.0f) qi = 0.0f;

        float shapeS = (s1 + 0.5f * sharp * s2) / norm;
        float shapeC = (c1 + sharp * c2) / norm;
        outHeight += amp * shapeS;
        nx += -dir.x * wa * shapeC;
        nz += -dir.y * wa * shapeC;
        ny += -qi * wa * shapeS;
    }
    outNormal = normalize(float3(nx, ny, nz));
}

void ComputeTangentFrame(float3 localHitPos, float3 localNormal, int shapeType, float4 texScaleFull,
                         out float3 T, out float3 B, out float2 uvScale)
{
    // HLSL port: the UV-space POM march is removed (SM5 forbids dynamic
    // texture indexing); keep an axis-aligned plane tangent frame for the
    // bump gradient and sun-shadow march below.
    float3 absN = abs(localNormal);
    if (absN.x >= absN.y && absN.x >= absN.z) {
        T = float3(0.0f, 0.0f, 1.0f);
        B = float3(0.0f, 1.0f, 0.0f);
    } else if (absN.y >= absN.x && absN.y >= absN.z) {
        T = float3(1.0f, 0.0f, 0.0f);
        B = float3(0.0f, 0.0f, 1.0f);
    } else {
        T = float3(1.0f, 0.0f, 0.0f);
        B = float3(0.0f, 1.0f, 0.0f);
    }
    uvScale = float2(1.0f, 1.0f);
}

// Relief height stack (Crimson-rock setup): texture macro-relief + procedural
// craggy breakup + strata banding + neural micro-surface. The linear march
// and binary refine sample the cheap macro height only; final/shadow/gradient
// taps use the full stack so detail rides on the macro silhouette.
float wl_pom_macro(int texID, float2 tapUV)
{
    return GetLuminance(TEX_SLOT_IDX(texID, tapUV, 0.0f).rgb);
}

float wl_pom_full(int texID, float2 tapUV, float3 wpBase, float3 nRef,
                  float mesoAmp, float strataW, float neuralK)
{
    float h = wl_pom_macro(texID, tapUV);
    if (mesoAmp > 0.001f)
    {
        float2 muv = tapUV * 6.0f;
        float n1 = wl_vol_noise(float3(muv, 3.7f));
        float n2 = wl_vol_noise(float3(muv * 2.3f + 5.0f, 9.1f));
        float st = sin(wpBase.y * 5.0f + n1 * 4.0f) * 0.5f + 0.5f;
        h += ((n1 - 0.5f) * 0.7f + (n2 - 0.5f) * 0.3f) * mesoAmp
           + (st - 0.5f) * mesoAmp * 0.8f * strataW;
    }
    if (neuralK > 0.001f)
        h += wl_neural_micro(tapUV, nRef) * 2.0f * neuralK;
    return clamp(h, 0.0f, 1.5f);
}

void ApplyPOM(RayTracingInstance inst, float3 localHitPos, float3 localNormal,
              float3 localViewDir, inout float2 uv, inout float3 worldPos,
              inout float3 worldNormal, out float selfOcclusion, out float dispPush,
              out float outHeight)
{
    selfOcclusion = 1.0f;
    dispPush = 0.0f;
    outHeight = -1.0f;

    if (u_QualityLevel <= 1)
        return;

    float3 V = normalize(localViewDir);
    float distToCam = length(worldPos - u_CameraPosition);
    float cosView = abs(dot(V, localNormal));

    // Perf tier: pull POM range down when tracing below full resolution
    // (dynamic resolution on older GPUs) — distFade already zeroes detail at maxDist.
    float pomPerfK = smoothstep(0.6f, 0.9f, u_RenderScale);
    float maxDist = (u_QualityLevel == 2) ? 25.0f : lerp_safe(35.0f, 50.0f, pomPerfK);
    if (distToCam > maxDist)
        return;
    // Grazing views skip the UV march (the view-ray projection blows up
    // edge-on) but still push + perturb from the center height below, so
    // relief never vanishes side-on.
    bool doMarch = (cosView >= 0.15f);
    // Angular march weight: the transition band above the gate still projects
    // each step across many texels (smear streaks), so collapse the offset
    // toward the center tap smoothly instead of switching hard.
    float marchK = smoothstep(0.10f, 0.45f, cosView);
    float marchLOD = (1.0f - marchK) * 2.0f;

    float dispScale = inst.DisplacementParams.x;
    float bumpStrength = inst.DisplacementParams.y;
    if (inst.TextureID < 0 || inst.TextureID >= 32)
        return;
    if (dispScale < 0.001f && bumpStrength < 0.001f)
        return;

    int shapeType = (int)(inst.MaterialParams.z);
    float3 T, B;
    float2 uvScale;
    ComputeTangentFrame(localHitPos, localNormal, shapeType, inst.TextureScale, T, B, uvScale);

    int texID = inst.TextureID;
    float eps = 1.0f / 256.0f;

    // Meso setup rides on Displacement Scale only (opt-in): existing scenes
    // with bump-only materials render pixel-identical to before.
    float3 wpBase = worldPos;
    float3 wNorm0 = normalize(worldNormal);
    float mesoAmp = 0.0f;
    float strataW = clamp(1.0f - abs(wNorm0.y), 0.15f, 1.0f);
    float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0f, 1.0f) : 0.0f;

    float maxDisplacement = max(dispScale, bumpStrength);
    float cosAngle = max(dot(V, localNormal), 0.05f);
    
    int baseLayers = (u_QualityLevel == 2) ? 6 : (int)(lerp_safe(8.0f, 16.0f, pomPerfK));
    int maxLayers  = (u_QualityLevel == 2) ? 16 : (int)(lerp_safe(20.0f, 32.0f, pomPerfK));
    int numLayers  = (int)(clamp((float)(baseLayers) / cosAngle, (float)(baseLayers), (float)(maxLayers)));


    float distFade = clamp(1.0f - (distToCam - (maxDist - 15.0f)) / 15.0f, 0.0f, 1.0f);
    dispScale *= distFade;
    bumpStrength *= distFade;
    mesoAmp = 0.25f * clamp(dispScale, 0.0f, 1.5f);


    // HLSL port: no UV march (SM5 dynamic texture indexing); push, normal
    // and shadow terms evaluate the height stack at the un-marched base uv.
    float finalHeight = 0.5f;
    if (dispScale > 0.001f)
    {
        // Detail rides on the macro silhouette: enrich after the march.
        finalHeight = wl_pom_full(texID, uv, wpBase, wNorm0, mesoAmp, strataW, neuralK);
        // Shape-preserving push: bilateral around the mid-level so the mesh
        // keeps its base shape — features rise AND recess instead of the whole
        // surface ballooning outward by up to full dispScale.
        worldPos += worldNormal * (finalHeight - 0.5f) * dispScale;
        dispPush = (finalHeight - 0.5f) * dispScale;
        outHeight = finalHeight;
    }

    if (bumpStrength > 0.001f)
    {
        float hU = wl_pom_full(texID, uv + float2(eps, 0.0f), wpBase, wNorm0, mesoAmp, strataW, neuralK);
        float hV = wl_pom_full(texID, uv + float2(0.0f, eps), wpBase, wNorm0, mesoAmp, strataW, neuralK);
        float dhdu = (hU - finalHeight) / eps;
        float dhdv = (hV - finalHeight) / eps;

        // UV-space gradient -> world-space gradient. The engine tiles ~1 texture
        // per meter (cube TextureScale = world scale, sphere scales are
        // circumference-based), so UV-per-world is ~1 and NO uvScale factor
        // belongs here. Multiplying by uvScale (50x on a 50m floor, up to 20x
        // near sphere poles) saturated normals into garbage and blackened
        // everything POM touched. (uvScale IS still correct for the march
        // step above, which walks in UV units.)
        float dh_dsT = dhdu;
        float dh_dsB = dhdv;

        float3 bumpGrad = (dh_dsT * T + dh_dsB * B) * bumpStrength;
        float3 localPerturbedNormal = localNormal - bumpGrad;
        float bumpLen = length(bumpGrad);
        // Cap the perturbation: with large texture scales (e.g. a 50m floor
        // with uvScale 50) the UV-space gradient saturates into garbage
        // normals and near-black shading. Direction kept, magnitude capped.
        if (bumpLen > 1.0f)
            localPerturbedNormal = localNormal - bumpGrad / bumpLen;
        localPerturbedNormal = normalize(localPerturbedNormal);
        float3x3 normalMatrix = transpose((float3x3)(inst.InvTransform));
        worldNormal = normalize(mul(normalMatrix, localPerturbedNormal));

        float gradMag = length(float2(dh_dsT, dh_dsB));
        float ridgeFactor = max(0.0f, finalHeight - 0.5f) * gradMag;
        selfOcclusion = clamp(1.0f - ridgeFactor * 0.04f * bumpStrength, 0.2f, 1.0f);
    }

    // Deep-crevice sun shadow: short heightfield march toward the sun so
    // crags cast onto each other. Displacement-gated (opt-in look change).
    if (dispScale > 0.001f)
    {
        float3 localSun = normalize(mul((float3x3)(inst.InvTransform), u_SunDirection));
        float2 sunUV = float2(dot(localSun, T), dot(localSun, B));
        float sunZ = dot(localSun, localNormal);
        if (sunZ > 0.05f)
        {
            int shSteps = (u_QualityLevel >= 3) ? 6 : 4;
            float2 shStep = (sunUV / sunZ) * (maxDisplacement * 0.05f / (float)(shSteps));
            if (length(shStep) > 0.1f)
                shStep = normalize(shStep) * 0.1f;
            float rayH = finalHeight + 0.02f;
            float rayRise = (1.0f - finalHeight + 0.05f) / (float)(shSteps);
            float occ = 1.0f;
            float2 suv = uv;
            for (int s = 0; s < 6; s++)
            {
                if (s >= shSteps)
                    break;
                suv += shStep;
                rayH += rayRise;
                float sh = wl_pom_full(texID, suv, wpBase, wNorm0, mesoAmp, strataW, neuralK);
                occ = min(occ, clamp((rayH - sh) * 6.0f, 0.0f, 1.0f));
            }
            selfOcclusion *= clamp(occ, 0.25f, 1.0f);
        }
    }
}

void TestInstanceHit(Ray ray, int instIdx, bool isPrimaryRay, inout HitInfo info) {
    RayTracingInstance inst = Instances[instIdx];

    float3 camDiff = u_CameraPosition - float3(inst.WorldTransform[0][3], inst.WorldTransform[1][3], inst.WorldTransform[2][3]);
    if (dot(camDiff, camDiff) > inst.MaxDistance * inst.MaxDistance) return;

    Ray localRay;
    localRay.Origin = (mul(inst.InvTransform, float4(ray.Origin, 1.0f))).xyz;
    localRay.Direction = normalize((mul(inst.InvTransform, float4(ray.Direction, 0.0f))).xyz);
    
    float3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), float3(0.00001f, 0.00001f, 0.00001f));
    float3 localInvDir = 1.0f / safeDir;

    if (!RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) return;
    float3 localNormal;
    float tLocal;
    tLocal = HitPrimitive(localRay, inst, localNormal);
    if (tLocal > 0.0f) {
        float3 localHitPos = localRay.Origin + tLocal * localRay.Direction;
        float3 worldHit = (mul(inst.WorldTransform, float4(localHitPos, 1.0f))).xyz;
        float tWorld = distance(ray.Origin, worldHit);
        if (tWorld < info.t) {
            info.t = tWorld;
            info.hit = true;
            info.worldPos = worldHit;
            info.normal = normalize((mul(float4(localNormal, 0.0f), inst.InvTransform)).xyz);

            float2 uv = CalculateUV(localHitPos, inst);

            float pomH = -1.0f;
            if (isPrimaryRay)
            {
                float pomOcclusion = 1.0f;
                float pomPush = 0.0f;
                ApplyPOM(inst, localHitPos, localNormal, -localRay.Direction, uv, info.worldPos, info.normal, pomOcclusion, pomPush, pomH);
                info.occlusion = pomOcclusion;
                // Screen-space displacement: re-anchor the POM hit onto the
                // primary world ray. The UV march + normal push reconstruct
                // the relief point approximately (exact only for planar,
                // uniformly-scaled surfaces); on spheres / scaled instances
                // it drifts off-ray, corrupting trace depth, secondary-ray
                // origins, and temporal stability. Intersecting the pixel ray
                // with the tangent plane through the pushed point keeps the
                // height estimate but guarantees screen-space correctness.
                // No extra texture taps. Signed push: raised features anchor
                // in front of the base hit, dented recesses slightly behind.
                WL_RAYANCHOR(ray, info, pomPush);
            }

            float dist = distance(worldHit, u_CameraPosition);
            // LOD from texel DENSITY (tiles per meter), not raw TextureScale:
            // scale grows with object size (50m floor tiles 50x) but density
            // is ~1/m like a unit cube. Raw scale pushed big surfaces to max
            // LOD meters from the camera, erasing all texture detail.
            // (Axis-aligned approx; rotation ignored.)
            float3 wAxis = float3(length(float3(inst.WorldTransform[0][0], inst.WorldTransform[1][0], inst.WorldTransform[2][0])),
                              length(float3(inst.WorldTransform[0][1], inst.WorldTransform[1][1], inst.WorldTransform[2][1])),
                              length(float3(inst.WorldTransform[0][2], inst.WorldTransform[1][2], inst.WorldTransform[2][2])));
            float3 wSize = max((inst.Max.xyz - inst.Min.xyz) * wAxis, float3(0.0001f, 0.0001f, 0.0001f));
            float worldExtent = max(max(wSize.x, wSize.y), wSize.z);
            float scaleLen = max(length(inst.TextureScale.xy) / worldExtent, 0.0001f);
            // Gentle distance ramp (holds LOD 0 ~4x farther out) + extra blur
            // at grazing angles, where one isotropic LOD can't cover the
            // stretched footprint and shimmer/RGB striping appears.
            // (textureLod bypasses driver anisotropy, so compensate manually.)
            float ndv = abs(dot(info.normal, ray.Direction));
            float grazing = 1.0f - clamp(ndv, 0.0f, 1.0f);
            float mipLevel = log2(max(dist * scaleLen * 0.008f, 0.0001f)) + grazing * grazing * 2.0f;
            mipLevel = clamp(mipLevel, 0.0f, 4.0f);

            float3 sampledAlbedo = float3(1.0f, 1.0f, 1.0f);
            if (inst.TextureID >= 0 && inst.TextureID < 32)
                sampledAlbedo = TEX_SLOT_IDX(inst.TextureID, uv, mipLevel).rgb;

            info.albedo = sampledAlbedo * inst.Albedo.rgb;
            // Water marker: skip neural blocks for water (waves handle their own shading)
            bool wlWater = (inst.DisplacementParams.z > 0.5f);
            // Neural texture detail: MLP-synthesized micro-surface (OpenGL neural path)
            if (!wlWater && u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001f)
                info.albedo *= lerp_safe(float3(1.0f, 1.0f, 1.0f), wl_neural_tex_detail(uv, info.normal), clamp(u_NeuralTexStrength, 0.0f, 1.0f));
            info.metal = clamp(inst.MaterialParams.x, 0.0f, 1.0f);
            info.rough = inst.MaterialParams.y > 0.0f ? inst.MaterialParams.y : 0.5f;
            info.f0Tint = float3(1.0f, 1.0f, 1.0f);
            // Neural material: learned PBR params feed the GGX BRDF (OpenGL neural path)
            if (!wlWater && u_NeuralEnabled == 1 && u_NeuralMatStrength > 0.001f)
            {
                float nmk = clamp(u_NeuralMatStrength, 0.0f, 1.0f);
                float3 nma = info.albedo;
                float nmm = info.metal;
                float nmr = info.rough;
                float3 nmf = float3(1.0f, 1.0f, 1.0f);
                wl_neural_material(uv, info.worldPos, info.normal, nma, nmm, nmr, nmf);
                info.albedo = lerp_safe(info.albedo, nma, nmk);
                info.metal = lerp_safe(info.metal, nmm, nmk);
                info.rough = lerp_safe(info.rough, nmr, nmk);
                info.f0Tint = lerp_safe(float3(1.0f, 1.0f, 1.0f), nmf, nmk);
            }

            // ---- Water: Gerstner swell + ripple detail + whitecaps ----
            // Water marker: DisplacementParams.z > 0.5 flags water surfaces.
            // The geometric box stays flat (no per-pixel ray re-intersection);
            // realism comes from an accurate wave normal driving the mirror
            // reflection, pixel-level ripple octaves for close-up glitter, and
            // noise-broken crest foam. Packed auxiliaries (see DrawWaterPlane
            // Nova path): TextureScale.xy = swell dir, .zw = flow velocity,
            // DisplacementParams.x = foam amount.
            if (wlWater)
            {
                float wAmplitude = inst.WaveParams.x;
                float wLength = inst.WaveParams.y;
                float wSpeed = inst.WaveParams.z;
                float wChop = clamp(inst.WaveParams.w, 0.0f, 1.0f);
                // Steepness rides in MaterialParams.w (free for cubes);
                // .z stays 0 so the shape discriminator keeps HitCube.
                float wSteep = inst.MaterialParams.w;
                float2 wBaseDir = inst.TextureScale.xy;
                float2 wFlow = inst.TextureScale.zw;
                float wFoamAmt = clamp(inst.DisplacementParams.x, 0.0f, 1.0f);
                float wDist = distance(info.worldPos, u_CameraPosition);
                // Up-facing weight: only the surface skin takes wave normals;
                // the thin-box sides keep their geometric normal.
                float wUpWeight = clamp(info.normal.y, 0.0f, 1.0f);
                // Quality-scaled distance cutoff: skip waves on far water
                float waveMaxDist = 80.0f;
                if (u_QualityLevel == 0) waveMaxDist = 30.0f;
                else if (u_QualityLevel == 1) waveMaxDist = 50.0f;
                else if (u_QualityLevel == 2) waveMaxDist = 80.0f;
                else waveMaxDist = 120.0f;
                float wH = 0.0f;
                float3 wN = float3(0.0f, 1.0f, 0.0f);
                if (wDist < waveMaxDist && wUpWeight > 0.001f)
                {
                    // Distance-based spectral LOD: fade high-frequency
                    // components that would alias at this distance
                    // (matches CPU-side LOD)
                    float minWavelength = wDist * 0.2f;
                    EvaluateGerstnerWaves(info.worldPos.xz, u_Time, wAmplitude, wLength,
                                          wSpeed, wChop, wSteep, minWavelength,
                                          wBaseDir, wFlow, wH, wN);
                    // Blend-replace: the old add-and-halve flattened swell
                    // (up + up = up), hiding waves from reflections.
                    info.normal = normalize(lerp_safe(info.normal, wN, wUpWeight));
                }
                // Ripple detail: pixel-level chop octaves (mirrors the Basic
                // raster path) so the mirror breaks into sun glitter near the
                // camera instead of staying a flat sheet.
                float2 wRippleGrad = float2(0.0f, 0.0f);
                float wRippleFade = clamp(1.0f - (wDist - 25.0f) / 60.0f, 0.0f, 1.0f) * wUpWeight;
                if (wRippleFade > 0.001f && wChop > 0.001f)
                {
                    float2 ruv = info.worldPos.xz - wFlow * u_Time;
                    float wt = u_Time;
                    wRippleGrad += float2(0.96, 0.28) * (cos(dot(float2(0.96, 0.28), ruv) * 2.1 + wt * 1.6) * 0.22);
                    wRippleGrad += float2(-0.42, 0.91) * (cos(dot(float2(-0.42, 0.91), ruv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
                    wRippleGrad += float2(0.66, -0.75) * (cos(dot(float2(0.66, -0.75), ruv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
                    float wRippleStrength = 0.55 * wRippleFade * (0.25f + 0.75f * wChop);
                    info.normal = normalize(info.normal + float3(-wRippleGrad.x, 0.0f, -wRippleGrad.y) * wRippleStrength);
                }
                // Whitecaps: crest-gated, broken by advected cell noise so foam
                // reads as patches instead of clean bands. FoamAmount gates
                // coverage, so calm lakes stay glassy.
                float wMaxH = max(inst.DisplacementParams.w, 0.05f);
                float wCrest = smoothstep(wMaxH * 0.5f, wMaxH * 0.9f, wH);
                float wFoamMask = 0.0f;
                if (wFoamAmt > 0.001f && wCrest > 0.001f)
                {
                    float2 fuv = info.worldPos.xz * 0.9f - wFlow * (u_Time * 0.7f) + float2(u_Time * 0.03f, -u_Time * 0.021f);
                    float breakup = wl_hash12(floor(fuv * 3.0f)) * 0.6f + wl_hash12(floor(fuv * 7.0f + 13.1f)) * 0.4f;
                    float wFoamN = breakup * 0.7f + wCrest * 0.3f;
                    wFoamMask = smoothstep(1.0f - wFoamAmt, 1.01f - wFoamAmt * 0.5f, wFoamN);
                }
                wFoamMask = clamp(wFoamMask, 0.0f, 1.0f);
                info.albedo = lerp_safe(info.albedo, float3(0.9f, 0.95f, 1.0f), wFoamMask * 0.85f);
                // Ripple slopes roughen the mirror into glitter; foam is matte.
                // Crests themselves stay glossy so swells catch the sun.
                float wRippleMag = clamp(length(wRippleGrad), 0.0f, 1.0f);
                info.rough = clamp(info.rough + wRippleMag * 0.12f * wRippleFade, 0.0f, 1.0f);
                info.rough = lerp_safe(info.rough, 0.85f, wFoamMask);
                // Water is a mirror: metallic = 1.0
                info.metal = 1.0f;
            }
            // Relief weathering: dust settles in pits, sun bleaches ridge tops.
            // Displacement-gated: only set when the relief march ran with push.
            if (pomH >= 0.0f)
            {
                float cav = smoothstep(0.45f, 0.05f, pomH);
                float ridge = smoothstep(0.55f, 0.95f, pomH);
                info.albedo *= (1.0f - 0.30f * cav);
                info.albedo = lerp_safe(info.albedo, info.albedo * float3(1.10f, 1.03f, 0.92f), ridge * 0.65f);
                info.occlusion *= (1.0f - 0.35f * cav);
            }
            info.emission = inst.Emission.xyz * inst.Emission.w;
        }
    }
}

HitInfo TraceScene(Ray ray) {
    HitInfo info = (HitInfo)0;
    info.hit = false;
    info.t = 1e20;
    info.occlusion = 1.0f;
    info.f0Tint = float3(1.0f, 1.0f, 1.0f);

    if (u_InstanceCount == 0) return info;

    bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01f;
    float3 safeDir = sign(ray.Direction) * max(abs(ray.Direction), float3(0.00001f, 0.00001f, 0.00001f));
    float3 invDir = 1.0f / safeDir;

    int sBase = g_tid * 32;
    int stackPtr = 0;
    gs_trav_stack[sBase + stackPtr] = 0; stackPtr++;

    while (stackPtr > 0) {
        int nodeIdx = (int)gs_trav_stack[sBase + --stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        float3 t0 = (node.MinBounds.xyz - ray.Origin) * invDir;
        float3 t1 = (node.MaxBounds.xyz - ray.Origin) * invDir;
        float3 tMin = min(t0, t1);
        float3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        bool nodeMiss = (tNear > tFar) || (tFar < 0.0f) || (tNear > info.t);
        if (!nodeMiss) {
        int encoded = (int)(node.MinBounds.w);
        if (encoded < 0) {
            int first = -encoded - 1;
            int count = (int)(node.MaxBounds.w);
            int li = 0;
            while (li < count) {
                TestInstanceHit(ray, first + li, isPrimaryRay, info);
                li++;
            }
        } else {
            if (stackPtr < 30) {
                gs_trav_stack[sBase + stackPtr] = (int)(node.MaxBounds.w); stackPtr++;
                gs_trav_stack[sBase + stackPtr] = encoded; stackPtr++;
            }
        }
        }
    }
    return info;
}

float3 ComputeDirectLighting(HitInfo h, float3 V, uint lightSeed, bool skipEnv) {
    float3 F0 = lerp_safe(float3(0.04f, 0.04f, 0.04f), max(h.albedo, float3(0.04f, 0.04f, 0.04f)), h.metal) * h.f0Tint;
    float3 diffuseColor = h.albedo * (1.0f - h.metal);
        float3 directLight = h.emission;

    int maxLightsCap = (u_QualityLevel == 0) ? 1 :
                       ((u_QualityLevel == 1) ? 2 :
                       ((u_QualityLevel == 2) ? 4 : u_MaxLights));
    int lightsToSample = min(u_LightCount, maxLightsCap);

    // Stochastic subset: evaluate 2 rotating lights instead of all N shadow
    // rays, scaled to stay unbiased. Temporal accumulation converges the
    // rotation; ~4x cheaper direct lighting at Ultra.
    int evalCount = lightsToSample;
    int lightStart = 0;
    int lightStride = 1;
    if (lightsToSample > 2)
    {
        evalCount = 2;
        lightStart = (int)(lightSeed % (uint)(lightsToSample));
        lightStride = (int)((float)(lightsToSample) / 2.0);
        if (lightStride < 1) lightStride = 1;
    }
    float lightScale = (float)(lightsToSample) / (float)(max(evalCount, 1.0f));

    for (int k = 0; k < evalCount; k++) {
        int li = (lightStart + k * lightStride) % lightsToSample;
        int i = LightIndices[li];
        RayTracingInstance light = Instances[i];
        float3 toLight = float3(light.WorldTransform[0][3], light.WorldTransform[1][3], light.WorldTransform[2][3]) - h.worldPos;
        float distToLight = length(toLight);
        float3 dirToLight = toLight / distToLight;
        Ray shadowRay = MakeRay(h.worldPos + h.normal * 0.001f, dirToLight);
        // Pass the light's own instance index so it can't self-shadow
        // (shadow rays target the light center, inside its geometry).
        if (!IsOccluded(shadowRay, distToLight, i)) {
            float NdotL = max(dot(h.normal, dirToLight), 0.0f);
            if (NdotL <= 0.0f) continue;
            float3 H = normalize(V + dirToLight);
            float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
            float D = DistributionGGX(max(dot(h.normal, H), 0.0f), h.rough);
            float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
            float NdotV = max(dot(h.normal, V), 0.0f);
            float3 brdf = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - h.metal) * h.albedo / PI + (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);
            float3 contrib = brdf * light.Emission.xyz * light.Emission.w * NdotL;

            float brightness = dot(contrib, float3(0.2126f, 0.7152f, 0.0722f));
            if (brightness > 10.0f) contrib *= (10.0f / brightness);
            directLight += contrib * lightScale;
        }
    }

    directLight += h.emission;

    // Analytic component lights (directional/point/spot/area). All submitted
    // lights are evaluated (capped at 8) with shadow rays, using the same
    // Cook-Torrance BRDF as the emissive-geometry lights above.
    for (int a = 0; a < 8; a++) {
        if (a >= u_ALightCount) break;
        float atype = u_ALightData[a].x;
        float3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0f);
        float afall = max(u_ALightData[a].w, 0.5f);
        float3 L;
        float att = 1.0f;
        float shadowLen = 100000.0f;
        if (atype < 0.5f) {
            L = -u_ALightDir[a];
        } else {
            float3 toL = u_ALightPos[a] - h.worldPos;
            float d = length(toL);
            L = toL / max(d, 0.00001f);
            if (arange > 0.0f) {
                if (d >= arange) continue;
                att *= pow(clamp(1.0f - d / arange, 0.0f, 1.0f), afall);
                shadowLen = d;
            } else {
                att *= 1.0f / (1.0f + 0.1f * afall * d * d);
                shadowLen = min(d, 500.0f);
            }
            if (atype > 1.5f && atype < 2.5f) {
                float c = dot(-L, u_ALightDir[a]);
                att *= smoothstep(u_ALightData2[a].y, u_ALightData2[a].x, c);
                if (att <= 0.0f) continue;
            } else if (atype > 2.5f) {
                float front = dot(-L, u_ALightDir[a]);
                if (u_ALightData2[a].w < 0.5f) {
                    if (front <= 0.0f) continue;
                    att *= clamp(front * 3.0f, 0.0f, 1.0f);
                }
                float asize = max(u_ALightData2[a].z, 0.01f);
                att *= asize / (d + asize);
            }
        }
        float aNdotL = max(dot(h.normal, L), 0.0f);
        if (aNdotL <= 0.0f) continue;
        Ray aShadowRay = MakeRay(h.worldPos + h.normal * 0.001f, L);
        if (!IsOccluded(aShadowRay, shadowLen, -1)) {
            float3 aH = normalize(V + L);
            float3 aF = FresnelSchlick(max(dot(aH, V), 0.0f), F0);
            float aD = DistributionGGX(max(dot(h.normal, aH), 0.0f), h.rough);
            float aG = GeometrySmith(h.normal, V, L, h.rough);
            float aNdotV = max(dot(h.normal, V), 0.0f);
            float3 abrdf = (float3(1.0f, 1.0f, 1.0f) - aF) * (1.0f - h.metal) * h.albedo / PI + (aD * aG * aF) / max(4.0f * aNdotV * aNdotL, 0.001f);
            float3 acontrib = abrdf * lcol * att * aNdotL;

            float abright = dot(acontrib, float3(0.2126f, 0.7152f, 0.0722f));
            if (abright > 10.0f) acontrib *= (10.0f / abright);
            directLight += acontrib;
        }
    }

    float skyT = 0.5f * (h.normal.y + 1.0f);
    float3 skyAmbient = lerp_safe(u_SkyBottomColor, u_SkyTopColor, skyT) * 0.15f;
    directLight += diffuseColor * skyAmbient * h.occlusion;

    // Analytic sky env approx for metals. Skipped when the caller traces a
    // true mirror ray instead (Low-preset metals) so the sky isn't counted twice.
    if (h.metal > 0.0f && !skipEnv) {
        float3 R = reflect(-V, h.normal);
        float3 specSky = GetSkyColor(R);
        float3 diffSky = skyAmbient / 0.15f; 
        float3 envColor = lerp_safe(specSky, diffSky, h.rough * h.rough);
        directLight += F0 * envColor * h.metal * h.occlusion;
    }

    return directLight;
}

// God-ray sun visibility: one shadow tap toward the sun for fogged pixels.
// Fog in shadowed regions keeps ambient but loses sun in-scatter, so shafts
// appear where geometry breaks the sunlight. Skipped (returns 1) when the ray
// enters no fog volume. Tested at the surface end for hits, at the camera
// for sky rays — a single-tap approximation, noted as such.
float wl_godray_visibility(float3 ro, float3 rd, float tHit)
{
    if (u_FogEnabled != 1 || u_FogCount <= 0)
        return 1.0f;
    bool hitsFog = false;
    for (int i = 0; i < 4; i++)
    {
        if (i >= u_FogCount)
            break;
        if (u_FogData2[i].w < 0.5f)
            continue;
        float2 r = wl_box_range(ro, rd, u_FogMin[i], u_FogMax[i]);
        if (min(r.y, tHit) > max(r.x, 0.0f))
        {
            hitsFog = true;
            break;
        }
    }
    if (!hitsFog)
        return 1.0f;
    float3 tapPoint = (tHit < 500.0f) ? (ro + rd * tHit) : ro;
    Ray sunRay = MakeRay(tapPoint + u_SunDirection * 0.05f, u_SunDirection);
    if (IsOccluded(sunRay, 100000.0f, -1))
        return 0.12f;
    return 1.0f;
}

float3 GetSkyColor(float3 dir) {
    float t = 0.5f * (normalize(dir).y + 1.0f);
    return lerp_safe(u_SkyBottomColor, u_SkyTopColor, t);
}

void RunTraceAndDenoise()
{
    int2 pixelCoords = int2(g_dt.xy);
    int2 imgSize = IMG_SIZE(img_Output);
    int2 traceSize = wl_trace_extent(imgSize);
    if (pixelCoords.x >= traceSize.x || pixelCoords.y >= traceSize.y) return;

    seed = (uint)(pixelCoords.y * 1024 + pixelCoords.x) + (uint)(u_FrameIndex * 1000);
    float2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
    float2 jitteredUV = ((float2(pixelCoords) + jitter) / float2(traceSize)) * 2.0f - 1.0f;
    
    float4 farPoint = mul(u_InverseViewProjection, float4(jitteredUV, 1.0f, 1.0f));
    Ray primaryRay = MakeRay(u_CameraPosition, normalize(farPoint.xyz / farPoint.w - u_CameraPosition));
    HitInfo primary = TraceScene(primaryRay);

    IMG_STORE(img_AlbedoHit, pixelCoords, float4(primary.hit ? primary.albedo : float3(1.0f, 1.0f, 1.0f), primary.hit ? 1.0f : 0.0f));

    // Trace G-buffer: NDC depth (far on miss) + packed world normal for the
    // velocity / temporal / bilateral passes. Full-size buffer, subregion data.
    // (RunResolve was removed; binding 6 was dead. Image units are capped at
    // 0-7 on this GL driver, so depth+normal share one RGBA32F image.)
    if (!primary.hit) {
        IMG_STORE(img_TraceGBuffer, pixelCoords, float4(1.0f, 0.5f, 0.5f, 1.0f));
    } else {
        float4 tclip = mul(u_ViewProjection, float4(primary.worldPos, 1.0f));
        float tndc = tclip.z / max(tclip.w, 1e-6f);
        IMG_STORE(img_TraceGBuffer, pixelCoords, float4(clamp(tndc * 0.5f + 0.5f, 0.0f, 1.0f), primary.normal * 0.5f + 0.5f));
    }

    if (!primary.hit) {
        float3 skyCol = GetSkyColor(primaryRay.Direction);
        // Static per-pixel dither (NOT frame-animated): accumulation converges
        // a stable field in a few frames, while a cycling dither keeps every
        // frame different and the image shimmers forever, especially upsized.
        float skyDith = wl_hash12(float2(pixelCoords));
        // Skip the volume march entirely when the scene submits no volumes
        // (the per-volume loops would break at count 0 anyway).
        if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
        {
            float skySunVis = wl_godray_visibility(primaryRay.Origin, primaryRay.Direction, 600.0f);
            skyCol = wl_apply_volumetrics(primaryRay.Origin, primaryRay.Direction, 600.0f, skyCol, skyDith, skySunVis);
        }
        IMG_STORE(img_Bloom, pixelCoords, float4(skyCol, 1.0f));
        return;
    }

    HitInfo tileHit = primary;
    Ray lightRay = primaryRay;

    float3 directLight = float3(0.0f, 0.0f, 0.0f);
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    if (tileHit.hit) {
        float3 V = normalize(-lightRay.Direction);
        uint lightSeed = (uint)(pixelCoords.x * 73 + pixelCoords.y * 149 + u_FrameIndex * 2);
        int maxIndirectRays = (u_QualityLevel == 0) ? 0 :
                              ((u_QualityLevel <= 2) ? min(u_IndirectRays, 1) : u_IndirectRays);
        // Perf tier: at reduced render scale the neural cache carries diffuse
        // GI (it already blends 65% into the traced result) — skip the second
        // traced bounce for non-metals and save ~40% of the trace pass.
        // Metals keep the ray so reflections stay sharp.
        if (maxIndirectRays > 0 && u_RenderScale <= 0.75f && tileHit.metal <= 0.5f &&
            u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f)
            maxIndirectRays = 0;
        // Mirror gap-fill: Low preset traces no indirect rays, so metals
        // would only get the analytic sky approx. Trace one real mirror ray
        // instead and skip the approx inside ComputeDirectLighting.
        bool doMirror = (tileHit.metal > 0.01f && maxIndirectRays == 0);
        directLight = ComputeDirectLighting(tileHit, V, lightSeed, doMirror);

        if (maxIndirectRays > 0) {
            float3 diffuseColor = tileHit.albedo * (1.0f - tileHit.metal);
            float3 F0 = lerp_safe(float3(0.04f, 0.04f, 0.04f), max(tileHit.albedo, float3(0.04f, 0.04f, 0.04f)), tileHit.metal) * tileHit.f0Tint;
            float3 indirectReflectance = lerp_safe(diffuseColor, F0, tileHit.metal);

            for (int r = 0; r < maxIndirectRays; r++) {
                seed = (uint)(pixelCoords.y * 1024 + pixelCoords.x) + (uint)(u_FrameIndex * 1000) + (uint)((r + 1) * 7919);
                
                float3 indirectDir;
                if (hash() < tileHit.metal) {
                    float3 specDir = reflect(-V, tileHit.normal);
                    float3 randHem = random_in_hemisphere(tileHit.normal);
                    indirectDir = normalize(lerp_safe(specDir, randHem, tileHit.rough * tileHit.rough));
                } else {
                    indirectDir = random_in_hemisphere(tileHit.normal);
                }

                Ray indirectRay = MakeRay(tileHit.worldPos + tileHit.normal * 0.001f, indirectDir);
                HitInfo indirect = TraceScene(indirectRay);

                if (indirect.hit) {
                    float3 indirectV = normalize(-indirectDir);
                    float3 bouncedLight = ComputeDirectLighting(indirect, indirectV, lightSeed + 1u, false);
                    indirectLight += indirectReflectance * bouncedLight;
                } else {
                    indirectLight += indirectReflectance * GetSkyColor(indirectDir);
                }
            }
            indirectLight /= (float)(maxIndirectRays);
            indirectLight *= tileHit.occlusion;
            // Neural radiance cache blend: learned GI steadies the 1-ray estimate (OpenGL neural path).
            // Weighted down on metals so the traced specular reflection dominates.
            if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
                float3 neuralGI = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V);
                float neuralK = clamp(u_NeuralLightStrength, 0.0f, 1.0f) * 0.65f * (1.0f - tileHit.metal * 0.8f);
                indirectLight = lerp_safe(indirectLight, neuralGI * tileHit.occlusion, neuralK);
            }
        } else if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
            // No traced bounces at this quality level: neural cache IS the GI (e.g. Low preset)
            indirectLight = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V) * tileHit.occlusion;
        }

        // Sharp scene reflection for metals with no traced bounce (Low preset):
        // one roughness-jittered mirror ray, Fresnel-tinted. Shadow-correct
        // because the reflected hit is shaded with full shadowed direct light.
        if (doMirror) {
            seed = (uint)(pixelCoords.y * 1024 + pixelCoords.x) + (uint)(u_FrameIndex * 1000) + 40543u;
            float3 R = reflect(-V, tileHit.normal);
            float3 mirrorDir = normalize(lerp_safe(R, random_in_hemisphere(tileHit.normal), tileHit.rough * tileHit.rough));
            Ray mirrorRay = MakeRay(tileHit.worldPos + tileHit.normal * 0.002f, mirrorDir);
            HitInfo mirrorHit = TraceScene(mirrorRay);
            float3 mirrorColor;
            if (mirrorHit.hit) {
                float3 mirrorV = normalize(-mirrorRay.Direction);
                mirrorColor = ComputeDirectLighting(mirrorHit, mirrorV, lightSeed + 7u, false);
            } else {
                mirrorColor = GetSkyColor(mirrorRay.Direction);
            }
            float3 mF0 = lerp_safe(float3(0.04f, 0.04f, 0.04f), max(tileHit.albedo, float3(0.04f, 0.04f, 0.04f)), tileHit.metal) * tileHit.f0Tint;
            float3 mF = FresnelSchlick(clamp(dot(tileHit.normal, V), 0.0f, 1.0f), mF0);
            directLight += mF * mirrorColor * tileHit.metal;
        }
    } else {
        directLight = GetSkyColor(lightRay.Direction);
    }

    directLight = clamp(directLight, float3(0.0f, 0.0f, 0.0f), float3(10.0f, 10.0f, 10.0f));
    indirectLight = clamp(indirectLight, float3(0.0f, 0.0f, 0.0f), float3(5.0f, 5.0f, 5.0f));

    // ComputeDirectLighting already folds albedo into the BRDF (diffuse/spec/
    // ambient/emission), and indirectLight already carries tile reflectance -
    // do NOT multiply by albedo again (albedo^2 pushed textured floors to black).
    float3 finalPixelColor = directLight + indirectLight;
    // Volumetric atmosphere over the primary ray (clouds, then fog).
    // Skipped when the scene submits no volumes (see sky path above).
    if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
    {
        float volDist = distance(u_CameraPosition, tileHit.worldPos);
        float volDith = wl_hash12(float2(pixelCoords) + 7.3f);
        float volSunVis = wl_godray_visibility(lightRay.Origin, lightRay.Direction, volDist);
        finalPixelColor = wl_apply_volumetrics(lightRay.Origin, lightRay.Direction, volDist, finalPixelColor, volDith, volSunVis);
    }
    
    // Global atmospheric fog: distance haze for all surfaces
    {
        float surfDist = distance(u_CameraPosition, tileHit.worldPos);
        float gfDith = wl_hash12(float2(pixelCoords) + 3.7f);
        finalPixelColor = wl_apply_global_fog(finalPixelColor, surfDist, lightRay.Direction, gfDith);
    }
    
    IMG_STORE(img_Bloom, pixelCoords, float4(finalPixelColor, 1.0f));
}

void RunTemporalAccumulation() {
    int2 pixelCoords = int2(g_dt.xy);
    int2 imgSize = wl_trace_extent(IMG_SIZE(img_Accumulation));
    float2 uv = (float2(pixelCoords) + 0.5f) / float2(imgSize);

    float3 currentLight = IMG_LOAD(img_Bloom, pixelCoords).rgb;
    float2 velocity = IMG_LOAD(img_Velocity, pixelCoords).xy;
    float2 prevUV = uv - velocity;
    // Accumulation + depth buffers hold the subregion; map history UVs to it.
    float2 histUV = prevUV * u_RenderScale;
    // Keep bilinear taps inside the valid subregion: outside it lives stale
    // data (e.g. from full-res frames), which would streak the frame edges.
    float2 fullRes = float2(IMG_SIZE(img_Accumulation));
    histUV = min(histUV, float2(u_RenderScale, u_RenderScale) - 0.5f / fullRes);

    bool validHistory = u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;
    float motionMag = length(velocity * float2(imgSize));

    if (validHistory) {
        float currentDepth = TEX(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = TEX(s_DepthBuffer, histUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    float3 historyLight = TEX(s_Accumulation, histUV).rgb;

    if (validHistory) {
        if (u_QualityLevel >= 3) {
            float3 m1 = float3(0.0f, 0.0f, 0.0f);
            float3 m2 = float3(0.0f, 0.0f, 0.0f);
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    int2 sp = clamp(pixelCoords + int2(x, y), int2(0, 0), imgSize - int2(1, 1));
                    float3 s = RGB2YCoCg(IMG_LOAD(img_Bloom, sp).rgb);
                    m1 += s;
                    m2 += s * s;
                }
            }
            float3 mean = m1 / 9.0f;
            float3 stddev = sqrt(max(float3(0.0f, 0.0f, 0.0f), m2 / 9.0f - mean * mean));

            float3 historyYCoCg = RGB2YCoCg(historyLight);
            float3 minCol = mean - 1.25f * stddev;
            float3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        } else {
            // History clamp at EVERY tier (not just High/Ultra): without it,
            // per-frame variations (trace jitter cycle, shadow-seed rotation,
            // mirror-ray randomness on metals) smear unboundedly through the
            // blend and Low/Medium never resolve sharp. Pulls history toward
            // the current frame's local neighborhood; slight noise cost.
            int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
            float3 m1 = float3(0.0f, 0.0f, 0.0f);
            float3 m2 = float3(0.0f, 0.0f, 0.0f);
            for (int i = 0; i < 5; i++) {
                int2 sp = clamp(pixelCoords + offsets[i], int2(0, 0), imgSize - int2(1, 1));
                float3 s = RGB2YCoCg(IMG_LOAD(img_Bloom, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
            float3 mean = m1 / 5.0f;
            float3 stddev = sqrt(max(float3(0.0f, 0.0f, 0.0f), m2 / 5.0f - mean * mean));

            float3 historyYCoCg = RGB2YCoCg(historyLight);
            float3 minCol = mean - 1.25f * stddev;
            float3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        }
    }

    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? lerp_safe(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;

    float3 result = lerp_safe(historyLight, currentLight, alpha);
    IMG_STORE(img_Accumulation, pixelCoords, float4(result, 1.0f));
}

void RunBloomThreshold() {
    int2 pos = int2(g_dt.xy);
    float4 color = IMG_LOAD(img_Output, pos);
    
    float threshold = u_BloomThreshold;
    float knee = 0.1f; 
    
    float brightness = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float soft = smoothstep(threshold - knee, threshold + knee, brightness);
    
    IMG_STORE(img_Bloom, pos, color * soft);
}

void RunBloomBlurHorizontal() {
    int2 pos = int2(g_dt.xy);
    float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };
    float4 sum = IMG_LOAD(img_Bloom, pos) * weights[0];
    
    for(int i = 1; i < 5; i++) {
        sum += IMG_LOAD(img_Bloom, pos + int2(i, 0)) * weights[i];
        sum += IMG_LOAD(img_Bloom, pos - int2(i, 0)) * weights[i];
    }
    IMG_STORE(img_Bloom_Temp, pos, sum);
}

void RunBloomBlurVertical()
{
    int2 pos = int2(g_dt.xy);
    float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };
    float4 sum = IMG_LOAD(img_Bloom_Temp, pos) * weights[0];

    for(int i = 1; i < 5; i++) {
        sum += IMG_LOAD(img_Bloom_Temp, pos + int2(0, i)) * weights[i];
        sum += IMG_LOAD(img_Bloom_Temp, pos + int2(0, -i)) * weights[i];
    }

    float3 existing = IMG_LOAD(img_Output, pos).rgb;
    // u_BloomIntensity was unreachable before; the sum was added at full weight.
    float3 finalColor = existing + sum.rgb * u_BloomIntensity;
    IMG_STORE(img_Output, pos, float4(finalColor, 1.0f));
}

// Bilinear tap into img_Accumulation at a full-frame UV, honouring the internal
// render scale. Extracted from the upscale block so chromatic aberration
// resamples identically instead of duplicating (and drifting from) that logic.
float3 BilinearAccum(float2 uv, float renderScale)
{
    int2 fullSize = int2(g_dt.xy);
    int2 srcSize = int2(float2(fullSize) * max(renderScale, 0.001f));
    srcSize = max(srcSize, int2(1, 1));

    float2 srcCoord = uv * float2(srcSize);
    float2 srcPixel = srcCoord - float2(0.5f, 0.5f);
    int2 s0 = clamp(int2(floor(srcPixel)), int2(0, 0), srcSize - int2(1, 1));
    int2 s1 = clamp(s0 + int2(1, 1), int2(0, 0), srcSize - int2(1, 1));
    float2 f = clamp(srcPixel - float2(s0), float2(0.0f, 0.0f), float2(1.0f, 1.0f));

    float3 c00 = IMG_LOAD(img_Accumulation, s0).rgb;
    float3 c10 = IMG_LOAD(img_Accumulation, int2(s1.x, s0.y)).rgb;
    float3 c01 = IMG_LOAD(img_Accumulation, int2(s0.x, s1.y)).rgb;
    float3 c11 = IMG_LOAD(img_Accumulation, s1).rgb;
    return lerp_safe(lerp_safe(c00, c10, f.x), lerp_safe(c01, c11, f.x), f.y);
}

void RunComposite() {
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    // img_Accumulation / img_Bloom_Temp already hold FINAL shaded color
    // (albedo * direct + indirect, see RunTraceAndDenoise), so it must NOT
    // be multiplied by albedo again here. The old `denoised * albedo` applied
    // albedo twice (albedo^2 on direct, albedo-scaled indirect) and pushed
    // textured surfaces (e.g. the floor) toward black.
    float3 denoised;

    if (u_RenderScale < 1.0f) {
        int2 srcSize = int2(float2(fullSize) * u_RenderScale);
        if (srcSize.x < 1) srcSize.x = 1;
        if (srcSize.y < 1) srcSize.y = 1;

        float2 srcCoord = (float2(pos) + 0.5f) * u_RenderScale;
        float2 srcPixel = srcCoord - float2(0.5f, 0.5f);
        int2 s0 = clamp(int2(floor(srcPixel)), int2(0, 0), srcSize - int2(1, 1));
        int2 s1 = clamp(s0 + int2(1, 1), int2(0, 0), srcSize - int2(1, 1));
        // Clamp weights: at the frame border srcPixel can go slightly negative
        // and unclamped extrapolation rings bright/dark edges.
        float2 f = clamp(srcPixel - float2(s0), float2(0.0f, 0.0f), float2(1.0f, 1.0f));

        float3 c00 = IMG_LOAD(img_Accumulation, s0).rgb;
        float3 c10 = IMG_LOAD(img_Accumulation, int2(s1.x, s0.y)).rgb;
        float3 c01 = IMG_LOAD(img_Accumulation, int2(s0.x, s1.y)).rgb;
        float3 c11 = IMG_LOAD(img_Accumulation, s1).rgb;
        denoised = lerp_safe(lerp_safe(c00, c10, f.x), lerp_safe(c01, c11, f.x), f.y);
    } else {
        denoised = IMG_LOAD(img_Accumulation, pos).rgb;
    }

    float3 combined = denoised;

    float2 uv = (float2(pos) + 0.5f) / float2(fullSize);
    float2 centered = uv * 2.0f - 1.0f;

    // CHROMATIC ABERRATION: per-channel radial resample, done after the resolve
    // because it is a lens effect rather than a lighting one.
    if (u_ChromaticAberrationEnabled == 1 && u_ChromaticAberrationIntensity > 0.001f) {
        float caAmount = u_ChromaticAberrationIntensity * 0.01f;
        float2 dir = centered * caAmount;
        combined.r = BilinearAccum(uv + dir, u_RenderScale).r;
        combined.b = BilinearAccum(uv - dir, u_RenderScale).b;
    }

    // VIGNETTE: radial falloff with a soft shoulder.
    if (u_VignetteEnabled == 1 && u_VignetteIntensity > 0.001f) {
        float r = length(centered) / max(u_VignetteRadius, 0.001f);
        float vig = smoothstep(1.0f, 1.0f - max(u_VignetteSoftness, 0.001f), r);
        combined *= lerp_safe(1.0, vig, clamp(u_VignetteIntensity, 0.0, 2.0));
    }

    float3 mapped = combined / (combined + float3(1.0f, 1.0f, 1.0f));

    // COLOUR GRADING in display space; temperature/tint are signed scalars.
    if (u_ColorGradingEnabled == 1) {
        mapped.r += u_ColorTemperature * 0.10 + u_ColorTint * 0.04;
        mapped.b -= u_ColorTemperature * 0.10 - u_ColorTint * 0.04;
        mapped.g += u_ColorTint * 0.06;
        mapped = clamp(mapped, float3(0.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 1.0f));
    }

    // DXC rejects a nested scalar splat inside pow()'s argument list
    // ("too few elements in vector initialization"); fxc accepts it. Naming the
    // exponent keeps this portable to DXC/SPIR-V, which the Vulkan and RTX Kit
    // paths need. Same value, no behaviour change.
    float gammaExponent = 1.0f / max(u_ColorGamma, 0.1f);
    float3 finalColor = pow(mapped, float3(gammaExponent, gammaExponent, gammaExponent));

    // FILM GRAIN after gamma, weighted toward midtones.
    if (u_FilmGrainEnabled == 1 && u_FilmGrainIntensity > 0.001f) {
        float n = frac(sin(dot(float2(pos) + float2(float(u_FrameIndex) * 17.0f, 0.0f),
                               float2(12.9898f, 78.233f))) * 43758.5453f);
        float lum = dot(finalColor, float3(0.2126f, 0.7152f, 0.0722f));
        finalColor += (n - 0.5) * u_FilmGrainIntensity * 0.15 * (1.0 - abs(lum * 2.0 - 1.0));
    }

    finalColor = clamp(finalColor, 0.0, 1.0);
    IMG_STORE(img_Output, pos, float4(finalColor, 1.0f));

    // Bloom threshold must run at ANY scale: passes 4/5 blur img_Bloom at full
    // res, so leaving stale trace-subregion data in it (by skipping this when
    // scaled) smears a ghost copy of the frame over the output.
    if (u_QualityLevel >= 1) {
        float brightness = dot(combined, float3(0.2126f, 0.7152f, 0.0722f));
        float threshold = u_BloomThreshold;
        float knee = 0.15f;
        float soft = smoothstep(threshold - knee, threshold + knee, brightness);
        IMG_STORE(img_Bloom, pos, float4(combined * soft, 1.0f));
    }
}

void RunBilateralBlur() {
    if (u_QualityLevel <= 1) return;

    int2 pos = int2(g_dt.xy);
    int2 imgSize = wl_trace_extent(IMG_SIZE(img_Bloom_Temp));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    float3 centerColor = IMG_LOAD(img_Accumulation, pos).rgb;
    
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        IMG_STORE(img_Bloom_Temp, pos, float4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    float2 uv = (float2(pos) + 0.5f) / float2(imgSize);
    float centerDepth = TEX(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (centerDepth >= 1.0f) { 
        IMG_STORE(img_Bloom_Temp, pos, float4(centerColor, 1.0f));
        return;
    }

    float3 centerNormal = TEX(s_NormalBuffer, wl_buf_uv(uv)).gba * 2.0f - 1.0f;
    float centerLuma = GetLuminance(centerColor);

    int step = (int)(max(1.0f, (float)(u_StepSize)));
    float3 totalColor = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    if (u_QualityLevel == 2) {
        int2 offsets[9] = { 
            int2(0, 0), 
            int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
            int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2)
         };
        for (int i = 0; i < 9; i++) {
            int2 samplePos = clamp(pos + offsets[i] * step, int2(0, 0), imgSize - int2(1, 1));

            float3 neighborColor = IMG_LOAD(img_Accumulation, samplePos).rgb;
            float2 sampleUV = (float2(samplePos) + 0.5f) / float2(imgSize);

            float neighborDepth = TEX(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            float3 neighborNormal = TEX(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
            float neighborLuma = GetLuminance(neighborColor);

            float spatialWeight = (i == 0) ? 0.375f : ((i <= 4) ? 0.125f : 0.03125f);
            float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 16.0f);
            float depthDiff = abs(centerDepth - neighborDepth);
            float depthWeight = exp(-depthDiff * 100.0f);
            float lumaDiff = abs(centerLuma - neighborLuma);
            float lumaWeight = exp(-lumaDiff * 4.0f);

            float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    } else if (u_QualityLevel >= 3) {
        float lumaSum = 0.0f;
        float lumaSqSum = 0.0f;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                int2 sp = clamp(pos + int2(x, y), int2(0, 0), imgSize - int2(1, 1));
                float l = GetLuminance(IMG_LOAD(img_Accumulation, sp).rgb);
                lumaSum += l;
                lumaSqSum += l * l;
            }
        }
        float lumaMean = lumaSum / 9.0f;
        float lumaVar = sqrt(max(0.0f, (lumaSqSum / 9.0f) - (lumaMean * lumaMean)));

        static const float kernel[3] = { 0.375f, 0.25f, 0.0625f };

        for (int x = -2; x <= 2; x++) {
            for (int y = -2; y <= 2; y++) {
                int2 samplePos = clamp(pos + int2(x, y) * step, int2(0, 0), imgSize - int2(1, 1));

                float3 neighborColor = IMG_LOAD(img_Accumulation, samplePos).rgb;
                float2 sampleUV = (float2(samplePos) + 0.5f) / float2(imgSize);

                float neighborDepth = TEX(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
                float3 neighborNormal = TEX(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
                float neighborLuma = GetLuminance(neighborColor);

                float spatialWeight = kernel[abs(x)] * kernel[abs(y)];
                float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 32.0f);
                float depthDiff = abs(centerDepth - neighborDepth);
                float depthWeight = exp(-depthDiff * 100.0f);
                float lumaDiff = abs(centerLuma - neighborLuma);
                float lumaWeight = exp(-lumaDiff / (lumaVar * 4.0f + 0.001f));

                float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

                totalColor += neighborColor * weight;
                totalWeight += weight;
            }
        }
    }

    float3 finalFiltered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : centerColor;
    IMG_STORE(img_Bloom_Temp, pos, float4(finalFiltered, 1.0f));
}

void GenerateMaterialMaps() {
    int2 pos = int2(g_dt.xy);
    int2 size = IMG_SIZE(img_MaterialPacked);
    
    float I[3][3];
    for(int i = -1; i <= 1; i++) {
        for(int j = -1; j <= 1; j++) {
            int2 samplePos = clamp(pos + int2(i, j), int2(0, 0), size - int2(1, 1));
            float2 uv = (float2(samplePos) + 0.5f) / float2(size);
            I[i+1][j+1] = GetLuminance(TEX(s_InputAlbedo, uv).rgb);
        }
    }

    float resX = 0.0f;
    float resY = 0.0f;
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++) {
            resX += I[i][j] * Gx[i][j];
            resY += I[i][j] * Gy[i][j];
        }
    }

    float normalStrength = u_NormalStrength; 
    float3 normal = normalize(float3(-resX * normalStrength, -resY * normalStrength, 1.0f));

    float2 packedNormal = normal.xy * 0.5f + 0.5f;
    
    float ao = 1.0f - (length(float2(resX, resY)) * 5.0f * u_AOIntensity);
    float rough = clamp((1.0f - I[1][1]) + u_RoughnessBias, 0.0f, 1.0f);
    
    float4 packedData = float4(packedNormal.x, packedNormal.y, rough, ao);
    
    IMG_STORE(img_MaterialPacked, pos, packedData);
}

void RunUpscale() {
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    int2 srcSize = int2(float2(fullSize) * u_RenderScale);
    if (srcSize.x < 1) srcSize.x = 1;
    if (srcSize.y < 1) srcSize.y = 1;

    float2 srcCoord = (float2(pos) + 0.5f) * u_RenderScale;
    float2 srcPixel = srcCoord - float2(0.5f, 0.5f);
    int2 src0 = int2(floor(srcPixel));
    int2 src1 = src0 + int2(1, 1);
    float2 frac_ = srcPixel - float2(src0);

    src0 = clamp(src0, int2(0, 0), srcSize - int2(1, 1));
    src1 = clamp(src1, int2(0, 0), srcSize - int2(1, 1));

    float3 c00 = IMG_LOAD(img_Bloom_Temp, src0).rgb;
    float3 c10 = IMG_LOAD(img_Bloom_Temp, int2(src1.x, src0.y)).rgb;
    float3 c01 = IMG_LOAD(img_Bloom_Temp, int2(src0.x, src1.y)).rgb;
    float3 c11 = IMG_LOAD(img_Bloom_Temp, src1).rgb;

    float3 c0 = lerp_safe(c00, c10, frac_.x);
    float3 c1 = lerp_safe(c01, c11, frac_.x);
    float3 color = lerp_safe(c0, c1, frac_.y);

    IMG_STORE(img_Output, pos, float4(color, 1.0f));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    g_dt = dispatchThreadID;
    g_tid = (dispatchThreadID.y % 8) * 8 + (dispatchThreadID.x % 8);
    img_Output.GetDimensions(g_dims.x, g_dims.y);
    if (any(dispatchThreadID.xy >= g_dims)) return;
    if ((u_PassID == 0 || u_PassID == 1 || u_PassID == 2 || u_PassID == 7) &&
        any(dispatchThreadID.xy >= uint2(wl_trace_extent(int2(g_dims))))) return;
    switch(u_PassID)
    {
        case 0: RunVisibilityAndVelocity(); break;
        case 1: RunTraceAndDenoise(); break;
        case 2: RunTemporalAccumulation(); break;
        case 4: RunBloomBlurHorizontal(); break;
        case 5: RunBloomBlurVertical(); break;
        case 6: RunComposite(); break;
        case 7: RunBilateralBlur(); break;
        case 8: RunUpscale(); break;
        case 9: GenerateMaterialMaps(); break;
        case 10: RunGodRays(); break;
    }
}
