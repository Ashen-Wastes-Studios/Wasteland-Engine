// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

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
    float theta = 2.0 * 3.14159 * u;
    float phi = acos(2.0 * v - 1.0);
    vec3 dir = vec3(sin(phi)*cos(theta), sin(phi)*sin(theta), cos(phi));
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
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
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
    float t = (-b - sqrt(discriminant)) / (2.0 * a);
    if (t < 0.0) t = (-b + sqrt(discriminant)) / (2.0 * a);
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
        
        // Ensure distance check here too
        float distToObj = distance(u_CameraPosition, inst.WorldTransform[3].xyz);
        if (distToObj > inst.MaxDistance) continue;

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

void GetVarianceClippingBounds(ivec2 center, out vec3 minCol, out vec3 maxCol) 
{
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    float n = 9.0;

    for(int x = -1; x <= 1; x++) {
        for(int y = -1; y <= 1; y++) {
            vec3 col = imageLoad(img_Output, center + ivec2(x, y)).rgb;
            m1 += col;
            m2 += col * col;
        }
    }
    vec3 mean = m1 / n;
    vec3 stdDev = sqrt(max(vec3(0.0), (m2 / n) - (mean * mean)));
    minCol = mean - 2.0 * stdDev;
    maxCol = mean + 2.0 * stdDev;
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
    // When the camera is stationary, advance the Halton sequence each frame.
    // When the camera is moving, freeze the jitter so the history does not reproject incorrectly.
    int effectiveIndex = (cameraMoved > 0.5) ? 0 : frameIndex;
    float x = Halton(effectiveIndex % 16 + 1, 2);
    float y = Halton(effectiveIndex % 16 + 1, 3);
    return vec2(x, y) - 0.5;
}

float CalculateLightPDF(float dist, float area) { return 1.0 / area; }
float CalculateBSDFPDF(float NdotL) { return max(NdotL / PI, 0.0001); }

vec3 ReconstructWorldPos(vec2 uv, float depth) 
{
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

const mat3 Gx = mat3(-1, 0, 1, -2, 0, 2, -1, 0, 1);
const mat3 Gy = mat3(-1, -2, -1, 0, 0, 0, 1, 2, 1);

float GetLuminance(vec3 col) {
    return dot(col, vec3(0.2126, 0.7152, 0.0722));
}

vec2 CalculateUV(vec3 localPos, RayTracingInstance inst) {
    vec3 size = inst.Max.xyz - inst.Min.xyz;
    vec3 normalizedPos = (localPos - inst.Min.xyz) / size;

    // 0 = Cube, 1 = Sphere (based on your logic)
    if (int(inst.MaterialParams.z) == 0) {
        // Cube UV mapping - use localPos directly (range [-0.5, 0.5]) for proper 1:1 texture mapping
        vec3 absLocal = abs(localPos);
        float maxAxis = max(max(absLocal.x, absLocal.y), absLocal.z);

        vec2 uv;
        vec2 uvScale;
        if (maxAxis == absLocal.x) {
            // +/- X face: use Z and Y
            float signX = sign(localPos.x);
            uv = vec2(localPos.z * signX + 0.5, localPos.y + 0.5);
            uvScale = vec2(inst.TextureScale.z, inst.TextureScale.y);
        } else if (maxAxis == absLocal.y) {
            // +/- Y face: use X and Z
            float signY = sign(localPos.y);
            uv = vec2(localPos.x * signY + 0.5, localPos.z + 0.5);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.w);
        } else {
            // +/- Z face: use X and Y
            float signZ = sign(localPos.z);
            uv = vec2(localPos.x * signZ + 0.5, localPos.y + 0.5);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.y);
        }

        return uv * uvScale;
    } else {
        // Sphere UV logic - FIXED VERSION
        // For spheres, we need to calculate proper spherical coordinates
        float phi = atan(localPos.z, localPos.x);  // atan2(z, x)
        float theta = acos(clamp(localPos.y, -1.0, 1.0));  // acos(y) for latitude

        // Convert to UV coordinates [0,1]
        float u = phi / (2.0 * PI) + 0.5;
        float v = theta / PI;

        return vec2(u, v) * inst.TextureScale.xy;
    }
}

