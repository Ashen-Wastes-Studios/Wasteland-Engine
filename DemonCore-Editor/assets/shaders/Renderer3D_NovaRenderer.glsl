// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

#extension GL_EXT_gpu_shader5 : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, binding = 0) uniform image2D img_Output;
layout(rgba32f, binding = 1) uniform image2D img_Accumulation;
layout(rgba32f, binding = 2) uniform image2D img_Bloom;
layout(rgba32f, binding = 7) uniform image2D img_Bloom_Temp;
layout(rg16f, binding = 5) uniform image2D img_Velocity;
layout(rgba32f, binding = 6) uniform image2D img_TraceGBuffer; // r=NDC depth (1=sky), gba=packed normal
layout(binding = 3) uniform sampler2D s_Accumulation;
layout(binding = 4) uniform sampler2D s_DepthBuffer;
layout(binding = 8) uniform sampler2D s_Output;
layout(binding = 9) uniform sampler2D s_Bloom;
layout(binding = 2) uniform sampler2D s_Indirect;
layout(binding = 10) uniform sampler2D s_NormalBuffer;
layout(rgba8, binding = 3) uniform writeonly image2D img_MaterialPacked;
layout(binding = 11) uniform sampler2D s_InputAlbedo;
layout(binding = 12) uniform sampler2D s_PackedMaterialMap;
layout(rgba8, binding = 4) uniform image2D img_AlbedoHit; // RGB = albedo, A = hitMask

struct RayTracingInstance 
{
    mat4 InvTransform;      
    mat4 WorldTransform;    
    vec4 Albedo;            
    vec4 MaterialParams;    
    vec4 Min;
    vec4 Max;
    vec4 Emission;
    float MaxDistance;
    int LODLevel;
    int TextureID;
    int PackedMaterialMapID;
    vec4 TextureScale;
    vec4 DisplacementParams;
};

layout(std430, binding = 1) buffer SceneInstances
{
    RayTracingInstance Instances[];
};

struct BVHNode {
    vec4 MinBounds; // xyz = AABB min, w = leftChild (inner) or -(firstInstance+1) (leaf)
    vec4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
};

layout(std430, binding = 2) buffer BVHBuffer
{
    BVHNode BVHNodes[];
};

layout(std430, binding = 3) buffer LightListBuffer
{
    int LightIndices[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform mat4 u_PrevViewProjection; 
uniform int u_InstanceCount; 
uniform int u_FrameIndex;
uniform int u_SamplesPerPixel;
uniform int u_PassID;
uniform int u_DepthBuffer;
uniform vec2 u_Jitter;
uniform vec2 u_PrevJitter;
uniform float u_CameraMoved;
uniform mat4 u_ViewProjection;
uniform vec3 u_SkyBottomColor;
uniform vec3 u_SkyTopColor;
uniform float u_AccumulationAlpha;
uniform float u_NormalStrength;
uniform float u_RoughnessBias;
uniform float u_AOIntensity;
uniform int u_QualityLevel; // 0 = Low, 1 = Medium, 2 = High, 3 = Ultra
uniform int u_MaxBounces;
uniform int u_MaxLights;
uniform int u_IndirectRays;
uniform int u_StepSize;
uniform float u_RenderScale;
uniform int u_LightCount;
uniform sampler2D u_SceneTextures[32];

// Neural Rendering (OpenGL): tiny in-shader MLP radiance cache + texture detail.
// u_NeuralEnabled gates both; strengths blend 0 (classic path) -> 1 (full neural).
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;
uniform float u_NeuralLightStrength;
// Neural material blend. Defaults to 0 (classic PBR) until Renderer3D uploads it.
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

// Global atmospheric fog (distance haze everywhere)
uniform int u_GlobalFogEnabled;
uniform float u_GlobalFogDensity;
uniform float u_GlobalFogHeightFalloff;
uniform vec3 u_GlobalFogColor;
uniform float u_GlobalFogBaseHeight;
uniform float u_GlobalFogDistAtten;

// Screen-space god rays (radial blur from sun)
uniform int u_GodRaysEnabled;
uniform vec2 u_GodSunScreenPos;
uniform int u_GodSunValid;
uniform float u_GodRayIntensity;
uniform float u_GodRayDecay;
uniform int u_GodRaySamples;
uniform float u_GodRayDensity;
uniform vec3 u_GodRayColor;

// Analytic component lights (Renderer3D uploads; locations skipped if absent).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)
// u_ALightDir = travel direction (dir/spot) or rect normal facing scene (area).
uniform int u_ALightCount;
uniform vec3 u_ALightPos[8];
uniform vec3 u_ALightDir[8];
uniform vec3 u_ALightColor[8];
uniform vec4 u_ALightData[8];
uniform vec4 u_ALightData2[8];

float wl_softsign(float x) { return x / (1.0f + abs(x)); }

// Reduced-resolution tracing: passes 0/1/2/7 dispatch at ComputeWidth/Height
// (= full size * u_RenderScale) into the corner subregion, which RunComposite
// upscales. UV math must use the dispatched extent, not the full image.
// Identical to imageSize() at scale 1, so lower presets are unaffected.
ivec2 wl_trace_extent(ivec2 fullSize)
{
    ivec2 t = ivec2(vec2(fullSize) * u_RenderScale);
    return ivec2(max(t.x, 1f), max(t.y, 1f));
}

// Trace G-buffers (depth/normal) live full-size with valid data in the trace
// subregion; trace-space UVs span [0,1] across dispatched pixels, so scale
// them to buffer space when sampling. Identity at scale 1.
vec2 wl_buf_uv(vec2 traceUV) { return traceUV * u_RenderScale; }

float wl_hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return fract((p3.x + p3.y) * p3.z);
}

// Neural texture detail: sinusoidal UV encoding x normal -> 4-unit MLP -> ~1.0 multiplier.
vec3 wl_neural_tex_detail(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    vec4 feat = vec4(sin(s1 + n.x * 3.1f),
                     sin(s1 * 1.7f + n.y * 2.3f),
                     sin(s2 + n.z * 3.7f),
                     sin(s2 * 1.3f + s1 * 0.7f));
    float h0 = wl_softsign( 0.9f * feat.x - 0.7f * feat.y + 0.5f * feat.z - 0.3f * feat.w + 0.15f);
    float h1 = wl_softsign(-0.6f * feat.x + 0.8f * feat.y - 0.4f * feat.z + 0.6f * feat.w - 0.10f);
    float h2 = wl_softsign( 0.4f * feat.x + 0.5f * feat.y + 0.9f * feat.z + 0.2f * feat.w + 0.05f);
    float h3 = wl_softsign(-0.3f * feat.x - 0.5f * feat.y + 0.6f * feat.z - 0.8f * feat.w + 0.20f);
    float grain = wl_hash12(floor(uv * 64.0f)) - 0.5f;
    float d = 1.0f + 0.10f * h0 + 0.07f * h1 - 0.06f * h2 + 0.05f * h3 + 0.06f * grain;
    vec3 tint = vec3(1.0f + 0.03f * h1, 1.0f + 0.02f * h2, 1.0f - 0.03f * h0);
    return tint * d;
}

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain): the grain
// stays in the albedo tint where it sparkles, but must not imprint voxel
// cells into displaced height.
float wl_neural_micro(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    vec4 feat = vec4(sin(s1 + n.x * 3.1f),
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
vec3 wl_neural_indirect(vec3 wp, vec3 n, vec3 albedo, float rough, float metal, vec3 v)
{
    vec3 p = wp * 0.35f;
    vec4 f = vec4(sin(p.x * 2.1f + n.x * 2.0f),
                  sin(p.y * 1.7f + n.y * 2.0f),
                  sin(p.z * 2.3f + n.z * 2.0f),
                  sin((p.x + p.y + p.z) * 1.3f + rough * 3.0f));
    float h0 = wl_softsign( 0.8f * f.x - 0.6f * f.y + 0.4f * f.z + 0.5f * f.w + 0.10f);
    float h1 = wl_softsign(-0.5f * f.x + 0.9f * f.y - 0.3f * f.z + 0.4f * f.w - 0.05f);
    float h2 = wl_softsign( 0.3f * f.x + 0.4f * f.y + 0.8f * f.z - 0.6f * f.w + 0.00f);
    float h3 = wl_softsign(-0.4f * f.x - 0.3f * f.y + 0.5f * f.z + 0.7f * f.w + 0.15f);
    float up = n.y * 0.5f + 0.5f;
    vec3 sky = mix(u_SkyBottomColor, u_SkyTopColor, up);
    vec3 diffLobe = albedo * (1.0f - metal) * sky * (0.45f + 0.30f * h0 + 0.15f * h1);
    float ndv = clamp(dot(n, v), 0.0f, 1.0f);
    vec3 F0 = mix(vec3(0.04f), max(albedo, vec3(0.04f)), clamp(1.0f - rough, 0.0f, 1.0f) * 0.5f);
    vec3 spec = F0 * sky * (0.25f + 0.35f * h2) * (0.3f + 0.7f * ndv) * (1.0f - rough * 0.7f);
    float occ = 0.85f + 0.15f * h3;
    return (diffLobe + spec) * occ;
}

// Neural material: learned spatially-varying PBR params. Encodes UV + world
// position + normal into a 4-unit MLP whose outputs drive porosity (darken +
// roughen), patina hue shift, sparse metallic flakes, and a dielectric
// clearcoat-like F0 lift. Feeds the analytic GGX BRDF, so glints stay
// view/light-correct instead of painted on.
void wl_neural_material(vec2 uv, vec3 wp, vec3 n,
                        inout vec3 albedo, inout float metal, inout float rough,
                        inout vec3 f0Tint)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    vec3 q = wp * 0.9f;
    vec4 f = vec4(sin(s1 + q.x),
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
    albedo *= (vec3(1.0f) + vec3(-0.06f, 0.02f, 0.05f) * h1);
    // Metallic flakes: sparse cells go mirror-smooth, tinted by albedo
    float fl = flakeCell * clamp(h2 * 1.5f, 0.0f, 1.0f);
    metal = clamp(metal + fl * 0.9f, 0.0f, 1.0f);
    rough = clamp(mix(rough, 0.08f, fl), 0.03f, 1.0f);
    // Dielectric coating: warm F0 lift (clearcoat-like), suppressed on metals
    float coat = clamp(h3, 0.0f, 1.0f) * (1.0f - metal);
    f0Tint = vec3(1.0f) + vec3(1.2f, 0.9f, 0.6f) * coat;
}

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
float wl_vol_hash(vec3 p)
{
    p = fract(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract((p.x + p.y) * p.z);
}

float wl_vol_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0f - 2.0f * f);
    return mix(mix(mix(wl_vol_hash(i), wl_vol_hash(i + vec3(1.0f, 0.0f, 0.0f)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0f, 1.0f, 0.0f)), wl_vol_hash(i + vec3(1.0f, 1.0f, 0.0f)), u.x), u.y),
               mix(mix(wl_vol_hash(i + vec3(0.0f, 0.0f, 1.0f)), wl_vol_hash(i + vec3(1.0f, 0.0f, 1.0f)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0f, 1.0f, 1.0f)), wl_vol_hash(i + vec3(1.0f, 1.0f, 1.0f)), u.x), u.y), u.z);
}

