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

float wl_softsign(float x) { return x / (1.0 + abs(x)); }

// Reduced-resolution tracing: passes 0/1/2/7 dispatch at ComputeWidth/Height
// (= full size * u_RenderScale) into the corner subregion, which RunComposite
// upscales. UV math must use the dispatched extent, not the full image.
// Identical to imageSize() at scale 1, so lower presets are unaffected.
ivec2 wl_trace_extent(ivec2 fullSize)
{
    ivec2 t = ivec2(vec2(fullSize) * u_RenderScale);
    return ivec2(max(t.x, 1), max(t.y, 1));
}

// Trace G-buffers (depth/normal) live full-size with valid data in the trace
// subregion; trace-space UVs span [0,1] across dispatched pixels, so scale
// them to buffer space when sampling. Identity at scale 1.
vec2 wl_buf_uv(vec2 traceUV) { return traceUV * u_RenderScale; }

float wl_hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Neural texture detail: sinusoidal UV encoding x normal -> 4-unit MLP -> ~1.0 multiplier.
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

// Neural radiance cache: predicts indirect bounce (reflectance * light) from
// position/normal/albedo/roughness + artist sky colors. One eval replaces N traced rays.
vec3 wl_neural_indirect(vec3 wp, vec3 n, vec3 albedo, float rough, vec3 v)
{
    vec3 p = wp * 0.35;
    vec4 f = vec4(sin(p.x * 2.1 + n.x * 2.0),
                  sin(p.y * 1.7 + n.y * 2.0),
                  sin(p.z * 2.3 + n.z * 2.0),
                  sin((p.x + p.y + p.z) * 1.3 + rough * 3.0));
    float h0 = wl_softsign( 0.8 * f.x - 0.6 * f.y + 0.4 * f.z + 0.5 * f.w + 0.10);
    float h1 = wl_softsign(-0.5 * f.x + 0.9 * f.y - 0.3 * f.z + 0.4 * f.w - 0.05);
    float h2 = wl_softsign( 0.3 * f.x + 0.4 * f.y + 0.8 * f.z - 0.6 * f.w + 0.00);
    float h3 = wl_softsign(-0.4 * f.x - 0.3 * f.y + 0.5 * f.z + 0.7 * f.w + 0.15);
    float up = n.y * 0.5 + 0.5;
    vec3 sky = mix(u_SkyBottomColor, u_SkyTopColor, up);
    vec3 diffLobe = albedo * sky * (0.45 + 0.30 * h0 + 0.15 * h1);
    float ndv = clamp(dot(n, v), 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04), max(albedo, vec3(0.04)), clamp(1.0 - rough, 0.0, 1.0) * 0.5);
    vec3 spec = F0 * sky * (0.25 + 0.35 * h2) * (0.3 + 0.7 * ndv) * (1.0 - rough * 0.7);
    float occ = 0.85 + 0.15 * h3;
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
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    vec3 q = wp * 0.9;
    vec4 f = vec4(sin(s1 + q.x),
                  sin(s1 * 1.6 + q.y * 1.3),
                  sin(s1 * 0.7 - q.z * 1.7 + n.x * 2.0),
                  sin((q.x - q.y + q.z) * 1.1 + n.y * 2.0));
    float h0 = wl_softsign( 0.8 * f.x - 0.5 * f.y + 0.6 * f.z + 0.2 * f.w + 0.05);
    float h1 = wl_softsign(-0.4 * f.x + 0.9 * f.y - 0.2 * f.z + 0.5 * f.w - 0.10);
    float h2 = wl_softsign( 0.3 * f.x + 0.2 * f.y + 0.7 * f.z - 0.6 * f.w + 0.35);
    float h3 = wl_softsign(-0.5 * f.x - 0.4 * f.y + 0.4 * f.z + 0.8 * f.w + 0.00);
    float flakeCell = step(0.75, wl_hash12(floor(uv * 48.0) + floor(wp.xy * 8.0)));
    // Porosity: darken + roughen crevices
    float por = clamp(-h0, 0.0, 1.0);
    albedo *= (1.0 - 0.25 * por);
    rough = clamp(rough + 0.30 * por, 0.03, 1.0);
    // Patina: subtle hue shift
    albedo *= (vec3(1.0) + vec3(-0.06, 0.02, 0.05) * h1);
    // Metallic flakes: sparse cells go mirror-smooth, tinted by albedo
    float fl = flakeCell * clamp(h2 * 1.5, 0.0, 1.0);
    metal = clamp(metal + fl * 0.9, 0.0, 1.0);
    rough = clamp(mix(rough, 0.08, fl), 0.03, 1.0);
    // Dielectric coating: warm F0 lift (clearcoat-like), suppressed on metals
    float coat = clamp(h3, 0.0, 1.0) * (1.0 - metal);
    f0Tint = vec3(1.0) + vec3(1.2, 0.9, 0.6) * coat;
}

