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
    //float Padding;
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
    int effectiveIndex = (cameraMoved > 0.5) ? frameIndex : 0;
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
    // 0 = Cube, 1 = Sphere (based on your logic)
    if (int(inst.MaterialParams.z) == 0) {
        // Cube UV logic (approximate mapping)
        vec3 absPos = abs(localPos);
        if (absPos.x > absPos.y && absPos.x > absPos.z) return localPos.yz * 0.5 + 0.5; // X-face
        if (absPos.y > absPos.x && absPos.y > absPos.z) return localPos.xz * 0.5 + 0.5; // Y-face
        return localPos.xy * 0.5 + 0.5; // Z-face
    } else {
        // Sphere UV logic
        float phi = atan(localPos.z, localPos.x);
        float theta = asin(localPos.y);
        return vec2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);
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

void RunTraceAndDenoise() 
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    vec3 finalColor = vec3(0.0);

    vec3 totalIncomingLight = vec3(0.0);
    vec3 lastHitPoint = vec3(0.0);
    bool hitAnything = false;

    // Declare this outside the loop so it remains in scope for the sky logic later
    Ray currentRay;

    // Loop through samples
    for (int s = 0; s < u_SamplesPerPixel; s++) 
    {
        // Re-seed for this specific sample
        seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000) + uint(s * 1337);
        
        vec2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
        vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(imgSize)) * 2.0 - 1.0;

        currentRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0, 1.0)).xyz));

        vec3 throughput = vec3(1.0);
        vec3 sampleIncomingLight = vec3(0.0);
        bool sampleHitAnything = false;

        for(int bounce = 0; bounce < 5; bounce++) 
        {
            vec3 surfaceNormal = vec3(0.0);
            float surfaceRough = 0.0;
            bool hasHit = false;

            float closestHit = 1e20;
            int hitIndex = -1;
            vec3 hitNormal = surfaceNormal, hitAlbedo;
            vec3 localHitPoint = vec3(0.0);

            for(int i = 0; i < u_InstanceCount; i++) 
            {
                RayTracingInstance inst = Instances[i];
                float distToObj = distance(u_CameraPosition, inst.WorldTransform[3].xyz);
                if (distToObj > inst.MaxDistance) continue;

                Ray localRay;
                localRay.Origin = (inst.InvTransform * vec4(currentRay.Origin, 1.0)).xyz;
                localRay.Direction = (inst.InvTransform * vec4(currentRay.Direction, 0.0)).xyz;
                
                if (!RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) continue;
                vec3 localNormal;
                float tLocal = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
                if (tLocal > 0.0) 
                {
                    // Calculate hit point and world normal
                    vec3 worldHit = (inst.WorldTransform * vec4(localRay.Origin + tLocal * localRay.Direction, 1.0)).xyz;
                    vec3 worldNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);

                    // 2. Perform Texture/Material Sampling
                    vec2 uv = CalculateUV(localRay.Origin + tLocal * localRay.Direction, inst);
                    vec4 packedData = texture(s_PackedMaterialMap, uv);
                    
                    // Unpack and Transform Normal
                    vec3 localTangentNormal = normalize(packedData.rgb * 2.0 - 1.0);
                    
                    // TRANSFORM TO WORLD SPACE:
                    // Assuming the packed data is in object space, we use the model matrix.
                    // If it's a normal map, you'd usually need a TBN matrix. 
                    // For now, this is a basic world-space conversion:
                    surfaceNormal = normalize((inst.WorldTransform * vec4(localTangentNormal, 0.0)).xyz);
                    surfaceRough = packedData.b; // Use sampled roughness
                    
                    hasHit = true;

                    vec3 albedo = texture(u_SceneTextures[inst.TextureID], uv).rgb;
                    float tWorld = distance(currentRay.Origin, worldHit);
                    if (tWorld < closestHit) 
                    {
                        closestHit = tWorld;
                        hitIndex = i;
                        hitNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                        localHitPoint = worldHit; 
                        hitAlbedo = inst.Albedo.rgb;
                    }
                    finalColor += albedo * hitAlbedo;
                }
            }

            if (hasHit)
            {
                sampleHitAnything = true;
                vec3 V = normalize(-currentRay.Direction);
                float metal = clamp(Instances[hitIndex].MaterialParams.x, 0.0, 1.0);
                float rough = surfaceRough;
                vec3 F0 = mix(vec3(0.04), hitAlbedo, metal);
                vec3 diffuseColor = hitAlbedo * (1.0 - metal);

                vec3 directLight = vec3(0.0);
                for(int i = 0; i < u_InstanceCount; i++) 
                {
                    if(Instances[i].Emission.w > 0.0) 
                    {
                        vec3 lightPos = Instances[i].WorldTransform[3].xyz;
                        vec3 dirToLight = normalize(lightPos - localHitPoint);
                        float distToLight = length(lightPos - localHitPoint);
                        Ray shadowRay = Ray(localHitPoint + hitNormal * 0.001, dirToLight);
                        if(!IsOccluded(shadowRay, distToLight)) 
                        {
                            float NdotL = max(dot(hitNormal, dirToLight), 0.0);
                            vec3 H = normalize(V + dirToLight);
                            vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
                            float D = DistributionGGX(max(dot(hitNormal, H), 0.0), rough);
                            float G = GeometrySmith(hitNormal, V, dirToLight, rough);
                            vec3 brdf = (vec3(1.0) - F) * (1.0 - metal) * hitAlbedo / PI + (D * G * F) / max(4.0 * max(dot(hitNormal, V), 0.0) * NdotL, 0.001);
                            vec3 lightContribution = brdf * Instances[i].Emission.xyz * Instances[i].Emission.w * NdotL;

                            float maxBrightness = 10.0; // Adjust this threshold based on your scene exposure
                            float brightness = dot(lightContribution, vec3(0.2126, 0.7152, 0.0722));
                            if (brightness > maxBrightness) {
                                lightContribution *= (maxBrightness / brightness);
                            }

                            directLight += lightContribution;
                        }
                    }
                }

                sampleIncomingLight += throughput * directLight;
                sampleIncomingLight += throughput * (Instances[hitIndex].Emission.xyz * Instances[hitIndex].Emission.w);
                
                float specularChance = mix(0.2, 0.95, metal);
                if (hash() < specularChance)
                {
                    vec3 reflection = reflect(currentRay.Direction, hitNormal);
                    currentRay.Direction = normalize(mix(reflection, random_in_hemisphere(hitNormal), rough));
                    throughput *= F0;
                }
                else
                {
                    currentRay.Direction = random_in_hemisphere(hitNormal);
                    throughput *= diffuseColor;
                }
                currentRay.Origin = localHitPoint + hitNormal * 0.001;
                
                // Track hit for motion
                if (bounce == 0) { lastHitPoint = localHitPoint; hitAnything = true; }
            }
            else
            {
                float t = 0.5 * (normalize(currentRay.Direction).y + 1.0);
                sampleIncomingLight += throughput * mix(u_SkyBottomColor, u_SkyTopColor, t);
                break;
            }
        }
        totalIncomingLight += sampleIncomingLight;
    }

    vec3 incomingLight = totalIncomingLight / float(u_SamplesPerPixel);
    incomingLight = clamp(incomingLight, vec3(0.0), vec3(10.0));

    imageStore(img_Output, pixelCoords, vec4(incomingLight, 1.0));
}

