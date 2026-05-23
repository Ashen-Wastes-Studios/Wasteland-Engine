// Ray Tracing Shader for 3D Rendering

#type compute
#version 430 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Bindings
layout(rgba32f, binding = 0) uniform image2D img_Output;
layout(rgba32f, binding = 1) uniform image2D img_Accumulation;
layout(rgba32f, binding = 2) uniform image2D img_Bloom;
layout(rg16f, binding = 5) uniform image2D img_Velocity;
layout(rgba32f, binding = 6) uniform image2D img_FinalDisplay;
layout(binding = 3) uniform sampler2D s_Accumulation;
layout(binding = 4) uniform sampler2D s_DepthBuffer;

struct RayTracingInstance 
{
    mat4 InvTransform;      
    mat4 WorldTransform;    
    vec4 Albedo;            
    vec4 MaterialParams;    
    vec4 Min;
    vec4 Max;
    vec4 Emission; 
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
uniform float u_CameraMoved;
uniform mat4 u_ViewProjection;

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

bool IsOccluded(Ray r, float maxDist) {
    for(int i = 0; i < u_InstanceCount; i++) {
        RayTracingInstance inst = Instances[i];
        Ray localRay;
        localRay.Origin = (inst.InvTransform * vec4(r.Origin, 1.0)).xyz;
        localRay.Direction = (inst.InvTransform * vec4(r.Direction, 0.0)).xyz;
        
        if (RayAABB(localRay, inst.Min.xyz, inst.Max.xyz)) {
            vec3 localNormal;
            float t = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
            if (t > 0.0 && t < maxDist) return true; // It hit something!
        }
    }
    return false;
}

vec3 GetNormal(RayTracingInstance inst, vec3 hitPoint) {
    // Transform the hit point into the object's local space
    vec3 localPoint = (inst.InvTransform * vec4(hitPoint, 1.0)).xyz;
    vec3 localNormal;

    // Check if it's a Sphere (MaterialParams.z == 1) or Cube (MaterialParams.z == 0)
    // Adjust the index (z) based on how you set it in C++
    if (uint(inst.MaterialParams.z) == 1) { 
        // Sphere normal is just the normalized point itself
        localNormal = normalize(localPoint);
    } else { 
        // Cube normal (find the face closest to the edge)
        // We compare localPoint to the Min/Max bounds
        vec3 absP = abs(localPoint);
        vec3 boxDim = (inst.Max.xyz - inst.Min.xyz) * 0.5;
        
        // Find which face is hit by looking for the largest component
        if (absP.x > absP.y && absP.x > absP.z) localNormal = vec3(sign(localPoint.x), 0.0, 0.0);
        else if (absP.y > absP.z)              localNormal = vec3(0.0, sign(localPoint.y), 0.0);
        else                                   localNormal = vec3(0.0, 0.0, sign(localPoint.z));
    }

    // Transform local normal to world space
    // We use the transpose of the inverse model matrix (or just the WorldTransform if no scale)
    return normalize(mat3(inst.WorldTransform) * localNormal);
}

void GetNeighborhoodBounds(ivec2 center, out vec3 minCol, out vec3 maxCol) 
{
    minCol = vec3(1e6);
    maxCol = vec3(-1e6);

    for(int x = -1; x <= 1; x++) 
    {
        for(int y = -1; y <= 1; y++) 
        {
            // Note: This reads from img_Output, which should contain the raw 
            // current frame result BEFORE accumulation.
            vec3 col = imageLoad(img_Output, center + ivec2(x, y)).rgb;
            minCol = min(minCol, col);
            maxCol = max(maxCol, col);
        }
    }
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

vec2 GetJitter(int frameIndex) {
    return vec2(Halton(frameIndex, 2), Halton(frameIndex, 3)) - 0.5;
}

void RunTraceAndDenoise() 
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    vec3 accumulatedLight = vec3(0.0);
    vec2 frameVelocity = vec2(0.0); // Track velocity for this pixel
    bool hitAnything = false; // To track if we need to calculate motion

    for (int s = 0; s < u_SamplesPerPixel; s++) 
    {
        vec2 jitter = GetJitter(u_FrameIndex);
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
                float tLocal = (uint(inst.MaterialParams.z) == 0) ? HitCube(localRay, localNormal) : HitSphere(localRay, inst.MaterialParams.w, localNormal);
                
                if (tLocal > 0.0) 
                {
                    vec3 worldHit = (inst.WorldTransform * vec4(localRay.Origin + tLocal * localRay.Direction, 1.0)).xyz;
                    float tWorld = distance(currentRay.Origin, worldHit);
                    if (tWorld < closestHit) 
                    {
                        closestHit = tWorld; hitIndex = i;
                        hitNormal = normalize((vec4(localNormal, 0.0) * inst.InvTransform).xyz);
                        hitPoint = worldHit; hitAlbedo = inst.Albedo.rgb;
                    }
                }
            }

            if (hitIndex != -1)
            {
                if (bounce == 0) {
                    vec4 currentPosNDC = u_ViewProjection * vec4(hitPoint, 1.0);
                    currentPosNDC.xy /= currentPosNDC.w;
                    
                    vec4 prevPosNDC = u_PrevViewProjection * vec4(hitPoint, 1.0);
                    prevPosNDC.xy /= prevPosNDC.w;
                    
                    frameVelocity = currentPosNDC.xy - prevPosNDC.xy;
                    hitAnything = true;
                }

                // WE HIT SOMETHING: Shade it
                vec3 directLight = vec3(0.0);
                
                // NEE: Direct Lighting
                for(int i = 0; i < u_InstanceCount; i++) 
                {
                    if(Instances[i].Emission.w > 0.0) 
                    {
                        vec3 lightPos = Instances[i].WorldTransform[3].xyz;
                        vec3 dirToLight = normalize(lightPos - hitPoint);
                        float distToLight = length(lightPos - hitPoint);
                        
                        Ray shadowRay = Ray(hitPoint + hitNormal * 0.001, dirToLight);
                        if(!IsOccluded(shadowRay, distToLight)) 
                        {
                            float diff = max(dot(hitNormal, dirToLight), 0.0);
                            directLight += Instances[i].Emission.xyz * Instances[i].Emission.w * diff;
                        }
                    }
                }

                incomingLight += throughput * directLight; 
                incomingLight += throughput * (Instances[hitIndex].Emission.xyz * Instances[hitIndex].Emission.w);
                
                // Update for next bounce
                throughput *= hitAlbedo;
                currentRay.Origin = hitPoint + hitNormal * 0.001;
                currentRay.Direction = random_in_hemisphere(hitNormal);
            }
            else
            {
                float t = 0.5 * (normalize(currentRay.Direction).y + 1.0);
                vec3 skyColor = mix(vec3(0.1), vec3(0.2, 0.3, 0.7), t);
                incomingLight += throughput * skyColor;
                break;
            }
        }
        accumulatedLight += incomingLight;
    }

    vec3 currentColor = accumulatedLight / float(u_SamplesPerPixel);
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 uv = (vec2(pixelCoords) + 0.5) / vec2(imgSize);

    float depth = texture(s_DepthBuffer, uv).r;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 prevClipPos = u_PrevViewProjection * (u_InverseViewProjection * clipPos);
    vec2 prevUV = uv - velocity;

    vec2 currentJitter = GetJitter(u_FrameIndex);
    vec2 prevJitter = GetJitter(u_FrameIndex - 1);
    vec2 jitterOffset = (currentJitter - prevJitter) / vec2(imgSize);

    bool inBounds = prevUV.x >= 0.0 && prevUV.x <= 1.0 && prevUV.y >= 0.0 && prevUV.y <= 1.0;

    imageStore(img_Output, pixelCoords, vec4(currentColor, 1.0));
    memoryBarrierImage();

    vec3 history = vec3(0.0);
    if(inBounds && u_CameraMoved < 0.5) {
        history = texture(s_Accumulation, prevUV).rgb;
    } else {
        history = currentColor; // Reset if out of bounds or camera just moved
    }

    float diff = distance(history, currentColor);
    if (diff > 0.5) history = currentColor;

    vec3 minCol, maxCol;
    GetNeighborhoodBounds(pixelCoords, minCol, maxCol);
    history = clamp(history, minCol, maxCol);

    float alpha = 0.1;
    vec3 resultColor = mix(history, currentColor, alpha);

    imageStore(img_Accumulation, pixelCoords, vec4(resultColor, 1.0));
    imageStore(img_Output, pixelCoords, vec4(resultColor, 1.0));
}