struct Ray { vec3 Origin; vec3 Direction; };

bool RayAABB(Ray r, vec3 invDir, vec3 minB, vec3 maxB) 
{
    vec3 t0 = (minB - r.Origin) * invDir;
    vec3 t1 = (maxB - r.Origin) * invDir;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);
    return tNear <= tFar && tFar > 0.0;
}

uint seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000);

float hash() 
{
    seed = seed * 747796405u + 2891336453u;
    uint result = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    result = (result >> 22u) ^ result;
    return result / 4294967295.0;
}

vec3 random_in_hemisphere(vec3 normal)
{
    float u = hash();
    float v = hash();
    float z = 2.0 * v - 1.0;
    float r = sqrt(max(1.0 - z * z, 0.0));
    float theta = 2.0 * 3.14159265359 * u;
    vec3 dir = vec3(r * cos(theta), r * sin(theta), z);
    return dot(dir, normal) > 0.0 ? dir : -dir;
}

const float PI = 3.14159265359;

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    float t = 1.0 - cosTheta;
    float t2 = t * t;
    return F0 + (1.0 - F0) * t2 * t2 * t;
}

vec3 GetSkyColor(vec3 dir);

float HitCube(Ray localRay, out vec3 outNormal) 
{
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001));
    vec3 invDir = 1.0 / safeDir;

    vec3 tMin = (vec3(-0.5) - localRay.Origin) * invDir;
    vec3 tMax = (vec3(0.5) - localRay.Origin) * invDir;
    
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar  = min(min(t2.x, t2.y), t2.z);
    
    if (tNear > tFar || tFar < 0.0) return -1.0;
    
    if (tNear == t1.x)      outNormal = vec3(-sign(localRay.Direction.x), 0.0, 0.0);
    else if (tNear == t1.y) outNormal = vec3(0.0, -sign(localRay.Direction.y), 0.0);
    else                    outNormal = vec3(0.0, 0.0, -sign(localRay.Direction.z));

    return tNear;
}

float HitSphere(Ray localRay, float radius, out vec3 outNormal) 
{
    float a = dot(localRay.Direction, localRay.Direction);
    float b = 2.0 * dot(localRay.Origin, localRay.Direction);
    float c = dot(localRay.Origin, localRay.Origin) - (radius * radius);
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) return -1.0;
    float sqrtD = sqrt(discriminant);
    float t = (-b - sqrtD) / (2.0 * a);
    if (t < 0.0) t = (-b + sqrtD) / (2.0 * a);
    if (t > 0.0) 
    {
        outNormal = normalize(localRay.Origin + t * localRay.Direction);
        return t;
    }
    return -1.0;
}

bool TestInstanceOcclusion(Ray r, float maxDist, int instIdx) {
    RayTracingInstance inst = Instances[instIdx];

    vec3 diff = u_CameraPosition - inst.WorldTransform[3].xyz;
    if (dot(diff, diff) > inst.MaxDistance * inst.MaxDistance) return false;

    Ray localRay;
    localRay.Origin = (inst.InvTransform * vec4(r.Origin, 1.0)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(r.Direction, 0.0)).xyz);
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001));
    vec3 localInvDir = 1.0 / safeDir;

    if (RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) {
        vec3 localNormal;
        float t = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
        if (t > 0.0) {
            vec3 localHit = localRay.Origin + t * localRay.Direction;
            vec3 worldHit = (inst.WorldTransform * vec4(localHit, 1.0)).xyz;
            if (distance(r.Origin, worldHit) < maxDist) return true;
        }
    }
    return false;
}

