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
layout(rgba32f, binding = 6) uniform image2D img_FinalDisplay;
layout(binding = 3) uniform sampler2D s_Accumulation;
layout(binding = 4) uniform sampler2D s_DepthBuffer;
layout(binding = 8) uniform sampler2D s_Output;
layout(binding = 9) uniform sampler2D s_Bloom;
layout(binding = 2) uniform sampler2D s_Indirect;
layout(binding = 10) uniform sampler2D s_NormalBuffer;
layout(rgba8, binding = 3) uniform writeonly image2D img_MaterialPacked;
layout(binding = 11) uniform sampler2D s_InputAlbedo;
layout(binding = 12) uniform sampler2D s_PackedMaterialMap;

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
uniform int u_QualityLevel;
uniform int u_MaxBounces;
uniform int u_MaxLights;
uniform int u_IndirectRays;
uniform int u_StepSize;
uniform sampler2D u_SceneTextures[32];

struct Ray { vec3 Origin; vec3 Direction; };

bool RayAABB(Ray r, vec3 minB, vec3 maxB) 
{
    vec3 invDir = 1.0 / r.Direction;
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
    vec3 tMin = (vec3(-0.5) - localRay.Origin) / localRay.Direction;
    vec3 tMax = (vec3(0.5) - localRay.Origin) / localRay.Direction;
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar = min(min(t2.x, t2.y), t2.z);
    if (tNear > tFar || tFar < 0.0) return -1.0;
    
    vec3 hitPoint = localRay.Origin + tNear * localRay.Direction;
    vec3 absHit = abs(hitPoint);
    if (absHit.x > 0.499) outNormal = vec3(sign(hitPoint.x), 0.0, 0.0);
    else if (absHit.y > 0.499) outNormal = vec3(0.0, sign(hitPoint.y), 0.0);
    else outNormal = vec3(0.0, 0.0, sign(hitPoint.z));
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

bool IsOccluded(Ray r, float maxDist) {
    for(int i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];

        vec3 diff = u_CameraPosition - inst.WorldTransform[3].xyz;
        if (dot(diff, diff) > inst.MaxDistance * inst.MaxDistance) continue;

        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(r.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(r.Direction, 0.0)).xyz;
        if (RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) {
            vec3 localNormal;
            float t = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
            if (t > 0.0 && t < maxDist) return true;
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
    vec3 size = inst.Max.xyz - inst.Min.xyz;

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
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imageSize(img_Output));
    
    float depth = texture(s_DepthBuffer, uv).r;
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
};

void ComputeTangentFrame(vec3 localHitPos, vec3 localNormal, int shapeType, vec2 texScale,
                         out vec3 T, out vec3 B, out vec2 uvScale)
{
    const float PI = 3.14159265359;

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

        uvScale.x = (1.0 / (2.0 * PI * radiusXZ)) * texScale.x;
        uvScale.y = (1.0 / (PI * radius)) * texScale.y;
    }
    else 
    {
        vec3 absN = abs(localNormal);

        if (absN.x > absN.y && absN.x > absN.z) {
            T = vec3(0.0, 0.0, sign(localNormal.x));
            B = vec3(0.0, 1.0, 0.0);
        } else if (absN.y > absN.x && absN.y > absN.z) {
            T = vec3(1.0, 0.0, 0.0);
            B = vec3(0.0, 0.0, -sign(localNormal.y));
        } else {
            T = vec3(-sign(localNormal.z), 0.0, 0.0);
            B = vec3(0.0, 1.0, 0.0);
        }

        uvScale = texScale;
    }
}

