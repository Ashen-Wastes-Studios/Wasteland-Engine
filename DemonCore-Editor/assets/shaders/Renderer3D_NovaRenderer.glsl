// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Bindings
layout(rgba32f, binding = 0) uniform image2D img_Output;
layout(rgba32f, binding = 1) uniform image2D img_Accumulation;
layout(rgba32f, binding = 2) uniform image2D img_Bloom;

struct RayTracingInstance 
{
    mat4 InvTransform;      
    mat4 WorldTransform;    
    vec4 Albedo;            
    vec4 MaterialParams;    
    vec4 Min;
    vec4 Max;
    vec4 Emission; // xyz = color, w = intensity
};

layout(std430, binding = 1) buffer SceneInstances 
{
    RayTracingInstance Instances[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform int u_InstanceCount; 
uniform int u_FrameIndex;
uniform int u_SamplesPerPixel;
uniform int u_PassID;

struct Ray { vec3 Origin; vec3 Direction; };

// --- Utility Functions ---

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

void RunTraceAndDenoise() 
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    vec3 accumulatedLight = vec3(0.0);

    for (int s = 0; s < u_SamplesPerPixel; s++) 
    {
        // Tent filter for smoother anti-aliasing
        float r1 = hash();
        float r2 = hash();
        float dx = (r1 < 0.5) ? sqrt(2.0 * r1) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * r1);
        float dy = (r2 < 0.5) ? sqrt(2.0 * r2) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * r2);
        vec2 jitter = vec2(dx, dy) * 0.5; // Offset by half a pixel
        
        // This shifts the sample point slightly within the pixel bounds
        vec2 uv = ((vec2(pixelCoords) + jitter) / vec2(imgSize)) * 2.0 - 1.0;

        Ray currentRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(uv, 1.0, 1.0)).xyz));
        vec3 throughput = vec3(1.0);
        vec3 incomingLight = vec3(0.0);

        for(int bounce = 0; bounce < 5; bounce++) 
        {
            float closestHit = 1e20;
            int hitIndex = -1;
            vec3 hitNormal, hitPoint, hitAlbedo;

            for(int i = 0; i < u_InstanceCount; i++) 
            {
                RayTracingInstance inst = Instances[i];
                Ray localRay;
                localRay.Origin = (inst.InvTransform * vec4(currentRay.Origin, 1.0)).xyz;
                localRay.Direction = (inst.InvTransform * vec4(currentRay.Direction, 0.0)).xyz;
                
                if (!RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) continue;

                vec3 localNormal;
                float tLocal = -1.0;
                uint type = uint(inst.MaterialParams.z);
                    
                if (type == 0) tLocal = HitCube(localRay, localNormal);
                else if (type == 1) tLocal = HitSphere(localRay, inst.MaterialParams.w, localNormal);
                    
                if (tLocal > 0.0) 
                {
                    vec3 worldHit = (inst.WorldTransform * vec4(localRay.Origin + tLocal * localRay.Direction, 1.0)).xyz;
                    float tWorld = distance(currentRay.Origin, worldHit);
                    if (tWorld < closestHit) 
                    {
                        closestHit = tWorld;
                        hitIndex = i;
                        hitNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                        hitPoint = worldHit;
                        hitAlbedo = inst.Albedo.rgb;
                    }
                }
            }

            // Handle Miss or Hit
            if (hitIndex == -1) { 
                incomingLight += throughput * vec3(0.0, 0.0, 0.0); // Sky
                break; 
            }

            vec3 hitEmission = Instances[hitIndex].Emission.xyz * Instances[hitIndex].Emission.w;
            incomingLight += throughput * hitEmission;

            // Bounce
            throughput *= hitAlbedo;
            currentRay.Origin = hitPoint + hitNormal * 0.05;
            currentRay.Direction = random_in_hemisphere(hitNormal);
        }
        accumulatedLight += incomingLight;
    }

    vec3 finalIncomingLight = accumulatedLight / float(u_SamplesPerPixel);
    vec4 incomingColor = vec4(finalIncomingLight, 1.0);

    // --- ACCUMULATION BUFFER UPDATE ---
    vec4 resultColor;
    if (u_FrameIndex == 0) resultColor = incomingColor;
    else {
        vec4 prevAccum = imageLoad(img_Accumulation, pixelCoords);
        resultColor = mix(prevAccum, incomingColor, 1.0 / float(u_FrameIndex + 1));
    }
    imageStore(img_Accumulation, pixelCoords, resultColor);

    // --- TONE MAPPING & GAMMA CORRECTION ---
    vec3 mappedColor = resultColor.rgb / (resultColor.rgb + vec3(1.0));
    vec3 finalOutput = pow(mappedColor, vec3(1.0 / 2.2));
        
    imageStore(img_Output, pixelCoords, vec4(finalOutput, 1.0));
}

void RunBloomThreshold() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = imageLoad(img_Output, pos);
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    
    // Only keep bright pixels
    imageStore(img_Bloom, pos, (brightness > 1.0) ? color : vec4(0.0));
}

void RunBloomBlur() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 sum = vec4(0.0);
    int count = 0;
    for(int x = -2; x <= 2; x++) {
        for(int y = -2; y <= 2; y++) {
            sum += imageLoad(img_Bloom, pos + ivec2(x, y));
            count++;
        }
    }
    imageStore(img_Bloom, pos, sum / float(count));
}

void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 scene = imageLoad(img_Output, pos);
    vec4 bloom = imageLoad(img_Bloom, pos);
    imageStore(img_Output, pos, scene + (bloom * 0.5));
}

void main()
{
    switch(u_PassID) 
    {
        case 0: RunTraceAndDenoise(); break;
        case 1: RunBloomThreshold(); break;
        case 2: RunBloomBlur(); break;
        case 3: RunComposite(); break;
    }
}