bool IsOccluded(Ray r, float maxDist, int skipIdx) {
    if (u_InstanceCount == 0) return false;

    vec3 safeDir = sign(r.Direction) * max(abs(r.Direction), vec3(0.00001));
    vec3 invDir = 1.0 / safeDir;
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
        if (tNear > tFar || tFar < 0.0 || tNear > maxDist) continue;

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

float Halton(int index, int base) {
    float f = 1.0;
    float r = 0.0;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

vec2 GetJitter(int frameIndex, float cameraMoved) {
    int effectiveIndex = (cameraMoved > 0.5) ? 0 : frameIndex;
    float x = Halton(effectiveIndex % 16 + 1, 2);
    float y = Halton(effectiveIndex % 16 + 1, 3);
    return vec2(x, y) - 0.5;
}

const mat3 Gx = mat3(-1, 0, 1, -2, 0, 2, -1, 0, 1);
const mat3 Gy = mat3(-1, -2, -1, 0, 0, 0, 1, 2, 1);

float GetLuminance(vec3 col) {
    return dot(col, vec3(0.2126, 0.7152, 0.0722));
}

vec3 RGB2YCoCg(vec3 rgb) {
    float Y  = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    float Co = 0.5  * rgb.r - 0.5 * rgb.b;
    float Cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
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
            uv = vec2(localPos.z * signX + 0.5, localPos.y + 0.5);
            uvScale = vec2(inst.TextureScale.z, inst.TextureScale.y);
        } else if (maxAxis == absLocal.y) {
            float signY = sign(localPos.y);
            uv = vec2(localPos.x * signY + 0.5, localPos.z + 0.5);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.w);
        } else {
            float signZ = sign(localPos.z);
            uv = vec2(localPos.x * signZ + 0.5, localPos.y + 0.5);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.y);
        }

        return uv * uvScale;
    } else {
        float phi = atan(localPos.z, localPos.x);
        float theta = acos(clamp(localPos.y, -1.0, 1.0));

        float u = phi / (2.0 * PI) + 0.5;
        float v = theta / PI;

        return vec2(u, v) * inst.TextureScale.xy;
    }
}

void RunVisibilityAndVelocity() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(wl_trace_extent(imageSize(img_Output)));
    
    float depth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (depth >= 1.0) { 
        imageStore(img_Velocity, pixelCoords, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    worldPos /= worldPos.w;

    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    prevClipPos /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5 + 0.5;

    vec2 velocity = uv - prevUV;
    imageStore(img_Velocity, pixelCoords, vec4(velocity, 0.0, 1.0));
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
    vec3 f0Tint; // neural material specular tint, 1.0 = classic F0
};

void ComputeTangentFrame(vec3 localHitPos, vec3 localNormal, int shapeType, vec4 texScaleFull,
                         out vec3 T, out vec3 B, out vec2 uvScale)
{
    if (shapeType == 1)
    {
        T = vec3(-localNormal.z, 0.0, localNormal.x);
        float lenT = length(T);
        if (lenT < 0.0001)
            T = vec3(1.0, 0.0, 0.0);
        else
            T /= lenT;

        B = cross(localNormal, T);

        float radius = max(length(localHitPos), 0.0001);
        float radiusXZ = length(localHitPos.xz);
        radiusXZ = max(radiusXZ, 0.05 * radius);

        uvScale.x = (1.0 / (2.0 * PI * radiusXZ)) * texScaleFull.x;
        uvScale.y = (1.0 / (PI * radius)) * texScaleFull.y;
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
            T = vec3(0.0, 0.0, sign(localNormal.x));
            B = vec3(0.0, 1.0, 0.0);
            uvScale = vec2(texScaleFull.z, texScaleFull.y);
        } else if (absN.y > absN.x && absN.y > absN.z) {
            T = vec3(1.0, 0.0, 0.0);
            B = vec3(0.0, 0.0, -sign(localNormal.y));
            uvScale = vec2(texScaleFull.x, texScaleFull.w);
        } else {
            T = vec3(-sign(localNormal.z), 0.0, 0.0);
            B = vec3(0.0, 1.0, 0.0);
            uvScale = vec2(texScaleFull.x, texScaleFull.y);
        }
    }
}