void ApplyPOM(RayTracingInstance inst, vec3 localHitPos, vec3 localNormal,
              vec3 localViewDir, inout vec2 uv, inout vec3 worldPos,
              inout vec3 worldNormal, out float selfOcclusion)
{
    selfOcclusion = 1.0;

    vec3 V = normalize(localViewDir);
    float distToCam = length(worldPos - u_CameraPosition);
    float cosView = abs(dot(V, localNormal));

    if (cosView < 0.15 || distToCam > 50.0)
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
    ComputeTangentFrame(localHitPos, localNormal, shapeType, inst.TextureScale.xy, T, B, uvScale);

    int texID = inst.TextureID;
    float eps = 1.0 / 256.0;

    float maxDisplacement = max(dispScale, bumpStrength);
    float cosAngle = max(dot(V, localNormal), 0.05);
    int baseLayers = int(mix(6.0, 32.0, float(u_QualityLevel) / 3.0));
    int numLayers = int(clamp(float(baseLayers) / cosAngle, float(baseLayers), 96.0));

    float vnDot = dot(V, localNormal);
    vec2 viewUVDir = vec2(dot(V, T), dot(V, B));
    vec2 uvOffsetPerHeight = -viewUVDir / max(abs(vnDot), 0.05);

    float distFade = clamp(1.0 - (distToCam - 30.0) / 20.0, 0.0, 1.0);
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

        float dh_dsT = dhdu * uvScale.x;
        float dh_dsB = dhdv * uvScale.y;

        vec3 localPerturbedNormal = normalize(localNormal - (dh_dsT * T + dh_dsB * B) * bumpStrength);
        mat3 normalMatrix = transpose(mat3(inst.InvTransform));
        worldNormal = normalize(normalMatrix * localPerturbedNormal);

        float gradMag = length(vec2(dh_dsT, dh_dsB));
        float ridgeFactor = max(0.0, finalHeight - 0.5) * gradMag;
        selfOcclusion = clamp(1.0 - ridgeFactor * 0.04 * bumpStrength, 0.2, 1.0);
    }
}

HitInfo TraceScene(Ray ray) {
    HitInfo info;
    info.hit = false;
    info.t = 1e20;
    info.occlusion = 1.0;

    bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01;

    for (int i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];

        vec3 camDiff = u_CameraPosition - inst.WorldTransform[3].xyz;
        if (dot(camDiff, camDiff) > inst.MaxDistance * inst.MaxDistance) continue;

        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(ray.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(ray.Direction, 0.0)).xyz;

        if (!RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) continue;
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
                float scaleLen = max(length(inst.TextureScale.xy), 0.0001);
                float mipLevel = clamp(log2(max(dist * scaleLen * 0.02, 0.0001)), 0.0, 5.0);

                vec3 sampledAlbedo = vec3(1.0);
                if (inst.TextureID >= 0 && inst.TextureID < 32)
                    sampledAlbedo = textureLod(u_SceneTextures[inst.TextureID], uv, mipLevel).rgb;

                info.albedo = sampledAlbedo * inst.Albedo.rgb;
                info.metal = clamp(inst.MaterialParams.x, 0.0, 1.0);
                info.rough = inst.MaterialParams.y > 0.0 ? inst.MaterialParams.y : 0.5;
                info.emission = inst.Emission.xyz * inst.Emission.w;
            }
        }
    }
    return info;
}

