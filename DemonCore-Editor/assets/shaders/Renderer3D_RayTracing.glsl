// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, binding = 0) uniform image2D img_Output;

struct RayTracingInstance {
    mat4 InvTransform;   // 64 bytes
    mat4 WorldTransform; // 64 bytes
    vec4 Color;          // 16 bytes
    vec4 Properties;     // 16 bytes (x = Type, y = Radius)
};

layout(std430, binding = 1) buffer SceneInstances
{
    layout(column_major) RayTracingInstance Instances[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform int u_InstanceCount; 

struct Ray { vec3 Origin; vec3 Direction; };

float HitCube(Ray localRay, out vec3 outNormal)
{
    vec3 safeDir = vec3(
        localRay.Direction.x == 0.0 ? 1e-6 : localRay.Direction.x,
        localRay.Direction.y == 0.0 ? 1e-6 : localRay.Direction.y,
        localRay.Direction.z == 0.0 ? 1e-6 : localRay.Direction.z
    );

    vec3 tMin = (vec3(-0.5) - localRay.Origin) / safeDir;
    vec3 tMax = (vec3(0.5) - localRay.Origin) / safeDir;
    
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar = min(min(t2.x, t2.y), t2.z);
    
    if (tNear > tFar || tFar < 0.0) return -1.0;
    
    vec3 hitPoint = localRay.Origin + tNear * localRay.Direction;
    vec3 absHit = abs(hitPoint);
    float eps = 0.001;
    
    if (absHit.x > 0.5 - eps) outNormal = vec3(sign(hitPoint.x), 0.0, 0.0);
    else if (absHit.y > 0.5 - eps) outNormal = vec3(0.0, sign(hitPoint.y), 0.0);
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
        vec3 localHitPoint = localRay.Origin + t * localRay.Direction;
        outNormal = normalize(localHitPoint); 
        return t;
    }
    return -1.0;
}

void main() 
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    vec2 ndc = (vec2(pixelCoords) / vec2(imgSize)) * 2.0 - 1.0;
    vec4 target = u_InverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 targetPos = target.xyz / target.w;
    vec3 rayDir = normalize(targetPos - u_CameraPosition);
    
    Ray worldRay = Ray(u_CameraPosition, rayDir);
    vec4 pixelColor = vec4(0.25, 0.5, 1.0, 1.0); // Ambient background
    
    float closestHit = 1e20;
    
    for(int i = 0; i < u_InstanceCount; i++) 
    {
        RayTracingInstance inst = Instances[i];
        uint type = uint(inst.Properties.x);
        float baseRadius = inst.Properties.y;
        
        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(worldRay.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(worldRay.Direction, 0.0)).xyz; 
        
        float tLocal = -1.0;
        vec3 localNormal = vec3(0.0); 
        
        if (type == 0) tLocal = HitCube(localRay, localNormal);
        else if (type == 1) tLocal = HitSphere(localRay, baseRadius, localNormal);
        
        if (tLocal > 0.0)
        {
            vec3 localHitPoint = localRay.Origin + tLocal * localRay.Direction;
            vec3 worldHitPoint = (inst.WorldTransform * vec4(localHitPoint, 1.0)).xyz;
            float tWorld = distance(worldRay.Origin, worldHitPoint);
            
            if (tWorld < closestHit)
            {
                closestHit = tWorld;
                vec3 worldNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                vec3 lightDir = normalize(vec3(1.0, 2.0, 0.5));
                float lightIntensity = max(dot(worldNormal, lightDir), 0.1);
                pixelColor = inst.Color * lightIntensity;
            }
        }
    }
    imageStore(img_Output, pixelCoords, pixelColor);
}