void RunVisibilityAndVelocity() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imageSize(img_Output));
    
    float depth = texture(s_DepthBuffer, uv).r;
    if (depth >= 1.0) { // Sky/Background
        imageStore(img_Velocity, pixelCoords, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    // Reconstruct world position from depth
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    worldPos /= worldPos.w;

    // Reproject to previous frame
    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    prevClipPos /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5 + 0.5;

    // Calculate motion vector (Delta)
    vec2 velocity = uv - prevUV;
    
    // Store in your Velocity Buffer (binding 5)
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

void ComputeTangentFrame(vec3 localHitPos, vec3 localNormal, int shapeType, out vec3 T, out vec3 B)
{
    vec3 absN = abs(localNormal);
    if (shapeType == 0)
    {
        if (absN.x > absN.y && absN.x > absN.z)
        { T = vec3(0.0, 0.0, sign(localNormal.x)); B = vec3(0.0, 1.0, 0.0); }
        else if (absN.y > absN.z)
        { T = vec3(sign(localNormal.y), 0.0, 0.0); B = vec3(0.0, 0.0, 1.0); }
        else
        { T = vec3(sign(localNormal.z), 0.0, 0.0); B = vec3(0.0, 1.0, 0.0); }
    }
    else
    {
        vec3 r = normalize(localHitPos);
        T = normalize(cross(vec3(0.0, 1.0, 0.0), r));
        B = cross(r, T);
    }
}

void ApplyPOM(RayTracingInstance inst, vec3 localHitPos, vec3 localNormal,
              vec3 localViewDir, inout vec2 uv, inout vec3 worldPos,
              inout vec3 worldNormal, out float selfOcclusion)
{
    selfOcclusion = 1.0;
    float dispScale = inst.DisplacementParams.x;
    float bumpStrength = inst.DisplacementParams.y;
    if (inst.TextureID < 0 || inst.TextureID >= 32)
        return;
    if (dispScale < 0.001 && bumpStrength < 0.001)
        return;

    int shapeType = int(inst.MaterialParams.z);
    vec2 uvScale = inst.TextureScale.xy;
    vec3 T, B;
    ComputeTangentFrame(localHitPos, localNormal, shapeType, T, B);
    int texID = inst.TextureID;

    float eps = 1.0 / 256.0;
    float maxDisplacement = max(dispScale, bumpStrength);

    // ---------- Parallax Occlusion Mapping ----------
    float cosAngle = max(dot(-localViewDir, localNormal), 0.05);
    int baseLayers = int(mix(6.0, 32.0, float(u_QualityLevel) / 3.0));
    int numLayers = int(clamp(float(baseLayers) / cosAngle, float(baseLayers), 96.0));

    // UV offset per unit of height displacement (view direction projected onto surface plane)
    float vnDot = dot(localViewDir, localNormal);
    vec2 viewUVDir = vec2(dot(localViewDir, T), dot(localViewDir, B));
    vec2 uvOffsetPerHeight = -viewUVDir / max(abs(vnDot), 0.05) * uvScale;

    // Ray march from top of height volume (h=1) down to bottom (h=0)
    float dHeight = 1.0 / float(numLayers);
    vec2 dUV = uvOffsetPerHeight * dHeight * maxDisplacement;

    vec2 prevUV = uv;
    float prevLayerH = 1.0;
    float currLayerH = 1.0;

    float prevTexH = GetLuminance(texture(u_SceneTextures[texID], uv).rgb);
    vec2 currUV = uv;
    float currTexH = prevTexH;

    // March through height layers
    for (int i = 0; i < numLayers; i++)
    {
        prevUV = currUV;
        prevLayerH = currLayerH;
        prevTexH = currTexH;

        currUV += dUV;
        currLayerH -= dHeight;
        currTexH = GetLuminance(texture(u_SceneTextures[texID], currUV).rgb);

        if (currTexH > currLayerH)
            break;
    }

    // Linear interpolation refinement between last two layers
    float d1 = prevLayerH - prevTexH;
    float d2 = currTexH - currLayerH;
    float weight = d1 / max(d1 + d2, 0.0001);
    vec2 pomUV = mix(prevUV, currUV, weight);

    // Silhouette edge: clamp POM offset to prevent UV bleed across faces
    vec2 uvDelta = pomUV - uv;
    float maxOffset = 0.15;
    if (length(uvDelta) > maxOffset)
        uvDelta = normalize(uvDelta) * maxOffset;
    pomUV = uv + uvDelta;

    // Apply POM-corrected UV
    uv = pomUV;

    // Position displacement at POM intersection (optional)
    float pomHeight = GetLuminance(texture(u_SceneTextures[texID], uv).rgb);
    if (dispScale > 0.001)
        worldPos += worldNormal * pomHeight * dispScale;

    // ---------- Self-Occlusion Shadow March ----------
    // March from the POM-corrected point toward the surface normal direction
    // to detect if surrounding higher geometry blocks light
    if (dispScale > 0.001 || bumpStrength > 0.5)
    {
        vec3 shadowDir = localNormal;
        float shDot = dot(shadowDir, localNormal);
        vec2 shUVDir = vec2(dot(shadowDir, T), dot(shadowDir, B));
        vec2 shUVStep = shUVDir / max(abs(shDot), 0.1) * uvScale * eps * 8.0;

        float shadowH = pomHeight;
        vec2 shUV = uv;
        float occlusion = 0.0;
        int shadowSteps = (u_QualityLevel >= 2) ? 8 : 4;

        for (int s = 1; s <= shadowSteps; s++)
        {
            shUV += shUVStep * float(s);
            float sampleH = GetLuminance(texture(u_SceneTextures[texID], shUV).rgb);
            float expectedH = shadowH - float(s) * 0.1;
            occlusion += max(sampleH - expectedH, 0.0);
        }
        selfOcclusion = clamp(1.0 - occlusion * 3.0, 0.15, 1.0);
    }

    // ---------- Normal Perturbation at POM-corrected UV ----------
    if (bumpStrength > 0.001)
    {
        float h0 = GetLuminance(texture(u_SceneTextures[texID], uv).rgb);
        float hU = GetLuminance(texture(u_SceneTextures[texID], uv + vec2(eps, 0.0) * uvScale).rgb);
        float hV = GetLuminance(texture(u_SceneTextures[texID], uv + vec2(0.0, eps) * uvScale).rgb);
        float dhdu = (hU - h0) / eps;
        float dhdv = (hV - h0) / eps;

        vec3 localPerturbedNormal = normalize(localNormal - (dhdu * T + dhdv * B) * bumpStrength);
        mat3 normalMatrix = transpose(mat3(inst.InvTransform));
        worldNormal = normalize(normalMatrix * localPerturbedNormal);
    }
}

HitInfo TraceScene(Ray ray) {
    HitInfo info;
    info.hit = false;
    info.t = 1e20;
    info.occlusion = 1.0;

    for (int i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];
        float distToObj = distance(u_CameraPosition, inst.WorldTransform[3].xyz);
        if (distToObj > inst.MaxDistance) continue;

        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(ray.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(ray.Direction, 0.0)).xyz;

        if (!RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) continue;
        vec3 localNormal;
        float tLocal = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
        if (tLocal > 0.0) {
            vec3 worldHit = (inst.WorldTransform * vec4(localRay.Origin + tLocal * localRay.Direction, 1.0)).xyz;
            float tWorld = distance(ray.Origin, worldHit);
            if (tWorld < info.t) {
                info.t = tWorld;
                info.hit = true;
                info.worldPos = worldHit;
                vec3 localHitPos = localRay.Origin + tLocal * localRay.Direction;
                info.normal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);

                vec2 uv = CalculateUV(localHitPos, inst);
                float pomOcclusion = 1.0;
                ApplyPOM(inst, localHitPos, localNormal, localRay.Direction, uv, info.worldPos, info.normal, pomOcclusion);
                info.occlusion = pomOcclusion;

                vec3 sampledAlbedo = vec3(1.0);
                if (inst.TextureID >= 0 && inst.TextureID < 32)
                    sampledAlbedo = texture(u_SceneTextures[inst.TextureID], uv).rgb;

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
    vec3 F0 = mix(vec3(0.04), h.albedo, h.metal);
    vec3 diffuseColor = h.albedo * (1.0 - h.metal);
    vec3 directLight = vec3(0.0);

    int lightsSampled = 0;
    for (int i = 0; i < u_InstanceCount; i++) {
        if (Instances[i].Emission.w > 0.0) {
            if (lightsSampled >= u_MaxLights) break;
            lightsSampled++;
            vec3 lightPos = Instances[i].WorldTransform[3].xyz;
            vec3 dirToLight = normalize(lightPos - h.worldPos);
            float distToLight = length(lightPos - h.worldPos);
            Ray shadowRay = Ray(h.worldPos + h.normal * 0.001, dirToLight);
            if (!IsOccluded(shadowRay, distToLight)) {
                float NdotL = max(dot(h.normal, dirToLight), 0.0);
                vec3 H = normalize(V + dirToLight);
                vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
                float D = DistributionGGX(max(dot(h.normal, H), 0.0), h.rough);
                float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
                vec3 brdf = (vec3(1.0) - F) * (1.0 - h.metal) * h.albedo / PI + (D * G * F) / max(4.0 * max(dot(h.normal, V), 0.0) * NdotL, 0.001);
                vec3 contrib = brdf * Instances[i].Emission.xyz * Instances[i].Emission.w * NdotL;

                float brightness = dot(contrib, vec3(0.2126, 0.7152, 0.0722));
                if (brightness > 10.0) contrib *= (10.0 / brightness);
                directLight += contrib;
            }
        }
    }

    // Emission from the surface itself
    directLight += h.emission;

    // Ambient sky bounce (diffuse) — attenuated by POM self-occlusion
    float skyT = 0.5 * (h.normal.y + 1.0);
    vec3 skyAmbient = mix(u_SkyBottomColor, u_SkyTopColor, skyT) * 0.15;
    directLight += diffuseColor * skyAmbient * h.occlusion;

    // Simple environment reflection for metallic materials
    if (h.metal > 0.0) {
        vec3 R = reflect(-V, h.normal);
        vec3 envColor = GetSkyColor(R);
        float envGloss = pow(1.0 - h.rough, 2.0);
        vec3 F0 = mix(vec3(0.04), h.albedo, h.metal);
        directLight += F0 * envColor * envGloss * h.metal;
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

        // Spawn indirect rays from the primary hit point
        vec3 diffuseColor = primary.albedo * (1.0 - primary.metal);
        vec3 F0 = mix(vec3(0.04), primary.albedo, primary.metal);
        // For non-metals: reflect diffusely. For metals: reflect specularly (colored F0)
        vec3 indirectReflectance = mix(diffuseColor, F0, primary.metal);
        for (int r = 0; r < u_IndirectRays; r++) {
            seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000) + uint((r + 1) * 7919);
            vec3 indirectDir = random_in_hemisphere(primary.normal);
            Ray indirectRay = Ray(primary.worldPos + primary.normal * 0.001, indirectDir);

            HitInfo indirect = TraceScene(indirectRay);
            if (indirect.hit) {
                // Gather direct lighting at the indirect hit point (one-bounce indirect)
                vec3 indirectV = normalize(-indirectDir);
                vec3 bouncedLight = ComputeDirectLighting(indirect, indirectV);
                indirectLight += indirectReflectance * bouncedLight;
            } else {
                // Indirect ray hit sky
                indirectLight += indirectReflectance * GetSkyColor(indirectDir);
            }
        }
        if (u_IndirectRays > 0)
            indirectLight /= float(u_IndirectRays);
        indirectLight *= primary.occlusion;
    } else {
        // Primary ray hit sky — no surface, just sky color as direct
        directLight = GetSkyColor(primaryRay.Direction);
    }

    directLight = clamp(directLight, vec3(0.0), vec3(10.0));
    indirectLight = clamp(indirectLight, vec3(0.0), vec3(5.0));

    imageStore(img_Output, pixelCoords, vec4(directLight, 1.0));
    imageStore(img_Bloom, pixelCoords, vec4(indirectLight, 1.0));
}

void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Accumulation);
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imgSize);

    // Current frame's raw indirect irradiance
    vec3 currentIndirect = imageLoad(img_Bloom, pixelCoords).rgb;

    // Read velocity to find previous frame position
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 prevUV = uv - velocity;

    // Reject history if reprojected position is off-screen
    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 && prevUV.y >= 0.0 && prevUV.y <= 1.0;

    // Motion vector magnitude — fast camera motion reduces history weight
    float motionMag = length(velocity * vec2(imgSize));
    if (motionMag > 4.0) validHistory = false;

    // Depth-based history validation
    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, uv).r;
        float prevDepth = texture(s_DepthBuffer, prevUV).r;
        float depthDiff = abs(currentDepth - prevDepth);
        if (depthDiff > 0.02) validHistory = false;
    }

    // Sample history indirect from reprojected position
    vec3 historyIndirect = texture(s_Accumulation, prevUV).rgb;

    // Variance clipping — reject history that differs too much from current neighborhood
    if (validHistory) {
        vec3 m1 = vec3(0.0);
        vec3 m2 = vec3(0.0);
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                ivec2 sp = clamp(pixelCoords + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                vec3 s = imageLoad(img_Bloom, sp).rgb;
                m1 += s;
                m2 += s * s;
            }
        }
        vec3 mean = m1 / 9.0;
        vec3 stddev = sqrt(max(vec3(0.0), m2 / 9.0 - mean * mean));
        vec3 minCol = mean - 1.25 * stddev;
        vec3 maxCol = mean + 1.25 * stddev;
        historyIndirect = clamp(historyIndirect, minCol, maxCol);
    }

    // Blend: motion-aware alpha — more motion = trust current frame more
    float motionAlpha = clamp(motionMag * 0.1, 0.0, 0.8);
    float alpha = validHistory ? max(u_AccumulationAlpha, motionAlpha) : 1.0;
    vec3 result = mix(historyIndirect, currentIndirect, alpha);

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

    // Composite blurred bloom onto the tonemapped output
    vec3 existing = imageLoad(img_Output, pos).rgb;
    vec3 finalColor = existing + sum.rgb;
    imageStore(img_Output, pos, vec4(finalColor, 1.0));
}