void RunBloomThreshold() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = imageLoad(img_Output, pos);
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    imageStore(img_Bloom, pos, (brightness > 1.0) ? color : vec4(0.0));
}

void RunBloomBlur() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 sum = vec4(0.0);
    int radius = 5;
    float count = 0.0;
    
    for(int x = -radius; x <= radius; x++) {
        for(int y = -radius; y <= radius; y++) {
            sum += imageLoad(img_Bloom, pos + ivec2(x, y));
            count += 1.0;
        }
    }
    imageStore(img_Bloom, pos, sum / float(count));
}

void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec3 scene = imageLoad(img_Output, pos).rgb;
    vec3 bloom = imageLoad(img_Bloom, pos).rgb;
    
    // Multiply bloom by a factor (e.g., 2.0 or 5.0) to exaggerate it
    vec3 combined = scene + (bloom * 5.0); 
    
    vec3 mappedColor = combined / (combined + vec3(1.0));
    imageStore(img_Output, pos, vec4(pow(mappedColor, vec3(1.0 / 2.2)), 1.0));
}

void RunBilateralBlur() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec3 centerColor = imageLoad(img_Output, pos).rgb;
    
    vec3 totalColor = vec3(0.0);
    float totalWeight = 0.0;
    
    // Blur window
    for(int x = -2; x <= 2; x++) {
        for(int y = -2; y <= 2; y++) {
            ivec2 samplePos = pos + ivec2(x, y);
            vec3 sampleColor = imageLoad(img_Output, samplePos).rgb;
            
            // Weight based on color difference (similarity)
            float weight = exp(-distance(centerColor, sampleColor) * 10.0);
            
            totalColor += sampleColor * weight;
            totalWeight += weight;
        }
    }
    imageStore(img_Output, pos, vec4(totalColor / totalWeight, 1.0));
}