float wl_vol_fbm(vec3 p)
{
    float v = 0.0f;
    float a = 0.5f;
    for (int o = 0; o < 4; o++)
    {
        v += a * wl_vol_noise(p);
        p = p * 2.03f + vec3(17.3f);
        a *= 0.5f;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
float wl_vol_fbm2(vec3 p)
{
    return (0.5f * wl_vol_noise(p) + 0.25f * wl_vol_noise(p * 2.03f + vec3(17.3f))) * 1.25f;
}

// Ultra-cheap single-hash noise: 1 hash call instead of 24.
float wl_vol_cheap(vec3 p)
{
    p = fract(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract((p.x + p.y) * p.z);
}

// Slab test: returns (entry, exit) distances, negative entry range when missed.
vec2 wl_box_range(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax)
{
    vec3 safe = rd + (vec3(1.0f) - step(vec3(0.00001f), abs(rd))) * 0.00001f;
    vec3 inv = 1.0f / safe;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    return 0.07957747f * (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosT, 0.001f), 1.5f);
}

// Front-to-back march through one fog box. Returns (scatteredLight, transmittance).
// sunVis gates the sun in-scatter (god-ray shafts): 1 = fully sunlit fog.
struct Ray { vec3 Origin; vec3 Direction; };

bool RayAABB(Ray r, vec3 invDir, vec3 minB, vec3 maxB) 
{
    vec3 t0 = (minB - r.Origin) * invDir;
    vec3 t1 = (maxB - r.Origin) * invDir;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);
    return tNear <= tFar && tFar > 0.0f;
}

uint seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000);

float hash() 
{
    seed = seed * 747796405u + 2891336453u;
    uint result = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    result = (result >> 22u) ^ result;
    return result / 4294967295.0f;
}

vec3 random_in_hemisphere(vec3 normal)
{
    float u = hash();
    float v = hash();
    float z = 2.0f * v - 1.0f;
    float r = sqrt(max(1.0f - z * z, 0.0f));
    float theta = 2.0f * 3.14159265359f * u;
    vec3 dir = vec3(r * cos(theta), r * sin(theta), z);
    return dot(dir, normal) > 0.0f ? dir : -dir;
}

const float PI = 3.14159265359f;

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

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    return F0 + (1.0f - F0) * t2 * t2 * t;
}

vec3 GetSkyColor(vec3 dir);

float HitCube(Ray localRay, out vec3 outNormal) 
{
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;

    vec3 tMin = (vec3(-0.5f) - localRay.Origin) * invDir;
    vec3 tMax = (vec3(0.5f) - localRay.Origin) * invDir;
    
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar  = min(min(t2.x, t2.y), t2.z);
    
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    
    if (tNear == t1.x)      outNormal = vec3(-sign(localRay.Direction.x), 0.0f, 0.0f);
    else if (tNear == t1.y) outNormal = vec3(0.0f, -sign(localRay.Direction.y), 0.0f);
    else                    outNormal = vec3(0.0f, 0.0f, -sign(localRay.Direction.z));

    return tNear;
}

float HitSphere(Ray localRay, float radius, out vec3 outNormal) 
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

bool TestInstanceOcclusion(Ray r, float maxDist, int instIdx) {
    RayTracingInstance inst = Instances[instIdx];

    vec3 diff = u_CameraPosition - inst.WorldTransform[3].xyz;
    if (dot(diff, diff) > inst.MaxDistance * inst.MaxDistance) return false;

    Ray localRay;
    localRay.Origin = (inst.InvTransform * vec4(r.Origin, 1.0f)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(r.Direction, 0.0f)).xyz);
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 localInvDir = 1.0f / safeDir;

    if (RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) {
        vec3 localNormal;
        float t = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
        if (t > 0.0f) {
            vec3 localHit = localRay.Origin + t * localRay.Direction;
            vec3 worldHit = (inst.WorldTransform * vec4(localHit, 1.0f)).xyz;
            if (distance(r.Origin, worldHit) < maxDist) return true;
        }
    }
    return false;
}

