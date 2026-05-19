// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Layout match for output image color targets
layout(rgba32f, binding = 0) uniform image2D img_Output;

struct RayTracingInstance {
    mat4 Transform;
    vec4 Color;
    uint Type;
    float Radius;
    int EntityID;
    float padding; // Aligns the struct to a clean 16-byte multiple if needed by your driver
};

// SSBO read layout receiving our batched scene geometry data
layout(std430, binding = 1) buffer SceneInstances
{
    RayTracingInstance Instances[];
};

uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform uint u_InstanceCount;

struct Ray { vec3 Origin; vec3 Direction; };

// Analytical intersection function for a sphere
float HitSphere(Ray ray, vec3 center, float radius) 
{
    vec3 oc = ray.Origin - center;
    float a = dot(ray.Direction, ray.Direction);
    float b = 2.0 * dot(oc, ray.Direction);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b*b - 4.*a*c;
    if (discriminant < 0.0) return -1.0;
    return (-b - sqrt(discriminant)) / (2.0 * a);
}

void main() 
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    // Convert pixel to Normalized Device Coordinates (-1 to 1)
    vec2 ndc = (vec2(pixelCoords) / vec2(imgSize)) * 2.0 - 1.0;
    
    // Calculate Ray Direction out into world-space coordinates
    vec4 target = u_InverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize(target.xyz / target.w - u_CameraPosition);
    
    Ray ray = Ray(u_CameraPosition, rayDir);
    vec4 pixelColor = vec4(0.1, 0.15, 0.2, 1.0); // Ambient background sky color
    
    float closestHit = 1e20;
    
    // Simplistic Linear Scan (Replace this logic with a BVH tree traversal)
    for(uint i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];
        vec3 pos = vec3(inst.Transform[3]); // Extract translation vector
        
        if (inst.Type == 1) { // Sphere type handling
            float t = HitSphere(ray, pos, inst.Radius);
            if (t > 0.0 && t < closestHit) {
                closestHit = t;
                
                // Calculate lighting at point of impact
                vec3 hitPoint = ray.Origin + t * ray.Direction;
                vec3 normal = normalize(hitPoint - pos);
                float light = max(dot(normal, normalize(vec3(1, 2, 0.5))), 0.1);
                
                pixelColor = inst.Color * light;
            }
        }
    }
    
    imageStore(img_Output, pixelCoords, pixelColor);
}