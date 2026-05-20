// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, binding = 0) uniform image2D img_Output;

struct RayTracingInstance {
    mat4 InvTransform;      // 64 bytes
    mat4 WorldTransform;    // 64 bytes
    vec4 Albedo;            // 16 bytes
    vec4 MaterialParams;    // 16 bytes (x: Metallic, y: Roughness, z: Type, w: Radius)
};

layout(std430, binding = 1) buffer SceneInstances
{
    layout(column_major) RayTracingInstance Instances[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform int u_InstanceCount; 

const float PI = 3.14159265359;

struct Ray { vec3 Origin; vec3 Direction; };

// --- PBR Helpers ---
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// --- Geometry Functions ---
float HitCube(Ray localRay, out vec3 outNormal) {
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

float HitSphere(Ray localRay, float radius, out vec3 outNormal) {
    float a = dot(localRay.Direction, localRay.Direction);
    float b = 2.0 * dot(localRay.Origin, localRay.Direction);
    float c = dot(localRay.Origin, localRay.Origin) - (radius * radius);
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) return -1.0;
    float t = (-b - sqrt(discriminant)) / (2.0 * a);
    if (t < 0.0) t = (-b + sqrt(discriminant)) / (2.0 * a);
    if (t > 0.0) {
        outNormal = normalize(localRay.Origin + t * localRay.Direction); 
        return t;
    }
    return -1.0;
}

void main() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    vec2 ndc = (vec2(pixelCoords) / vec2(imgSize)) * 2.0 - 1.0;
    vec4 target = u_InverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize(target.xyz / target.w - u_CameraPosition);
    Ray worldRay = Ray(u_CameraPosition, rayDir);
    
    float closestHit = 1e20;
    vec4 finalColor = vec4(0.25, 0.5, 1.0, 1.0); // Background
    
    for(int i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];
        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(worldRay.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(worldRay.Direction, 0.0)).xyz; 
        
        float tLocal = -1.0;
        vec3 localNormal = vec3(0.0); 
        
        uint type = uint(inst.MaterialParams.z);
        if (type == 0) tLocal = HitCube(localRay, localNormal);
        else if (type == 1) tLocal = HitSphere(localRay, inst.MaterialParams.w, localNormal);
        
        if (tLocal > 0.0) {
            vec3 worldHitPoint = (inst.WorldTransform * vec4(localRay.Origin + tLocal * localRay.Direction, 1.0)).xyz;
            float tWorld = distance(worldRay.Origin, worldHitPoint);
            
            if (tWorld < closestHit) {
                closestHit = tWorld;
                vec3 N = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                vec3 V = normalize(u_CameraPosition - worldHitPoint);
                vec3 L = normalize(vec3(1.0, 2.0, 0.5));
                vec3 H = normalize(V + L);
                
                // Shadowing
                float shadow = 1.0;
                vec3 shadowOrigin = worldHitPoint + (N * 0.005);
                for(int j = 0; j < u_InstanceCount; j++) {
                    RayTracingInstance occ = Instances[j];
                    Ray sRay = Ray((occ.InvTransform * vec4(shadowOrigin, 1.0)).xyz, (occ.InvTransform * vec4(L, 0.0)).xyz);
                    vec3 d; float tS = -1.0;
                    uint tType = uint(occ.MaterialParams.z);
                    if (tType == 0) tS = HitCube(sRay, d);
                    else if (tType == 1) tS = HitSphere(sRay, occ.MaterialParams.w, d);
                    if (tS > 0.0) { shadow = 0.3; break; }
                }

                // PBR Calculation
                vec3 albedo = inst.Albedo.rgb;
                float metallic = inst.MaterialParams.x;
                float roughness = inst.MaterialParams.y;
                vec3 F0 = mix(vec3(0.04), albedo, metallic);
                
                float NDF = DistributionGGX(N, H, roughness);
                float G = GeometrySmith(N, V, L, roughness);
                vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
                
                vec3 kS = F;
                vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
                vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
                
                vec3 radiance = vec3(10.0);
                finalColor = vec4((kD * albedo / PI + specular) * radiance * max(dot(N, L), 0.0) * shadow, 1.0);
            }
        }
    }
    imageStore(img_Output, pixelCoords, finalColor);
}