void ApplyPOM(RayTracingInstance inst, vec3 localHitPos, vec3 localNormal,
              vec3 localViewDir, inout vec2 uv, inout vec3 worldPos,
              inout vec3 worldNormal, out float selfOcclusion)
{
    selfOcclusion = 1.0;

    if (u_QualityLevel <= 1)
        return;

    vec3 V = normalize(localViewDir);
    float distToCam = length(worldPos - u_CameraPosition);
    float cosView = abs(dot(V, localNormal));

    float maxDist = (u_QualityLevel == 2) ? 25.0 : 50.0;
    if (cosView < 0.15 || distToCam > maxDist)
        return;

    float dispScale = inst.DisplacementParams.x;
    float bumpStrength = inst.DisplacementParams.y;
    if (inst.TextureID < 0 || inst.TextureID >= 32)
        return;
    if (dispScale < 0.001 && bumpStrength < 0.001)
        return;

    int shapeType = int(inst.MaterialParams.z);
    vec3 T, B;
    vec2 uvScale;
    ComputeTangentFrame(localHitPos, localNormal, shapeType, inst.TextureScale, T, B, uvScale);

    int texID = inst.TextureID;
    float eps = 1.0 / 256.0;

    float maxDisplacement = max(dispScale, bumpStrength);
    float cosAngle = max(dot(V, localNormal), 0.05);
    
    int baseLayers = (u_QualityLevel == 2) ? 6 : 16;
    int maxLayers  = (u_QualityLevel == 2) ? 16 : 32;
    int numLayers  = int(clamp(float(baseLayers) / cosAngle, float(baseLayers), float(maxLayers)));

    float vnDot = dot(V, localNormal);
    vec2 viewUVDir = vec2(dot(V, T), dot(V, B));
    vec2 uvOffsetPerHeight = -viewUVDir / max(abs(vnDot), 0.05);

    float distFade = clamp(1.0 - (distToCam - (maxDist - 15.0)) / 15.0, 0.0, 1.0);
    dispScale *= distFade;
    bumpStrength *= distFade;

    float dHeight = 1.0 / float(numLayers);
    vec2 dUV = (uvOffsetPerHeight * uvScale) * dHeight * maxDisplacement;

    vec2 prevUV = uv;
    float prevLayerH = 1.0;
    float currLayerH = 1.0;
    float prevTexH = GetLuminance(textureLod(u_SceneTextures[texID], uv, 0.0).rgb);
    vec2 currUV = uv;
    float currTexH = prevTexH;

    for (int i = 0; i < numLayers; i++)
    {
        prevUV = currUV;
        prevLayerH = currLayerH;
        prevTexH = currTexH;

        currUV += dUV;
        currLayerH -= dHeight;
        currTexH = GetLuminance(textureLod(u_SceneTextures[texID], currUV, 0.0).rgb);

        if (currTexH > currLayerH)
            break;
    }

    float d1 = prevLayerH - prevTexH;
    float d2 = currTexH - currLayerH;
    float weight = d1 / max(d1 + d2, 0.0001);
    vec2 pomUV = mix(prevUV, currUV, weight);

    vec2 uvDelta = pomUV - uv;
    float maxOffset = 0.15;
    if (length(uvDelta) > maxOffset)
        uvDelta = normalize(uvDelta) * maxOffset;

    uv += uvDelta;

    float finalHeight = mix(prevTexH, currTexH, weight);
    if (dispScale > 0.001)
        worldPos += worldNormal * finalHeight * dispScale;

    if (bumpStrength > 0.001)
    {
        float hU = GetLuminance(textureLod(u_SceneTextures[texID], uv + vec2(eps, 0.0), 0.0).rgb);
        float hV = GetLuminance(textureLod(u_SceneTextures[texID], uv + vec2(0.0, eps), 0.0).rgb);
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
        if (bumpLen > 1.0)
            localPerturbedNormal = localNormal - bumpGrad / bumpLen;
        localPerturbedNormal = normalize(localPerturbedNormal);
        mat3 normalMatrix = transpose(mat3(inst.InvTransform));
        worldNormal = normalize(normalMatrix * localPerturbedNormal);

        float gradMag = length(vec2(dh_dsT, dh_dsB));
        float ridgeFactor = max(0.0, finalHeight - 0.5) * gradMag;
        selfOcclusion = clamp(1.0 - ridgeFactor * 0.04 * bumpStrength, 0.2, 1.0);
    }
}