bool IsOccluded(Ray r, float maxDist, int skipIdx) {
    if (u_InstanceCount == 0) return false;

    vec3 safeDir = sign(r.Direction) * max(abs(r.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;
    int stack[32];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0) {
        int nodeIdx = stack[--stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        vec3 t0 = (node.MinBounds.xyz - r.Origin) * invDir;
        vec3 t1 = (node.MaxBounds.xyz - r.Origin) * invDir;
        vec3 tMin = min(t0, t1);
        vec3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        if (tNear > tFar || tFar < 0.0f || tNear > maxDist) continue;

        int encoded = int(node.MinBounds.w);
        if (encoded < 0) {
            int first = -encoded - 1;
            int count = int(node.MaxBounds.w);
            for (int i = first; i < first + count; i++) {
                // Never let a light shadow itself: shadow rays aim at the
                // light's center, so without this the light's own front
                // surface always occludes and kills all direct light.
                if (i == skipIdx) continue;
                if (TestInstanceOcclusion(r, maxDist, i)) return true;
            }
        } else {
            if (stackPtr < 30) {
                stack[stackPtr++] = int(node.MaxBounds.w);
                stack[stackPtr++] = encoded;
            }
        }
    }
    return false;
}


vec4 wl_march_fog_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                      vec3 tint, vec4 d1, vec4 d2, float dither, float sunVis)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float fogLen = max(t1 - t0, 0.0f);
    float fogPerfK = mix(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    int steps = int(clamp(d2.z * fogPerfK * clamp(fogLen / 16.0f, 0.15f, 1.0f), 2.0f, 32.0f));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / float(steps);
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9f, 0.9f));
    vec3 ambCol = mix(u_SkyBottomColor, u_SkyTopColor, 0.5f);
    vec3 sunLight = ambCol * 0.7f + u_SunLightColor * sunVis * (0.25f + phase * 2.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    vec3 acc = vec3(0.0f);
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
        vec3 p = ro + rd * t;
        float d = max(d1.x, 0.0f);
        float h = clamp((p.y - bmin.y) / hgt, 0.0f, 1.0f);
        d *= mix(1.0f, exp(-h * 4.0f), clamp(d2.y, 0.0f, 1.0f));
        if (d1.z > 0.001f && d > 0.001f)
        {
            vec3 np = p * max(d1.w, 0.001f) + vec3(u_Time * d2.x, 0.0f, u_Time * d2.x * 0.6f);
            float n = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            d *= (1.0f - d1.z * 0.5f) + d1.z * n;
        }
        float a = 1.0f - exp(-max(d, 0.0f) * dt);
        if (a > 0.001f)
        {
            vec3 sampleLight = sunLight;
            for (int li = lightStart; li < lightEnd; li++)
            {
                float atype = u_ALightData[li].x;
                if (atype < 0.5f) continue;
                vec3 lpos = u_ALightPos[li];
                vec3 lcol = u_ALightColor[li] * u_ALightData[li].y;
                float arange = max(u_ALightData[li].z, 0.0f);
                vec3 toL = lpos - p;
                float dL = length(toL);
                if (arange > 0.0f && dL >= arange) continue;
                vec3 L = toL / max(dL, 0.0001f);
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
                Ray lRay = Ray(p + L * 0.01f, L);
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
    return vec4(acc, trans);
}

// Front-to-back march through one cloud layer box. Returns (scatteredLight, transmittance).
vec4 wl_march_cloud_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                        vec3 tint, vec3 amb, vec4 d1, vec4 d2, vec4 d3, float dither)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0f);
    // Perf tier: same render-scale step scaling as wl_march_fog_vol.
    float cloudPerfK = mix(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    int steps = int(clamp(d2.w * cloudPerfK * clamp(clLen / 8.0f, 0.25f, 1.0f), 2.0f, 32.0f));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / float(steps);
    vec2 wnd = vec2(d2.y, d2.z);
    if (dot(wnd, wnd) < 0.00000001f)
        wnd = vec2(1.0f, 0.0f);
    wnd = normalize(wnd) * d2.x * u_Time;
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0f, 1.0f), 6.0f);
    vec3 sunCol = vec3(1.2f, 1.1f, 1.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    float nScale = max(d1.z, 0.001f);
    vec3 acc = vec3(0.0f);
    float trans = 1.0f;
    float t = t0 + dt * dither;
    for (int s = 0; s < 32; s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        vec3 q = p * nScale + vec3(wnd.x, 0.0f, wnd.y);
        float base = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0f, 1.0f);
        float d = smoothstep(1.0f - cov, 1.0f - cov + 0.35f, base);
        if (d > 0.001f && d1.w > 0.001f)
        {
            float det = wl_vol_noise(q * 3.7f + vec3(0.0f, u_Time * d2.x * 0.5f, 0.0f));
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
                vec3 sp = p + u_SunDirection * dt * 2.0f;
                float sd = wl_vol_noise(sp * nScale + vec3(wnd.x, 0.0f, wnd.y)) * max(d1.y, 0.0f);
                sh = exp(-sd * dt * 4.0f * clamp(d3.y, 0.0f, 1.0f));
            }
            vec3 scol = tint * (amb * (0.35f + 0.65f * sh) + sunCol * sh * 0.9f)
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
    return vec4(acc, trans);
}

// Applies all submitted cloud + fog volumes to a primary-ray sample.
// sunVis scales fog sun in-scatter (god rays); clouds keep self-shadowing.
vec3 wl_apply_volumetrics(vec3 ro, vec3 rd, float tHit, vec3 baseColor, float dither, float sunVis)
{
    vec3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5f)
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
            if (u_FogData2[i].w < 0.5f)
                continue;
            vec4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    return col;
}
// Global atmospheric fog: distance + height haze that applies everywhere
vec3 wl_apply_global_fog(vec3 col, float dist, vec3 viewDir, float dither)
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
    vec3 fc = u_GlobalFogColor;
    float luma = dot(col, vec3(0.2126f, 0.7152f, 0.0722f));
    vec3 target = mix(col, fc, 0.5f + 0.3f * dither);
    target = mix(col, target, smoothstep(0.0f, 0.85f, luma));
    return mix(col, target, ext);
}

// Screen-space god rays: radial blur from sun screen position
void RunGodRays()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    if (u_GodRaysEnabled != 1 || u_GodSunValid != 1 || u_GodRaySamples <= 0)
        return;

    vec2 sunPos = u_GodSunScreenPos;
    vec2 uv = (vec2(pos) + 0.5f) / vec2(fullSize);

    vec2 delta = (sunPos - uv) * vec2(fullSize.x / max(float(fullSize.y), 1.0f), 1.0f);
    float len = length(delta);
    if (len < 0.001f) return;

    vec2 stepDir = delta / len / float(u_GodRaySamples);
    vec2 currentUV = uv;

    vec3 accum = vec3(0.0f);
    float decay = u_GodRayDecay;
    float weight = u_GodRayDensity;
    float illumDecay = 1.0f;

    int samples = clamp(int(u_GodRaySamples), 1, 32);

    for (int i = 0; i < samples; i++)
    {
        currentUV += stepDir;
        if (currentUV.x < 0.0f || currentUV.x > 1.0f || currentUV.y < 0.0f || currentUV.y > 1.0f)
            break;

        vec3 s = texture(s_Output, currentUV).rgb;
        s *= illumDecay * weight;
        accum += s;
        illumDecay *= decay;
    }

    accum *= u_GodRayIntensity / float(samples);
    accum *= u_GodRayColor;

    vec3 existing = imageLoad(img_Output, pos).rgb;
    imageStore(img_Output, pos, vec4(existing + accum, 1.0f));
}



float Halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

vec2 GetJitter(int frameIndex, float cameraMoved) {
    int effectiveIndex = (cameraMoved > 0.5f) ? 0 : frameIndex;
    float x = Halton(effectiveIndex % 16 + 1, 2);
    float y = Halton(effectiveIndex % 16 + 1, 3);
    return vec2(x, y) - 0.5f;
}

const mat3 Gx = mat3(-1, 0, 1, -2, 0, 2, -1, 0, 1);
const mat3 Gy = mat3(-1, -2, -1, 0, 0, 0, 1, 2, 1);

float GetLuminance(vec3 col) {
    return dot(col, vec3(0.2126f, 0.7152f, 0.0722f));
}

vec3 RGB2YCoCg(vec3 rgb) {
    float Y  = 0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co = 0.5f  * rgb.r - 0.5f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return vec3(Y, Co, Cg);
}

vec3 YCoCg2RGB(vec3 ycocg) {
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return vec3(
        Y + Co - Cg,
        Y + Cg,
        Y - Co - Cg
    );
}

vec2 CalculateUV(vec3 localPos, RayTracingInstance inst) {
    if (int(inst.MaterialParams.z) == 0) {
        vec3 absLocal = abs(localPos);
        float maxAxis = max(max(absLocal.x, absLocal.y), absLocal.z);

        vec2 uv;
        vec2 uvScale;
        if (maxAxis == absLocal.x) {
            float signX = sign(localPos.x);
            uv = vec2(localPos.z * signX + 0.5f, localPos.y + 0.5f);
            uvScale = vec2(inst.TextureScale.z, inst.TextureScale.y);
        } else if (maxAxis == absLocal.y) {
            float signY = sign(localPos.y);
            uv = vec2(localPos.x * signY + 0.5f, localPos.z + 0.5f);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.w);
        } else {
            float signZ = sign(localPos.z);
            uv = vec2(localPos.x * signZ + 0.5f, localPos.y + 0.5f);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.y);
        }

        return uv * uvScale;
    } else {
        float phi = atan(localPos.z, localPos.x);
        float theta = acos(clamp(localPos.y, -1.0f, 1.0f));

        float u = phi / (2.0f * PI) + 0.5f;
        float v = theta / PI;

        return vec2(u, v) * inst.TextureScale.xy;
    }
}