void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = vec2(pixelCoords) / vec2(imageSize(img_Accumulation));

    // 1. Get history from previous frame
    vec3 history = texture(s_Accumulation, uv).rgb;
    
    // 2. Get current noisy trace
    vec3 current = imageLoad(img_Output, pixelCoords).rgb;

    // 3. Blend: This is what "cleans up" the noise
    // 0.05 is slow, high quality. 0.2 is fast, low quality.
    float alpha = 0.2; 
    vec3 result = mix(history, current, alpha);

    // 4. Store in accumulation so it becomes the "history" for next frame
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
    imageStore(img_Bloom, pos, sum);
}

void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec3 scene = imageLoad(img_Output, pos).rgb;
    vec3 bloom = imageLoad(img_Bloom, pos).rgb;
    
    // We add the bloom, but we do NOT tonemap here.
    // We store this in a temporary buffer or a dedicated "Composition" buffer.
    // For now, let's assume we store it in img_Output
    vec3 combined = scene + bloom; 
    
    imageStore(img_Output, pos, vec4(combined, 1.0));
}

void RunBilateralBlur() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pos) + 0.5) / vec2(imageSize(img_Output));

    // Now you can use pos and uv
    vec3 centerColor = imageLoad(img_Output, pos).rgb;
    vec3 centerNormal = texelFetch(s_NormalBuffer, pos, 0).xyz;
    float centerDepth = texture(s_DepthBuffer, uv).r;
    
    vec3 totalColor = vec3(0.0);
    float totalWeight = 0.0;
    
    float depthSigma = 50.0;
    float normSigma = 20.0;
    
    for(int x = -2; x <= 2; x++) {
        for(int y = -2; y <= 2; y++) {
            ivec2 samplePos = pos + ivec2(x, y);
            
            // Boundary check to prevent reading outside the buffer
            if(samplePos.x < 0 || samplePos.x >= imageSize(img_Output).x || 
               samplePos.y < 0 || samplePos.y >= imageSize(img_Output).y) continue;

            vec3 neighborColor = imageLoad(img_Output, samplePos).rgb;
            vec3 neighborNormal = texelFetch(s_NormalBuffer, samplePos, 0).xyz;
            vec2 sampleUV = (vec2(samplePos) + 0.5) / vec2(imageSize(img_Output));
            float neighborDepth = texture(s_DepthBuffer, sampleUV).r;
            
            float d_color = length(centerColor - neighborColor);
            float d_depth = abs(centerDepth - neighborDepth);
            float d_norm = 1.0 - dot(centerNormal, neighborNormal);
            
            float weight = exp(-(d_color * d_color) - (d_depth * depthSigma) - (d_norm * normSigma));
            
            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    }
    imageStore(img_Output, pos, vec4(totalColor / totalWeight, 1.0));
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
        case 3: RunBloomThreshold(); break;
        case 4: RunBloomBlurHorizontal(); break;
        case 5: RunBloomBlurVertical(); break;
        case 6: RunComposite(); break;
        case 7: RunBilateralBlur(); break;
        case 8: RunResolve(); break;
        case 9: GenerateMaterialMaps(); break;
    }
}