vec3 ComputeDirectLighting(HitInfo h, vec3 V) {
    vec3 F0 = mix(vec3(0.04), max(h.albedo, vec3(0.04)), h.metal);
    vec3 diffuseColor = h.albedo * (1.0 - h.metal);
    vec3 directLight = vec3(0.0);

    int lightsSampled = 0;
    for (int i = 0; i < u_InstanceCount; i++) {
        if (Instances[i].Emission.w > 0.0) {
            if (lightsSampled >= u_MaxLights) break;
            lightsSampled++;
            vec3 toLight = Instances[i].WorldTransform[3].xyz - h.worldPos;
            float distToLight = length(toLight);
            vec3 dirToLight = toLight / distToLight;
            Ray shadowRay = Ray(h.worldPos + h.normal * 0.001, dirToLight);
            if (!IsOccluded(shadowRay, distToLight)) {
                float NdotL = max(dot(h.normal, dirToLight), 0.0);
                if (NdotL <= 0.0) continue;
                vec3 H = normalize(V + dirToLight);
                vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
                float D = DistributionGGX(max(dot(h.normal, H), 0.0), h.rough);
                float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
                float NdotV = max(dot(h.normal, V), 0.0);
                vec3 brdf = (vec3(1.0) - F) * (1.0 - h.metal) * h.albedo / PI + (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
                vec3 contrib = brdf * Instances[i].Emission.xyz * Instances[i].Emission.w * NdotL;

                float brightness = dot(contrib, vec3(0.2126, 0.7152, 0.0722));
                if (brightness > 10.0) contrib *= (10.0 / brightness);
                directLight += contrib;
            }
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
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000);

    vec2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
    vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(imgSize)) * 2.0 - 1.0;
    Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0, 1.0)).xyz));

    HitInfo primary = TraceScene(primaryRay);

    vec3 directLight = vec3(0.0);
    vec3 indirectLight = vec3(0.0);

    if (primary.hit) {
        vec3 V = normalize(-primaryRay.Direction);
        directLight = ComputeDirectLighting(primary, V);

        vec3 diffuseColor = primary.albedo * (1.0 - primary.metal);
        vec3 F0 = mix(vec3(0.04), max(primary.albedo, vec3(0.04)), primary.metal);
        vec3 indirectReflectance = mix(diffuseColor, F0, primary.metal);

        for (int r = 0; r < u_IndirectRays; r++) {
            seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000) + uint((r + 1) * 7919);
            
            vec3 indirectDir;
            if (hash() < primary.metal) {
                vec3 specDir = reflect(-V, primary.normal);
                vec3 randHem = random_in_hemisphere(primary.normal);
                indirectDir = normalize(mix(specDir, randHem, primary.rough * primary.rough));
            } else {
                indirectDir = random_in_hemisphere(primary.normal);
            }

            Ray indirectRay = Ray(primary.worldPos + primary.normal * 0.001, indirectDir);

            HitInfo indirect = TraceScene(indirectRay);
            if (indirect.hit) {
                vec3 indirectV = normalize(-indirectDir);
                vec3 bouncedLight = ComputeDirectLighting(indirect, indirectV);
                indirectLight += indirectReflectance * bouncedLight;
            } else {
                indirectLight += indirectReflectance * GetSkyColor(indirectDir);
            }
        }
        if (u_IndirectRays > 0)
            indirectLight /= float(u_IndirectRays);
        indirectLight *= primary.occlusion;
    } else {
        directLight = GetSkyColor(primaryRay.Direction);
    }

    directLight = clamp(directLight, vec3(0.0), vec3(10.0));
    indirectLight = clamp(indirectLight, vec3(0.0), vec3(5.0));

    // Demodulate indirect light by primary surface albedo before outputting
    vec3 demodulatedIndirect = indirectLight / max(primary.albedo, vec3(0.01));
    vec3 totalRawIrradiance = directLight + demodulatedIndirect;

    imageStore(img_Bloom, pixelCoords, vec4(totalRawIrradiance, 1.0));
}

void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Accumulation);
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imgSize);

    vec3 currentLight = imageLoad(img_Bloom, pixelCoords).rgb;
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 prevUV = uv - velocity;

    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 && prevUV.y >= 0.0 && prevUV.y <= 1.0;
    float motionMag = length(velocity * vec2(imgSize));

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, uv).r;
        float prevDepth = texture(s_DepthBuffer, prevUV).r;
        if (abs(currentDepth - prevDepth) > 0.02) validHistory = false;
    }

    vec3 historyLight = texture(s_Accumulation, prevUV).rgb;

    if (validHistory) {
        // Calculate 3x3 neighborhood statistics in YCoCg space
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

        // Clamp history sample in YCoCg space
        vec3 historyYCoCg = RGB2YCoCg(historyLight);
        vec3 minCol = mean - 1.25 * stddev;
        vec3 maxCol = mean + 1.25 * stddev;
        historyYCoCg = clamp(historyYCoCg, minCol, maxCol);

        historyLight = YCoCg2RGB(historyYCoCg);
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
    vec2 uv = (vec2(pos) + 0.5) / vec2(imageSize(img_Accumulation));

    vec3 denoisedIrradiance = (u_QualityLevel >= 2) ? imageLoad(img_Bloom_Temp, pos).rgb : imageLoad(img_Accumulation, pos).rgb;

    // Trace primary ray hit to recover surface albedo for remodulation
    vec2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
    vec2 jitteredUV = ((vec2(pos) + jitter) / vec2(imageSize(img_Accumulation))) * 2.0 - 1.0;
    Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0, 1.0)).xyz));
    HitInfo primary = TraceScene(primaryRay);

    vec3 albedo = primary.hit ? primary.albedo : vec3(1.0);
    vec3 combined = primary.hit ? (denoisedIrradiance * albedo) : denoisedIrradiance;

    // Tonemap + gamma
    vec3 mapped = combined / (combined + vec3(1.0));
    vec3 finalColor = pow(mapped, vec3(1.0 / 2.2));

    imageStore(img_Output, pos, vec4(finalColor, 1.0));

    // Extract bloom seeds if enabled
    if (u_QualityLevel >= 1) {
        float brightness = dot(combined, vec3(0.2126, 0.7152, 0.0722));
        float threshold = 0.8;
        float knee = 0.15;
        float soft = smoothstep(threshold - knee, threshold + knee, brightness);
        imageStore(img_Bloom, pos, vec4(combined * soft, 1.0));
    }
}