void RunVisibilityAndVelocity() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(wl_trace_extent(imageSize(img_Output)));
    
    float depth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (depth >= 1.0f) { 
        imageStore(img_Velocity, pixelCoords, vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    vec4 clipPos = vec4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    worldPos /= worldPos.w;

    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    prevClipPos /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5f + 0.5f;

    // NOTE: intentionally the UNjittered grid UV. History is stored per-pixel
    // on that grid, so velocity = uv - prevUV lands the lookup on the texel
    // that saw this surface point. Biasing by the frame jitter (tempting,
    // tried) offsets the lookup by a per-frame-varying amount and smears.
    vec2 velocity = uv - prevUV;
    imageStore(img_Velocity, pixelCoords, vec4(velocity, 0.0f, 1.0f));
}

struct HitInfo {
    bool hit;
    float t;
    vec3 worldPos;
    vec3 normal;
    vec3 albedo;
    float metal;
    float rough;
    vec3 emission;
    float occlusion;
    vec3 f0Tint; // neural material specular tint, 1.0f = classic F0
};

void ComputeTangentFrame(vec3 localHitPos, vec3 localNormal, int shapeType, vec4 texScaleFull,
                         out vec3 T, out vec3 B, out vec2 uvScale)
{
    if (shapeType == 1)
    {
        T = vec3(-localNormal.z, 0.0f, localNormal.x);
        float lenT = length(T);
        if (lenT < 0.0001f)
            T = vec3(1.0f, 0.0f, 0.0f);
        else
            T /= lenT;

        B = cross(localNormal, T);

        float radius = max(length(localHitPos), 0.0001f);
        float radiusXZ = length(localHitPos.xz);
        radiusXZ = max(radiusXZ, 0.05f * radius);

        uvScale.x = (1.0f / (2.0f * PI * radiusXZ)) * texScaleFull.x;
        uvScale.y = (1.0f / (PI * radius)) * texScaleFull.y;
    }
    else
    {
        // Cube: mirror CalculateUV's per-face tiling so the POM march steps
        // in the same UV space the heights are sampled from. A flat texScale
        // here skews the march along one axis (e.g. floor top samples with
        // (x, w) = (50, 50) but marched with (x, y) = (50, 1)), producing
        // directional relief and occlusion banding.
        vec3 absN = abs(localNormal);

        if (absN.x > absN.y && absN.x > absN.z) {
            T = vec3(0.0f, 0.0f, sign(localNormal.x));
            B = vec3(0.0f, 1.0f, 0.0f);
            uvScale = vec2(texScaleFull.z, texScaleFull.y);
        } else if (absN.y > absN.x && absN.y > absN.z) {
            T = vec3(1.0f, 0.0f, 0.0f);
            B = vec3(0.0f, 0.0f, -sign(localNormal.y));
            uvScale = vec2(texScaleFull.x, texScaleFull.w);
        } else {
            T = vec3(-sign(localNormal.z), 0.0f, 0.0f);
            B = vec3(0.0f, 1.0f, 0.0f);
            uvScale = vec2(texScaleFull.x, texScaleFull.y);
        }
    }
}

// Relief height stack (Crimson-rock setup): texture macro-relief + procedural
// craggy breakup + strata banding + neural micro-surface. The linear march
// and binary refine sample the cheap macro height only; final/shadow/gradient
// taps use the full stack so detail rides on the macro silhouette.
float wl_pom_macro(int texID, vec2 tapUV)
{
    return GetLuminance(textureLod(u_SceneTextures[texID], tapUV, 0.0f).rgb);
}

float wl_pom_full(int texID, vec2 tapUV, vec3 wpBase, vec3 nRef,
                  float mesoAmp, float strataW, float neuralK)
{
    float h = wl_pom_macro(texID, tapUV);
    if (mesoAmp > 0.001f)
    {
        vec2 muv = tapUV * 6.0f;
        float n1 = wl_vol_noise(vec3(muv, 3.7f));
        float n2 = wl_vol_noise(vec3(muv * 2.3f + 5.0f, 9.1f));
        float st = sin(wpBase.y * 5.0f + n1 * 4.0f) * 0.5f + 0.5f;
        h += ((n1 - 0.5f) * 0.7f + (n2 - 0.5f) * 0.3f) * mesoAmp
           + (st - 0.5f) * mesoAmp * 0.8f * strataW;
    }
    if (neuralK > 0.001f)
        h += wl_neural_micro(tapUV, nRef) * 2.0f * neuralK;
    return clamp(h, 0.0f, 1.5f);
}

void ApplyPOM(RayTracingInstance inst, vec3 localHitPos, vec3 localNormal,
              vec3 localViewDir, inout vec2 uv, inout vec3 worldPos,
              inout vec3 worldNormal, out float selfOcclusion, out float dispPush,
              out float outHeight)
{
    selfOcclusion = 1.0f;
    dispPush = 0.0f;
    outHeight = -1.0f;

    if (u_QualityLevel <= 1)
        return;

    vec3 V = normalize(localViewDir);
    float distToCam = length(worldPos - u_CameraPosition);
    float cosView = abs(dot(V, localNormal));

    // Perf tier: pull POM range down when tracing below full resolution
    // (dynamic resolution on older GPUs) — distFade already zeroes detail at maxDist.
    float pomPerfK = smoothstep(0.6f, 0.9f, u_RenderScale);
    float maxDist = (u_QualityLevel == 2) ? 25.0f : mix(35.0f, 50.0f, pomPerfK);
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

    int shapeType = int(inst.MaterialParams.z);
    vec3 T, B;
    vec2 uvScale;
    ComputeTangentFrame(localHitPos, localNormal, shapeType, inst.TextureScale, T, B, uvScale);

    int texID = inst.TextureID;
    float eps = 1.0f / 256.0f;

    // Meso setup rides on Displacement Scale only (opt-in): existing scenes
    // with bump-only materials render pixel-identical to before.
    vec3 wpBase = worldPos;
    vec3 wNorm0 = normalize(worldNormal);
    float mesoAmp = 0.0f;
    float strataW = clamp(1.0f - abs(wNorm0.y), 0.15f, 1.0f);
    float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0f, 1.0f) : 0.0f;

    float maxDisplacement = max(dispScale, bumpStrength);
    float cosAngle = max(dot(V, localNormal), 0.05f);
    
    int baseLayers = (u_QualityLevel == 2) ? 6 : int(mix(8.0f, 16.0f, pomPerfK));
    int maxLayers  = (u_QualityLevel == 2) ? 16 : int(mix(20.0f, 32.0f, pomPerfK));
    int numLayers  = int(clamp(float(baseLayers) / cosAngle, float(baseLayers), float(maxLayers)));

    float vnDot = dot(V, localNormal);
    vec2 viewUVDir = vec2(dot(V, T), dot(V, B));
    vec2 uvOffsetPerHeight = -viewUVDir / max(abs(vnDot), 0.05f);

    float distFade = clamp(1.0f - (distToCam - (maxDist - 15.0f)) / 15.0f, 0.0f, 1.0f);
    dispScale *= distFade;
    bumpStrength *= distFade;
    mesoAmp = 0.25f * clamp(dispScale, 0.0f, 1.5f);

    float dHeight = 1.0f / float(numLayers);
    // March in geometric-depth units: the step must scale with dispScale
    // (world meters of relief), NOT max(disp, bump). Bump-only materials
    // carry relief in the gradient normal; giving them full-depth parallax
    // spans dozens of tiles (uvScale ~50 on a 50 m wall) and swims with
    // the view. (Raster Basic already marches on disp only.)
    vec2 dUV = (uvOffsetPerHeight * uvScale) * dHeight * dispScale;

    vec2 prevUV = uv;
    float prevLayerH = 1.0f;
    float currLayerH = 1.0f;
    float prevTexH = GetLuminance(textureLod(u_SceneTextures[texID], uv, marchLOD).rgb);
    vec2 currUV = uv;
    float currTexH = prevTexH;

    if (doMarch)
    for (int i = 0; i < numLayers; i++)
    {
        prevUV = currUV;
        prevLayerH = currLayerH;
        prevTexH = currTexH;

        currUV += dUV;
        currLayerH -= dHeight;
        currTexH = GetLuminance(textureLod(u_SceneTextures[texID], currUV, marchLOD).rgb);

        if (currTexH > currLayerH)
            break;
    }

    // Binary refine: crisp heightfield intersections for deep relief instead
    // of the linear-interp smear. Macro sampler only (cheap, silhouette-grade).
    int refineSteps = (u_QualityLevel >= 3) ? 5 : 3;
    if (doMarch)
    for (int b = 0; b < 5; b++)
    {
        if (b >= refineSteps)
            break;
        vec2 midUV = (prevUV + currUV) * 0.5f;
        float midH = wl_pom_macro(texID, midUV);
        float midL = (prevLayerH + currLayerH) * 0.5f;
        if (midH > midL)
        {
            currUV = midUV;
            currLayerH = midL;
            currTexH = midH;
        }
        else
        {
            prevUV = midUV;
            prevLayerH = midL;
            prevTexH = midH;
        }
    }

    // March skipped (grazing): pomUV stays on the center tap, weight is moot.
    vec2 pomUV = uv;
    float weight = 0.5f;
    if (doMarch)
    {
        float d1 = prevLayerH - prevTexH;
        float d2 = currTexH - currLayerH;
        weight = d1 / max(d1 + d2, 0.0001f);
        pomUV = mix(prevUV, currUV, weight);
    }

    vec2 uvDelta = (pomUV - uv) * marchK;
    float maxOffset = 0.15f;
    if (length(uvDelta) > maxOffset)
        uvDelta = normalize(uvDelta) * maxOffset;

    uv += uvDelta;

    float finalHeight = mix(prevTexH, currTexH, weight);
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
        float hU = wl_pom_full(texID, uv + vec2(eps, 0.0f), wpBase, wNorm0, mesoAmp, strataW, neuralK);
        float hV = wl_pom_full(texID, uv + vec2(0.0f, eps), wpBase, wNorm0, mesoAmp, strataW, neuralK);
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

        vec3 bumpGrad = (dh_dsT * T + dh_dsB * B) * bumpStrength;
        vec3 localPerturbedNormal = localNormal - bumpGrad;
        float bumpLen = length(bumpGrad);
        // Cap the perturbation: with large texture scales (e.g. a 50m floor
        // with uvScale 50) the UV-space gradient saturates into garbage
        // normals and near-black shading. Direction kept, magnitude capped.
        if (bumpLen > 1.0f)
            localPerturbedNormal = localNormal - bumpGrad / bumpLen;
        localPerturbedNormal = normalize(localPerturbedNormal);
        mat3 normalMatrix = transpose(mat3(inst.InvTransform));
        worldNormal = normalize(normalMatrix * localPerturbedNormal);

        float gradMag = length(vec2(dh_dsT, dh_dsB));
        float ridgeFactor = max(0.0f, finalHeight - 0.5f) * gradMag;
        selfOcclusion = clamp(1.0f - ridgeFactor * 0.04f * bumpStrength, 0.2f, 1.0f);
    }

    // Deep-crevice sun shadow: short heightfield march toward the sun so
    // crags cast onto each other. Displacement-gated (opt-in look change).
    if (dispScale > 0.001f)
    {
        vec3 localSun = normalize(mat3(inst.InvTransform) * u_SunDirection);
        vec2 sunUV = vec2(dot(localSun, T), dot(localSun, B));
        float sunZ = dot(localSun, localNormal);
        if (sunZ > 0.05f)
        {
            int shSteps = (u_QualityLevel >= 3) ? 6 : 4;
            vec2 shStep = (sunUV / sunZ) * (maxDisplacement * 0.05f / float(shSteps));
            if (length(shStep) > 0.1f)
                shStep = normalize(shStep) * 0.1f;
            float rayH = finalHeight + 0.02f;
            float rayRise = (1.0f - finalHeight + 0.05f) / float(shSteps);
            float occ = 1.0f;
            vec2 suv = uv;
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

    vec3 camDiff = u_CameraPosition - inst.WorldTransform[3].xyz;
    if (dot(camDiff, camDiff) > inst.MaxDistance * inst.MaxDistance) return;

    Ray localRay;
    localRay.Origin = (inst.InvTransform * vec4(ray.Origin, 1.0f)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(ray.Direction, 0.0f)).xyz);
    
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 localInvDir = 1.0f / safeDir;

    if (!RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) return;
    vec3 localNormal;
    float tLocal = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
    if (tLocal > 0.0f) {
        vec3 localHitPos = localRay.Origin + tLocal * localRay.Direction;
        vec3 worldHit = (inst.WorldTransform * vec4(localHitPos, 1.0f)).xyz;
        float tWorld = distance(ray.Origin, worldHit);
        if (tWorld < info.t) {
            info.t = tWorld;
            info.hit = true;
            info.worldPos = worldHit;
            info.normal = normalize((vec4(localNormal, 0.0f) * inst.InvTransform).xyz);

            vec2 uv = CalculateUV(localHitPos, inst);

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
                float pomPushAbs = abs(pomPush);
                if (pomPushAbs > 0.00001f)
                {
                    float pomDenom = dot(ray.Direction, info.normal);
                    if (abs(pomDenom) > 1e-4)
                    {
                        float tAnchor = dot(info.worldPos - ray.Origin, info.normal) / pomDenom;
                        if (tAnchor > 0.0f && tAnchor < info.t + pomPushAbs + 0.001f)
                        {
                            info.t = tAnchor;
                            info.worldPos = ray.Origin + ray.Direction * tAnchor;
                        }
                    }
                }
            }

            float dist = distance(worldHit, u_CameraPosition);
            // LOD from texel DENSITY (tiles per meter), not raw TextureScale:
            // scale grows with object size (50m floor tiles 50x) but density
            // is ~1/m like a unit cube. Raw scale pushed big surfaces to max
            // LOD meters from the camera, erasing all texture detail.
            // (Axis-aligned approx; rotation ignored.)
            vec3 wAxis = vec3(length(inst.WorldTransform[0].xyz),
                              length(inst.WorldTransform[1].xyz),
                              length(inst.WorldTransform[2].xyz));
            vec3 wSize = max((inst.Max.xyz - inst.Min.xyz) * wAxis, vec3(0.0001f));
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

            vec3 sampledAlbedo = vec3(1.0f);
            if (inst.TextureID >= 0 && inst.TextureID < 32)
                sampledAlbedo = textureLod(u_SceneTextures[inst.TextureID], uv, mipLevel).rgb;

            info.albedo = sampledAlbedo * inst.Albedo.rgb;
            // Neural texture detail: MLP-synthesized micro-surface (OpenGL neural path)
            if (u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001f)
                info.albedo *= mix(vec3(1.0f), wl_neural_tex_detail(uv, info.normal), clamp(u_NeuralTexStrength, 0.0f, 1.0f));
            info.metal = clamp(inst.MaterialParams.x, 0.0f, 1.0f);
            info.rough = inst.MaterialParams.y > 0.0f ? inst.MaterialParams.y : 0.5f;
            info.f0Tint = vec3(1.0f);
            // Neural material: learned PBR params feed the GGX BRDF (OpenGL neural path)
            if (u_NeuralEnabled == 1 && u_NeuralMatStrength > 0.001f)
            {
                float nmk = clamp(u_NeuralMatStrength, 0.0f, 1.0f);
                vec3 nma = info.albedo;
                float nmm = info.metal;
                float nmr = info.rough;
                vec3 nmf = vec3(1.0f);
                wl_neural_material(uv, info.worldPos, info.normal, nma, nmm, nmr, nmf);
                info.albedo = mix(info.albedo, nma, nmk);
                info.metal = mix(info.metal, nmm, nmk);
                info.rough = mix(info.rough, nmr, nmk);
                info.f0Tint = mix(vec3(1.0f), nmf, nmk);
            }
            // Relief weathering: dust settles in pits, sun bleaches ridge tops.
            // Displacement-gated: only set when the relief march ran with push.
            if (pomH >= 0.0f)
            {
                float cav = smoothstep(0.45f, 0.05f, pomH);
                float ridge = smoothstep(0.55f, 0.95f, pomH);
                info.albedo *= (1.0f - 0.30f * cav);
                info.albedo = mix(info.albedo, info.albedo * vec3(1.10f, 1.03f, 0.92f), ridge * 0.65f);
                info.occlusion *= (1.0f - 0.35f * cav);
            }
            info.emission = inst.Emission.xyz * inst.Emission.w;
        }
    }
}