void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pos) + 0.5) / vec2(imageSize(img_Output));

    // Direct lighting from pass 1
    vec3 direct = imageLoad(img_Output, pos).rgb;

    // Temporally filtered indirect GI from pass 2
    vec3 indirect = texture(s_Accumulation, uv).rgb;

    vec3 combined = direct + indirect;

    // Tonemap + gamma
    vec3 mapped = combined / (combined + vec3(1.0));
    vec3 finalColor = pow(mapped, vec3(1.0 / 2.2));

    imageStore(img_Output, pos, vec4(finalColor, 1.0));

    // Extract bloom seeds into img_Bloom (for Medium+ quality bloom passes)
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
    ivec2 imgSize = imageSize(img_Bloom);
    vec2 uv = (vec2(pos) + 0.5) / vec2(imgSize);

    vec3 centerIndirect = imageLoad(img_Bloom, pos).rgb;
    float centerDepth = texture(s_DepthBuffer, uv).r;

    // Skip sky pixels
    if (centerDepth >= 1.0) {
        imageStore(img_Bloom_Temp, pos, vec4(centerIndirect, 1.0));
        return;
    }

    vec3 totalColor = vec3(0.0);
    float totalWeight = 0.0;

    // Kernel size based on quality: Medium = 1 (3x3), High/Ultra = 2 (5x5)
    int radius = (u_QualityLevel >= 2) ? 2 : 1;
    float depthSigma = 30.0;
    float colorSigma = 2.0;

    for (int x = -radius; x <= radius; x++) {
        for (int y = -radius; y <= radius; y++) {
            ivec2 samplePos = pos + ivec2(x, y);
            if (samplePos.x < 0 || samplePos.x >= imgSize.x ||
                samplePos.y < 0 || samplePos.y >= imgSize.y) continue;

            vec3 neighborIndirect = imageLoad(img_Bloom, samplePos).rgb;
            vec2 sampleUV = (vec2(samplePos) + 0.5) / vec2(imgSize);
            float neighborDepth = texture(s_DepthBuffer, sampleUV).r;

            float d_color = length(centerIndirect - neighborIndirect);
            float d_depth = abs(centerDepth - neighborDepth);

            float weight = exp(-(d_color * d_color) / colorSigma - (d_depth * d_depth) * depthSigma);
            totalColor += neighborIndirect * weight;
            totalWeight += weight;
        }
    }
    imageStore(img_Bloom_Temp, pos, vec4(totalColor / max(totalWeight, 0.001), 1.0));
}

