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
    int TerrainVertexOffset;
    int TerrainCellsPerSide;
    int TerrainPad0;
    int TerrainPad1;
};

#ifdef WASTELAND_NOVA_TERRAIN_GRID
// One packed 32-byte vertex in the uploaded row-major heightfield grid.
struct TerrainNovaVertex
{
    float4 Position;
    float4 Normal;
};
#endif

struct BVHNode
{
    float4 MinBounds; // xyz = AABB min, w = leftChild (inner) or -(firstInstance+1) (leaf)
    float4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
};

struct Ray { float3 Origin; float3 Direction; };
Ray MakeRay(float3 o, float3 d) { Ray r; r.Origin = o; r.Direction = d; return r; }

// ---- ReSTIR DI reservoir (32 bytes per pixel) ----
// Bitterli et al. ACM TOG 2020. The reservoir stores a single selected
// light sample with its weight, plus the running sum of target PDFs and
// the sample count. bary.x stores the target PDF of the selected sample
// (needed for the W update); bary.y, normalOct and flagsValid are for
// spatial reuse validation.
struct LightReservoir {
    uint lightIndex;   // +0  selected light index
    uint M;            // +4  number of samples seen
    float W;           // +8  finalized RIS estimator weight
    float wSum;        // +12 aggregate candidate weight (target/source PDF)
    float2 bary;       // +16 legacy name: x=selected pHat, y=receiver depth
    uint normalOct;    // +24 octahedral-encoded receiver normal (temporal validation)
    uint flagsValid;   // +28 bit 0 valid; upper bits producer-frame tag
};

// ---- images (GL image units -> u registers) ----
RWTexture2D<float4> img_Output        : register(u0);
RWTexture2D<float4> img_Accumulation  : register(u1);
RWTexture2D<float4> img_Bloom         : register(u2);
RWTexture2D<float2> img_Velocity      : register(u5);
RWTexture2D<float4> img_TraceGBuffer  : register(u6);
RWTexture2D<float4> img_Bloom_Temp    : register(u7);
RWTexture2D<unorm float4> img_MaterialPacked : register(u3);
RWTexture2D<float4> img_AlbedoHit     : register(u4);
RWTexture2D<float2> img_Moments       : register(u9);
RWTexture2D<float4> img_HistoryBuffer : register(u11);
// u12: the neural stage's globals. An RWStructuredBuffer<uint> whose element
// layout is byte-for-byte the GLSL's NeuralGlobals SSBO -- elements 0..3 are the
// four frame means as RAW FLOAT BITS (the GLSL's `float g_Mean[4]` at byte 0),
// elements 4.. are g_Partial[] (the GLSL's `uint g_Partial[NEURAL_MAX_BLOCKS*4]`
// at byte 16), then g_GateMean (a float at element 4+MAX*4) and g_GatePartial[].
// The named NEURAL_*_BASE constants below carry the offsets so the two twins
// cannot drift apart silently.
// Both layouts put element i at byte 4i, so the two shaders agree without either
// reinterpreting anything. `asfloat`/`asuint` are the only conversions.
//
// It was an empty slot before this stage: nothing in the shader claimed u12, and
// the binding reservation (docs/NEURAL_RENDERING_BINDING_PLAN.md) assigns it
// here. Taking it is what forced the DX12 descriptor tables wider -- see
// DX12Nova.h's heap layout comment.
RWStructuredBuffer<uint> NeuralGlobals : register(u12);

// ---- scene data (GL SSBO bindings 1/2/3 -> t registers) ----
StructuredBuffer<RayTracingInstance> Instances    : register(t1);
StructuredBuffer<BVHNode>            BVHNodes     : register(t2);
StructuredBuffer<int>                LightIndices : register(t3);
#ifdef WASTELAND_NOVA_TERRAIN_GRID
StructuredBuffer<TerrainNovaVertex>  TerrainVertices : register(t4);
#endif

// ---- ReSTIR DI reservoir ping-pong pair (t10/u10) ----
StructuredBuffer<LightReservoir> ReservoirRead  : register(t10);
RWStructuredBuffer<LightReservoir> ReservoirWrite : register(u10);

// ---- sampled buffers (GL sampler units shifted to t48+ to avoid
// colliding with the StructuredBuffer registers t1/t2/t3) ----
Texture2D s_Accumulation  : register(t48);
Texture2D s_DepthBuffer   : register(t49);
Texture2D s_Output        : register(t50);
Texture2D s_NormalBuffer  : register(t51);
Texture2D s_InputAlbedo   : register(t52);
Texture2D s_Moments       : register(t53);
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
    // RECONSTRUCTION FILTER -- the HLSL twin of the GLSL declaration. This was
    // plumbed from the CPU (enum, cbuffer slot, all three NVRHI Set() calls) but
    // never declared, so the upload went nowhere and the editor's AA dropdown did
    // nothing on any backend. The values match Renderer3D::ReconstructionFilter:
    // 0 None, 1 Nearest, 2 Bilinear, 3 Box, 4 Bicubic, 5 Lanczos, and 6..11 are
    // separate passes that fall through to bilinear rather than being ignored.
    int     u_ReconstructionFilter;
    int     u_SamplePattern;
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
    // Upper bound on the volumetric march step count (see the GLSL twin's note).
    // 0 means the previous behaviour, 32.
    int     u_VolStepCap;
    // Runtime cap on the per-step volumetric light loop (see the GLSL twin's
    // u_VolLightCap note). 0 means "use WL_VOL_LIGHT_CAP", the previous behaviour.
    int     u_VolLightCap;
    // 1 enables the hoisted segment range cull; 0 restores the pre-cull
    // behaviour of traversing every selected light. See the GLSL twin.
    int     u_VolRangeCull;
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
    float3  u_ALightPos[32];
    float3  u_ALightDir[32];
    float3  u_ALightColor[32];
    float4  u_ALightData[32];
    float4  u_ALightData2[32];
    // Temporal Super-Resolution (experimental, default-off)
    int     u_TSREnabled;
    float   u_TSRSharpenStrength;
    // WLSR sharpen gate. RCAS (pass 15) is a no-op until this is 1, so the stage
    // can be shipped inert and turned on by the A/B harness or the editor without
    // a rebuild. See the comment above RunRCAS for why it must default off.
    int     u_RCSEnabled;
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

// ---------------------------------------------------------------------------
// TUNING CONSTANTS
//
// WL_VOL_LIGHT_CAP -- the volumetric multi-light cap. This number MUST be
// identical in BOTH twins (Renderer3D_NovaRenderer.glsl / .hlsl).
//
// It is a PERFORMANCE multiplier, not a quality preference: every one of the
// march steps below runs one BVH traversal per light, and IsOccluded() is a
// complete scene traversal rather than a cheap test. So the per-pixel cost is
// steps x WL_VOL_LIGHT_CAP traversals. A divergence between the twins is not a
// "quality difference" -- the same scene, the same preset, renders DIFFERENT
// LIGHTING on each backend, and it costs 4x here (32 vs 8 meant the OpenGL path
// did 1024 traversals per pixel where DX did 256). tools/verify_nova_glsl.py
// asserts the two files agree on this value; changing it in one twin alone
// fails that check.
// ---------------------------------------------------------------------------
#define WL_VOL_LIGHT_CAP 8

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
Texture2D<float2> read_img_Moments : register(t73);
// t74 is claimed, by NeuralWeights below: the readable-snapshot block was widened
// 10 -> 11 to cover it, which is what moved the DX12 UAV block from heap index 52
// to 53. t75 remains spare.
StructuredBuffer<float> NeuralWeights : register(t74);
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
// SV_GroupID, set in main. The neural reduction's block index needs it: the
// GLSL equivalent multiplies gl_WorkGroupID.y by gl_NumWorkGroups.x, and HLSL has
// no dispatch-extent builtin, so the x extent is derived from g_dims instead.
static uint3 g_gid  = uint3(0, 0, 0);
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



























// Neural Rendering (HLSL): in-shader MLPs for texture detail and material
// appearance. The GI network is gone from here -- it lived inside the shading
// estimator and has moved to a trained end-of-frame stage
// (docs/NEURAL_RENDERING.md).
//
// u_NeuralEnabled now selects between two MODES rather than a blend: 0 is the
// classic PBR renderer with no neural term at all, 1 enables the neural
// material and texture paths below. The strength uniforms still scale those
// paths so they can be faded, but the toggle is a switch -- there is no
// mid-estimator mix any more, which is what previously let "neural on" and
// "neural off" differ only in degree.



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

// ---------------------------------------------------------------------------
// RECONSTRUCTION FILTER KERNELS -- HLSL twin of the GLSL helpers.
//
// Applied separably so a 2D filter costs its tap count rather than its square.
// Catmull-Rom is the cubic B-spline with a=-0.5; Lanczos-3 is sinc(x)*sinc(x/3)
// truncated at |x|<3. Both are textbook forms from the published definitions.
// ---------------------------------------------------------------------------

// Catmull-Rom cubic. Returns a NEGATIVE value over part of its support -- the
// negative lobes are what preserve edges rather than blur them, and they are why
// the caller must normalise by the sum of the weights.
float wl_cubic_kernel(float x)
{
    x = abs(x);
    float x2 = x * x;
    float x3 = x2 * x;
    if (x < 1.0f)
        return (1.5f * x3 - 2.5f * x2 + 1.0f);
    if (x < 2.0f)
        return (-0.5f * x3 + 2.5f * x2 - 4.0f * x + 2.0f);
    return 0.0f;
}

// Lanczos with a 3-lobe window. The window is what makes the kernel decay to
// zero at the boundary instead of continuing to oscillate.
float wl_lanczos3(float x)
{
    if (x == 0.0f)
        return 1.0f;
    float px = 3.14159265359f * x;
    float sinc = sin(px) / px;
    float px3 = px / 3.0f;
    float win = sin(px3) / px3;
    return sinc * win;
}

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

#ifdef WASTELAND_NOVA_TERRAIN_GRID
// Clip one ray component against an axis-aligned interval. Parallel rays are
// handled without forming 1/0; this also makes a flat (min == max) Y bound valid.
bool ClipTerrainAxis(float origin, float direction, float minimum, float maximum,
                     inout float tEnter, inout float tExit)
{
    if (direction == 0.0f)
        return origin >= minimum && origin <= maximum;

    float t0 = (minimum - origin) / direction;
    float t1 = (maximum - origin) / direction;
    tEnter = max(tEnter, min(t0, t1));
    tExit = min(tExit, max(t0, t1));
    return tEnter <= tExit;
}

// Two-sided Moller-Trumbore test; barycentrics also interpolate the packed
// local-space vertex normals. Triangle order matches CPU indices {a,c,b,b,c,d}.
bool HitTerrainTriangle(Ray ray, TerrainNovaVertex v0, TerrainNovaVertex v1,
                        TerrainNovaVertex v2, float tMin, out float hitT, out float3 hitNormal)
{
    hitT = -1.0f;
    hitNormal = float3(0.0f, 1.0f, 0.0f);
    float3 edge1 = v1.Position.xyz - v0.Position.xyz;
    float3 edge2 = v2.Position.xyz - v0.Position.xyz;
    float3 p = cross(ray.Direction, edge2);
    float determinant = dot(edge1, p);
    float determinantScale = length(edge1) * length(edge2) * length(ray.Direction);
    if (!(determinantScale > 0.0f) || abs(determinant) <= determinantScale * 1e-7f) return false;

    float inverseDeterminant = 1.0f / determinant;
    float3 fromV0 = ray.Origin - v0.Position.xyz;
    float u = dot(fromV0, p) * inverseDeterminant;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return false;

    float3 q = cross(fromV0, edge1);
    float v = dot(ray.Direction, q) * inverseDeterminant;
    if (v < -1e-6f || u + v > 1.0f + 1e-6f) return false;

    float t = dot(edge2, q) * inverseDeterminant;
    if (t <= tMin) return false;

    float w = 1.0f - u - v;
    float3 interpolated = v0.Normal.xyz * w + v1.Normal.xyz * u + v2.Normal.xyz * v;
    float normalLengthSquared = dot(interpolated, interpolated);
    hitNormal = normalLengthSquared > 1e-12f
        ? interpolated * rsqrt(normalLengthSquared)
        : normalize(cross(edge1, edge2));
    hitT = t;
    return true;
}