HitInfo TraceScene(Ray ray) {
    HitInfo info;
    info.hit = false;
    info.t = 1e20;
    info.occlusion = 1.0f;
    info.f0Tint = vec3(1.0f);

    if (u_InstanceCount == 0) return info;

    bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01f;
    vec3 safeDir = sign(ray.Direction) * max(abs(ray.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;

    int stack[32];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0) {
        int nodeIdx = stack[--stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        vec3 t0 = (node.MinBounds.xyz - ray.Origin) * invDir;
        vec3 t1 = (node.MaxBounds.xyz - ray.Origin) * invDir;
        vec3 tMin = min(t0, t1);
        vec3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        if (tNear > tFar || tFar < 0.0f || tNear > info.t) continue;

        int encoded = int(node.MinBounds.w);
        if (encoded < 0) {
            int first = -encoded - 1;
            int count = int(node.MaxBounds.w);
            for (int i = first; i < first + count; i++) {
                TestInstanceHit(ray, i, isPrimaryRay, info);
            }
        } else {
            if (stackPtr < 30) {
                stack[stackPtr++] = int(node.MaxBounds.w);
                stack[stackPtr++] = encoded;
            }
        }
    }
    return info;
}

vec3 ComputeDirectLighting(HitInfo h, vec3 V, uint lightSeed, bool skipEnv) {
    vec3 F0 = mix(vec3(0.04f), max(h.albedo, vec3(0.04f)), h.metal) * h.f0Tint;
    vec3 diffuseColor = h.albedo * (1.0f - h.metal);
    vec3 directLight = vec3(0.0f);

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
        lightStart = int(lightSeed % uint(lightsToSample));
        lightStride = lightsToSample / 2;
        if (lightStride < 1) lightStride = 1;
    }
    float lightScale = float(lightsToSample) / float(max(evalCount, 1f));

    for (int k = 0; k < evalCount; k++) {
        int li = (lightStart + k * lightStride) % lightsToSample;
        int i = LightIndices[li];
        RayTracingInstance light = Instances[i];
        vec3 toLight = light.WorldTransform[3].xyz - h.worldPos;
        float distToLight = length(toLight);
        vec3 dirToLight = toLight / distToLight;
        Ray shadowRay = Ray(h.worldPos + h.normal * 0.001f, dirToLight);
        // Pass the light's own instance index so it can't self-shadow
        // (shadow rays target the light center, inside its geometry).
        if (!IsOccluded(shadowRay, distToLight, i)) {
            float NdotL = max(dot(h.normal, dirToLight), 0.0f);
            if (NdotL <= 0.0f) continue;
            vec3 H = normalize(V + dirToLight);
            vec3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
            float D = DistributionGGX(max(dot(h.normal, H), 0.0f), h.rough);
            float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
            float NdotV = max(dot(h.normal, V), 0.0f);
            vec3 brdf = (vec3(1.0f) - F) * (1.0f - h.metal) * h.albedo / PI + (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);
            vec3 contrib = brdf * light.Emission.xyz * light.Emission.w * NdotL;

            float brightness = dot(contrib, vec3(0.2126f, 0.7152f, 0.0722f));
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
        vec3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0f);
        float afall = max(u_ALightData[a].w, 0.5f);
        vec3 L;
        float att = 1.0f;
        float shadowLen = 100000.0f;
        if (atype < 0.5f) {
            L = -u_ALightDir[a];
        } else {
            vec3 toL = u_ALightPos[a] - h.worldPos;
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
        Ray aShadowRay = Ray(h.worldPos + h.normal * 0.001f, L);
        if (!IsOccluded(aShadowRay, shadowLen, -1)) {
            vec3 aH = normalize(V + L);
            vec3 aF = FresnelSchlick(max(dot(aH, V), 0.0f), F0);
            float aD = DistributionGGX(max(dot(h.normal, aH), 0.0f), h.rough);
            float aG = GeometrySmith(h.normal, V, L, h.rough);
            float aNdotV = max(dot(h.normal, V), 0.0f);
            vec3 abrdf = (vec3(1.0f) - aF) * (1.0f - h.metal) * h.albedo / PI + (aD * aG * aF) / max(4.0f * aNdotV * aNdotL, 0.001f);
            vec3 acontrib = abrdf * lcol * att * aNdotL;

            float abright = dot(acontrib, vec3(0.2126f, 0.7152f, 0.0722f));
            if (abright > 10.0f) acontrib *= (10.0f / abright);
            directLight += acontrib;
        }
    }

    float skyT = 0.5f * (h.normal.y + 1.0f);
    vec3 skyAmbient = mix(u_SkyBottomColor, u_SkyTopColor, skyT) * 0.15f;
    directLight += diffuseColor * skyAmbient * h.occlusion;

    // Analytic sky env approx for metals. Skipped when the caller traces a
    // true mirror ray instead (Low-preset metals) so the sky isn't counted twice.
    if (h.metal > 0.0f && !skipEnv) {
        vec3 R = reflect(-V, h.normal);
        vec3 specSky = GetSkyColor(R);
        vec3 diffSky = skyAmbient / 0.15f; 
        vec3 envColor = mix(specSky, diffSky, h.rough * h.rough);
        directLight += F0 * envColor * h.metal * h.occlusion;
    }

    return directLight;
}

// God-ray sun visibility: one shadow tap toward the sun for fogged pixels.
// Fog in shadowed regions keeps ambient but loses sun in-scatter, so shafts
// appear where geometry breaks the sunlight. Skipped (returns 1) when the ray
// enters no fog volume. Tested at the surface end for hits, at the camera
// for sky rays — a single-tap approximation, noted as such.
float wl_godray_visibility(vec3 ro, vec3 rd, float tHit)
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
        vec2 r = wl_box_range(ro, rd, u_FogMin[i], u_FogMax[i]);
        if (min(r.y, tHit) > max(r.x, 0.0f))
        {
            hitsFog = true;
            break;
        }
    }
    if (!hitsFog)
        return 1.0f;
    vec3 tapPoint = (tHit < 500.0f) ? (ro + rd * tHit) : ro;
    Ray sunRay = Ray(tapPoint + u_SunDirection * 0.05f, u_SunDirection);
    if (IsOccluded(sunRay, 100000.0f, -1))
        return 0.12f;
    return 1.0f;
}

