// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, binding = 0) uniform image2D img_Output;

struct RayTracingInstance {
    mat4 InvTransform; // Holds the Inverse Transform Matrix from C++
    vec4 Color;
    vec4 Properties;   // x = Type (0=Cube, 1=Sphere), y = Radius, z = EntityID, w = Unused
};

// Explicit column_major forces the matrix memory architecture to mirror GLM's buffer layout configuration
layout(std430, binding = 1) buffer SceneInstances
{
    layout(column_major) RayTracingInstance Instances[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform int u_InstanceCount; 

struct Ray { vec3 Origin; vec3 Direction; };

// Local AABB Cube Intersection (-0.5 to 0.5 bounds) with safe division handling
float HitCube(Ray localRay, out vec3 outNormal)
{
    // Prevent division by zero if a ray axis is perfectly parallel to an AABB wall plane
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
    
    // Calculate local normal based on which face was hit
    vec3 hitPoint = localRay.Origin + tNear * localRay.Direction;
    vec3 absHit = abs(hitPoint);
    float eps = 0.001;
    
    if (absHit.x > 0.5 - eps) outNormal = vec3(sign(hitPoint.x), 0.0, 0.0);
    else if (absHit.y > 0.5 - eps) outNormal = vec3(0.0, sign(hitPoint.y), 0.0);
    else outNormal = vec3(0.0, 0.0, sign(hitPoint.z));
    
    return tNear;
}

// Local Unit Sphere Intersection (Centered at 0,0,0)
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

    // Convert pixel to Normalized Device Coordinates (-1 to 1)
    vec2 ndc = (vec2(pixelCoords) / vec2(imgSize)) * 2.0 - 1.0;
    
    // Decoupled perspective vector math: Projects a clean ray vector outward from the camera space position
    vec4 target = u_InverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize(target.xyz - u_CameraPosition);
    
    Ray worldRay = Ray(u_CameraPosition, rayDir);
    vec4 pixelColor = vec4(0.1, 0.15, 0.2, 1.0); // Ambient slate blue clear background color
    
    float closestHit = 1e20;
    
    for(int i = 0; i < u_InstanceCount; i++) 
    {
        RayTracingInstance inst = Instances[i];
        uint type = uint(inst.Properties.x);
        float baseRadius = inst.Properties.y;
        
        // Transform the world space ray into the object's clean LOCAL space bounds
        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(worldRay.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(worldRay.Direction, 0.0)).xyz; 
        
        float tLocal = -1.0;
        vec3 localNormal = vec3(0.0);
        
        if (type == 0) // Cube Path
        {
            tLocal = HitCube(localRay, localNormal);
        }
        else if (type == 1) // Sphere Path
        {
            tLocal = HitSphere(localRay, baseRadius, localNormal);
        }
        
        if (tLocal > 0.0)
        {
            // Compute real world depth space metrics to correctly resolve overlapping geometries
            vec3 localHitPoint = localRay.Origin + tLocal * localRay.Direction;
            vec3 worldHitPoint = (inverse(inst.InvTransform) * vec4(localHitPoint, 1.0)).xyz;
            float tWorld = distance(worldRay.Origin, worldHitPoint);
            
            if (tWorld < closestHit)
            {
                closestHit = tWorld;
                
                // Convert surface normal back into world coordinates using the inverse transpose method
                vec3 worldNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                
                // Classic directional light source calculation
                vec3 lightDir = normalize(vec3(1.0, 2.0, 0.5));
                float lightIntensity = max(dot(worldNormal, lightDir), 0.1);
                
                pixelColor = inst.Color * lightIntensity;
            }
        }
    }
    
    imageStore(img_Output, pixelCoords, pixelColor);
}