// Intersect the row-major heightfield by visiting only grid cells crossed by
// the local ray. The DDA takes at most 2*cells+2 steps; it never scans the grid.
float HitTerrainGrid(Ray localRay, RayTracingInstance inst, out float3 outNormal)
{
    outNormal = float3(0.0f, 1.0f, 0.0f);
    int cells = inst.TerrainCellsPerSide;
    if (inst.TerrainVertexOffset < 0 || cells <= 0 || cells > 64) return -1.0f;

    float extentX = inst.Max.x - inst.Min.x;
    float extentZ = inst.Max.z - inst.Min.z;
    if (!isfinite(extentX) || !isfinite(extentZ) || extentX <= 0.0f || extentZ <= 0.0f)
        return -1.0f;
    float cellWidthX = extentX / (float)cells;
    float cellWidthZ = extentZ / (float)cells;
    float cellScale = min(cellWidthX, cellWidthZ);
    float directionLength = length(localRay.Direction);
    if (!(directionLength > 0.0f) || !isfinite(directionLength)) return -1.0f;
    float tMin = min(1e-5f, cellScale * 1e-4f / directionLength);

    float tEnter = 0.0f;
    float tExit = 1e30f;
    if (!ClipTerrainAxis(localRay.Origin.x, localRay.Direction.x, inst.Min.x, inst.Max.x, tEnter, tExit) ||
        !ClipTerrainAxis(localRay.Origin.y, localRay.Direction.y, inst.Min.y, inst.Max.y, tEnter, tExit) ||
        !ClipTerrainAxis(localRay.Origin.z, localRay.Direction.z, inst.Min.z, inst.Max.z, tEnter, tExit) ||
        tExit <= 0.0f)
        return -1.0f;

    float3 entry = localRay.Origin + localRay.Direction * tEnter;
    float gridX = clamp((entry.x - inst.Min.x) / cellWidthX, 0.0f, (float)cells);
    float gridZ = clamp((entry.z - inst.Min.z) / cellWidthZ, 0.0f, (float)cells);
    int cellX = min((int)floor(gridX), cells - 1);
    int cellZ = min((int)floor(gridZ), cells - 1);
    const float gridEpsilon = 1e-5f;
    int stepX = localRay.Direction.x > 0.0f ? 1 :
                (localRay.Direction.x < 0.0f ? -1 : 0);
    int stepZ = localRay.Direction.z > 0.0f ? 1 :
                (localRay.Direction.z < 0.0f ? -1 : 0);

    // On an interior grid boundary, a negative ray enters the cell on its
    // negative side. At the maximum edge floor() is already clamped correctly.
    if (stepX < 0 && gridX > 0.0f && gridX < (float)cells &&
        abs(gridX - round(gridX)) <= gridEpsilon)
        cellX--;
    if (stepZ < 0 && gridZ > 0.0f && gridZ < (float)cells &&
        abs(gridZ - round(gridZ)) <= gridEpsilon)
        cellZ--;

    float tMaxX = 1e30f;
    float tMaxZ = 1e30f;
    float tDeltaX = 1e30f;
    float tDeltaZ = 1e30f;
    if (stepX != 0)
    {
        float boundaryX = inst.Min.x + (float)(stepX > 0 ? cellX + 1 : cellX) * cellWidthX;
        tMaxX = tEnter + (boundaryX - entry.x) / localRay.Direction.x;
        tDeltaX = cellWidthX / abs(localRay.Direction.x);
    }
    if (stepZ != 0)
    {
        float boundaryZ = inst.Min.z + (float)(stepZ > 0 ? cellZ + 1 : cellZ) * cellWidthZ;
        tMaxZ = tEnter + (boundaryZ - entry.z) / localRay.Direction.z;
        tDeltaZ = cellWidthZ / abs(localRay.Direction.z);
    }

    int rowStride = cells + 1;
    int maximumSteps = 2 * cells + 2;
    float cellEnter = tEnter;
    for (int step = 0; step < maximumSteps; ++step)
    {
        if (cellX < 0 || cellX >= cells || cellZ < 0 || cellZ >= cells) break;
        float cellExit = min(min(tMaxX, tMaxZ), tExit);
        int vertex = inst.TerrainVertexOffset + cellZ * rowStride + cellX;
        TerrainNovaVertex a = TerrainVertices[vertex];
        TerrainNovaVertex b = TerrainVertices[vertex + 1];
        TerrainNovaVertex c = TerrainVertices[vertex + rowStride];
        TerrainNovaVertex d = TerrainVertices[vertex + rowStride + 1];

        float candidateT;
        float3 candidateNormal;
        float bestT = 1e30f;
        float3 bestNormal = outNormal;
        if (HitTerrainTriangle(localRay, a, c, b, tMin, candidateT, candidateNormal) &&
            candidateT >= cellEnter - tMin && candidateT <= cellExit + tMin && candidateT < bestT)
        {
            bestT = candidateT;
            bestNormal = candidateNormal;
        }
        if (HitTerrainTriangle(localRay, b, c, d, tMin, candidateT, candidateNormal) &&
            candidateT >= cellEnter - tMin && candidateT <= cellExit + tMin && candidateT < bestT)
        {
            bestT = candidateT;
            bestNormal = candidateNormal;
        }
        if (bestT < 1e30f)
        {
            outNormal = bestNormal;
            return bestT;
        }
        if (cellExit >= tExit || (stepX == 0 && stepZ == 0)) break;

        if (tMaxX == tMaxZ)
        {
            cellEnter = max(tMaxX, tMaxZ);
            cellX += stepX;
            cellZ += stepZ;
            tMaxX += tDeltaX;
            tMaxZ += tDeltaZ;
        }
        else if (tMaxX < tMaxZ)
        {
            cellEnter = tMaxX;
            cellX += stepX;
            tMaxX += tDeltaX;
        }
        else
        {
            cellEnter = tMaxZ;
            cellZ += stepZ;
            tMaxZ += tDeltaZ;
        }
    }
    return -1.0f;
}
#endif

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
#ifdef WASTELAND_NOVA_TERRAIN_GRID
    if (shape == 8) return HitTerrainGrid(localRay, inst, outNormal);
#endif

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
                // Phase 1: LODLevel is a SHADING BUDGET, not a visibility switch.
                // TestInstanceHit (primary + closest-hit) never reads LODLevel, so far
                // geometry keeps its exact silhouette and nothing pops. Only shadow rays
                // skip the far band here, which trades the shadow of a distant instance
                // for a cheaper traversal. Never apply this to the closest-hit path: that
                // would reproduce the 2 km distance-cull popping bug that raising
                // TraceMaxDistance to 16 km was specifically done to fix.
                if (Instances[i].LODLevel >= 2) continue;
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
    // Guard: an unset cbuffer field is 0 and clamp(x, 2, 0) is 0, which would
    // silently disable the volume. Fall back to the previous behaviour.
    float stepCap = u_VolStepCap > 0 ? (float)u_VolStepCap : 32.0f;
    int steps = (int)(clamp(d2.z * fogPerfK * clamp(fogLen / 16.0f, 0.15f, 1.0f), 2.0f, stepCap));
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
    {
        int volLightCap = u_VolLightCap > 0 ? u_VolLightCap : WL_VOL_LIGHT_CAP;
        // Never exceed the compile-time array size: lightVis is declared with
        // WL_VOL_LIGHT_CAP elements and a larger runtime cap would overrun it.
        volLightCap = min(volLightCap, WL_VOL_LIGHT_CAP);
        lightEnd = min(u_ALightCount, volLightCap);
    }

    // The shadow test is HOISTED out of the march loop.
    // ---
    // IsOccluded() is a complete BVH traversal over every scene instance, not a
    // cheap test. Running it per march step per light made the per-pixel cost
    // steps x lightEnd traversals -- up to 32 x 32 = 1024 on the OpenGL path,
    // against 32 x 8 = 256 on DX. Same scene, same preset, different lighting:
    // the divergence was a correctness bug as well as a 4x backend difference.
    //
    // A light's visibility to a given RECEIVER changes far more slowly than the
    // sample position does along the ray, so the traversal is done ONCE per light
    // per pixel, from the volume entry point -- the representative position for
    // the whole march -- and the result gates the per-step light loop.
    //
    // What stays INSIDE the loop, deliberately: the range cull (arange > 0 &&
    // dL >= arange) and the spot-cone test. Both are functions of the per-step
    // sample position p and cannot be hoisted without changing the answer.
    //
    // The guard makes the hoist a no-op at lightEnd == 0 (Low quality), which
    // matches the previous behaviour exactly: the volume is then lit by the sun
    // alone and the array is never built.
    float lightVis[WL_VOL_LIGHT_CAP];
    if (lightEnd > 0)
    {
        float3 entry = ro + rd * t0;
        for (int li = 0; li < lightEnd; li++)
        {
            // RANGE CULL, hoisted above the traversal, tested against the whole
            // segment rather than the entry point alone -- see the GLSL twin's
            // note. The segment test is what makes this exact: if the closest
            // point on [t0, t1] to the light is beyond the range, no sample in
            // the volume can be within it, so skipping the traversal cannot
            // change the answer.
            float arange = max(u_ALightData[li].z, 0.0f);
            float3 elpos = u_ALightPos[li];
            float3 etoL = elpos - entry;
            float edL = length(etoL);
            if (u_VolRangeCull != 0 && arange > 0.0f)
            {
                float3 segEnd = ro + rd * t1;
                float3 seg = segEnd - entry;
                float segLen2 = dot(seg, seg);
                float tSeg = segLen2 > 1e-6f ? clamp(dot(etoL, seg) / segLen2, 0.0f, 1.0f) : 0.0f;
                float dClosest = length(etoL - seg * tSeg);
                if (dClosest >= arange)
                {
                    lightVis[li] = 0.0f;
                    continue;
                }
            }
            // Same 1cm origin nudge the per-step ray uses, for the same reason:
            // without it the ray starts exactly on a surface and self-occludes.
            float3 eL = etoL / max(edL, 0.0001f);
            Ray eRay = MakeRay(entry + eL * 0.01f, eL);
            lightVis[li] = IsOccluded(eRay, edL, -1) ? 0.0f : 1.0f;
        }
    }
    
    for (int s = 0; s < (int)stepCap; s++)
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
                // The shadow traversal was hoisted out of this loop (see the note
                // above the march): one BVH traversal per light per pixel, instead
                // of one per step per light.
                if (lightVis[li] < 0.5f) continue;
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
                // No shadow ray in this loop any more: the BVH traversal was hoisted
                // out of it (see lightVis, above the march), so the per-step cost per
                // light is now the compare at the top of the loop rather than a
                // traversal. Only the phase depends on L.
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