vec3 GetSkyColor(vec3 dir) {
    float t = 0.5f * (normalize(dir).y + 1.0f);
    return mix(u_SkyBottomColor, u_SkyTopColor, t);
}

void RunTraceAndDenoise()
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    ivec2 traceSize = wl_trace_extent(imgSize);
    if (pixelCoords.x >= traceSize.x || pixelCoords.y >= traceSize.y) return;

    seed = uint(pixelCoords.y * 1024 + pixelCoords.x) + uint(u_FrameIndex * 1000);
    vec2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
    vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(traceSize)) * 2.0f - 1.0f;
    
    Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0f, 1.0f)).xyz));
    HitInfo primary = TraceScene(primaryRay);

    imageStore(img_AlbedoHit, pixelCoords, vec4(primary.hit ? primary.albedo : vec3(1.0f), primary.hit ? 1.0f : 0.0f));

    // Trace G-buffer: NDC depth (far on miss) + packed world normal for the
    // velocity / temporal / bilateral passes. Full-size buffer, subregion data.
    // (RunResolve was removed; binding 6 was dead. Image units are capped at
    // 0-7 on this GL driver, so depth+normal share one RGBA32F image.)
    if (!primary.hit) {
        imageStore(img_TraceGBuffer, pixelCoords, vec4(1.0f, 0.5f, 0.5f, 1.0f));
    } else {
        vec4 tclip = u_ViewProjection * vec4(primary.worldPos, 1.0f);
        float tndc = tclip.z / max(tclip.w, 1e-6f);
        imageStore(img_TraceGBuffer, pixelCoords, vec4(clamp(tndc * 0.5f + 0.5f, 0.0f, 1.0f), primary.normal * 0.5f + 0.5f));
    }

    if (!primary.hit) {
        vec3 skyCol = GetSkyColor(primaryRay.Direction);
        // Static per-pixel dither (NOT frame-animated): accumulation converges
        // a stable field in a few frames, while a cycling dither keeps every
        // frame different and the image shimmers forever, especially upsized.
        float skyDith = wl_hash12(vec2(pixelCoords));
        // Skip the volume march entirely when the scene submits no volumes
        // (the per-volume loops would break at count 0 anyway).
        if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
        {
            float skySunVis = wl_godray_visibility(primaryRay.Origin, primaryRay.Direction, 600.0f);
            skyCol = wl_apply_volumetrics(primaryRay.Origin, primaryRay.Direction, 600.0f, skyCol, skyDith, skySunVis);
        }
        imageStore(img_Bloom, pixelCoords, vec4(skyCol, 1.0f));
        return;
    }

    HitInfo tileHit = primary;
    Ray lightRay = primaryRay;

    vec3 directLight = vec3(0.0f);
    vec3 indirectLight = vec3(0.0f);

    if (tileHit.hit) {
        vec3 V = normalize(-lightRay.Direction);
        uint lightSeed = uint(pixelCoords.x * 73 + pixelCoords.y * 149 + u_FrameIndex * 2);
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
            vec3 diffuseColor = tileHit.albedo * (1.0f - tileHit.metal);
            vec3 F0 = mix(vec3(0.04f), max(tileHit.albedo, vec3(0.04f)), tileHit.metal) * tileHit.f0Tint;
            vec3 indirectReflectance = mix(diffuseColor, F0, tileHit.metal);

            for (int r = 0; r < maxIndirectRays; r++) {
                seed = uint(pixelCoords.y * 1024 + pixelCoords.x) + uint(u_FrameIndex * 1000) + uint((r + 1) * 7919);
                
                vec3 indirectDir;
                if (hash() < tileHit.metal) {
                    vec3 specDir = reflect(-V, tileHit.normal);
                    vec3 randHem = random_in_hemisphere(tileHit.normal);
                    indirectDir = normalize(mix(specDir, randHem, tileHit.rough * tileHit.rough));
                } else {
                    indirectDir = random_in_hemisphere(tileHit.normal);
                }

                Ray indirectRay = Ray(tileHit.worldPos + tileHit.normal * 0.001f, indirectDir);
                HitInfo indirect = TraceScene(indirectRay);

                if (indirect.hit) {
                    vec3 indirectV = normalize(-indirectDir);
                    vec3 bouncedLight = ComputeDirectLighting(indirect, indirectV, lightSeed + 1u, false);
                    indirectLight += indirectReflectance * bouncedLight;
                } else {
                    indirectLight += indirectReflectance * GetSkyColor(indirectDir);
                }
            }
            indirectLight /= float(maxIndirectRays);
            indirectLight *= tileHit.occlusion;
            // Neural radiance cache blend: learned GI steadies the 1-ray estimate (OpenGL neural path).
            // Weighted down on metals so the traced specular reflection dominates.
            if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
                vec3 neuralGI = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V);
                float neuralK = clamp(u_NeuralLightStrength, 0.0f, 1.0f) * 0.65f * (1.0f - tileHit.metal * 0.8f);
                indirectLight = mix(indirectLight, neuralGI * tileHit.occlusion, neuralK);
            }
        } else if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
            // No traced bounces at this quality level: neural cache IS the GI (e.g. Low preset)
            indirectLight = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V) * tileHit.occlusion;
        }

        // Sharp scene reflection for metals with no traced bounce (Low preset):
        // one roughness-jittered mirror ray, Fresnel-tinted. Shadow-correct
        // because the reflected hit is shaded with full shadowed direct light.
        if (doMirror) {
            seed = uint(pixelCoords.y * 1024 + pixelCoords.x) + uint(u_FrameIndex * 1000) + 40543u;
            vec3 R = reflect(-V, tileHit.normal);
            vec3 mirrorDir = normalize(mix(R, random_in_hemisphere(tileHit.normal), tileHit.rough * tileHit.rough));
            Ray mirrorRay = Ray(tileHit.worldPos + tileHit.normal * 0.002f, mirrorDir);
            HitInfo mirrorHit = TraceScene(mirrorRay);
            vec3 mirrorColor;
            if (mirrorHit.hit) {
                vec3 mirrorV = normalize(-mirrorRay.Direction);
                mirrorColor = ComputeDirectLighting(mirrorHit, mirrorV, lightSeed + 7u, false);
            } else {
                mirrorColor = GetSkyColor(mirrorRay.Direction);
            }
            vec3 mF0 = mix(vec3(0.04f), max(tileHit.albedo, vec3(0.04f)), tileHit.metal) * tileHit.f0Tint;
            vec3 mF = FresnelSchlick(clamp(dot(tileHit.normal, V), 0.0f, 1.0f), mF0);
            directLight += mF * mirrorColor * tileHit.metal;
        }
    } else {
        directLight = GetSkyColor(lightRay.Direction);
    }

    directLight = clamp(directLight, vec3(0.0f), vec3(10.0f));
    indirectLight = clamp(indirectLight, vec3(0.0f), vec3(5.0f));

    // ComputeDirectLighting already folds albedo into the BRDF (diffuse/spec/
    // ambient/emission), and indirectLight already carries tile reflectance -
    // do NOT multiply by albedo again (albedo^2 pushed textured floors to black).
    vec3 finalPixelColor = directLight + indirectLight;
    // Volumetric atmosphere over the primary ray (clouds, then fog).
    // Skipped when the scene submits no volumes (see sky path above).
    if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
    {
        float volDist = distance(u_CameraPosition, tileHit.worldPos);
        float volDith = wl_hash12(vec2(pixelCoords) + 7.3f);
        float volSunVis = wl_godray_visibility(lightRay.Origin, lightRay.Direction, volDist);
        finalPixelColor = wl_apply_volumetrics(lightRay.Origin, lightRay.Direction, volDist, finalPixelColor, volDith, volSunVis);
    }
    
    // Global atmospheric fog: distance haze for all surfaces
    {
        float surfDist = distance(u_CameraPosition, tileHit.worldPos);
        float gfDith = wl_hash12(vec2(pixelCoords) + 3.7f);
        finalPixelColor = wl_apply_global_fog(finalPixelColor, surfDist, lightRay.Direction, gfDith);
    }
    
    imageStore(img_Bloom, pixelCoords, vec4(finalPixelColor, 1.0f));
}