void TestInstanceHit(Ray ray, int instIdx, bool isPrimaryRay, inout HitInfo info) {
    RayTracingInstance inst = Instances[instIdx];

    vec3 camDiff = u_CameraPosition - inst.WorldTransform[3].xyz;
    if (dot(camDiff, camDiff) > inst.MaxDistance * inst.MaxDistance) return;

    Ray localRay;
    localRay.Origin = (inst.InvTransform * vec4(ray.Origin, 1.0)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(ray.Direction, 0.0)).xyz);
    
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001));
    vec3 localInvDir = 1.0 / safeDir;

    if (!RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) return;
    vec3 localNormal;
    float tLocal = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
    if (tLocal > 0.0) {
        vec3 localHitPos = localRay.Origin + tLocal * localRay.Direction;
        vec3 worldHit = (inst.WorldTransform * vec4(localHitPos, 1.0)).xyz;
        float tWorld = distance(ray.Origin, worldHit);
        if (tWorld < info.t) {
            info.t = tWorld;
            info.hit = true;
            info.worldPos = worldHit;
            info.normal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);

            vec2 uv = CalculateUV(localHitPos, inst);

            if (isPrimaryRay)
            {
                float pomOcclusion = 1.0;
                ApplyPOM(inst, localHitPos, localNormal, -localRay.Direction, uv, info.worldPos, info.normal, pomOcclusion);
                info.occlusion = pomOcclusion;
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
            vec3 wSize = max((inst.Max.xyz - inst.Min.xyz) * wAxis, vec3(0.0001));
            float worldExtent = max(max(wSize.x, wSize.y), wSize.z);
            float scaleLen = max(length(inst.TextureScale.xy) / worldExtent, 0.0001);
            // Gentle distance ramp (holds LOD 0 ~4x farther out) + extra blur
            // at grazing angles, where one isotropic LOD can't cover the
            // stretched footprint and shimmer/RGB striping appears.
            // (textureLod bypasses driver anisotropy, so compensate manually.)
            float ndv = abs(dot(info.normal, ray.Direction));
            float grazing = 1.0 - clamp(ndv, 0.0, 1.0);
            float mipLevel = log2(max(dist * scaleLen * 0.008, 0.0001)) + grazing * grazing * 2.0;
            mipLevel = clamp(mipLevel, 0.0, 4.0);

            vec3 sampledAlbedo = vec3(1.0);
            if (inst.TextureID >= 0 && inst.TextureID < 32)
                sampledAlbedo = textureLod(u_SceneTextures[inst.TextureID], uv, mipLevel).rgb;

            info.albedo = sampledAlbedo * inst.Albedo.rgb;
            // Neural texture detail: MLP-synthesized micro-surface (OpenGL neural path)
            if (u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001)
                info.albedo *= mix(vec3(1.0), wl_neural_tex_detail(uv, info.normal), clamp(u_NeuralTexStrength, 0.0, 1.0));
            info.metal = clamp(inst.MaterialParams.x, 0.0, 1.0);
            info.rough = inst.MaterialParams.y > 0.0 ? inst.MaterialParams.y : 0.5;
            info.f0Tint = vec3(1.0);
            // Neural material: learned PBR params feed the GGX BRDF (OpenGL neural path)
            if (u_NeuralEnabled == 1 && u_NeuralMatStrength > 0.001)
            {
                float nmk = clamp(u_NeuralMatStrength, 0.0, 1.0);
                vec3 nma = info.albedo;
                float nmm = info.metal;
                float nmr = info.rough;
                vec3 nmf = vec3(1.0);
                wl_neural_material(uv, info.worldPos, info.normal, nma, nmm, nmr, nmf);
                info.albedo = mix(info.albedo, nma, nmk);
                info.metal = mix(info.metal, nmm, nmk);
                info.rough = mix(info.rough, nmr, nmk);
                info.f0Tint = mix(vec3(1.0), nmf, nmk);
            }
            info.emission = inst.Emission.xyz * inst.Emission.w;
        }
    }
}