void RunResolve() 
{
    ivec2 displayPos = ivec2(gl_GlobalInvocationID.xy);
    vec2 displaySize = vec2(imageSize(img_FinalDisplay));
    vec2 uv = vec2(displayPos) / displaySize;

    vec3 color = texture(s_Output, uv).rgb; // Raw, un-bloomed, accumulated path trace
    vec3 bloom = texture(s_Bloom, uv).rgb;  // Sample your bloom texture separately
    
    vec3 combined = color + bloom; // Combine them here at the very end
    
    // Tonemapping
    vec3 mapped = combined / (combined + vec3(1.0));
    vec3 finalColor = pow(mapped, vec3(1.0 / 2.2));

    imageStore(img_FinalDisplay, displayPos, vec4(finalColor, 1.0));
}

void GenerateMaterialMaps() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(img_MaterialPacked);
    
    // Sample 3x3 grid
    float I[3][3];
    for(int i = -1; i <= 1; i++) {
        for(int j = -1; j <= 1; j++) {
            ivec2 samplePos = clamp(pos + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec2 uv = (vec2(samplePos) + 0.5) / vec2(size);
            I[i+1][j+1] = GetLuminance(texture(s_InputAlbedo, uv).rgb);
        }
    }

    // Compute Sobel Gradients
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

    // Pack into 0.0 to 1.0 range
    vec2 packedNormal = normal.xy * 0.5 + 0.5;
    
    // AO and Roughness
    float ao = 1.0 - (length(vec2(resX, resY)) * 5.0 * u_AOIntensity);
    float rough = clamp((1.0 - I[1][1]) + u_RoughnessBias, 0.0, 1.0);
    
    // PACK: R=NormalX, G=NormalY, B=Roughness, A=AO
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