void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Accumulation));
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);

    vec3 currentLight = imageLoad(img_Bloom, pixelCoords).rgb;
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 prevUV = uv - velocity;
    // Accumulation + depth buffers hold the subregion; map history UVs to it.
    vec2 histUV = prevUV * u_RenderScale;
    // Keep bilinear taps inside the valid subregion: outside it lives stale
    // data (e.g. from full-res frames), which would streak the frame edges.
    vec2 fullRes = vec2(imageSize(img_Accumulation));
    histUV = min(histUV, vec2(u_RenderScale) - 0.5f / fullRes);

    bool validHistory = prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;
    float motionMag = length(velocity * vec2(imgSize));

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = texture(s_DepthBuffer, histUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    vec3 historyLight = texture(s_Accumulation, histUV).rgb;

    if (validHistory) {
        if (u_QualityLevel >= 3) {
            vec3 m1 = vec3(0.0f);
            vec3 m2 = vec3(0.0f);
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    ivec2 sp = clamp(pixelCoords + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                    vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                    m1 += s;
                    m2 += s * s;
                }
            }
            vec3 mean = m1 / 9.0f;
            vec3 stddev = sqrt(max(vec3(0.0f), m2 / 9.0f - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            vec3 minCol = mean - 1.25f * stddev;
            vec3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        } else {
            // History clamp at EVERY tier (not just High/Ultra): without it,
            // per-frame variations (trace jitter cycle, shadow-seed rotation,
            // mirror-ray randomness on metals) smear unboundedly through the
            // blend and Low/Medium never resolve sharp. Pulls history toward
            // the current frame's local neighborhood; slight noise cost.
            ivec2 offsets[5] = ivec2[](ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
            vec3 m1 = vec3(0.0f);
            vec3 m2 = vec3(0.0f);
            for (int i = 0; i < 5; i++) {
                ivec2 sp = clamp(pixelCoords + offsets[i], ivec2(0), imgSize - ivec2(1));
                vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
            vec3 mean = m1 / 5.0f;
            vec3 stddev = sqrt(max(vec3(0.0f), m2 / 5.0f - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            vec3 minCol = mean - 1.25f * stddev;
            vec3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        }
    }

    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? mix(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;

    vec3 result = mix(historyLight, currentLight, alpha);
    imageStore(img_Accumulation, pixelCoords, vec4(result, 1.0f));
}

void RunBloomThreshold() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = imageLoad(img_Output, pos);
    
    float threshold = 0.5f;
    float knee = 0.1f; 
    
    float brightness = dot(color.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
    float soft = smoothstep(threshold - knee, threshold + knee, brightness);
    
    imageStore(img_Bloom, pos, color * soft);
}

void RunBloomBlurHorizontal() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    float weights[5] = float[](0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f);
    vec4 sum = imageLoad(img_Bloom, pos) * weights[0];
    
    for(int i = 1; i < 5; i++) {
        sum += imageLoad(img_Bloom, pos + ivec2(i, 0)) * weights[i];
        sum += imageLoad(img_Bloom, pos - ivec2(i, 0)) * weights[i];
    }
    imageStore(img_Bloom_Temp, pos, sum);
}

void RunBloomBlurVertical()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    float weights[5] = float[](0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f);
    vec4 sum = imageLoad(img_Bloom_Temp, pos) * weights[0];

    for(int i = 1; i < 5; i++) {
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, i)) * weights[i];
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, -i)) * weights[i];
    }

    vec3 existing = imageLoad(img_Output, pos).rgb;
    vec3 finalColor = existing + sum.rgb;
    imageStore(img_Output, pos, vec4(finalColor, 1.0f));
}