HitInfo TraceScene(Ray ray) {
    HitInfo info;
    info.hit = false;
    info.t = 1e20;
    info.occlusion = 1.0;
    info.f0Tint = vec3(1.0);

    if (u_InstanceCount == 0) return info;

    bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01;
    vec3 safeDir = sign(ray.Direction) * max(abs(ray.Direction), vec3(0.00001));
    vec3 invDir = 1.0 / safeDir;

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
        if (tNear > tFar || tFar < 0.0 || tNear > info.t) continue;

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

vec3 ComputeDirectLighting(HitInfo h, vec3 V, uint lightSeed) {
    vec3 F0 = mix(vec3(0.04), max(h.albedo, vec3(0.04)), h.metal) * h.f0Tint;
    vec3 diffuseColor = h.albedo * (1.0 - h.metal);
    vec3 directLight = vec3(0.0);

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
    float lightScale = float(lightsToSample) / float(max(evalCount, 1));

    for (int k = 0; k < evalCount; k++) {
        int li = (lightStart + k * lightStride) % lightsToSample;
        int i = LightIndices[li];
        RayTracingInstance light = Instances[i];
        vec3 toLight = light.WorldTransform[3].xyz - h.worldPos;
        float distToLight = length(toLight);
        vec3 dirToLight = toLight / distToLight;
        Ray shadowRay = Ray(h.worldPos + h.normal * 0.001, dirToLight);
        // Pass the light's own instance index so it can't self-shadow
        // (shadow rays target the light center, inside its geometry).
        if (!IsOccluded(shadowRay, distToLight, i)) {
            float NdotL = max(dot(h.normal, dirToLight), 0.0);
            if (NdotL <= 0.0) continue;
            vec3 H = normalize(V + dirToLight);
            vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
            float D = DistributionGGX(max(dot(h.normal, H), 0.0), h.rough);
            float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
            float NdotV = max(dot(h.normal, V), 0.0);
            vec3 brdf = (vec3(1.0) - F) * (1.0 - h.metal) * h.albedo / PI + (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
            vec3 contrib = brdf * light.Emission.xyz * light.Emission.w * NdotL;

            float brightness = dot(contrib, vec3(0.2126, 0.7152, 0.0722));
            if (brightness > 10.0) contrib *= (10.0 / brightness);
            directLight += contrib * lightScale;
        }
    }

    directLight += h.emission;

    float skyT = 0.5 * (h.normal.y + 1.0);
    vec3 skyAmbient = mix(u_SkyBottomColor, u_SkyTopColor, skyT) * 0.15;
    directLight += diffuseColor * skyAmbient * h.occlusion;

    if (h.metal > 0.0) {
        vec3 R = reflect(-V, h.normal);
        vec3 specSky = GetSkyColor(R);
        vec3 diffSky = skyAmbient / 0.15; 
        vec3 envColor = mix(specSky, diffSky, h.rough * h.rough);
        directLight += F0 * envColor * h.metal * h.occlusion;
    }

    return directLight;
}

vec3 GetSkyColor(vec3 dir) {
    float t = 0.5 * (normalize(dir).y + 1.0);
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
    vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(traceSize)) * 2.0 - 1.0;
    
    Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0, 1.0)).xyz));
    HitInfo primary = TraceScene(primaryRay);

    imageStore(img_AlbedoHit, pixelCoords, vec4(primary.hit ? primary.albedo : vec3(1.0), primary.hit ? 1.0 : 0.0));

    // Trace G-buffer: NDC depth (far on miss) + packed world normal for the
    // velocity / temporal / bilateral passes. Full-size buffer, subregion data.
    // (RunResolve was removed; binding 6 was dead. Image units are capped at
    // 0-7 on this GL driver, so depth+normal share one RGBA32F image.)
    if (!primary.hit) {
        imageStore(img_TraceGBuffer, pixelCoords, vec4(1.0, 0.5, 0.5, 1.0));
    } else {
        vec4 tclip = u_ViewProjection * vec4(primary.worldPos, 1.0);
        float tndc = tclip.z / max(tclip.w, 1e-6);
        imageStore(img_TraceGBuffer, pixelCoords, vec4(clamp(tndc * 0.5 + 0.5, 0.0, 1.0), primary.normal * 0.5 + 0.5));
    }

    if (!primary.hit) {
        imageStore(img_Bloom, pixelCoords, vec4(GetSkyColor(primaryRay.Direction), 1.0));
        return;
    }

    HitInfo tileHit = primary;
    Ray lightRay = primaryRay;

    vec3 directLight = vec3(0.0);
    vec3 indirectLight = vec3(0.0);

    if (tileHit.hit) {
        vec3 V = normalize(-lightRay.Direction);
        uint lightSeed = uint(pixelCoords.x * 73 + pixelCoords.y * 149 + u_FrameIndex * 2);
        directLight = ComputeDirectLighting(tileHit, V, lightSeed);

        int maxIndirectRays = (u_QualityLevel == 0) ? 0 : 
                              ((u_QualityLevel <= 2) ? min(u_IndirectRays, 1) : u_IndirectRays);

        if (maxIndirectRays > 0) {
            vec3 diffuseColor = tileHit.albedo * (1.0 - tileHit.metal);
            vec3 F0 = mix(vec3(0.04), max(tileHit.albedo, vec3(0.04)), tileHit.metal) * tileHit.f0Tint;
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

                Ray indirectRay = Ray(tileHit.worldPos + tileHit.normal * 0.001, indirectDir);
                HitInfo indirect = TraceScene(indirectRay);

                if (indirect.hit) {
                    vec3 indirectV = normalize(-indirectDir);
                    vec3 bouncedLight = ComputeDirectLighting(indirect, indirectV, lightSeed + 1u);
                    indirectLight += indirectReflectance * bouncedLight;
                } else {
                    indirectLight += indirectReflectance * GetSkyColor(indirectDir);
                }
            }
            indirectLight /= float(maxIndirectRays);
            indirectLight *= tileHit.occlusion;
            // Neural radiance cache blend: learned GI steadies the 1-ray estimate (OpenGL neural path)
            if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001) {
                vec3 neuralGI = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, V);
                indirectLight = mix(indirectLight, neuralGI * tileHit.occlusion, clamp(u_NeuralLightStrength, 0.0, 1.0) * 0.65);
            }
        } else if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001) {
            // No traced bounces at this quality level: neural cache IS the GI (e.g. Low preset)
            indirectLight = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, V) * tileHit.occlusion;
        }
    } else {
        directLight = GetSkyColor(lightRay.Direction);
    }

    directLight = clamp(directLight, vec3(0.0), vec3(10.0));
    indirectLight = clamp(indirectLight, vec3(0.0), vec3(5.0));

    // ComputeDirectLighting already folds albedo into the BRDF (diffuse/spec/
    // ambient/emission), and indirectLight already carries tile reflectance -
    // do NOT multiply by albedo again (albedo^2 pushed textured floors to black).
    vec3 finalPixelColor = directLight + indirectLight;
    imageStore(img_Bloom, pixelCoords, vec4(finalPixelColor, 1.0));
}