void RunResolve() 
{
    ivec2 displayPos = ivec2(gl_GlobalInvocationID.xy);
    vec2 displaySize = vec2(imageSize(img_FinalDisplay));
    vec2 renderSize = vec2(imageSize(img_Output));

    vec2 uv = vec2(displayPos) / displaySize;

    // We sample from the accumulation buffer (the low-res result)
    vec3 color = texture(s_Accumulation, uv).rgb;

    // This makes the upscaled image look "native"
    vec3 center = color;
    vec3 up = texture(s_Accumulation, uv + vec2(0.0, 1.0/renderSize.y)).rgb;
    vec3 down = texture(s_Accumulation, uv - vec2(0.0, 1.0/renderSize.y)).rgb;
    vec3 left = texture(s_Accumulation, uv - vec2(1.0/renderSize.x, 0.0)).rgb;
    vec3 right = texture(s_Accumulation, uv + vec2(1.0/renderSize.x, 0.0)).rgb;

    // Apply a sharpening kernel
    vec3 sharp = mix(center, center * 5.0 - (up + down + left + right) * 1.0, 0.5);
    
    // Store in display texture
    imageStore(img_FinalDisplay, displayPos, vec4(sharp, 1.0));
}

void main()
{
    switch(u_PassID) 
    {
        case 0: RunTraceAndDenoise(); break;
        case 1: RunBloomThreshold(); break;
        case 2: RunBloomBlur(); break;
        case 3: RunComposite(); break;
        case 4: RunBilateralBlur(); break;
        case 5: RunResolve(); break;
    }
}