void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    // img_Accumulation / img_Bloom_Temp already hold FINAL shaded color
    // (albedo * direct + indirect, see RunTraceAndDenoise), so it must NOT
    // be multiplied by albedo again here. The old `denoised * albedo` applied
    // albedo twice (albedo^2 on direct, albedo-scaled indirect) and pushed
    // textured surfaces (e.g. the floor) toward black.
    vec3 denoised;

    if (u_RenderScale < 1.0f) {
        ivec2 srcSize = ivec2(vec2(fullSize) * u_RenderScale);
        if (srcSize.x < 1) srcSize.x = 1;
        if (srcSize.y < 1) srcSize.y = 1;

        vec2 srcCoord = (vec2(pos) + 0.5f) * u_RenderScale;
        vec2 srcPixel = srcCoord - vec2(0.5f);
        ivec2 s0 = clamp(ivec2(floor(srcPixel)), ivec2(0), srcSize - ivec2(1));
        ivec2 s1 = clamp(s0 + ivec2(1), ivec2(0), srcSize - ivec2(1));
        // Clamp weights: at the frame border srcPixel can go slightly negative
        // and unclamped extrapolation rings bright/dark edges.
        vec2 f = clamp(srcPixel - vec2(s0), vec2(0.0f), vec2(1.0f));

        vec3 c00 = imageLoad(img_Accumulation, s0).rgb;
        vec3 c10 = imageLoad(img_Accumulation, ivec2(s1.x, s0.y)).rgb;
        vec3 c01 = imageLoad(img_Accumulation, ivec2(s0.x, s1.y)).rgb;
        vec3 c11 = imageLoad(img_Accumulation, s1).rgb;
        denoised = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {
        denoised = (u_QualityLevel >= 3) ? imageLoad(img_Bloom_Temp, pos).rgb : imageLoad(img_Accumulation, pos).rgb;
    }

    vec3 combined = denoised;

    vec3 mapped = combined / (combined + vec3(1.0f));
    vec3 finalColor = pow(mapped, vec3(1.0f / 2.2f));

    imageStore(img_Output, pos, vec4(finalColor, 1.0f));

    // Bloom threshold must run at ANY scale: passes 4/5 blur img_Bloom at full
    // res, so leaving stale trace-subregion data in it (by skipping this when
    // scaled) smears a ghost copy of the frame over the output.
    if (u_QualityLevel >= 1) {
        float brightness = dot(combined, vec3(0.2126f, 0.7152f, 0.0722f));
        float threshold = 0.8f;
        float knee = 0.15f;
        float soft = smoothstep(threshold - knee, threshold + knee, brightness);
        imageStore(img_Bloom, pos, vec4(combined * soft, 1.0f));
    }
}

void RunBilateralBlur() {
    if (u_QualityLevel <= 1) return;

    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Bloom_Temp));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    vec3 centerColor = imageLoad(img_Accumulation, pos).rgb;
    
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        imageStore(img_Bloom_Temp, pos, vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    vec2 uv = (vec2(pos) + 0.5f) / vec2(imgSize);
    float centerDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (centerDepth >= 1.0f) { 
        imageStore(img_Bloom_Temp, pos, vec4(centerColor, 1.0f));
        return;
    }

    vec3 centerNormal = texture(s_NormalBuffer, wl_buf_uv(uv)).rgb * 2.0f - 1.0f;
    float centerLuma = GetLuminance(centerColor);

    int step = int(max(1f, u_StepSize));
    vec3 totalColor = vec3(0.0f);
    float totalWeight = 0.0f;

    if (u_QualityLevel == 2) {
        ivec2 offsets[9] = ivec2[](
            ivec2(0, 0), 
            ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
            ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2)
        );
        for (int i = 0; i < 9; i++) {
            ivec2 samplePos = clamp(pos + offsets[i] * step, ivec2(0), imgSize - ivec2(1));

            vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
            vec2 sampleUV = (vec2(samplePos) + 0.5f) / vec2(imgSize);

            float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).rgb * 2.0f - 1.0f;
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
                ivec2 sp = clamp(pos + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                float l = GetLuminance(imageLoad(img_Accumulation, sp).rgb);
                lumaSum += l;
                lumaSqSum += l * l;
            }
        }
        float lumaMean = lumaSum / 9.0f;
        float lumaVar = sqrt(max(0.0f, (lumaSqSum / 9.0f) - (lumaMean * lumaMean)));

        const float kernel[3] = float[](0.375f, 0.25f, 0.0625f);

        for (int x = -2; x <= 2; x++) {
            for (int y = -2; y <= 2; y++) {
                ivec2 samplePos = clamp(pos + ivec2(x, y) * step, ivec2(0), imgSize - ivec2(1));

                vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
                vec2 sampleUV = (vec2(samplePos) + 0.5f) / vec2(imgSize);

                float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
                vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).rgb * 2.0f - 1.0f;
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

    vec3 finalFiltered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : centerColor;
    imageStore(img_Bloom_Temp, pos, vec4(finalFiltered, 1.0f));
}

void GenerateMaterialMaps() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(img_MaterialPacked);
    
    float I[3][3];
    for(int i = -1; i <= 1; i++) {
        for(int j = -1; j <= 1; j++) {
            ivec2 samplePos = clamp(pos + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec2 uv = (vec2(samplePos) + 0.5f) / vec2(size);
            I[i+1][j+1] = GetLuminance(texture(s_InputAlbedo, uv).rgb);
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
    vec3 normal = normalize(vec3(-resX * normalStrength, -resY * normalStrength, 1.0f));

    vec2 packedNormal = normal.xy * 0.5f + 0.5f;
    
    float ao = 1.0f - (length(vec2(resX, resY)) * 5.0f * u_AOIntensity);
    float rough = clamp((1.0f - I[1][1]) + u_RoughnessBias, 0.0f, 1.0f);
    
    vec4 packedData = vec4(packedNormal.x, packedNormal.y, rough, ao);
    
    imageStore(img_MaterialPacked, pos, packedData);
}

void RunUpscale() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    ivec2 srcSize = ivec2(vec2(fullSize) * u_RenderScale);
    if (srcSize.x < 1) srcSize.x = 1;
    if (srcSize.y < 1) srcSize.y = 1;

    vec2 srcCoord = (vec2(pos) + 0.5f) * u_RenderScale;
    vec2 srcPixel = srcCoord - vec2(0.5f);
    ivec2 src0 = ivec2(floor(srcPixel));
    ivec2 src1 = src0 + ivec2(1);
    vec2 frac_ = srcPixel - vec2(src0);

    src0 = clamp(src0, ivec2(0), srcSize - ivec2(1));
    src1 = clamp(src1, ivec2(0), srcSize - ivec2(1));

    vec3 c00 = imageLoad(img_Bloom_Temp, src0).rgb;
    vec3 c10 = imageLoad(img_Bloom_Temp, ivec2(src1.x, src0.y)).rgb;
    vec3 c01 = imageLoad(img_Bloom_Temp, ivec2(src0.x, src1.y)).rgb;
    vec3 c11 = imageLoad(img_Bloom_Temp, src1).rgb;

    vec3 c0 = mix(c00, c10, frac_.x);
    vec3 c1 = mix(c01, c11, frac_.x);
    vec3 color = mix(c0, c1, frac_.y);

    imageStore(img_Output, pos, vec4(color, 1.0f));
}

void main()
{
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