// Returns the CPU-generated STBN jitter in PIXEL units, roughly [-0.5, 0.5].
// --
// This used to compute its own Halton(offset % 16 + 1) sequence on the GPU, which
// repeated exactly every 16 frames. u_Jitter was declared and uploaded on every backend
// but never read, so the blue-noise volume sat unused while the 16-frame pattern shipped.
//
// Two unit contracts have to hold together:
//   - BlueNoise::Jitter returns a-0.5 / b-0.5, i.e. PIXELS. It previously divided by the
//     viewport width/height, giving ~0.0005 px at 1920 wide -- effectively zero.
//   - The call site does ((pixelCoords + jitter) / traceSize) * 2 - 1, so what is added
//     here is in pixels before that division.
//
// frameIndex/cameraMoved stay in the signature so call sites and the HLSL/GLSL twin
// guard (tools/verify_nova_glsl.py) are unchanged; FrameIndex is already reset by the CPU
// on camera movement and still-frame invalidation, so the shader needs no camera test.
float2 GetJitter(int frameIndex, float cameraMoved) {
    return u_Jitter;
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
    bool isTerrain = false;
#ifdef WASTELAND_NOVA_TERRAIN_GRID
    isTerrain = (int)inst.MaterialParams.z == 8;
#endif

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

#ifdef WASTELAND_NOVA_TERRAIN_GRID
            float2 uv = isTerrain ? float2(0.0f, 0.0f) : CalculateUV(localHitPos, inst);
#else
            float2 uv = CalculateUV(localHitPos, inst);
#endif

            float pomH = -1.0f;
#ifdef WASTELAND_NOVA_TERRAIN_GRID
            if (isPrimaryRay && !isTerrain)
#else
            if (isPrimaryRay)
#endif
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
            // The upper bound used to be a hardcoded 4.0f, commented as a deliberate
            // quality limit, but it was not one. NVRHITexture.cpp's ComputeMipLevels
            // derives the chain length from the largest axis and UploadMipChain fills all
            // of it, yet every sample asked for at most level 4 -- so the top of every
            // chain was unreachable.
            //
            // MEASURED against this repo's real assets, because the obvious size guess is
            // wrong: metal_floor/wood_floor are 225x225 (8 levels), wood_wall 447x447 (9),
            // Frame 64x64 (7), Checkerboard 8x8 (4). The whole chain is 1.55 MB and the
            // levels above 4 total ~0.002 MB. So this is NOT a 33 MB memory win -- that
            // figure assumed 2048^2 assets this repo does not contain. See
            // docs/TEXTURE_INVENTORY.md.
            //
            // 12 is the level count of a 4096^2 texture. Hardware clamps an explicit LOD
            // to the chain it was given, so over-specifying is safe (you get the coarsest
            // level) where under-specifying silently blurs. The true per-texture maximum
            // is deliberately NOT plumbed through as a uniform: that means extending
            // NovaConstants on all four backends, and DX11Nova's SRV/UAV binding tables
            // are fixed-size literals -- real collision risk (u8 collides with
            // NovaConstants on Vulkan) for a bound that is safe to over-specify.
            mipLevel = clamp(mipLevel, 0.0f, 12.0f);

            float3 sampledAlbedo = float3(1.0f, 1.0f, 1.0f);
#ifdef WASTELAND_NOVA_TERRAIN_GRID
            if (!isTerrain && inst.TextureID >= 0 && inst.TextureID < 32)
#else
            if (inst.TextureID >= 0 && inst.TextureID < 32)
#endif
                sampledAlbedo = TEX_SLOT_IDX(inst.TextureID, uv, mipLevel).rgb;

            info.albedo = sampledAlbedo * inst.Albedo.rgb;
            // Water marker: skip neural blocks for water (waves handle their own shading)
            bool wlWater = (inst.DisplacementParams.z > 0.5f);
            // Neural texture detail: MLP-synthesized micro-surface (OpenGL neural path)
#ifdef WASTELAND_NOVA_TERRAIN_GRID
            if (!isTerrain && !wlWater && u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001f)
#else
            if (!wlWater && u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001f)
#endif
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
        float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // u_MaxLights, not a literal. The GLSL twin reads this uniform here; the HLSL
    // hardcoded 32, which ignored a live uniform the CPU uploads every frame
    // (Renderer3D.cpp:3450) and is set to 4 at Ultra (preset at :5117). That gave
    // DX11/DX12/Vulkan 8x the intended light budget at the preset this engine
    // actually ships. Only the Ultra branch differed -- Low/Medium/High are the
    // same 1/2/4 ladder in both -- which is why no non-Ultra test could see it.
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

    seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
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
        // The bounce is always traced now. This used to be skipped at reduced
        // render scale on non-metals, because the neural cache stood in for the
        // traced GI -- the skip existed only to save the trace cost the cache had
        // replaced. With the cache removed the estimator is resolution-independent
        // again, and the new end-of-frame neural stage owns the GI enhancement.
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
            // The neural radiance cache blend is gone. It mixed wl_neural_indirect()
            // into the traced estimate here at strength x 0.65, which is why the
            // toggle was a mid-estimator blend rather than a mode switch: the traced
            // bounce was always partially replaced, so "neural on" was never a
            // different estimator, just a weighted average of two. The estimator is
            // now unconditionally the traced one, and the new end-of-frame stage
            // owns the neural enhancement instead.
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
    // Clamp on BOTH sides. min() alone let a reprojected UV below zero wrap to the far
    // edge of the history buffer, pasting last frame's opposite border into this pixel --
    // visible as a smear on motion, and worst on a camera cut where prevUV can land far
    // outside [0,1]. The lower bound is half a texel, matching the upper one.
    histUV = clamp(histUV, 0.5f / fullRes, float2(u_RenderScale, u_RenderScale) - 0.5f / fullRes);

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

// Pass 12: temporal moments accumulation.
// Computes running mean (m1) and mean-of-squares (m2) of the luminance
// channel, reprojected to the previous frame. The A-Trous filter (pass 13)
// derives variance from these moments: var = m2 - m1^2.
// --
// The reprojection logic is identical to RunTemporalAccumulation: same
// velocity lookup, same histUV computation, same clamp, same depth test.
// The only difference is that we blend scalar moments instead of RGB colour.
void RunTemporalMoments() {
    int2 pixelCoords = int2(g_dt.xy);
    int2 imgSize = wl_trace_extent(IMG_SIZE(img_Accumulation));
    float2 uv = (float2(pixelCoords) + 0.5f) / float2(imgSize);

    // Current frame luminance from the bloom buffer (same source as temporal accumulation).
    float3 currentLight = IMG_LOAD(img_Bloom, pixelCoords).rgb;
    float currentLum = dot(currentLight, float3(0.2126f, 0.7152f, 0.0722f));

    float2 velocity = IMG_LOAD(img_Velocity, pixelCoords).xy;
    float2 prevUV = uv - velocity;
    float2 histUV = prevUV * u_RenderScale;
    float2 fullRes = float2(IMG_SIZE(img_Accumulation));
    histUV = clamp(histUV, 0.5f / fullRes, float2(u_RenderScale, u_RenderScale) - 0.5f / fullRes);

    bool validHistory = u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;
    float motionMag = length(velocity * float2(imgSize));

    if (validHistory) {
        float currentDepth = TEX(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = TEX(s_DepthBuffer, histUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    float2 prevMoments = float2(0.0f, 0.0f);
    if (validHistory) {
        prevMoments = TEX(s_Moments, histUV).xy;
    }

    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? lerp_safe(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;

    float newM1 = lerp_safe(prevMoments.x, currentLum, alpha);
    float newM2 = lerp_safe(prevMoments.y, currentLum * currentLum, alpha);

    IMG_STORE(img_Moments, pixelCoords, float4(newM1, newM2, 0.0f, 0.0f));
}

// Pass 13: the A-Trous wavelet filter.
// ---
// A fixed 4-iteration edge-preserving filter that reads the temporal moments
// from pass 12 and uses the local variance to drive the edge-stopping weight.
// Higher variance means a wider/stronger filter: the kernel reaches further
// and smooths more aggressively in noisy regions, while flat regions are
// left untouched (kernel weights sum to 1).
//
// The step ladder {1, 2, 4, 8} is the same shape as RunBilateralBlur, but
// applied recursively: each iteration reads the result of the previous one,
// so the effective kernel grows exponentially. This is the classic "a trous"
// (French for "with holes") wavelet decomposition.
//
// The edge-stopping weight combines three terms:
//   - depth difference: prevents blurring across geometry edges
//   - normal difference: prevents blurring across shading discontinuities
//   - variance term: adapts the filter strength to the local noise level
// Pass 13: the A-Trous wavelet filter -- ONE iteration per dispatch.
// ---
// This pass performs a single edge-preserving wavelet iteration at the step
// size held in u_StepSize, reading img_Accumulation and writing img_Bloom_Temp.
// The C++ side drives the 4-step ladder (1, 2, 4, 8) by dispatching this pass
// four times and calling CommitDenoise() between them, which copies
// img_Bloom_Temp back into img_Accumulation -- the same ping-pong mechanism
// RunBilateralBlur already relies on.
//
// WHY ONE ITERATION PER DISPATCH, NOT A LOOP IN THE SHADER:
// a real A-Trous filter must feed iteration N's OUTPUT into iteration N+1. A
// loop that re-reads img_Accumulation on every pass computes four independent
// copies of iteration 1 and keeps only the last, so the step ladder does
// nothing at all -- the filter degenerates to a single 3x3 blur. The loop also
// cannot ping-pong internally: one UAV cannot be read and written in the same
// dispatch without a barrier, and this shader has no resource that is both.
// Driving the ladder from C++ sidesteps both problems with machinery that is
// already here and already tested.
//
// The B3-spline kernel weights (0.375 centre, 0.25 edge, 0.0625 corner) sum to
// 1 at every step, so a flat region comes back unchanged.
void RunATrousFilter() {
    int2 pos = int2(g_dt.xy);
    int2 imgSize = wl_trace_extent(IMG_SIZE(img_Accumulation));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    // One step per dispatch, supplied by the C++ ladder (1, 2, 4, 8).
    // max(1,...) guards a zero step, which would collapse the kernel onto the
    // centre tap and turn the whole pass into a no-op.
    int step = max(1, u_StepSize);

    float3 inputColor = IMG_LOAD(img_Accumulation, pos).rgb;

    // Centre moments. m1 is the accumulated mean luminance and m2 the mean of
    // squares, so the variance is m2 - m1^2. The max against zero matters:
    // m1^2 can exceed m2 by a rounding error, and a negative variance under
    // sqrt is NaN -- which would poison every weight in the kernel at once.
    float2 moments = IMG_LOAD(img_Moments, pos).xy;
    float centerVariance = max(moments.y - moments.x * moments.x, 0.0f);

    float2 uv = (float2(pos) + 0.5f) / float2(imgSize);
    float centerDepth = TEX(s_DepthBuffer, wl_buf_uv(uv)).r;

    // Sky has no depth to edge-stop against; pass it through untouched.
    if (centerDepth >= 1.0f) {
        IMG_STORE(img_Bloom_Temp, pos, float4(inputColor, 1.0f));
        return;
    }

    // The trace G-buffer packs the world normal into .yzw (see the writer in
    // RunTraceAndDenoise: float4(ndcDepth, normal * 0.5 + 0.5)), so the normal
    // decodes from .gba -- NOT .rgb, whose .r holds depth.
    float3 centerNormal = TEX(s_NormalBuffer, wl_buf_uv(uv)).gba * 2.0f - 1.0f;

    // Tolerance scale for the luminance weight, in units of the centre's own
    // standard deviation. See the weight's comment below for why it lives here.
    const float sigma_l = 4.0f;

    float3 totalColor = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            int2 samplePos = clamp(pos + int2(x, y) * step, int2(0, 0), imgSize - int2(1, 1));
            float2 sampleUV = (float2(samplePos) + 0.5f) / float2(imgSize);

            float3 neighborColor = IMG_LOAD(img_Accumulation, samplePos).rgb;
            float neighborDepth = TEX(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            float3 neighborNormal = TEX(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
            float neighborMean = IMG_LOAD(img_Moments, samplePos).x;

            // B3-spline spatial weights: centre 0.375, edge 0.25, corner 0.0625.
            float spatialWeight = (x == 0 && y == 0) ? 0.375f :
                                  ((x == 0 || y == 0) ? 0.25f : 0.0625f);

            // Depth edge-stop. 100 is the sharpness: a depth difference of 1% of
            // the depth range already halves the weight, so silhouettes survive
            // without rejecting a whole sloped surface.
            float depthWeight = exp(-abs(centerDepth - neighborDepth) * 100.0f);

            // Normal edge-stop. The exponent separates two faces meeting at a
            // crease while still admitting neighbours on a gently curved surface:
            // 5 degrees off normal keeps ~0.88, 10 degrees keeps ~0.61.
            float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 32.0f);

            // THE SVGF LUMINANCE WEIGHT -- the term that must vary per tap. It
            // compares the NEIGHBOUR's accumulated mean against the centre's,
            // with the CENTRE's variance setting the tolerance:
            //
            //     w_l = exp( -|mean_c - mean_n| / (sigma_l * sqrt(var_c) + eps) )
            //
            // A noisy region has a large var_c, so the tolerance widens and the
            // filter smooths harder; a converged region has a small var_c, so it
            // backs off and preserves detail. That is the entire point of feeding
            // temporal moments into a spatial filter.
            //
            // The previous version multiplied by (1 + var_c * 5) instead. That
            // term was inert: var_c is the CENTRE pixel's variance, so it was the
            // same constant for all nine taps, and a constant factor cancels
            // exactly in the normalisation below. It changed nothing whatsoever.
            float luminanceWeight =
                exp(-abs(moments.x - neighborMean) / (sigma_l * sqrt(centerVariance) + 1e-4f));

            float weight = spatialWeight * depthWeight * normalWeight * luminanceWeight;
            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    }

    // Normalise so the kernel sums to 1 and a flat region is unchanged.
    float3 filtered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : inputColor;
    IMG_STORE(img_Bloom_Temp, pos, float4(filtered, 1.0f));
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

// ============================================================================
// Edge-detect anti-aliasing: FXAA (ReconstructionFilter 6) and SMAA S1 (7).
// ============================================================================
// HLSL twin of the GLSL block of the same name -- see that comment for the full
// reasoning. Summary: both entries were plumbed (enum, editor combo, all three
// NVRHI Set() calls) and read by nothing, so the composite's ladder fell through
// to bilinear and picking either one changed the image not at all.
//
// Runs inside RunComposite on the resolved image, before the tonemap/post chain.
// The taps read the image the composite is READING (img_Accumulation, or
// img_Bloom_Temp at Ultra full-res) -- never img_Output, which this dispatch and
// pass 5 both write and which no compute dispatch orders between invocations.
//
// The edge metric is tonemapped luma even though the blend is in pre-tonemap
// space: FXAA's threshold is only perceptually meaningful on display-referred
// luma. The tonemap is applied per-tap for the metric alone, so the blend stays
// in the space the composite already works in and the real tonemap still runs once.
float EdgeMetricLuma(float3 c)
{
    float3 m = c / (c + float3(1.0f, 1.0f, 1.0f));
    return dot(m, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 EdgeAASrcTap(int2 fullPos, int2 fullSize, int2 srcSize, float renderScale)
{
    if ((renderScale >= 1.0f) && (u_QualityLevel >= 3))
        return IMG_LOAD(img_Bloom_Temp, clamp(fullPos, int2(0, 0), fullSize - int2(1, 1))).rgb;
    int2 sp = int2(float2(fullPos) * renderScale);
    return IMG_LOAD(img_Accumulation, clamp(sp, int2(0, 0), srcSize - int2(1, 1))).rgb;
}

float3 RunEdgeDetectAA(int2 pos, int2 fullSize, int2 srcSize, float renderScale)
{
    int2 maxC = fullSize - int2(1, 1);
    int2 c = clamp(pos, int2(0, 0), maxC);

    float3 rgbM = EdgeAASrcTap(c, fullSize, srcSize, renderScale);
    float lumaM = EdgeMetricLuma(rgbM);

    // The 4 diagonal neighbours, matching FXAA 3.11's NW/NE/SW/SE arrangement.
    // The cardinal taps are fetched only if an edge survives the test below.
    float3 rgbNW = EdgeAASrcTap(c + int2(-1, -1), fullSize, srcSize, renderScale);
    float3 rgbNE = EdgeAASrcTap(c + int2( 1, -1), fullSize, srcSize, renderScale);
    float3 rgbSW = EdgeAASrcTap(c + int2(-1,  1), fullSize, srcSize, renderScale);
    float3 rgbSE = EdgeAASrcTap(c + int2( 1,  1), fullSize, srcSize, renderScale);

    float lumaNW = EdgeMetricLuma(rgbNW);
    float lumaNE = EdgeMetricLuma(rgbNE);
    float lumaSW = EdgeMetricLuma(rgbSW);
    float lumaSE = EdgeMetricLuma(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // NO EDGE -> unchanged. FXAA 3.11's threshold is
    // max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD) with
    // EDGE_THRESHOLD_MIN = 0.0312 and EDGE_THRESHOLD = 0.125.
    float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(0.0312f, lumaMax * 0.125f))
        return rgbM;

    // The cardinal taps complete the 3x3.
    float3 rgbN = EdgeAASrcTap(c + int2(0, -1), fullSize, srcSize, renderScale);
    float3 rgbS = EdgeAASrcTap(c + int2(0,  1), fullSize, srcSize, renderScale);
    float3 rgbW = EdgeAASrcTap(c + int2(-1, 0), fullSize, srcSize, renderScale);
    float3 rgbE = EdgeAASrcTap(c + int2( 1, 0), fullSize, srcSize, renderScale);

    float lumaN = EdgeMetricLuma(rgbN);
    float lumaS = EdgeMetricLuma(rgbS);
    float lumaW = EdgeMetricLuma(rgbW);
    float lumaE = EdgeMetricLuma(rgbE);

    // DIRECTION SELECTION, verbatim from FXAA 3.11.
    float dirX = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    float dirY =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125f, 1.0f / 128.0f);
    float rcpDirMin = 1.0f / (min(abs(dirX), abs(dirY)) + dirReduce);
    float2 dir = clamp(float2(dirX, dirY) * rcpDirMin, -8.0f, 8.0f);

    int2 halfOff = int2(clamp(int(dir.x * 0.5f), -1, 1),
                        clamp(int(dir.y * 0.5f), -1, 1));

    // rgbA: the blend at half a texel in each direction. SMAA S1's extra step is
    // the symmetric half-texel pair; the FXAA entry point uses the plain 3-tap form.
    float3 rgbA;
    if (u_ReconstructionFilter == 7)
    {
        rgbA = (EdgeAASrcTap(c + halfOff, fullSize, srcSize, renderScale) +
                EdgeAASrcTap(c - halfOff, fullSize, srcSize, renderScale)) * 0.5f;
    }
    else
    {
        rgbA = (rgbM +
                EdgeAASrcTap(c + halfOff, fullSize, srcSize, renderScale) +
                EdgeAASrcTap(c - halfOff, fullSize, srcSize, renderScale)) / 3.0f;
    }

    // rgbB: the 5-tap cross blend.
    float3 rgbB = (rgbN + rgbS + rgbW + rgbE) * 0.25f * 0.5f + rgbM * 0.5f;
    float lumaB = EdgeMetricLuma(rgbB);

    // If the cross blend would land outside the local luma range the edge is too
    // sharp for it, and the half-texel blend is the better answer.
    if (lumaB < lumaMin || lumaB > lumaMax)
        return rgbA;
    return rgbB;
}

// ============================================================================
// FSR1 EASU -- Edge Adaptive Spatial Upsampling (filter 8).
//
// The HLSL twin of the GLSL EASU in this same file's sibling. The two are kept
// statement-for-statement identical so a difference between them cannot hide:
// this backend is compiled by fxc/dxc and the GLSL by the GL driver, and a
// divergence would show up only as a per-backend rendering difference.
//
// Adapted from the EASU stage of AMD FidelityFX Super Resolution 1
// (ffx_fsr1.h, (c) 2021 Advanced Micro Devices, Inc., MIT licensed). The
// adaptation is structural rather than a straight port: the reference gathers
// 2x2 texel blocks with a gather and drives the taps from four precomputed
// constant vectors, whereas this renderer samples a UAV, so the twelve taps are
// written out explicitly and the cell origin is passed in directly. The filter
// itself -- tap offsets, gradient and length shaping, the lanczos-2
// approximation, the min/max deringing clamp -- is unchanged.
//
// WHY IT EARNS A SLOT IN THE LADDER. Every other entry is separable: it applies
// a 1D kernel along x and then along y, which is why Lanczos rings along a
// diagonal edge and Bicubic smears across one. EASU instead measures the local
// gradient at the sample point and stretches the kernel ALONG the edge, so a
// diagonal is reconstructed from taps lying on it rather than across it. A
// ray tracer produces nothing but hard geometry edges -- exactly the case a
// separable kernel handles worst -- so this is the one entry that is not just a
// different trade of blur against ringing.
//
// WHY RCAS IS A SEPARATE PASS AND IS DEFERRED. FSR1 is EASU followed by RCAS, a
// contrast-adaptive sharpen. The earlier text here claimed RCAS would cost five
// EASU evaluations -- that is wrong, and it was worth correcting rather than
// leaving: RCAS samples five taps of the RECONSTRUCTED image (the centre plus the
// four diagonals) and drives a gain from the local contrast. It does not re-run
// EASU at those offsets. So it is ~5 taps, not 60, and it is cheap.
//
// It still cannot be folded into this branch: the four diagonal taps live at
// NEIGHBOURING OUTPUT PIXELS, and this invocation only holds the centre one in
// registers. A pass boundary is required.
//
// It is deferred, not omitted, and the reason is scheduling rather than cost:
// the clean-room reconstruction plan (.hermes/plans/
// 2026-10-26_001250-clean-room-dlss-class-stack.md, section 1.3) already
// allocates the dead pass-15/16 slots to RCAS and to the WLAA sharpen. Taking
// those slots from inside this composite would collide with that work, and the
// plan is also where the post-composite scratch-image rebind is designed.
// EASU's own negative lobes supply most of the high-frequency response RCAS
// would add, and the Bicubic entry covers the overshoot case for now.
// ============================================================================

// One of the twelve taps. Offsets are relative to the bilinear cell's upper-left
// texel, which is also the cell the sample point falls in:
//        b c
//      e f g h
//      i j k l
//        n o
float3 EASUTap(int2 cell, int2 srcSize, int2 off)
{
    return IMG_LOAD(img_Accumulation, clamp(cell + off, int2(0, 0), srcSize - int2(1, 1))).rgb;
}

// Two-channel luma, matching the reference's "luma times 2, in 2 FMA/MAD". The
// factor of two is irrelevant (every use is a ratio) but keeping the same weights
// means the gradient shaping is directly comparable to the original.
float EASULuma(float3 c)
{
    return c.b * 0.5f + (c.r * 0.5f + c.g);
}

// Filtering for one tap. `off` is the tap's offset from the SAMPLE POINT (not
// from the cell), which is why every call site passes (tapOffset - pp).
void EASUTapAccum(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len,
                  float lob, float clp, float3 c)
{
    // Rotate the offset by the gradient direction.
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    // Anisotropy.
    v *= len;
    // Distance^2, limited to the window -- at a corner two taps fall outside it.
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    // lanczos-2 without sin(), rcp() or sqrt():
    //   (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
    //   |_______________________|   |_______________|
    //             base                  window
    float wB = 2.0f / 5.0f * d2 - 1.0f;
    float wA = lob * d2 - 1.0f;
    wB *= wB;
    wA *= wA;
    wB = 25.0f / 16.0f * wB - (25.0f / 16.0f - 1.0f);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// Accumulate gradient direction and length from one bilinear weight.
void EASUSet(inout float2 dir, inout float len, float2 pp,
             bool biS, bool biT, bool biU, bool biV,
             float lA, float lB, float lC, float lD, float lE)
{
    // Bilinear weight. The predicates are compile-time constants here, so the
    // three unused branches fold away.
    //   s t
    //   u v
    float w = 0.0f;
    if (biS) w = (1.0f - pp.x) * (1.0f - pp.y);
    if (biT) w = pp.x * (1.0f - pp.y);
    if (biU) w = (1.0f - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    // Direction is the '+' diff:
    //     a
    //   b c d
    //     e
    // Length converts gradient reversal to 0 and non-reversal to 1, shaped, then
    // adds the horizontal and vertical terms.
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    // The reference uses a bit-level reciprocal approximation here precisely
    // because a plain divide yields +inf for a flat neighbourhood, and that inf
    // propagates into the length term. Flooring the denominator is equivalent
    // for every non-zero input and removes the inf.
    lenX = 1.0f / max(lenX, 1e-20f);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0f, 1.0f);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0f / max(lenY, 1e-20f);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0f, 1.0f);
    lenY *= lenY;
    len += lenY * w;
}

float3 RunFSR1EASU(int2 cell, float2 pp, int2 srcSize)
{
    // The twelve taps, in the reference's order.
    float3 b = EASUTap(cell, srcSize, int2( 0, -1));
    float3 c = EASUTap(cell, srcSize, int2( 1, -1));
    float3 e = EASUTap(cell, srcSize, int2(-1,  0));
    float3 f = EASUTap(cell, srcSize, int2( 0,  0));
    float3 g = EASUTap(cell, srcSize, int2( 1,  0));
    float3 h = EASUTap(cell, srcSize, int2( 2,  0));
    float3 i = EASUTap(cell, srcSize, int2(-1,  1));
    float3 j = EASUTap(cell, srcSize, int2( 0,  1));
    float3 k = EASUTap(cell, srcSize, int2( 1,  1));
    float3 l = EASUTap(cell, srcSize, int2( 2,  1));
    float3 n = EASUTap(cell, srcSize, int2( 0,  2));
    float3 o = EASUTap(cell, srcSize, int2( 1,  2));

    float bL = EASULuma(b);
    float cL = EASULuma(c);
    float eL = EASULuma(e);
    float fL = EASULuma(f);
    float gL = EASULuma(g);
    float hL = EASULuma(h);
    float iL = EASULuma(i);
    float jL = EASULuma(j);
    float kL = EASULuma(k);
    float lL = EASULuma(l);
    float nL = EASULuma(n);
    float oL = EASULuma(o);

    // Accumulate for bilinear interpolation. Each call supplies the '+' cross
    // around one of the four cell corners, weighted by that corner's bilinear
    // weight -- so the four calls together cover the cell exactly once.
    float2 dir = float2(0.0f, 0.0f);
    float len = 0.0f;
    EASUSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    EASUSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    EASUSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    EASUSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    // Normalize, and clean up values close to zero.
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0f / 32768.0f;
    // max() rather than the raw value: rsqrt(0) is +inf, and the zro override
    // only replaces the result, not the intermediate. dirR is a sum of squares
    // so it is never negative.
    dirR = rsqrt(max(dirR, 1.0f / 32768.0f));
    dirR = zro ? 1.0f : dirR;
    dir.x = zro ? 1.0f : dir.x;
    dir *= dirR;

    // Transform from {0 to 2} to {0 to 1} range, and shape with square.
    len = len * 0.5f;
    len *= len;

    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}. |dir| >= 1 after
    // the normalization above, so the denominator cannot vanish.
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));

    // Anisotropic length after rotation:
    //   x := 1.0 lerp to 'stretch' on edges
    //   y := 1.0 lerp to 2x on edges
    float2 len2 = float2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);

    // Based on the amount of 'edge', the window shifts from +/-sqrt(2.0) to
    // slightly beyond 2.0.
    float lob = 0.5f + (1.0f / 4.0f - 0.04f - 0.5f) * len;

    // Distance^2 clipping point at the end of the adjustable window.
    float clp = 1.0f / lob;

    // Accumulation, mixed with the min/max of the four nearest texels. This is
    // what removes the ringing the lanczos-2 negative lobes would otherwise
    // produce: the result can never leave the range of the 2x2 cell it was
    // reconstructed from.
    float3 min4 = min(min(min(f, g), j), k);
    float3 max4 = max(max(max(f, g), j), k);

    float3 aC = float3(0.0f, 0.0f, 0.0f);
    float aW = 0.0f;
    EASUTapAccum(aC, aW, float2( 0.0f, -1.0f) - pp, dir, len2, lob, clp, b);
    EASUTapAccum(aC, aW, float2( 1.0f, -1.0f) - pp, dir, len2, lob, clp, c);
    EASUTapAccum(aC, aW, float2(-1.0f,  1.0f) - pp, dir, len2, lob, clp, i);
    EASUTapAccum(aC, aW, float2( 0.0f,  1.0f) - pp, dir, len2, lob, clp, j);
    EASUTapAccum(aC, aW, float2( 0.0f,  0.0f) - pp, dir, len2, lob, clp, f);
    EASUTapAccum(aC, aW, float2(-1.0f,  0.0f) - pp, dir, len2, lob, clp, e);
    EASUTapAccum(aC, aW, float2( 1.0f,  1.0f) - pp, dir, len2, lob, clp, k);
    EASUTapAccum(aC, aW, float2( 2.0f,  1.0f) - pp, dir, len2, lob, clp, l);
    EASUTapAccum(aC, aW, float2( 2.0f,  0.0f) - pp, dir, len2, lob, clp, h);
    EASUTapAccum(aC, aW, float2( 1.0f,  0.0f) - pp, dir, len2, lob, clp, g);
    EASUTapAccum(aC, aW, float2( 1.0f,  2.0f) - pp, dir, len2, lob, clp, o);
    EASUTapAccum(aC, aW, float2( 0.0f,  2.0f) - pp, dir, len2, lob, clp, n);

    // Normalize and dering. aW is always >= 1 because the centre tap has d2 = 0
    // and therefore a weight of exactly 1.
    return min(max4, max(min4, aC * (1.0f / aW)));
}

// ============================================================================
// RCAS sharpen constants. Defined here, before RunComposite, because RunComposite's
// WLAA branch references WLAA_SHARPNESS and HLSL resolves #defines textually -- a
// use before the definition would not see it. The RunWlsrSharpenCore helper below
// (defined later, so RunComposite's WLAA branch and the pass-15 RunRCAS can
// forward-call it) uses RCAS_MAX_SHARPEN as its gain ceiling; a forward prototype
// bridges the ordering.
// ============================================================================
#define RCAS_MAX_SHARPEN 0.75

// The default sharpen amount for the WLAA native-resolution AA mode. WLAA is
// TAA (the accumulation) plus a mild sharpen, so the amount is modest -- enough
// to restore the edge crispness the temporal filter softens, not enough to ring.
// Overridable via the WLSR sharpen knob (u_TSRSharpenStrength); clamped either
// way. Kept a separate constant from RCAS_MAX_SHARPEN because that one is the
// per-channel GAIN ceiling, not the user amount.
#define WLAA_SHARPNESS 0.5f

// Forward declarations: RunWlsrSharpenCore and RunRCAS are defined later in the
// file (with the pass-15 entry point), but RunComposite's WLAA branch calls
// RunWlsrSharpenCore and the pass-15 entry calls RunRCAS. HLSL needs the
// prototypes because it does not scan the whole file for function bodies before
// resolving calls the way GLSL does.
float3 RunWlsrSharpenCore(float3 ctr, float3 t00, float3 t10, float3 t01, float3 t11, float sharpness, float mask);
float3 RunRCAS(int2 pos, int2 size, float sharpness);

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

        // THE RECONSTRUCTION LADDER -- HLSL twin of the GLSL branch. This used to
        // be a hardcoded bilinear, which is why the AA dropdown in the editor had
        // no effect: the uniform was uploaded and then dropped because the shader
        // never declared it. Each branch is a different way of rebuilding one
        // output pixel from the trace-resolution image, cheapest first.
        //
        // The taps are weighted from the SAME fractional coordinate `f` that
        // bilinear used, so switching filters changes the reconstruction kernel
        // and nothing else -- no change to the trace, accumulation or denoiser.
        //
        // Values outside the spatial range (Temporal, TSR, SMAA, FXAA,
        // SpatialDenoise) are separate passes rather than reconstruction
        // filters; they fall through to bilinear instead of being ignored, so
        // picking one degrades gracefully rather than appearing to do something
        // it cannot. FSR1 used to be in that list -- it was a dead entry, since
        // no EASU code existed anywhere in the tree -- and is now a real branch
        // above.
        if (u_ReconstructionFilter == 1)
        {
            // NEAREST -- 1 tap, point sampling. The cheapest possible upscale and
            // the most aliased; a legitimate "no AA" diagnostic baseline.
            denoised = IMG_LOAD(img_Accumulation, s0).rgb;
        }
        else if (u_ReconstructionFilter == 3)
        {
            // BOX -- 4 tap unweighted. The 2x2 footprint bilinear samples, with
            // the weights forced to 0.5 regardless of position: softer than
            // bilinear at edges because it does not interpolate.
            float3 b00 = IMG_LOAD(img_Accumulation, int2(s0.x, s0.y)).rgb;
            float3 b10 = IMG_LOAD(img_Accumulation, int2(s1.x, s0.y)).rgb;
            float3 b01 = IMG_LOAD(img_Accumulation, int2(s0.x, s1.y)).rgb;
            float3 b11 = IMG_LOAD(img_Accumulation, int2(s1.x, s1.y)).rgb;
            denoised = (b00 + b10 + b01 + b11) * 0.25f;
        }
        else if (u_ReconstructionFilter == 4)
        {
            // BICUBIC -- 16 tap Catmull-Rom, separable, normalised by the weight
            // sum so the filter cannot darken or brighten the image as a whole.
            float3 acc = float3(0.0f, 0.0f, 0.0f);
            float wsum = 0.0f;
            for (int j = -1; j <= 2; j++)
            {
                float fy = wl_cubic_kernel((float)j - f.y);
                for (int i = -1; i <= 2; i++)
                {
                    float w = wl_cubic_kernel((float)i - f.x) * fy;
                    int2 c = clamp(s0 + int2(i, j), int2(0, 0), srcSize - int2(1, 1));
                    acc += IMG_LOAD(img_Accumulation, c).rgb * w;
                    wsum += w;
                }
            }
            denoised = acc / max(wsum, 1e-5f);
        }
        else if (u_ReconstructionFilter == 5)
        {
            // LANCZOS -- 12 tap, 3-lobe windowed sinc. Best edge preservation of
            // the spatial group, at the cost of the most ringing.
            float3 acc = float3(0.0f, 0.0f, 0.0f);
            float wsum = 0.0f;
            for (int j = -2; j <= 3; j++)
            {
                float fy = wl_lanczos3((float)j - f.y);
                for (int i = -2; i <= 3; i++)
                {
                    float w = wl_lanczos3((float)i - f.x) * fy;
                    int2 c = clamp(s0 + int2(i, j), int2(0, 0), srcSize - int2(1, 1));
                    acc += IMG_LOAD(img_Accumulation, c).rgb * w;
                    wsum += w;
                }
            }
            denoised = acc / max(wsum, 1e-5f);
        }
        else if (u_ReconstructionFilter == 8)
        {
            // FSR1 EASU -- 12 tap, edge-adaptive, anisotropic lanczos-2. Unlike
            // the separable kernels above it stretches the footprint along the
            // local gradient, so it reconstructs a diagonal edge from taps lying
            // on it instead of smearing across it. The cell origin s0 and the
            // fractional offset f are the SAME values bilinear derived, so this
            // changes the kernel and nothing else -- the trace, the accumulation
            // and the denoiser are untouched.
            // REACTIVE MASK. Where the mask says the current frame cannot be
            // trusted (a disocclusion, fast motion, a camera cut), the EASU
            // result is blended back toward bilinear. EASU stretches its kernel
            // along an edge, which is exactly what must NOT happen across a
            // disocclusion, where the "edge" is two different surfaces.
            //
            // Kept identical to the GLSL twin, including the bilinear taps being
            // computed here rather than in the else-branch below.
            float3 easu = RunFSR1EASU(s0, f, srcSize);
            float3 bl00 = IMG_LOAD(img_Accumulation, s0).rgb;
            float3 bl10 = IMG_LOAD(img_Accumulation, int2(s1.x, s0.y)).rgb;
            float3 bl01 = IMG_LOAD(img_Accumulation, int2(s0.x, s1.y)).rgb;
            float3 bl11 = IMG_LOAD(img_Accumulation, s1).rgb;
            float3 bilinear = lerp(lerp(bl00, bl10, f.x), lerp(bl01, bl11, f.x), f.y);
            float mask = IMG_LOAD(img_Bloom_Temp, clamp(s0, int2(0, 0), srcSize - int2(1, 1))).a;
            denoised = lerp(easu, bilinear, clamp(mask, 0.0f, 1.0f));
        }
        else
        {
            // BILINEAR (the default) and the fall-through for every filter that
            // is a separate pass rather than a reconstruction kernel.
            float3 c00 = IMG_LOAD(img_Accumulation, s0).rgb;
            float3 c10 = IMG_LOAD(img_Accumulation, int2(s1.x, s0.y)).rgb;
            float3 c01 = IMG_LOAD(img_Accumulation, int2(s0.x, s1.y)).rgb;
            float3 c11 = IMG_LOAD(img_Accumulation, s1).rgb;
            denoised = lerp_safe(lerp_safe(c00, c10, f.x), lerp_safe(c01, c11, f.x), f.y);
        }
    } else {
        // Full resolution: no upscale. WLAA sharpens the resolved accumulation;
        // otherwise the accumulation is the result.
        if (u_ReconstructionFilter == 12)
        {
            // WLAA -- native-resolution anti-aliasing. The temporal accumulation
            // (pass 2) already ran; WLAA is that result plus the RCAS sharpen,
            // folded into the composite so the mode is one pass rather than a
            // composite followed by a separate full-resolution sharpen stage.
            // The sharpen reads the resolved accumulation -- exactly what a
            // native-res AA mode reconstructs -- and the amount defaults to
            // WLAA_SHARPNESS, overridable via the WLSR sharpen knob, clamped to
            // the range either way.
            float wlSharp = (u_TSRSharpenStrength > 0.001f) ? u_TSRSharpenStrength : WLAA_SHARPNESS;
            // Load the five taps from the resolved accumulation (not img_Output,
            // which is still the pre-composite trace colour here) and the mask
            // from the trace-resolution reactive mask, at the trace coordinate.
            int2 lo = int2(1, 1);
            int2 hi = max(fullSize - int2(2, 2), lo);
            int2 cc = clamp(pos, lo, hi);
            float3 ctr = read_img_Accumulation.Load(int3(cc, 0)).rgb;
            float3 t00 = read_img_Accumulation.Load(int3(clamp(cc + int2(-1, -1), lo, hi), 0)).rgb;
            float3 t10 = read_img_Accumulation.Load(int3(clamp(cc + int2( 1, -1), lo, hi), 0)).rgb;
            float3 t01 = read_img_Accumulation.Load(int3(clamp(cc + int2(-1,  1), lo, hi), 0)).rgb;
            float3 t11 = read_img_Accumulation.Load(int3(clamp(cc + int2( 1,  1), lo, hi), 0)).rgb;
            int2 traceSize = max(int2(float2(fullSize) * u_RenderScale), int2(1, 1));
            int2 mpos = clamp(int2((float2(pos) + 0.5f) * u_RenderScale), int2(0, 0), traceSize - int2(1, 1));
            float mask = IMG_LOAD(img_Bloom_Temp, mpos).a;
            denoised = RunWlsrSharpenCore(ctr, t00, t10, t01, t11, clamp(wlSharp, 0.0f, 1.0f), mask);
        }
        else
        {
            denoised = IMG_LOAD(img_Accumulation, pos).rgb;
        }
    }

    float3 combined = denoised;

    // EDGE-DETECT AA (FXAA / SMAA S1). Runs on the RESOLVED image, before the
    // tonemap/post chain, so the composite's existing post-processing still applies
    // to the filtered result -- no duplicated grading, no second pass.
    //
    // Gated on the filter value so every other entry is untouched: the default
    // path falls straight through to the post chain below, which is what keeps
    // the no-AA output identical to before this was wired.
    if (u_ReconstructionFilter == 6 || u_ReconstructionFilter == 7)
    {
        int2 srcSize = int2(float2(fullSize) * max(u_RenderScale, 0.001f));
        srcSize = max(srcSize, int2(1, 1));
        combined = RunEdgeDetectAA(pos, fullSize, srcSize, u_RenderScale);
    }

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


// ============================================================================
// ReSTIR DI Reservoir (Bitterli et al. ACM TOG 2020)
// ============================================================================
// The reservoir is a per-pixel data structure that stores one selected light
// sample. Each frame, M candidates are drawn from the light list; the
// reservoir updates via weighted reservoir sampling. Temporal reuse combines
// the current reservoir with the previous frame's (reprojected). Spatial
// reuse combines with neighbouring pixels' reservoirs.
//
// The MIS combine is Alg. 4 from the paper: the weight W is
//   W = (targetPdf(selected) / p_hat(selected)) * (1 / M)
// where p_hat is the PDF of the sampling strategy that produced the sample.
// For uniform light sampling, p_hat = 1/N, so W = targetPdf * N / M.

// Octahedral normal encoding for receiver-surface validation
uint wl_oct_encode(float3 n) {
    float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    float2 signNotZero = float2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
    float2 e = (n.z >= 0.0f) ? p : (1.0f - abs(p.yx)) * signNotZero;
    return uint((e.x * 0.5f + 0.5f) * 65535.0f) | (uint((e.y * 0.5f + 0.5f) * 65535.0f) << 16);
}

float3 wl_oct_decode(uint encoded) {
    float2 e = float2(encoded & 0xffffu, encoded >> 16) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float fold = max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -fold : fold;
    n.y += n.y >= 0.0f ? -fold : fold;
    return normalize(n);
}

bool wl_restir_has_sample(LightReservoir r) {
    return (r.flagsValid & 1u) != 0u;
}

// Tag each stored reservoir with the frame that produced it. Temporal reuse
// accepts only the immediately previous frame, rejecting uninitialized or stale records.
bool wl_restir_is_previous_frame(LightReservoir r) {
    uint previousFrameTag = (uint(u_FrameIndex) - 1u) & 0x7fffffffu;
    // M proves the record was written; bit 0 independently indicates whether
    // that record has a selected sample. Keep zero-target proposal counts.
    return r.M > 0 && (r.flagsValid >> 1) == previousFrameTag;
}

bool wl_restir_is_current_frame(LightReservoir r) {
    uint currentFrameTag = uint(u_FrameIndex) & 0x7fffffffu;
    return r.M > 0 && (r.flagsValid >> 1) == currentFrameTag;
}

// Reservoir update: incorporate one candidate sample into the reservoir.
// targetPdf: the target function value (e.g. light contribution weight)
// sourcePdf: the PDF of the sampling strategy that produced this sample
// rnd: random number in [0,1)
void ReservoirUpdate(inout LightReservoir r, uint lightIdx, float targetPdf, float sourcePdf, float rnd) {
    // M is set to the total number of proposals after the candidate loop, so
    // zero-weight proposals (occluded/back-facing/zero-emission lights) count
    // too. This keeps the estimator normalized to the actual proposal budget.
    float w = (sourcePdf > 0.0f) ? (max(targetPdf, 0.0f) / sourcePdf) : 0.0f;
    float newSum = r.wSum + w;
    if (w > 0.0f && rnd * newSum < w) {
        r.lightIndex = lightIdx;
        r.bary.x = targetPdf; // selected target value, used to finalize W
        r.flagsValid = 1;
    }
    r.wSum = newSum;
}

// Target used by this first ReSTIR DI slice: diffuse Lambert response to a
// point-sampled emissive instance. The light-list model matches the existing
// center-point source approximation; metal/specular and area-emitter sampling
// are not represented by the G-buffer data used here.
float RestirTarget(uint lightListIndex, float3 receiverPos, float3 receiverNormal, float3 receiverAlbedo) {
    uint lightCount = uint(min(u_LightCount, 64));
    if (lightListIndex >= lightCount) return 0.0f;
    int instanceIndex = LightIndices[lightListIndex];
    RayTracingInstance light = Instances[instanceIndex];
    float3 toLight = float3(light.WorldTransform[0][3], light.WorldTransform[1][3], light.WorldTransform[2][3]) - receiverPos;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 0.001f) return 0.0f;
    float3 dirToLight = toLight / distanceToLight;
    float NdotL = max(dot(receiverNormal, dirToLight), 0.0f);
    if (NdotL <= 0.0f) return 0.0f;
    Ray shadowRay = MakeRay(receiverPos + receiverNormal * 0.001f, dirToLight);
    if (IsOccluded(shadowRay, distanceToLight, instanceIndex)) return 0.0f;
    float3 emission = light.Emission.xyz * light.Emission.w;
    float3 diffuseContribution = receiverAlbedo * emission * (NdotL / PI);
    return max(dot(diffuseContribution, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

// Reuse a previous-frame reservoir at this receiver. Its selected target must
// be re-evaluated here; old W alone is not a valid merge weight after reprojection.
// Algorithm 6 source-support correction: the history's represented proposals
// count toward the denominator only when the history's source PDF supports the
// current target (historyTarget > 0). Unconditional total-M normalization is
// biased under source-support mismatch.
LightReservoir CombineReservoirs(LightReservoir current, LightReservoir history,
                                 float currentTarget, float historyTarget, float rnd) {
    LightReservoir result = current;
    float currentSum = (wl_restir_has_sample(current) && currentTarget > 0.0f) ? max(current.wSum, 0.0f) : 0.0f;
    float currentM = float(current.M);
    float historyCount = (history.M > 0)
        ? min(float(history.M), max(0.0f, 32.0f - currentM)) : 0.0f;
    float historyWeight = historyTarget * max(history.W, 0.0f) * historyCount;
    float totalWeight = currentSum + historyWeight;
    // Z(y) = sum_i M_i * 1[pHat_qi(y) > 0]: the history source supports the
    // current target only when historyTarget > 0; otherwise its M is excluded.
    float totalM = currentM + ((historyTarget > 0.0f) ? historyCount : 0.0f);

    result.wSum = totalWeight;
    result.M = uint(totalM);
    result.flagsValid = 0;
    result.W = 0.0f;
    if (totalWeight > 1e-8f && totalM > 0.0f) {
        if (historyWeight > 0.0f && rnd * totalWeight < historyWeight) {
            result.lightIndex = history.lightIndex;
            result.bary.x = historyTarget;
            result.flagsValid = 1;
        } else if (currentSum > 0.0f && currentTarget > 0.0f) {
            result.bary.x = currentTarget;
            result.flagsValid = 1;
        }
        if (result.flagsValid != 0 && result.bary.x > 1e-8f)
            result.W = totalWeight / (result.bary.x * totalM);
    }
    return result;
}

// Pass 3: ReSTIR Generate
// For each pixel, draw M candidates from the light list and update the reservoir.
// This replaces the stochastic subset in ComputeDirectLighting.
void RunRestirGenerate() {
    int2 pixelCoords = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Accumulation);
    int2 imgSize = wl_trace_extent(fullSize);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    // Integer boundaries partition the full-res plane into non-overlapping
    // tiles. Supported texture extents keep these 32-bit coordinate products bounded.
    int2 reservoirMin = pixelCoords * fullSize / imgSize;
    int2 reservoirMax = (pixelCoords + int2(1, 1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    LightReservoir r;
    r.lightIndex = 0;
    r.M = 0;
    r.W = 0.0f;
    r.wSum = 0.0f;
    r.bary = float2(0.0f, 0.0f);
    r.normalOct = 0;
    r.flagsValid = 0;

    float4 gbuffer = IMG_LOAD(img_TraceGBuffer, pixelCoords);
    float depth = gbuffer.r;
    if (depth < 1.0f) {
        float2 uv = (float2(pixelCoords) + 0.5f) / float2(imgSize);
        float2 ndc = uv * 2.0f - 1.0f;
        float4 worldPos4 = mul(u_InverseViewProjection, float4(ndc, depth * 2.0f - 1.0f, 1.0f));
        float3 worldPos = worldPos4.xyz / worldPos4.w;
        float3 normal = normalize(gbuffer.gba * 2.0f - 1.0f);
        r.normalOct = wl_oct_encode(normal);
        r.bary.y = depth; // previous receiver depth for temporal rejection

        float4 albedoHit = IMG_LOAD(img_AlbedoHit, pixelCoords);
        int M = 4; // proposals per pixel per frame
        int lightCount = min(u_LightCount, 64);
        if (albedoHit.a >= 0.5f && lightCount > 0) {
            seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
            float sourcePdf = 1.0f / float(lightCount);
            for (int m = 0; m < M; m++) {
                uint lightIdx = min(uint(hash() * lightCount), uint(lightCount - 1));
                int i = LightIndices[lightIdx];
                RayTracingInstance light = Instances[i];
                float3 toLight = float3(light.WorldTransform[0][3], light.WorldTransform[1][3], light.WorldTransform[2][3]) - worldPos;
                float distToLight = length(toLight);
                float targetPdf = 0.0f;
                if (distToLight > 0.001f) {
                    float3 dirToLight = toLight / distToLight;
                    float NdotL = max(dot(normal, dirToLight), 0.0f);
                    if (NdotL > 0.0f) {
                        Ray shadowRay = MakeRay(worldPos + normal * 0.001f, dirToLight);
                        if (!IsOccluded(shadowRay, distToLight, i)) {
                            float3 emission = light.Emission.xyz * light.Emission.w;
                            float3 diffuseContribution = albedoHit.rgb * emission * (NdotL / PI);
                            targetPdf = dot(diffuseContribution, float3(0.2126f, 0.7152f, 0.0722f));
                        }
                    }
                }
                // Submit every draw, including zero-target candidates, so M is
                // the actual proposal count for the uniform source distribution.
                ReservoirUpdate(r, lightIdx, targetPdf, sourcePdf, hash());
            }
            r.M = M;
            if (r.flagsValid != 0 && r.bary.x > 1e-8f)
                r.W = r.wSum / (float(M) * r.bary.x);
        }
    }

    if (r.M > 0)
        r.flagsValid = (r.flagsValid & 1u) | ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    // Write the tile representative once, then fill every other slot in the tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// Pass 11: ReSTIR temporal combine (spatial gather deliberately disabled).
//
// PING-PONG CONTRACT -- paired with pass 3:
//   ReservoirRead  = previous frame's combined reservoir (temporal history)
//   ReservoirWrite = this frame's generated candidates and per-pixel combine
//
// Pass 11 reads the current pixel from ReservoirWrite, temporal history from
// ReservoirRead, and writes its complete non-overlapping full-resolution tile.
// Tile ownership avoids write races. Reading neighboring ReservoirWrite slots
// here would still race with concurrent writes; spatial reuse requires an
// immutable input and a separate output buffer. The caller swaps after pass 11
// to promote this frame's result to next frame's history.
void RunRestirCombine() {
    int2 pixelCoords = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Accumulation);
    int2 imgSize = wl_trace_extent(fullSize);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    // Match Generate's integer partition; the tile's top-left is its representative.
    int2 reservoirMin = pixelCoords * fullSize / imgSize;
    int2 reservoirMax = (pixelCoords + int2(1, 1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    // The CURRENT pixel comes from ReservoirWrite, not ReservoirRead: pass 3
    // wrote this frame's fresh candidates there. Reading ReservoirRead here
    // would start from the previous frame's result and never see pass 3 at all.
    LightReservoir r = ReservoirWrite[flatIdx];

    float4 gb = IMG_LOAD(img_TraceGBuffer, pixelCoords);
    float depth = gb.r;
    float3 receiverN = normalize(gb.gba * 2.0f - 1.0f);
    float3 receiverPos = float3(0.0f, 0.0f, 0.0f);
    if (depth < 1.0f) {
        float2 receiverUV = (float2(pixelCoords) + 0.5f) / float2(imgSize);
        float2 receiverNdc = receiverUV * 2.0f - 1.0f;
        float4 receiverWorld4 = mul(u_InverseViewProjection, float4(receiverNdc, depth * 2.0f - 1.0f, 1.0f));
        receiverPos = receiverWorld4.xyz / receiverWorld4.w;
    }

    // Temporal reuse: reproject to previous frame
    float2 velocity = IMG_LOAD(img_Velocity, pixelCoords).xy;
    float2 uv = (float2(pixelCoords) + 0.5f) / float2(imgSize);
    float2 prevUV = uv - velocity;

    bool validHistory = depth < 1.0f && u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;

    if (validHistory) {
        // Read previous frame's reservoir
        // Address the stable full-resolution reservoir plane directly. The
        // current trace extent can differ from the one that produced history.
        int2 prevReservoirPixel = clamp(int2(prevUV * float2(fullSize)), int2(0, 0), fullSize - int2(1, 1));
        int prevFlatIdx = prevReservoirPixel.y * fullSize.x + prevReservoirPixel.x;
        LightReservoir prevR = ReservoirRead[prevFlatIdx];

        // Validate against surface metadata stored with the previous
        // reservoir, not two samples from the current frame's depth texture.
        float3 prevNormal = wl_oct_decode(prevR.normalOct);
        float3 receiverAlbedo = IMG_LOAD(img_AlbedoHit, pixelCoords).rgb;
        float historyTarget = (wl_restir_is_previous_frame(prevR) && wl_restir_has_sample(prevR))
            ? RestirTarget(prevR.lightIndex, receiverPos, receiverN, receiverAlbedo) : 0.0f;
        float currentTarget = (wl_restir_has_sample(r))
            ? RestirTarget(r.lightIndex, receiverPos, receiverN, receiverAlbedo) : 0.0f;
        if (wl_restir_is_previous_frame(prevR) && abs(depth - prevR.bary.y) < 0.02f && dot(receiverN, prevNormal) > 0.9f) {
            seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
            r = CombineReservoirs(r, prevR, currentTarget, historyTarget, hash());
        }
    }

    // Pass 14 performs spatial reuse and the single final shading write.

    // Keep the current receiver's surface metadata after temporal selection.
    r.bary.y = depth;
    r.normalOct = wl_oct_encode(normalize(gb.gba * 2.0f - 1.0f));
    if (r.M > 0)
        r.flagsValid = (wl_restir_has_sample(r) ? 1u : 0u) |
                       ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    else
        r.flagsValid = 0u;
    // Write the updated reservoir across this invocation's complete tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// Experimental spatial reuse with the Algorithm 6 source-support correction:
// a neighbor's represented proposals count toward the denominator only when
// the neighbor's source PDF supports the center receiver's target at the
// neighbor's selected sample. Differing visibility/target support no longer
// darkens the estimate through unconditional total-M normalization.
void RunRestirSpatial() {
    int2 pixelCoords = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Accumulation);
    int2 imgSize = wl_trace_extent(fullSize);
    if (any(pixelCoords < int2(0, 0)) || any(pixelCoords >= imgSize)) return;

    // One invocation owns one trace pixel's complete full-resolution tile.
    int2 reservoirMin = pixelCoords * fullSize / imgSize;
    int2 reservoirMax = (pixelCoords + int2(1, 1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    LightReservoir r = ReservoirRead[flatIdx];

    float4 gb = IMG_LOAD(img_TraceGBuffer, pixelCoords);
    float depth = gb.r;
    float3 receiverN = normalize(gb.gba * 2.0f - 1.0f);
    float4 albedoHit = IMG_LOAD(img_AlbedoHit, pixelCoords);
    float3 receiverAlbedo = albedoHit.rgb;
    float3 receiverPos = float3(0.0f, 0.0f, 0.0f);
    if (depth < 1.0f) {
        float2 receiverUV = (float2(pixelCoords) + 0.5f) / float2(imgSize);
        float2 receiverNdc = receiverUV * 2.0f - 1.0f;
        float4 receiverWorld4 = mul(u_InverseViewProjection,
            float4(receiverNdc, depth * 2.0f - 1.0f, 1.0f));
        receiverPos = receiverWorld4.xyz / receiverWorld4.w;
    }
    r.bary.y = depth;
    r.normalOct = wl_oct_encode(receiverN);

    uint totalM = r.M;
    float totalWeight = 0.0f;
    float selectedTarget = 0.0f;
    bool selectedValid = false;
    if (wl_restir_has_sample(r) && r.M > 0 && r.W > 0.0f && r.wSum > 0.0f && r.bary.x > 0.0f) {
        totalWeight = r.wSum;
        selectedTarget = r.bary.x;
        selectedValid = true;
    } else {
        r.W = 0.0f;
        r.wSum = 0.0f;
        r.bary.x = 0.0f;
        r.flagsValid = 0u;
    }

    // A distinct deterministic seed salt reduces direct RNG stream reuse; it
    // does not prove statistical independence of the input reservoir histories.
    seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x)
         + uint(u_FrameIndex) * 0x9e3779b9u + 0x85ebca6bu;
    if (depth < 1.0f && albedoHit.a >= 0.5f) {
        for (int direction = 0; direction < 4; ++direction) {
            int2 neighborTrace = pixelCoords;
            if (direction == 0) neighborTrace.x -= 1;
            else if (direction == 1) neighborTrace.x += 1;
            else if (direction == 2) neighborTrace.y -= 1;
            else neighborTrace.y += 1;
            if (any(neighborTrace < int2(0, 0)) || any(neighborTrace >= imgSize)) continue;

            int2 neighborMin = neighborTrace * fullSize / imgSize;
            int neighborFlatIdx = neighborMin.y * fullSize.x + neighborMin.x;
            LightReservoir neighborR = ReservoirRead[neighborFlatIdx];
            if (!wl_restir_is_current_frame(neighborR)) continue;
            float3 neighborNormal = wl_oct_decode(neighborR.normalOct);
            if (abs(depth - neighborR.bary.y) >= 0.02f || dot(receiverN, neighborNormal) <= 0.9f) continue;

            uint neighborCount = neighborR.M;
            float neighborTarget = 0.0f;
            if (wl_restir_has_sample(neighborR) && neighborR.W > 0.0f)
                neighborTarget = RestirTarget(neighborR.lightIndex, receiverPos, receiverN, receiverAlbedo);
            float neighborWeight = neighborTarget * max(neighborR.W, 0.0f) * float(neighborCount);
            float combinedWeight = totalWeight + neighborWeight;
            if (neighborWeight > 0.0f && hash() * combinedWeight < neighborWeight) {
                r.lightIndex = neighborR.lightIndex;
                selectedTarget = neighborTarget;
                selectedValid = true;
            }
            // Algorithm 6 source-support correction: count the neighbor's
            // represented proposals only when the neighbor's source PDF
            // supports the center receiver's target at the neighbor's
            // selected sample (neighborTarget > 0). Unconditional total-M
            // normalization is biased under source-support mismatch.
            totalWeight = combinedWeight;
            if (neighborTarget > 0.0f)
                totalM += neighborCount;
        }
    }

    r.M = totalM;
    r.wSum = totalWeight;
    r.bary.x = selectedValid ? selectedTarget : 0.0f;
    if (selectedValid && totalM > 0 && totalWeight > 1e-8f && selectedTarget > 1e-8f) {
        r.W = totalWeight / (selectedTarget * float(totalM));
        r.flagsValid = 1u;
    } else {
        r.W = 0.0f;
        r.flagsValid = 0u;
    }
    if (totalM > 0)
        r.flagsValid = (r.flagsValid & 1u) | ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    else
        r.flagsValid = 0u;

    float3 directLight = float3(0.0f, 0.0f, 0.0f);
    if (depth < 1.0f && albedoHit.a >= 0.5f && wl_restir_has_sample(r) && r.W > 0.0f) {
        int i = LightIndices[r.lightIndex];
        RayTracingInstance light = Instances[i];
        float3 toLight = float3(light.WorldTransform[0][3], light.WorldTransform[1][3], light.WorldTransform[2][3]) - receiverPos;
        float distToLight = length(toLight);
        if (distToLight > 0.001f) {
            float3 dirToLight = toLight / distToLight;
            float NdotL = max(dot(receiverN, dirToLight), 0.0f);
            float3 emission = light.Emission.xyz * light.Emission.w;
            directLight = receiverAlbedo * emission * (NdotL / PI) * r.W;
        }
        float4 traceColor = IMG_LOAD(img_Bloom, pixelCoords);
        IMG_STORE(img_Bloom, pixelCoords, float4(traceColor.rgb + directLight, traceColor.a));
    }

    // ReservoirRead is immutable during this dispatch; only the opposite
    // buffer is written, including every slot in this trace pixel's tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// ============================================================================
// Temporal Super-Resolution (experimental, default-off)
// ============================================================================
// Pass 15: TSR resolve. Reads the current frame color (img_Output), reprojects
// using motion vectors, samples the history buffer, rejects outliers via 3x3
// YCoCg neighborhood clamping, blends, and sharpens. Writes the result to
// img_Accumulation and the history buffer.
// Pass 16: copies img_Accumulation to img_Output (avoids read-write race).
//
// The history buffer (img_HistoryBuffer, u11) stores the TSR result from the
// previous frame. The C++ side must clear it on camera cuts and frame 0.
// ============================================================================
// ============================================================================
// RCAS -- Robust Contrast-Adaptive Sharpening (the WLSR sharpen stage).
//
// FIVE TAPS: the centre plus the four DIAGONALS. The diagonals are used rather
// than the axis neighbours because the axis pair is already exercised by the
// upscale's own kernel; the diagonals are what the upscale removed.
//
// THE GAIN COMES FROM THE LOCAL CONTRAST, not from a fixed slider -- that is
// what makes it robust. On a high-contrast edge the neighbourhood's maximum is
// large, the gain collapses toward zero, and the filter cannot ring. On a flat
// region the gain is bounded by the minimum, so it cannot amplify noise beyond
// the local range.
//
// THE REACTIVE MASK MODULATES THE GAIN, and the direction matters. Where the
// mask says the current frame cannot be trusted (a disocclusion, fast motion, a
// camera cut) the image is a single frame's worth of data, so sharpening it
// amplifies noise and aliasing. The gain is therefore scaled by (1 - mask):
// sharpening is REDUCED where the frame is untrusted, never increased.
//
// A PASS BOUNDARY IS REQUIRED. The four diagonal taps live at NEIGHBOURING
// OUTPUT PIXELS, and one invocation only holds the centre in registers -- so
// this cannot be folded into the EASU branch of RunComposite no matter how the
// taps are arranged. That is the whole reason this is pass 15.
//
// RCAS_MAX_SHARPEN and WLAA_SHARPNESS are defined above, before RunComposite,
// because RunComposite's WLAA branch references WLAA_SHARPNESS and HLSL resolves
// #defines textually -- a use before the definition would not see it.
//
// The sharpen core, image-agnostic: given the centre texel and its four diagonal
// neighbours plus the resolved sharpness and reactive mask, return the sharpened
// centre. Kept free of any image so pass 15 (RunRCAS below, which reads
// img_Output) and the WLAA branch of RunComposite (which reads img_Accumulation)
// can share it -- the two callers load their own taps and hand the five colours
// in, so the sharpen math lives in exactly one place.
float3 RunWlsrSharpenCore(float3 ctr, float3 t00, float3 t10, float3 t01, float3 t11, float sharpness, float mask)
{
    // Per-channel local range over the four diagonals.
    float3 mn = min(min(t00, t10), min(t01, t11));
    float3 mx = max(max(t00, t10), max(t01, t11));

    // Gain = local contrast x user sharpness x trust, per channel. The local
    // range is per channel, and a luma-only gain would let a saturated channel's
    // neighbours drag an unsaturated one. The gain is the CONTRAST (mx - mn),
    // not a robust ratio, so the filter sharpens edges rather than flat regions.
    // (1 - mask) is the trust term; the clamp is the anti-ringing guarantee.
    // Written as a two-line form to match the GLSL twin exactly.
    float3 contrast = mx - mn;
    float3 gain = contrast * clamp(sharpness, 0.0f, 1.0f) * (1.0f - clamp(mask, 0.0f, 1.0f));
    gain = clamp(gain, 0.0f, RCAS_MAX_SHARPEN);

    // Unsharp mask: the centre is pushed away from the average of its diagonal
    // neighbours. This is the symmetric, contrast-raising form. (The earlier
    // weighted-average form `(ctr + gain*diag)/(1+4*gain)` is a weighted mean
    // and can only move the centre toward its neighbours -- it blurs rather
    // than sharpens, which is why it lowered p99 contrast instead of raising it.)
    float3 blur = (t00 + t10 + t01 + t11) * 0.25f;
    float3 result = ctr + gain * (ctr - blur);
    return clamp(result, 0.0f, 1.0f);
}

// Pass 15 (WL_RCAS): the unsharp-mask sharpen over the final colour. Reads the
// trace-resolution reactive mask at the trace coordinate -- a point sample
// rather than a bilinear one, because the mask is a per-trace-texel decision and
// interpolating it would blend two independent disocclusion verdicts.
float3 RunRCAS(int2 pos, int2 size, float sharpness)
{
    // Clamp the five tap positions so the filter cannot read outside the image.
    // These are raw loads, which have no CLAMP_TO_EDGE mode, so the clamp is
    // explicit -- a clamped tap replicates the border texel.
    int2 lo = int2(1, 1);
    int2 hi = max(size - int2(2, 2), lo);
    int2 c  = clamp(pos, lo, hi);

    float3 ctr = read_img_Output.Load(int3(c, 0)).rgb;
    float3 t00 = read_img_Output.Load(int3(clamp(c + int2(-1, -1), lo, hi), 0)).rgb;
    float3 t10 = read_img_Output.Load(int3(clamp(c + int2( 1, -1), lo, hi), 0)).rgb;
    float3 t01 = read_img_Output.Load(int3(clamp(c + int2(-1,  1), lo, hi), 0)).rgb;
    float3 t11 = read_img_Output.Load(int3(clamp(c + int2( 1,  1), lo, hi), 0)).rgb;

    int2 traceSize = max(int2(float2(size) * u_RenderScale), int2(1, 1));
    int2 mpos = clamp(int2((float2(pos) + 0.5f) * u_RenderScale), int2(0, 0), traceSize - int2(1, 1));
    float mask = IMG_LOAD(img_Bloom_Temp, mpos).a;

    return RunWlsrSharpenCore(ctr, t00, t10, t01, t11, sharpness, mask);
}

// The pass entry point. Dispatches at FULL resolution and reads/writes
// img_Output in place -- each invocation writes exactly the texel it loaded, so
// there is no read/write race between invocations.
void RunRCAS() {
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    // u_TSRSharpenStrength is the existing WLSR sharpen knob: it is declared but
    // never uploaded, so it is 0 by default and RCAS is inert until something
    // sets it. That is the right default -- a sharpen stage that runs
    // unconditionally is a quality change nobody asked for.
    float sharpness = u_TSRSharpenStrength;

    // u_RCSEnabled is the gate. Absent it the pass is a no-op, so dispatching it
    // costs a full-resolution read/write and changes nothing. The gate is what
    // lets the A/B harness turn the stage on and off without a rebuild.
    if (u_RCSEnabled < 1 || sharpness <= 0.001f) {
        return;
    }

    IMG_STORE(img_Output, pos, float4(RunRCAS(pos, fullSize, sharpness), 1.0f));
}

void RunTSRResolve() {
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    float3 currentColor = IMG_LOAD(img_Output, pos).rgb;

    if (u_TSREnabled != 1) {
        IMG_STORE(img_Accumulation, pos, float4(currentColor, 1.0f));
        return;
    }

    float2 velocity = IMG_LOAD(img_Velocity, pos).xy;
    float2 uv = (float2(pos) + 0.5f) / float2(fullSize);
    float2 prevUV = uv - velocity;

    bool validHistory = u_FrameIndex > 0 &&
                        prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
                        prevUV.y >= 0.0f && prevUV.y <= 1.0f;

    if (validHistory) {
        float currentDepth = TEX(s_DepthBuffer, uv).r;
        float prevDepth = TEX(s_DepthBuffer, prevUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    float3 historyColor = float3(0.0f, 0.0f, 0.0f);

    if (validHistory) {
        // Bilinear sample from history buffer
        float2 historyCoord = prevUV * float2(fullSize) - float2(0.5f, 0.5f);
        int2 h0 = clamp(int2(floor(historyCoord)), int2(0, 0), fullSize - int2(1, 1));
        int2 h1 = clamp(h0 + int2(1, 1), int2(0, 0), fullSize - int2(1, 1));
        float2 hf = clamp(historyCoord - float2(h0), float2(0.0f, 0.0f), float2(1.0f, 1.0f));
        float3 c00 = img_HistoryBuffer[h0].rgb;
        float3 c10 = img_HistoryBuffer[int2(h1.x, h0.y)].rgb;
        float3 c01 = img_HistoryBuffer[int2(h0.x, h1.y)].rgb;
        float3 c11 = img_HistoryBuffer[h1].rgb;
        historyColor = lerp_safe(lerp_safe(c00, c10, hf.x), lerp_safe(c01, c11, hf.x), hf.y);

        // Neighborhood clamping (3x3 YCoCg variance clamp)
        float3 m1 = float3(0.0f, 0.0f, 0.0f);
        float3 m2 = float3(0.0f, 0.0f, 0.0f);
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                int2 sp = clamp(pos + int2(x, y), int2(0, 0), fullSize - int2(1, 1));
                float3 s = RGB2YCoCg(IMG_LOAD(img_Output, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
        }
        float3 mean = m1 / 9.0f;
        float3 stddev = sqrt(max(float3(0.0f, 0.0f, 0.0f), m2 / 9.0f - mean * mean));

        float3 historyYCoCg = RGB2YCoCg(historyColor);
        float3 minCol = mean - 1.25f * stddev;
        float3 maxCol = mean + 1.25f * stddev;
        historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
        historyColor = YCoCg2RGB(historyYCoCg);
    }

    // Blend history with current frame
    float motionMag = length(velocity * float2(fullSize));
    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? lerp_safe(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;
    float3 result = lerp_safe(historyColor, currentColor, alpha);

    // Sharpening (unsharp mask)
    if (u_TSRSharpenStrength > 0.001f) {
        float3 sharp = float3(0.0f, 0.0f, 0.0f);
        sharp += IMG_LOAD(img_Output, pos).rgb * 4.0f;
        sharp -= IMG_LOAD(img_Output, pos + int2(1, 0)).rgb;
        sharp -= IMG_LOAD(img_Output, pos - int2(1, 0)).rgb;
        sharp -= IMG_LOAD(img_Output, pos + int2(0, 1)).rgb;
        sharp -= IMG_LOAD(img_Output, pos - int2(0, 1)).rgb;
        result = clamp(result + sharp * u_TSRSharpenStrength * 0.25f, 0.0f, 1.0f);
    }

    IMG_STORE(img_Accumulation, pos, float4(result, 1.0f));
    IMG_STORE(img_HistoryBuffer, pos, float4(result, 1.0f));
}

// Pass 16: copy TSR result to output.
void RunTSRCopy() {
    int2 pos = int2(g_dt.xy);
    int2 fullSize = IMG_SIZE(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;
    float3 color = IMG_LOAD(img_Accumulation, pos).rgb;
    IMG_STORE(img_Output, pos, float4(color, 1.0f));
}

// ===========================================================================
// Trained end-of-frame neural stage (docs/NEURAL_RENDERING.md)
// ===========================================================================
// The HLSL twin of the GLSL stage. Same three passes, same order, same
// constants, same weight offsets -- the two files are kept in lockstep by
// tools/verify_neural_stage_parity.py, which compares the statements that
// actually compute rather than the comments around them.
//
// WHY THIS IS IN THE HLSL AT ALL: the HLSL serves DX11, DX12 and Vulkan (Vulkan
// loads the SPIR-V that DXC builds from this file), so a stage present only in
// the GLSL would be an OpenGL-only feature. The GL path is the only one that can
// be verified on this machine -- DX12 hangs during init and VulkanNova is
// compiled but never instantiated -- so this twin is verified by compilation and
// by statement parity, not by rendering.
//
// THE ANCHOR IS READ THROUGH THE SNAPSHOT SRV, NOT THE UAV. GLSL's
// imageLoad(img_Output, pos) reads the live image; HLSL's IMG_LOAD expands to
// read_img_Output, which is the per-dispatch snapshot that DX12Nova::Dispatch
// refreshes at the top of every dispatch (see the loop at ~DX12Nova.cpp:1423).
// The two are the same data here: pass 17 does not write img_Output, and pass 18
// has each thread read and write only its own texel, so the pre-pass value and
// the live value agree. Reading the snapshot is strictly safer -- it removes the
// read/write hazard on one texture through two bindings entirely.

// groupshared, not per-thread StructuredBuffer reads: per-thread reads were
// half of the ~238 ms/frame the naive version cost. Declared at file scope
// because fxc/dxc cannot compile a dynamically-indexed local array.
groupshared uint  gs_neural_r[64];
groupshared uint  gs_neural_g[64];
groupshared uint  gs_neural_b[64];
groupshared uint  gs_neural_l[64];
// Pass 18's per-block gate partial. Same shape and same reason as the four
// above: a groupshared tree reduce is what makes the per-block sum O(1)
// instead of a serialized UAV atomic.
groupshared uint  gs_neural_gate[64];
groupshared float gs_neural_w[1140];

static const int NEURAL_N_IN  = 8;
static const int NEURAL_N_HID = 32;

// Weight offsets into w[], derived from LAYER_SHAPES in
// tools/train_neural/model.py. W1 is (8,32), so W1[i][j] lives at i*32+j; every
// later layer follows suit. These are the SAME numbers the GLSL uses -- the
// parity verifier asserts it.
static const int O_W1 = 0;
static const int O_B1 = O_W1 + NEURAL_N_IN * NEURAL_N_HID;   // 256
static const int O_W2 = O_B1 + NEURAL_N_HID;                 // 288
static const int O_B2 = O_W2 + NEURAL_N_HID * NEURAL_N_IN;   // 544
static const int O_W3 = O_B2 + NEURAL_N_IN;                  // 552
static const int O_B3 = O_W3 + NEURAL_N_IN * NEURAL_N_HID;   // 808
static const int O_W4 = O_B3 + NEURAL_N_HID;                 // 840
static const int O_B4 = O_W4 + NEURAL_N_HID * NEURAL_N_IN;   // 1096
static const int O_WH = O_B4 + NEURAL_N_IN;                  // 1104
static const int O_BH = O_WH + NEURAL_N_IN * 3;              // 1128
static const int O_WG = O_BH + 3;                            // 1131
static const int O_BG = O_WG + NEURAL_N_IN;                  // 1139

// Total weight count, for the groupshared staging array above.
static const int NEURAL_N_W = 1140;

// Must match NEURAL_MAX_BLOCKS in the GLSL twin. 131072 is the next power of two
// above the 129600 blocks a 3840x2160 frame produces at 8x8 workgroups; 8192 was
// tried first and silently under-counted a 2332x1451 frame by 6.5x.
static const int NEURAL_MAX_BLOCKS = 131072;

// Element offset of g_Partial inside NeuralGlobals. The GLSL's SSBO is
// `float g_Mean[4]; uint g_Partial[]`, so the partials start at byte 16 -- which
// is element 4 of a uint buffer. Keeping this as a named constant is what makes
// the two files' layouts auditable rather than coincidental.
static const int NEURAL_PARTIAL_BASE = 4;

// Element offsets for the gate slots, derived from the GLSL SSBO's std430
// layout:
//   float g_Mean[4];              -> elements 0..3
//   uint  g_Partial[MAX * 4];     -> elements 4 .. 4+MAX*4-1
//   float g_GateMean;             -> element  4+MAX*4
//   uint  g_GatePartial[MAX];     -> elements 4+MAX*4+1 .. 4+MAX*5
// Both twins index this one buffer, so if this constant is wrong the DX12/Vulkan
// gate readout is wrong while the GL one is right -- which is exactly the kind of
// split that is invisible until someone checks the two against each other.
static const int NEURAL_GATEMEAN_BASE       = 4 + NEURAL_MAX_BLOCKS * 4;
static const int NEURAL_GATEPARTIAL_BASE    = NEURAL_GATEMEAN_BASE + 1;

// Pass 17, stage one: each block reduces its own 64 pixels and writes four
// fixed-point partials. NO atomics -- the atomic version cost 188 ms/frame
// (886 ns per atomic) because 212k atomicAdds onto four addresses serialise.
void RunNeuralReduce()
{
    const int2 size = int2(g_dims);
    const int2 pos = int2(g_dt.xy);
    const int li = int(g_tid);

    // Both GLSL and HLSL convert with a truncating float->uint cast, so the two
    // agree wherever the anchor is non-negative. The renderer cannot produce a
    // negative channel here -- the composite sums radiance, bloom and god rays,
    // all of which are non-negative -- but a negative value would be undefined
    // in GLSL and 0 in HLSL, so this is a documented assumption rather than a
    // silent divergence.
    float3 c = float3(0.0f, 0.0f, 0.0f);
    if (pos.x < size.x && pos.y < size.y)
        c = IMG_LOAD(img_Output, pos).rgb;
    const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));

    gs_neural_r[li] = uint(c.r * 256.0f + 0.5f);
    gs_neural_g[li] = uint(c.g * 256.0f + 0.5f);
    gs_neural_b[li] = uint(c.b * 256.0f + 0.5f);
    gs_neural_l[li] = uint(lum  * 256.0f + 0.5f);
    GroupMemoryBarrierWithGroupSync();

    for (int stride = 32; stride > 0; stride >>= 1)
    {
        if (li < stride)
        {
            gs_neural_r[li] += gs_neural_r[li + stride];
            gs_neural_g[li] += gs_neural_g[li + stride];
            gs_neural_b[li] += gs_neural_b[li + stride];
            gs_neural_l[li] += gs_neural_l[li + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (li == 0)
    {
        // One slot per block. The guard keeps a frame larger than
        // NEURAL_MAX_BLOCKS from writing past the allocation; such a frame would
        // under-count rather than corrupt, and no frame this engine renders
        // reaches it.
        //
        // The block index comes from SV_GroupID, which is the HLSL equivalent of
        // GLSL's gl_WorkGroupID. The GLSL multiplies by gl_NumWorkGroups.x; here
        // the same number is derived from g_dims, because SV_GroupID does not
        // carry the dispatch extent and the dispatch is always ceil(size/8).
        const uint blocksX = (size.x + 7u) / 8u;
        const uint bid = g_gid.y * blocksX + g_gid.x;
        if (bid < (uint)NEURAL_MAX_BLOCKS)
        {
            NeuralGlobals[NEURAL_PARTIAL_BASE + bid * 4u + 0u] = gs_neural_r[0];
            NeuralGlobals[NEURAL_PARTIAL_BASE + bid * 4u + 1u] = gs_neural_g[0];
            NeuralGlobals[NEURAL_PARTIAL_BASE + bid * 4u + 2u] = gs_neural_b[0];
            NeuralGlobals[NEURAL_PARTIAL_BASE + bid * 4u + 3u] = gs_neural_l[0];
        }
    }
}

// Pass 17, stage two: ONE block sums the partials and writes the four means.
//
// A single 64-thread block, each thread striding over the array. 4K produces
// 129600 partials; 64 threads is 2025 loads each, which is a few microseconds --
// three orders of magnitude cheaper than the 212k atomics this replaced.
//
// The block count is DERIVED FROM THE IMAGE SIZE, not from the dispatch extent.
// The GLSL version originally read gl_NumWorkGroups here, which reports the
// CURRENT dispatch's extent -- and this pass runs as one block, so it summed a
// single block's partials out of 129600 and wrote a mean of ~0. The eval pass
// then produced exactly the output it would have with a zero global vector.
// SV_GroupID has the same trap, so the count comes from g_dims in both files.
void RunNeuralReduceFinal()
{
    const int2 size = int2(g_dims);
    const uint blocksX = (size.x + 7u) / 8u;
    const uint blocksY = (size.y + 7u) / 8u;
    const uint numBlocks = blocksX * blocksY;
    const uint clamped = min(numBlocks, (uint)NEURAL_MAX_BLOCKS);
    const int li = int(g_tid);

    uint sr = 0u, sg = 0u, sb = 0u, sl = 0u, sgate = 0u;
    for (uint b = li; b < clamped; b += 64u)
    {
        sr    += NeuralGlobals[NEURAL_PARTIAL_BASE + b * 4u + 0u];
        sg    += NeuralGlobals[NEURAL_PARTIAL_BASE + b * 4u + 1u];
        sb    += NeuralGlobals[NEURAL_PARTIAL_BASE + b * 4u + 2u];
        sl    += NeuralGlobals[NEURAL_PARTIAL_BASE + b * 4u + 3u];
        sgate += NeuralGlobals[NEURAL_GATEPARTIAL_BASE + b];
    }

    gs_neural_r[li] = sr;
    gs_neural_g[li] = sg;
    gs_neural_b[li] = sb;
    gs_neural_l[li] = sl;
    gs_neural_gate[li] = sgate;
    GroupMemoryBarrierWithGroupSync();

    for (int stride = 32; stride > 0; stride >>= 1)
    {
        if (li < stride)
        {
            gs_neural_r[li] += gs_neural_r[li + stride];
            gs_neural_g[li] += gs_neural_g[li + stride];
            gs_neural_b[li] += gs_neural_b[li + stride];
            gs_neural_l[li] += gs_neural_l[li + stride];
            gs_neural_gate[li] += gs_neural_gate[li + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (li == 0)
    {
        // Fixed-point back to a mean: the sums are v * 256 accumulated over
        // every pixel, so divide by (256 * pixelCount). Written as raw bits
        // because NeuralGlobals is a uint buffer.
        const float inv = 1.0f / (256.0f * float(size.x) * float(size.y));
        NeuralGlobals[0] = asuint(float(gs_neural_r[0]) * inv);
        NeuralGlobals[1] = asuint(float(gs_neural_g[0]) * inv);
        NeuralGlobals[2] = asuint(float(gs_neural_b[0]) * inv);
        NeuralGlobals[3] = asuint(float(gs_neural_l[0]) * inv);
        NeuralGlobals[NEURAL_GATEMEAN_BASE] = asuint(float(gs_neural_gate[0]) * inv);
    }
}

// Pass 18: evaluate the network for one pixel and write the enhanced texel.
void RunNeuralEval()
{
    const int2 size = int2(g_dims);

    // Stage the weights into groupshared memory ONCE per block. Every thread in
    // the workgroup MUST run this loop and reach the barrier -- including the
    // threads whose pixel is out of bounds.
    //
    // The bounds guard used to sit above this loop, so a ragged workgroup (the
    // last row or column of a frame whose size is not a multiple of 8) had those
    // threads return before the barrier. They then never wrote their share of
    // gs_neural_w, and a barrier with absent threads is undefined behaviour: the
    // survivors read uninitialised shared memory. Measured on a 2332x1451 frame
    // (292x182 workgroups): the last 3 rows came out 79.9% wrong and the last 4
    // columns 56.5% wrong -- matching the 40/64 and 32/64 of the weight array
    // that the missing threads would have filled.
    for (int i = int(g_tid); i < NEURAL_N_W; i += 64)
        gs_neural_w[i] = NeuralWeights[i];
    GroupMemoryBarrierWithGroupSync();

    // The anchor is this thread's OWN texel, read through the snapshot SRV (see
    // the note at the top of this section for why that is not the UAV).
    const int2 pos = int2(g_dt.xy);
    const bool inBounds = (pos.x < size.x && pos.y < size.y);

    // The bounds guard is a BRANCH, not a return. Every thread has to reach the
    // gate barrier at the end of this function, because a barrier that
    // out-of-bounds threads skip is undefined behaviour -- and the ragged
    // workgroup is not hypothetical: a 2332x1451 frame has 40/64 valid threads in
    // its last row and 32/64 in its last column. Out-of-bounds lanes contribute
    // 0 to the gate sum, so they cannot bias it.
    // g must be declared OUTSIDE the bounds branch. It was declared inside the
    // in-bounds body, which scoped it to that branch alone -- so the else below
    // assigned to an undeclared identifier and the gate partial after the branch
    // read one. Both are compile errors (dxc: "use of undeclared identifier 'g'"),
    // and neither tools/verify_neural_stage_parity.py nor a GLSL compile can see
    // them: only compiling THIS file does. The GLSL twin was fixed the same way.
    float g = 0.0f;

    if (inBounds)
    {
    const float4 px = IMG_LOAD(img_Output, pos);
    const float3 anchor = px.rgb;
    const float lum = dot(anchor, float3(0.2126f, 0.7152f, 0.0722f));

    // The frame mean, written by pass 19.
    const float mr = asfloat(NeuralGlobals[0]);
    const float mg = asfloat(NeuralGlobals[1]);
    const float mb = asfloat(NeuralGlobals[2]);
    const float ml = asfloat(NeuralGlobals[3]);

    // Standardise. These eight constants are part of the ABI -- measured on the
    // 30 training pairs by tools/train_neural/dataset_stats.py. Without this
    // affine the layer-1 pre-activation has a std of 0.0118, half the ReLU units
    // die at init, and the network silently collapses to `out = anchor + const`.
    float x[NEURAL_N_IN];
    x[0] = (anchor.r - 0.1744f) / 0.1331f;
    x[1] = (anchor.g - 0.1700f) / 0.0835f;
    x[2] = (anchor.b - 0.2243f) / 0.1145f;
    x[3] = (lum      - 0.1749f) / 0.0863f;
    x[4] = (mr - 0.1742f) / 0.0964f;
    x[5] = (mg - 0.1699f) / 0.0688f;
    x[6] = (mb - 0.2244f) / 0.0969f;
    x[7] = (ml - 0.1748f) / 0.0687f;

    // Stage 1: 8 -> 32.
    float h1[NEURAL_N_HID];
    for (int j = 0; j < NEURAL_N_HID; ++j)
    {
        float s = gs_neural_w[O_B1 + j];
        for (int i = 0; i < NEURAL_N_IN; ++i)
            s += gs_neural_w[O_W1 + i * NEURAL_N_HID + j] * x[i];
        h1[j] = max(s, 0.0f);
    }

    // Stage 2: 32 -> 8.
    float h2[NEURAL_N_IN];
    for (int j = 0; j < NEURAL_N_IN; ++j)
    {
        float s = gs_neural_w[O_B2 + j];
        for (int i = 0; i < NEURAL_N_HID; ++i)
            s += gs_neural_w[O_W2 + i * NEURAL_N_IN + j] * h1[i];
        h2[j] = max(s, 0.0f);
    }

    // Stage 3: 8 -> 32.
    float h3[NEURAL_N_HID];
    for (int j = 0; j < NEURAL_N_HID; ++j)
    {
        float s = gs_neural_w[O_B3 + j];
        for (int i = 0; i < NEURAL_N_IN; ++i)
            s += gs_neural_w[O_W3 + i * NEURAL_N_HID + j] * h2[i];
        h3[j] = max(s, 0.0f);
    }

    // Stage 4: 32 -> 8.
    float h4[NEURAL_N_IN];
    for (int j = 0; j < NEURAL_N_IN; ++j)
    {
        float s = gs_neural_w[O_B4 + j];
        for (int i = 0; i < NEURAL_N_HID; ++i)
            s += gs_neural_w[O_W4 + i * NEURAL_N_IN + j] * h3[i];
        h4[j] = max(s, 0.0f);
    }

    // Head: 8 -> 3 residual.
    float residual[3];
    for (int j = 0; j < 3; ++j)
    {
        float s = gs_neural_w[O_BH + j];
        for (int i = 0; i < NEURAL_N_IN; ++i)
            s += gs_neural_w[O_WH + i * 3 + j] * h4[i];
        residual[j] = s;
    }

    // Gate: 8 -> 1 sigmoid.
    g = gs_neural_w[O_BG];
    for (int i = 0; i < NEURAL_N_IN; ++i)
        g += gs_neural_w[O_WG + i] * h4[i];
    g = 1.0f / (1.0f + exp(-g));

    // Residual, never a replacement: with the head zeroed at init this is
    // exactly the identity, so the worst case is "no enhancement".
    const float3 outc = anchor + g * float3(residual[0], residual[1], residual[2]);
    IMG_STORE(img_Output, pos, float4(clamp(outc, 0.0f, 1.0f), px.a));
    }
    else
    {
        // No pixel here. Contribute nothing to the gate sum, but still take the
        // lane so the barrier below is uniform across the whole workgroup.
        g = 0.0f;
    }

    // Per-block gate partial, reduced by thread 0 and written for pass 19 to
    // sum. Same fixed-point form as the mean partials so pass 19 can share one
    // code path, and the same 256x scale so the two are directly comparable.
    gs_neural_gate[g_tid] = uint(g * 256.0f + 0.5f);
    GroupMemoryBarrierWithGroupSync();

    if (g_tid == 0)
    {
        uint sum = 0u;
        for (int i = 0; i < 64; ++i)
            sum += gs_neural_gate[i];

        const int2 grid = (size + int2(7, 7)) / int2(8, 8);
        const uint bid = uint(g_gid.y) * uint(grid.x) + uint(g_gid.x);
        if (bid < (uint)NEURAL_MAX_BLOCKS)
            NeuralGlobals[NEURAL_GATEPARTIAL_BASE + bid] = sum;
    }
}

// ============================================================================
// Pass 24: the reactive mask.
// ============================================================================
// The HLSL twin of RunReactiveMask in Renderer3D_NovaRenderer.glsl. Kept
// IDENTICAL in constants and reprojection, because this is the shader DX11,
// DX12 and Vulkan all consume (the .spv is built from the HLSL, not the GLSL).
// A mask that differs between twins is a feature that behaves differently on
// the backend that actually renders here (OpenGL) than on the other three.
//
// Same storage decision as the GLSL: img_Bloom_Temp.a, a spare channel, because
// the GL side may declare at most 8 image uniforms. IMG_STORE writes a whole
// float4, so the alpha is the whole payload and rgb is zeroed.
void RunReactiveMask()
{
    int2 pos = int2(g_dt.xy);
    int2 imgSize = int2(wl_trace_extent(int2(g_dims)));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    float2 uv = (float2(pos) + 0.5f) / float2(imgSize);
    float2 velocity = IMG_LOAD(img_Velocity, pos).xy;
    float2 prevUV = uv - velocity;

    // Same clamp as the GLSL twin and as pass 2: the accumulation buffer holds
    // the subregion, and outside it lives stale data from a previous
    // full-resolution frame.
    float2 fullRes = float2(imgSize);
    // float2(u_RenderScale, u_RenderScale), NOT float2(u_RenderScale). GLSL
    // accepts the single-argument broadcast constructor and DXC does not: it
    // rejects it with "too few elements in vector initialization (expected 2
    // elements, have 1)". This is the only place the two twins' constructor
    // syntax legitimately differs, and it cost a failed Vulkan build -- which
    // left the .spv stale, so the Vulkan path silently ran an older shader.
    float2 histUV = clamp(prevUV * u_RenderScale,
                          0.5f / fullRes,
                          float2(u_RenderScale, u_RenderScale) - 0.5f / fullRes);

    // Initialised to 0.0 ("trust the history"), not 1.0. See the GLSL twin's
    // comment: starting at 1.0 with max() terms forces the mask to 1.0
    // everywhere, which silently replaces the EASU with bilinear.
    float mask = 0.0f;

    bool validHistory = u_FrameIndex > 0
        && prevUV.x >= 0.0f && prevUV.x <= 1.0f
        && prevUV.y >= 0.0f && prevUV.y <= 1.0f;

    if (!validHistory)
    {
        mask = 1.0f;
    }
    else
    {
        float currentDepth = s_DepthBuffer.SampleLevel(s_Sampler, wl_buf_uv(uv), 0).r;
        float prevDepth = s_DepthBuffer.SampleLevel(s_Sampler, histUV, 0).r;

        float depthErr = abs(currentDepth - prevDepth);
        if (depthErr > 0.02f)
        {
            validHistory = false;
            mask = 1.0f;
        }
        else
        {
            mask = min(depthErr / 0.10f, 1.0f) * 0.5f;
        }

        // .yzw, not .gba -- the G-buffer store puts NDC depth in .r and the
        // packed normal in .yzw (see the GLSL twin's comment at .glsl:6421).
        float3 currentNormal = IMG_LOAD(img_TraceGBuffer, pos).yzw;
        float3 prevNormal = IMG_LOAD(img_TraceGBuffer,
            int2(clamp(histUV * fullRes, float2(0.5f, 0.5f), fullRes - float2(0.5f, 0.5f)))).yzw;
        float normalErr = length(currentNormal - prevNormal);
        mask = max(mask, clamp(normalErr / 0.35f, 0.0f, 1.0f) * 0.75f);

        float4 currentAlbedoHit = IMG_LOAD(img_AlbedoHit, pos);
        float4 prevAlbedoHit = IMG_LOAD(img_AlbedoHit,
            int2(clamp(histUV * fullRes, float2(0.5f, 0.5f), fullRes - float2(0.5f, 0.5f))));
        mask = max(mask, abs(currentAlbedoHit.a - prevAlbedoHit.a));
        mask = max(mask, clamp(length(currentAlbedoHit.rgb - prevAlbedoHit.rgb), 0.0f, 1.0f) * 0.5f);
    }

    float motionMag = length(velocity * float2(imgSize));
    mask = max(mask, clamp(motionMag / 8.0f, 0.0f, 1.0f) * 0.5f);

    // u_CameraMoved is deliberately NOT used -- see the GLSL twin's comment.
    // It is `CurrentViewProjection != PrevViewProjection`, and TAA jitter makes
    // those differ every frame, so it would force the mask to 1.0 everywhere.
    IMG_STORE(img_Bloom_Temp, pos, float4(0.0f, 0.0f, 0.0f, clamp(mask, 0.0f, 1.0f)));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupID : SV_GroupID)
{
    g_dt = dispatchThreadID;
    g_gid = groupID;
    g_tid = (dispatchThreadID.y % 8) * 8 + (dispatchThreadID.x % 8);
    img_Output.GetDimensions(g_dims.x, g_dims.y);
    if (any(dispatchThreadID.xy >= g_dims)) return;
    if ((u_PassID == 0 || u_PassID == 1 || u_PassID == 2 || u_PassID == 7 ||
         u_PassID == 24) &&
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
        case 12: RunTemporalMoments(); break;
        // Pass 13: the A-Trous wavelet filter. Reads the temporal moments
        // from pass 12 and applies a variance-driven edge-preserving filter.
        case 13: RunATrousFilter(); break;
        // Pass 24: the reactive mask. Writes img_Bloom_Temp.a, which every
        // reconstruction pass (8/15/16) reads. Must run after the velocity and
        // G-buffer are produced (passes 0/1) and before the composite consumes
        // the bloom temp.
        case 24: RunReactiveMask(); break;
        // Pass 3: ReSTIR DI generate (reservoir update)
        case 3: RunRestirGenerate(); break;
        // Pass 11: temporal combine writes the per-pixel reservoir only.
        case 11: RunRestirCombine(); break;
        // Pass 14: immutable-input spatial reuse and the sole ReSTIR shading write.
        case 14: RunRestirSpatial(); break;
        // Pass 15: Temporal Super-Resolution resolve (experimental).
        case 15: RunRCAS(); break;
        // Pass 16: copy TSR result to output.
        case 16: RunTSRCopy(); break;
        // The trained end-of-frame neural stage, mirroring the GLSL. Run AFTER
        // the god-ray pass (10) so the mean describes the finished frame, and
        // before anything that reads img_Output.
        //
        // 17 reduces, 19 folds the partials into the four frame means, 18
        // evaluates. The order is 17 -> 19 -> 18, which is NOT numerical order:
        // 18 is the consumer and 19 is the producer, and 17 must run first. The
        // GLSL switch lists them 17, 19, 18 for the same reason.
        case 17: RunNeuralReduce(); break;
        case 19: RunNeuralReduceFinal(); break;
        case 18: RunNeuralEval(); break;
    }
}