void RunBilateralBlur() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Bloom_Temp);
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    vec3 centerColor = imageLoad(img_Accumulation, pos).rgb;
    
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        imageStore(img_Bloom_Temp, pos, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    vec2 uv = (vec2(pos) + 0.5) / vec2(imgSize);
    float centerDepth = texture(s_DepthBuffer, uv).r;
    if (centerDepth >= 1.0) { 
        imageStore(img_Bloom_Temp, pos, vec4(centerColor, 1.0));
        return;
    }

    vec3 centerNormal = texture(s_NormalBuffer, uv).rgb * 2.0 - 1.0;
    float centerLuma = GetLuminance(centerColor);

    // Compute local luminance variance across a 3x3 window for edge stopping
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

    // 5x5 À-Trous kernel weights [0.0625, 0.25, 0.375, 0.25, 0.0625]
    const float kernel[3] = float[](0.375, 0.25, 0.0625);
    vec3 totalColor = vec3(0.0);
    float totalWeight = 0.0;
    int step = max(1, u_StepSize);

    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            ivec2 samplePos = clamp(pos + ivec2(x, y) * step, ivec2(0), imgSize - ivec2(1));

            vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
            vec2 sampleUV = (vec2(samplePos) + 0.5) / vec2(imgSize);

            float neighborDepth = texture(s_DepthBuffer, sampleUV).r;
            vec3 neighborNormal = texture(s_NormalBuffer, sampleUV).rgb * 2.0 - 1.0;
            float neighborLuma = GetLuminance(neighborColor);

            // 1. Spatial weight (B-spline filter)
            float spatialWeight = kernel[abs(x)] * kernel[abs(y)];

            // 2. Normal weight
            float normalWeight = pow(max(0.0, dot(centerNormal, neighborNormal)), 32.0);

            // 3. Depth weight
            float depthDiff = abs(centerDepth - neighborDepth);
            float depthWeight = exp(-depthDiff * 100.0);

            // 4. Luminance Variance Weight (Edge-stopping weight)
            float lumaDiff = abs(centerLuma - neighborLuma);
            float lumaWeight = exp(-lumaDiff / (lumaVar * 4.0 + 0.001));

            float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    }

    vec3 finalFiltered = (totalWeight > 0.0001) ? (totalColor / totalWeight) : centerColor;
    imageStore(img_Bloom_Temp, pos, vec4(finalFiltered, 1.0));
}

void RunResolve() 
{
    ivec2 displayPos = ivec2(gl_GlobalInvocationID.xy);
    vec2 displaySize = vec2(imageSize(img_FinalDisplay));
    vec2 uv = vec2(displayPos) / displaySize;

    vec3 color = texture(s_Output, uv).rgb;
    vec3 bloom = texture(s_Bloom, uv).rgb;
    
    vec3 combined = color + bloom;
    
    vec3 mapped = combined / (combined + vec3(1.0));
    vec3 finalColor = pow(mapped, vec3(1.0 / 2.2));

    imageStore(img_FinalDisplay, displayPos, vec4(finalColor, 1.0));
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
        case 9: GenerateMaterialMaps(); break;
    }
}