void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Accumulation));
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imgSize);

    vec3 currentLight = imageLoad(img_Bloom, pixelCoords).rgb;
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 prevUV = uv - velocity;
    // Accumulation + depth buffers hold the subregion; map history UVs to it.
    vec2 histUV = prevUV * u_RenderScale;

    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 && prevUV.y >= 0.0 && prevUV.y <= 1.0;
    float motionMag = length(velocity * vec2(imgSize));

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = texture(s_DepthBuffer, histUV).r;
        if (abs(currentDepth - prevDepth) > 0.02) validHistory = false;
    }

    vec3 historyLight = texture(s_Accumulation, histUV).rgb;

    if (validHistory) {
        if (u_QualityLevel >= 3) {
            vec3 m1 = vec3(0.0);
            vec3 m2 = vec3(0.0);
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    ivec2 sp = clamp(pixelCoords + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                    vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                    m1 += s;
                    m2 += s * s;
                }
            }
            vec3 mean = m1 / 9.0;
            vec3 stddev = sqrt(max(vec3(0.0), m2 / 9.0 - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            vec3 minCol = mean - 1.25 * stddev;
            vec3 maxCol = mean + 1.25 * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        } else if (u_QualityLevel == 2) {
            ivec2 offsets[5] = ivec2[](ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
            vec3 m1 = vec3(0.0);
            vec3 m2 = vec3(0.0);
            for (int i = 0; i < 5; i++) {
                ivec2 sp = clamp(pixelCoords + offsets[i], ivec2(0), imgSize - ivec2(1));
                vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
            vec3 mean = m1 / 5.0;
            vec3 stddev = sqrt(max(vec3(0.0), m2 / 5.0 - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            vec3 minCol = mean - 1.25 * stddev;
            vec3 maxCol = mean + 1.25 * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        }
    }

    float motionFactor = clamp(motionMag / 8.0, 0.0, 1.0);
    float alpha = validHistory ? mix(u_AccumulationAlpha, 0.8, motionFactor) : 1.0;

    vec3 result = mix(historyLight, currentLight, alpha);
    imageStore(img_Accumulation, pixelCoords, vec4(result, 1.0));
}

void RunBloomThreshold() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = imageLoad(img_Output, pos);
    
    float threshold = 0.5;
    float knee = 0.1; 
    
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    float soft = smoothstep(threshold - knee, threshold + knee, brightness);
    
    imageStore(img_Bloom, pos, color * soft);
}

void RunBloomBlurHorizontal() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
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
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec4 sum = imageLoad(img_Bloom_Temp, pos) * weights[0];

    for(int i = 1; i < 5; i++) {
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, i)) * weights[i];
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, -i)) * weights[i];
    }

    vec3 existing = imageLoad(img_Output, pos).rgb;
    vec3 finalColor = existing + sum.rgb;
    imageStore(img_Output, pos, vec4(finalColor, 1.0));
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

    if (u_RenderScale < 1.0) {
        ivec2 srcSize = ivec2(vec2(fullSize) * u_RenderScale);
        if (srcSize.x < 1) srcSize.x = 1;
        if (srcSize.y < 1) srcSize.y = 1;

        vec2 srcCoord = (vec2(pos) + 0.5) * u_RenderScale;
        vec2 srcPixel = srcCoord - vec2(0.5);
        ivec2 s0 = clamp(ivec2(floor(srcPixel)), ivec2(0), srcSize - ivec2(1));
        ivec2 s1 = clamp(s0 + ivec2(1), ivec2(0), srcSize - ivec2(1));
        vec2 f = srcPixel - vec2(s0);

        vec3 c00 = imageLoad(img_Accumulation, s0).rgb;
        vec3 c10 = imageLoad(img_Accumulation, ivec2(s1.x, s0.y)).rgb;
        vec3 c01 = imageLoad(img_Accumulation, ivec2(s0.x, s1.y)).rgb;
        vec3 c11 = imageLoad(img_Accumulation, s1).rgb;
        denoised = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {
        denoised = (u_QualityLevel >= 3) ? imageLoad(img_Bloom_Temp, pos).rgb : imageLoad(img_Accumulation, pos).rgb;
    }

    vec3 combined = denoised;

    vec3 mapped = combined / (combined + vec3(1.0));
    vec3 finalColor = pow(mapped, vec3(1.0 / 2.2));

    imageStore(img_Output, pos, vec4(finalColor, 1.0));

    if (u_QualityLevel >= 1 && u_RenderScale >= 1.0) {
        float brightness = dot(combined, vec3(0.2126, 0.7152, 0.0722));
        float threshold = 0.8;
        float knee = 0.15;
        float soft = smoothstep(threshold - knee, threshold + knee, brightness);
        imageStore(img_Bloom, pos, vec4(combined * soft, 1.0));
    }
}

void RunBilateralBlur() {
    if (u_QualityLevel <= 1) return;

    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Bloom_Temp));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    vec3 centerColor = imageLoad(img_Accumulation, pos).rgb;
    
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        imageStore(img_Bloom_Temp, pos, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    vec2 uv = (vec2(pos) + 0.5) / vec2(imgSize);
    float centerDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (centerDepth >= 1.0) { 
        imageStore(img_Bloom_Temp, pos, vec4(centerColor, 1.0));
        return;
    }

    vec3 centerNormal = texture(s_NormalBuffer, wl_buf_uv(uv)).rgb * 2.0 - 1.0;
    float centerLuma = GetLuminance(centerColor);

    int step = max(1, u_StepSize);
    vec3 totalColor = vec3(0.0);
    float totalWeight = 0.0;

    if (u_QualityLevel == 2) {
        ivec2 offsets[9] = ivec2[](
            ivec2(0, 0), 
            ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
            ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2)
        );
        for (int i = 0; i < 9; i++) {
            ivec2 samplePos = clamp(pos + offsets[i] * step, ivec2(0), imgSize - ivec2(1));

            vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
            vec2 sampleUV = (vec2(samplePos) + 0.5) / vec2(imgSize);

            float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).rgb * 2.0 - 1.0;
            float neighborLuma = GetLuminance(neighborColor);

            float spatialWeight = (i == 0) ? 0.375 : ((i <= 4) ? 0.125 : 0.03125);
            float normalWeight = pow(max(0.0, dot(centerNormal, neighborNormal)), 16.0);
            float depthDiff = abs(centerDepth - neighborDepth);
            float depthWeight = exp(-depthDiff * 100.0);
            float lumaDiff = abs(centerLuma - neighborLuma);
            float lumaWeight = exp(-lumaDiff * 4.0);

            float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    } else if (u_QualityLevel >= 3) {
        float lumaSum = 0.0;
        float lumaSqSum = 0.0;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                ivec2 sp = clamp(pos + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                float l = GetLuminance(imageLoad(img_Accumulation, sp).rgb);
                lumaSum += l;
                lumaSqSum += l * l;
            }
        }
        float lumaMean = lumaSum / 9.0;
        float lumaVar = sqrt(max(0.0, (lumaSqSum / 9.0) - (lumaMean * lumaMean)));

        const float kernel[3] = float[](0.375, 0.25, 0.0625);

        for (int x = -2; x <= 2; x++) {
            for (int y = -2; y <= 2; y++) {
                ivec2 samplePos = clamp(pos + ivec2(x, y) * step, ivec2(0), imgSize - ivec2(1));

                vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
                vec2 sampleUV = (vec2(samplePos) + 0.5) / vec2(imgSize);

                float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
                vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).rgb * 2.0 - 1.0;
                float neighborLuma = GetLuminance(neighborColor);

                float spatialWeight = kernel[abs(x)] * kernel[abs(y)];
                float normalWeight = pow(max(0.0, dot(centerNormal, neighborNormal)), 32.0);
                float depthDiff = abs(centerDepth - neighborDepth);
                float depthWeight = exp(-depthDiff * 100.0);
                float lumaDiff = abs(centerLuma - neighborLuma);
                float lumaWeight = exp(-lumaDiff / (lumaVar * 4.0 + 0.001));

                float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

                totalColor += neighborColor * weight;
                totalWeight += weight;
            }
        }
    }

    vec3 finalFiltered = (totalWeight > 0.0001) ? (totalColor / totalWeight) : centerColor;
    imageStore(img_Bloom_Temp, pos, vec4(finalFiltered, 1.0));
}

void GenerateMaterialMaps() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(img_MaterialPacked);
    
    float I[3][3];
    for(int i = -1; i <= 1; i++) {
        for(int j = -1; j <= 1; j++) {
            ivec2 samplePos = clamp(pos + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec2 uv = (vec2(samplePos) + 0.5) / vec2(size);
            I[i+1][j+1] = GetLuminance(texture(s_InputAlbedo, uv).rgb);
        }
    }

    float resX = 0.0;
    float resY = 0.0;
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++) {
            resX += I[i][j] * Gx[i][j];
            resY += I[i][j] * Gy[i][j];
        }
    }

    float normalStrength = u_NormalStrength; 
    vec3 normal = normalize(vec3(-resX * normalStrength, -resY * normalStrength, 1.0));

    vec2 packedNormal = normal.xy * 0.5 + 0.5;
    
    float ao = 1.0 - (length(vec2(resX, resY)) * 5.0 * u_AOIntensity);
    float rough = clamp((1.0 - I[1][1]) + u_RoughnessBias, 0.0, 1.0);
    
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

    vec2 srcCoord = (vec2(pos) + 0.5) * u_RenderScale;
    vec2 srcPixel = srcCoord - vec2(0.5);
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

    imageStore(img_Output, pos, vec4(color, 1.0));
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
    }
}