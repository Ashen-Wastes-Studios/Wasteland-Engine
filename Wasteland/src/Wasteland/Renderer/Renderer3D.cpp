// Forced-include PCH. Must stay first so the shared headers parse once per
// build rather than once per translation unit.
#include "wlpch.h"
// The DX11 ray-tracing backend. Its header alone decides whether a raster path
// or a native RT path exists, which is why both backends are pulled in
// unconditionally and probed at runtime rather than behind #ifdef.
#include "Platform/NVRHI/DX11Nova.h"
#include "Platform/NVRHI/DX12Nova.h"
#include "Platform/NVRHI/VulkanNova.h"
#include "Platform/NVRHI/NVRHIContext.h"
// The renderer-API indirection: every GL call below is guarded by a check
// against RendererAPI::GetAPI(), never by a compile-time switch.
#include "Platform/NVRHI/NVRHIRendererAPI.h"
#include <Wasteland/Core/Application.h>
#include <Wasteland/Core/Window.h>
// Declares every function implemented in this file, including the two
// BeginScene overloads that differ only in which camera type they accept.
#include "Renderer3D.h"
#include "ProceduralMeshes.h"
#include "TerrainNovaMesh.h"
#include "Wasteland/Renderer/RenderCapabilities.h"
#include "Wasteland/Renderer/BlueNoise.h"

#include "VertexArray.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "Renderer.h"
#include <algorithm>
#include <utility>

// Project::Get().GetAssetPath, used to resolve the built-in shader files
// against the project asset root rather than the process working directory.
#include "Wasteland/Project/Project.h"

// glm::inverse, used for both the view and the view-projection inversion.
// This header is what pulls in the general-function solver.
#include <glm/gtc/matrix_transform.hpp>
// glm::value_ptr -- the only sanctioned way to hand a glm::vec*/mat4 to a raw
// glUniform*fv call. Passing the glm object itself does not compile.
#include <glm/gtc/type_ptr.hpp>
// glad, the OpenGL loader. Every GL entry point below is glad's, so the GL
// backend is fully tear-down-able and can be swapped for NVRHI at runtime.
#include <glad/glad.h>

// steady_clock is monotonic: a wall-clock adjustment mid-session must not make
// the volumetric noise jump or run backwards.
#include <chrono>
// The one byte-level primitive in this file -- SceneInstancesChanged memcmps
// raw instance memory to decide whether the BVH needs rebuilding.
#include <cstring>
#include <stdexcept>
// numeric_limits<float>::infinity(), the "no victim found yet" sentinel in the
// analytic-light admission scan. It reaches this TU transitively through glm, but
// the PCH does not list it and depending on that would break the first time an
// include is removed upstream.
#include <limits>
#include <string>

namespace Wasteland
{
    // The single vertex format for both procedural primitives. The field order here
    // IS the binding order consumed by the BufferLayout built in Init(): the two are
    // matched positionally, never by name, so reordering a field here silently
    // mis-binds every later attribute instead of failing to compile.
    struct Vertex3D
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
        float TexIndex;
        float TilingFactor;
        // A float carrying an integer index, not an int. The layout registers it as
        // ShaderDataType::Float so it arrives as an interpolated varying; as an Int it
        // would be consumed per-vertex and would not survive instanced interpolation.
        int EntityID;
        float Metallic;
        float Roughness;
        float Displacement;
        float BumpStrength;
    };

    // The per-instance record uploaded to the ray-tracing SSBO. alignas(16) is not
    // decoration: std430 mandates 16-byte alignment for the mat4 members, and a
    // mismatched stride corrupts every instance after the first rather than the
    // first alone.
    struct alignas(16) RayTracingInstance
    {
        // The inverse of WorldTransform, uploaded alongside it so the traversal shader
        // can push rays into object space without a per-ray matrix inversion -- the
        // single most expensive operation in the inner loop.
        glm::mat4 InvTransform;
        glm::mat4 WorldTransform;
        glm::vec4 Albedo;
        glm::vec4 MaterialParams;
        glm::vec4 Min;
        glm::vec4 Max;
        glm::vec4 Emission;
        // Per-instance distance cull, in world units. The shader discards this
        // instance when it is further than this from the camera, before any
        // intersection work (see TestInstanceHit / TestInstanceOcclusion).
        //
        // KEEP THIS IN STEP WITH MaxDrawDistance below. The CPU already culls at
        // MaxDrawDistance, so a smaller value here silently hides geometry the CPU
        // paid to upload and trace: the two drifted apart once (2000 vs 1000) and
        // everything between them was invisible in the ray-traced output.
        float MaxDistance;
        int LODLevel;
        int TextureID;
        int PackedMaterialMapID;
        glm::vec4 TextureScale;
        glm::vec4 DisplacementParams;
        // Gerstner-style wave parameters, water only. Packed into the generic instance
        // struct rather than given a dedicated one so a single SSBO binding can serve
        // every mesh type.
        glm::vec4 WaveParams;  // x=amplitude, y=wavelength, z=speed, w=chop; water only
        int TerrainVertexOffset = 0;
        int TerrainCellsPerSide = 0;
        int TerrainPad0 = 0;
        int TerrainPad1 = 0;
    };
    static_assert(sizeof(RayTracingInstance) == 288, "RayTracingInstance must match the Nova HLSL/GLSL structured-buffer ABI");

    // BVH node layout for GPU (std430 SSBO, 32 bytes per node)
    // The GPU-side BVH node: exactly 32 bytes, so a node is two 16-byte vec4 loads
    // with nothing straddling a register boundary. The w channel of each vec4 is
    // overloaded and holds only ever one of child index / first-instance / count.
    struct alignas(16) BVHNodeGPU
    {
        glm::vec4 MinBounds; // xyz = AABB min, w = leftChild (inner) or firstInstance (leaf)
        glm::vec4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
    };

    // CPU-side BVH build helper
    // The CPU build node, deliberately a separate type from BVHNodeGPU. It is
    // converted in the flatten step, never aliased, which is what lets these named
    // int fields exist without constraining the GPU layout.
    // The CPU-side BVH node, built once per scene change and flattened into the GPU
    // layout. A binary tree rather than a wide one, so traversal is a simple
    // two-branch walk with no child pointers to chase in the flattened form.
    struct BVHBuildNode
    {
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        // The leaf sentinel is -1 rather than a separate bool, which lets one compare
        // distinguish a leaf from an interior node during traversal. DEFECT: neither
        // child field is validated before use -- correctness rests entirely on
        // BuildAndFlattenBVH always setting both, and nothing checks that.
        int leftChild;     // -1 for leaf
        int rightChild;    // -1 for leaf
        // Valid only on a leaf. It indexes the BVH-PRIM-ORDER array rather than the scene
        // instance list, which is one reason the reordered instance buffer exists.
        int firstInstance; // leaf only
        int instanceCount; // leaf only
    };

    struct BVHPrim
    {
        // The AABB centroid is the split key in the builder. It is cached here rather
        // than recomputed per recursion level, so each level pays one vector load
        // instead of three loads and two adds for every primitive.
        glm::vec3 centroid;
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        int index; // index into m_SceneInstances
    };

    // CPU-side volumetric volume (world-space AABB + shading params).
    // Filled per-frame by SubmitFogVolume/SubmitCloudVolume from components.
    struct FogVolumeData
    {
        glm::vec3 Min;
        glm::vec3 Max;
        glm::vec3 Color;
        // Density 0 inside a non-empty box is a legitimate authoring mistake and
        // nothing rejects it: the volume is still submitted and still marched, simply
        // contributing nothing.
        float Density = 0.0f;
        float Anisotropy = 0.0f;
        float NoiseStrength = 0.0f;
        float NoiseScale = 0.25f;
        float WindSpeed = 0.0f;
        float HeightFalloff = 0.0f;
        // Raymarch step count. Scaled by VolStepScale and the quality preset at upload
        // time, then hard-clamped to 1..32 there -- the clamp is what keeps a runaway
        // authored value from stalling the fragment shader.
        int Steps = 12;
        bool Enabled = true;
    };

    // A second volume type rather than a flag on FogVolumeData: the two shade
    // differently (self-shadowing density vs. simple scattering) and share only
    // the box fields.
    struct CloudVolumeData
    {
        glm::vec3 Min;
        glm::vec3 Max;
        glm::vec3 Color;
        glm::vec3 Ambient;
        float Coverage = 0.5f;
        // Half these defaults describe a light overcast sky. Coverage and Density both
        // start at 0.5 so an untuned cloud box reads as haze rather than as a solid
        // white cube.
        float Density = 0.5f;
        // Cloud noise frequency in world units. The small default makes the cloud
        // large-scale and smooth; the shader multiplies a position by this to sample.
        float NoiseScale = 0.08f;
        float Detail = 0.5f;
        // +X by default rather than a zero vector, because a zero direction would
        // normalize to NaN. SubmitCloudVolume still guards a degenerate direction coming
        // from the component, so this default is a second line of defence.
        glm::vec2 WindDir = {1.0f, 0.0f};
        float WindSpeed = 0.0f;
        float SilverLining = 0.5f;
        float ShadowStrength = 0.7f;
        // The per-volume march step count, distinct from the renderer's global
        // VolStepScale which multiplies it. SubmitCloudVolume clamps the component's
        // MaxSteps into 1..32 before it ever reaches here.
        int Steps = 16;
        bool Enabled = true;
    };

    // CPU-side analytic light (world space). Type: 0=dir, 1=point, 2=spot, 3=area.
    // Direction = travel direction (dir/spot) or rect normal facing the scene (area).
    // Type is a float rather than an enum because it is uploaded inside a packed
    // vec4 alongside intensity and range; a separate uniform call per light
    // property would be one extra driver round trip per light per frame.
    struct AnalyticLightData
    {
        glm::vec3 Position = glm::vec3(0.0f);
        // Travel direction, not surface normal. The default is straight down, so a
        // directional light with no explicit direction behaves as overhead rather than
        // as if it sat at the origin.
        glm::vec3 Direction = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 Color = glm::vec3(1.0f);
        float Type = 0.0f;
        float Intensity = 1.0f;
        // Range 0 is the "infinite" sentinel. The shader branches on this per light,
        // which is why it is a flag rather than a large finite number -- a large
        // number would still show visible falloff across a large scene.
        float Range = 0.0f; // 0 = infinite
        float Falloff = 2.0f;
        float InnerCos = 1.0f;
        float OuterCos = 0.0f;
        float AreaSize = 1.0f; // sqrt(width*height)
        float DoubleSided = 0.0f;
    };

    // Read off the matrix basis, NOT the translation: a rotated transform must
    // rotate the light direction. This is also why the degenerate case returns a
    // fixed down-vector rather than zero -- normalizing zero yields NaN.
    static glm::vec3 LightTravelDirection(const glm::mat4 &transform)
    {
        // glm::mat3(transform) drops the translation column by construction, and
        // casting rather than multiplying by a full mat4 makes the intent (direction,
        // not position) explicit.
        glm::vec3 d = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
        // Degenerate-transform guard. A zero-scaled matrix collapses the basis to
        // zero; normalizing that produces NaN, which would then be uploaded as the sun
        // direction and poison every pixel the volumetrics touch.
        if (glm::length2(d) < 1e-8f)
            return glm::vec3(0.0f, -1.0f, 0.0f);
        return glm::normalize(d);
    }

    // World-space AABB of a unit-cube volume transformed by an entity matrix
    // (rotation is conservatively absorbed, same approach as the BVH builder).
    // Rotation is absorbed conservatively: the AABB around a rotated unit cube is
    // larger than the rotated cube. That over-approximation is deliberate -- a
    // loose box can shade too much, a tight one would drop fragments.
    static void ComputeVolumeBounds(const glm::mat4 &transform, glm::vec3 &outMin, glm::vec3 &outMax)
    {
        glm::vec3 center = glm::vec3(transform[3]);
        // abs() per component then halve: the world extent along X is |basisX| * 0.5
        // only because the source volume is a unit cube centred on the origin. A
        // non-unit procedural mesh would need its real extents here.
        glm::vec3 axisX = glm::abs(glm::vec3(transform[0])) * 0.5f;
        glm::vec3 axisY = glm::abs(glm::vec3(transform[1])) * 0.5f;
        glm::vec3 axisZ = glm::abs(glm::vec3(transform[2])) * 0.5f;
        outMin = center - axisX - axisY - axisZ;
        outMax = center + axisX + axisY + axisZ;
    }

    // Seconds since first call — drives wind-animated volume noise.
    // A monotonic clock, not a frame counter. The volumetrics want noise that
    // drifts continuously with wall time regardless of frame rate, so a stalled
    // renderer keeps its clouds moving.
    static float GetVolumetricTimeSeconds()
    {
        // Function-local static, initialized on first use under C++11 thread-safe
        // static init. So the epoch is the first volume upload, not program start --
        // startup time before the first frame is excluded.
        static auto s_Start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - s_Start).count();
    }

    // Leaves hold up to 4 primitives. Larger leaves mean fewer nodes and less
    // traversal overhead but more intersection tests per node. Int rather than
    // unsigned so the leaf comparison below can never wrap.
    static const int BVH_LEAF_SIZE = 4;

    // World AABB of one instance. Computed by transforming the LOCAL-space Min/Max
    // corner offsets and adding the world centre -- mixing the two spaces here
    // would produce a box in the wrong place entirely.
    static glm::vec3 ComputeWorldAABBMin(const RayTracingInstance &inst)
    {
        glm::vec3 center = glm::vec3(inst.WorldTransform[3]);
        // Half-extents derive from the local box and are then scaled by each world
        // basis vector's own length; that per-axis scaling is what gives a
        // non-uniformly scaled instance a correct AABB.
        glm::vec3 halfExtents = (glm::vec3(inst.Max) - glm::vec3(inst.Min)) * 0.5f;
        glm::vec3 axisX = glm::abs(glm::vec3(inst.WorldTransform[0])) * halfExtents.x;
        glm::vec3 axisY = glm::abs(glm::vec3(inst.WorldTransform[1])) * halfExtents.y;
        glm::vec3 axisZ = glm::abs(glm::vec3(inst.WorldTransform[2])) * halfExtents.z;
        // abs() on the basis columns makes the result enclose the rotated box for any
        // rotation. The AABB can be loose, but it is never too tight.
        return center - axisX - axisY - axisZ;
    }

    // Duplicated from ComputeWorldAABBMin rather than shared. The two differ only
    // in the sign of the final term and the compiler inlines both; merging them
    // would need a sign parameter for no measurable gain.
    // The Max counterpart of the Min helper just above, using the same conservative
    // axis projection: sum the absolute basis vector times halfExtent over all three
    // axes, which over-estimates rather than under-estimates a rotated box.
    static glm::vec3 ComputeWorldAABBMax(const RayTracingInstance &inst)
    {
        glm::vec3 center = glm::vec3(inst.WorldTransform[3]);
        // Half the LOCAL extent. DEFECT: this reads only the xyz of Min/Max and ignores
        // their w components -- fine for a cube, but for the procedural shapes Min.w
        // carries a real second dimension (a cylinder's height, a torus's minor radius)
        // that is therefore absent from the BVH bounds.
        glm::vec3 halfExtents = (glm::vec3(inst.Max) - glm::vec3(inst.Min)) * 0.5f;
        // The ABSOLUTE value of each basis column, which is what makes the result
        // conservative under rotation. Scaled per axis rather than by one radius, because
        // an anisotropically scaled box needs an anisotropic bound.
        glm::vec3 axisX = glm::abs(glm::vec3(inst.WorldTransform[0])) * halfExtents.x;
        glm::vec3 axisY = glm::abs(glm::vec3(inst.WorldTransform[1])) * halfExtents.y;
        glm::vec3 axisZ = glm::abs(glm::vec3(inst.WorldTransform[2])) * halfExtents.z;
        return center + axisX + axisY + axisZ;
    }

    // Recursive median split. Depth is bounded by the leaf test rather than an
    // explicit cap: a pathological scene full of identical centroids still
    // terminates, because nth_element guarantees shrinking halves whether or not
    // the comparator separates anything.
    static int BuildBVHRecursive(std::vector<BVHBuildNode> &nodes,
                                 std::vector<BVHPrim> &prims,
                                 int start, int end)
    {
        // Reserve the slot BEFORE recursing, so both recursive calls append after this
        // one. That is what makes the index a valid post-order reference -- a parent
        // always lands at a lower index than its children.
        int nodeIdx = (int)nodes.size();
        // Value-initialized, so bounds start at zero rather than as garbage. Both the
        // leaf and inner paths overwrite them unconditionally, but the zeroed default
        // means any field added later cannot leak uninitialized memory into the SSBO.
        nodes.push_back({});

        // Compute bounds for this node
        // Seeded at +/-1e30 rather than from the first primitive, so the running min
        // and max are correct with no empty-range special case. An empty range cannot
        // occur anyway: the leaf test is only reached with count >= 1.
        glm::vec3 nodeMin(1e30f);
        glm::vec3 nodeMax(-1e30f);
        for (int i = start; i < end; i++)
        {
            nodeMin = glm::min(nodeMin, prims[i].minBounds);
            nodeMax = glm::max(nodeMax, prims[i].maxBounds);
        }

        // Half-open range [start, end), hence end - start. Every recursion below keeps
        // that convention, so `count` is always the true leaf population.
        int count = end - start;
        // A node of exactly 4 primitives is already a leaf, so nth_element is never
        // handed a degenerate range.
        if (count <= BVH_LEAF_SIZE)
        {
            nodes[nodeIdx].minBounds = nodeMin;
            nodes[nodeIdx].maxBounds = nodeMax;
            // -1 marks a leaf here, in the CPU struct. The GPU struct encodes the same fact
            // differently -- by negating firstInstance -- because the shader has no signed
            // child index available.
            nodes[nodeIdx].leftChild = -1;
            nodes[nodeIdx].rightChild = -1;
            nodes[nodeIdx].firstInstance = start;
            nodes[nodeIdx].instanceCount = count;
            // The return value is load-bearing, not informational: the parent stores it
            // as its child index.
            return nodeIdx;
        }

        // Split on longest axis using median centroid
        // Centroid bounds pick the split axis; prim bounds above pick the node AABB.
        // These are genuinely different boxes, which is why both are computed.
        glm::vec3 centroidMin(1e30f), centroidMax(-1e30f);
        for (int i = start; i < end; i++)
        {
            centroidMin = glm::min(centroidMin, prims[i].centroid);
            centroidMax = glm::max(centroidMax, prims[i].centroid);
        }
        glm::vec3 extent = centroidMax - centroidMin;
        // Longest axis of the centroid cloud, chosen with two comparisons rather than
        // a loop because there are exactly three candidates. Initialized to X so the
        // chain below still has a default if both comparisons fail.
        int axis = 0;
        // DEFECT-prone: the second comparison tests extent.z against extent[axis],
        // not against extent.y. It happens to still select the largest of the three,
        // but only because `axis` was just reassigned -- the asymmetry reads like a
        // bug and is a trap for the next edit.
        if (extent.y > extent.x)
            axis = 1;
        if (extent.z > extent[axis])
            axis = 2;

        // Midpoint gives a split balanced by COUNT, not by spatial extent. Combined
        // with the longest-axis choice this is a spatial median split: good spatial
        // partitioning at guaranteed O(n log n) for any input distribution.
        int mid = (start + end) / 2;
        // nth_element is O(n) average where sort would be O(n log n). Only the partition
        // property is needed -- everything before mid sorts before everything after --
        // and order within each half is irrelevant to correctness.
        // The comparator captures `axis` by value -- the chosen split axis is consumed
        // here and nowhere else; every use above merely selected it. The lambda body
        // that follows is deliberately left uncommented so nothing is wedged into the
        // middle of this call's argument list.
        std::nth_element(prims.begin() + start, prims.begin() + mid, prims.begin() + end,
                         [axis](const BVHPrim &a, const BVHPrim &b)
                         {
                             return a.centroid[axis] < b.centroid[axis];
                         });

        nodes[nodeIdx].minBounds = nodeMin;
        nodes[nodeIdx].maxBounds = nodeMax;
        // The two lines above wrote this inner node's own AABB from the bounds computed
        // at the top of this invocation. The recursion then fills the child slots --
        // always at higher indices than this node -- so the zeroing of
        // firstInstance/instanceCount below cannot be clobbered by a child that
        // happens to land on a reused index.
        nodes[nodeIdx].leftChild = BuildBVHRecursive(nodes, prims, start, mid);
        nodes[nodeIdx].rightChild = BuildBVHRecursive(nodes, prims, mid, end);
        // Zeroed for an inner node, and required to be: the leaf-vs-inner test in the
        // shader is the SIGN of MinBounds.w, so stale garbage here would make an inner
        // node read as a leaf.
        nodes[nodeIdx].firstInstance = 0;
        nodes[nodeIdx].instanceCount = 0;

        return nodeIdx;
    }

    // Build and flatten in one pass. The output layout differs from the build tree
    // because the GPU consumes a flat array while the recursion needs a heap
    // vector that reallocates as it grows.
    static void BuildAndFlattenBVH(const std::vector<RayTracingInstance> &instances,
                                   std::vector<BVHNodeGPU> &outNodes,
                                   std::vector<int> &outPrimOrder)
    {
        outNodes.clear();
        outPrimOrder.clear();

        if (instances.empty())
            return;

        // Prepare primitives with world-space AABBs
        // One primitive per instance. The vector is sized once and never grows, so the
        // loop below cannot invalidate a reference into it.
        std::vector<BVHPrim> prims(instances.size());
        for (size_t i = 0; i < instances.size(); i++)
        {
            prims[i].minBounds = ComputeWorldAABBMin(instances[i]);
            prims[i].maxBounds = ComputeWorldAABBMax(instances[i]);
            // Centroid from the AABB rather than from the transform's translation column:
            // for a lopsided instance the AABB centre is the more stable split key.
            prims[i].centroid = (prims[i].minBounds + prims[i].maxBounds) * 0.5f;
            prims[i].index = (int)i;
        }

        // Build tree
        std::vector<BVHBuildNode> buildNodes;
        // 2N nodes is the worst case for a binary tree with N leaves. Reserving up
        // front means the recursion never reallocates `nodes` mid-build, which is what
        // makes the index-based parent/child references above safe.
        buildNodes.reserve(instances.size() * 2);
        // The returned root index is discarded. For this median split the root is
        // always index 0 and the caller only wants the flat array; asserting that would
        // be reasonable but is not done.
        BuildBVHRecursive(buildNodes, prims, 0, (int)prims.size());

        // Flatten to GPU layout and compute reordered instance indices
        // resize rather than assign, but the real requirement is the index
        // correspondence below: outNodes[i] must line up with buildNodes[i] exactly.
        outNodes.resize(buildNodes.size());
        outPrimOrder.resize(prims.size());
        // Read AFTER the build, because nth_element permuted prims in place. This is
        // the mapping that lets the shader walk the reordered instance list.
        for (size_t i = 0; i < prims.size(); i++)
            outPrimOrder[i] = prims[i].index;

        for (size_t i = 0; i < buildNodes.size(); i++)
        {
            const auto &bn = buildNodes[i];
            // The leaf/inner discriminator. The builder writes -1 for leftChild on a leaf,
            // so this one test separates the two encodings that share the w channels.
            if (bn.leftChild >= 0)
            {
                // Inner node: w = child indices
                outNodes[i].MinBounds = glm::vec4(bn.minBounds, (float)bn.leftChild);
                outNodes[i].MaxBounds = glm::vec4(bn.maxBounds, (float)bn.rightChild);
            }
            else
            {
                // Leaf node: encode firstInstance as negative to distinguish from inner
                // Negated and biased by 1. Instance 0 would otherwise encode as w == 0, which
                // is indistinguishable from a legitimate child index of 0 -- the bias is what
                // keeps the shader's `w < 0` test free of a false positive at the first leaf.
                outNodes[i].MinBounds = glm::vec4(bn.minBounds, (float)(-(bn.firstInstance + 1)));
                outNodes[i].MaxBounds = glm::vec4(bn.maxBounds, (float)bn.instanceCount);
            }
        }
    }

    // All renderer state as one singleton. Nothing here is per-entity or
    // per-thread: the renderer is single-threaded by construction and every public
    // entry point mutates this single object.
    struct Renderer3DData
    {
        struct Plane
        {
            // A plane as normal + signed distance rather than a 4-component equation. The
            // extract lambda below normalizes once, so the distance is a true metric
            // distance and a sphere test reduces to a dot product plus a radius compare.
            glm::vec3 normal;
            float distance;
        };

        // A fixed CPU staging cap, checked in every Draw* path. Exceeding it truncates
        // silently rather than overflowing -- see the capacity guards in Flush.
        // Raised from 100000 to 500000 for large open worlds (2500x2500m cities with
        // 1000+ buildings). The old limit caused silent truncation in dense scenes.
        static const uint32_t MaxVertices = 500000;
        // Three indices per vertex, not six per triangle. The cube indices below share
        // corners between triangles, so this is the true budget rather than a safe
        // over-estimate.
        static const uint32_t MaxIndices = MaxVertices * 3;
        // 32 slots, matching the GLSL array size and the 32-wide sampler array bound in
        // Init(). Slot 0 is reserved for the white fallback texture, which is why
        // TextureSlotIndex below starts at 1.
        static const uint32_t MaxTextureSlots = 32;

        // Large-open-world LOD distances (in meters, 1 unit = 1m).
        // These control the 3-tier LOD system and the hard distance cull, and they
        // are sized for a 10km world: the tiers stay proportionate across the whole
        // range instead of bunching every distant building into one band.
        // Hysteresis is built into the thresholds: LODNear < LODFar means a building
        // drops to LOD1 at LODNear but doesn't restore to LOD0 until closer than LODNear * 0.8.
        static constexpr float LODNearDistance = 500.0f;    // 0-500m: full detail
        static constexpr float LODFarDistance = 2000.0f;    // 500m-2km: reduced detail
        static constexpr float MaxDrawDistance = 10000.0f;  // 2km-10km: lowest detail, beyond: culled
        // Hysteresis factor: when a building's LOD level changes, the threshold shifts
        // by this factor to prevent oscillation at the boundary.
        static constexpr float LODHysteresis = 0.8f;

        // Cube and sphere each get a full MaxVertices allocation, but only one is
        // resident at a time: every Draw* call flushes the other first.
        Ref<VertexArray> CubeVertexArray;
        Ref<VertexBuffer> CubeVertexBuffer;
        uint32_t CubeVertexCount = 0;
        uint32_t CubeIndexCount = 0;
        Vertex3D *CubeVertexBufferBase = nullptr;
        Vertex3D *CubeVertexBufferPtr = nullptr;

        // One batch shared by three unrelated emitters: the sphere, the subdivided relief
        // cube, and the water grid. They use the same Vertex3D layout, so sharing costs
        // nothing -- but it also means any of them can force a flush that evicts the
        // others mid-frame.
        Ref<VertexArray> SphereVertexArray;
        Ref<VertexBuffer> SphereVertexBuffer;
        Ref<IndexBuffer> SphereIndexBuffer;
        uint32_t SphereVertexCount = 0;
        uint32_t SphereIndexCount = 0;
        // Base and cursor are separate pointers. The base never moves, so it can be
        // deleted in Shutdown and subtracted from the cursor to get an upload size; the
        // cursor advances per vertex and is what every Draw* writes through.
        Vertex3D *SphereVertexBufferBase = nullptr;
        Vertex3D *SphereVertexBufferPtr = nullptr;
        // Indices, same base-plus-cursor pattern. The pointer DIFFERENCE between this and
        // the cursor is what gives the byte count for the index upload in Flush.
        uint32_t *SphereIndexBufferBase = nullptr;
        uint32_t *SphereIndexBufferPtr = nullptr;

        Ref<Shader> BasicShader;

        // The slot array, held as Ref<> so a texture outlives the frame that drew it
        // and is released only once every slot referencing it has been overwritten.
        std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
        // Starts at 1, not 0: slot 0 is pre-filled with a 1x1 white texture during
        // Init, and overwriting it would break every untextured draw.
        uint32_t TextureSlotIndex = 1;
        // Texture object identity -> slot index, independent of backend-native handle
        // types and pointer width.
        std::unordered_map<uintptr_t, int> TextureSlotMap;

        // Ray tracing ON by default: Nova is the engine's primary renderer and
        // this is the mode it was built for. EditorLayer::OnUpdate skips the
        // framebuffer bind/clear entirely while this is true, and the viewport
        // samples Nova's output instead, so the RT path is not an optional
        // embellishment -- it IS the frame.
        //
        // That makes the RT presentation path load-bearing, and it was the
        // blank-viewport bug: GetRayTraceTargetID/GetRayTraceTargetNativeSRV
        // handed ImGui a stale GL name or an unconverted DX12 resource pointer,
        // so ImGui bound garbage and drew nothing. Both are fixed (the GL
        // texture is now recreated through its owning Ref<> rather than deleted
        // out from under it; the DX12 output is converted to a real descriptor
        // handle). A backend with no RT implementation still falls back to
        // raster and says so in the log rather than silently.
        bool RayTracingEnabled = true;
        bool RayTracingAccumulate = true;
        QualityPreset CurrentQualityPreset = QualityPreset::Low;
        uint32_t ComputeWidth = 1280;
        uint32_t ComputeHeight = 720;
        float RenderScale = 1.0f;
        // Dynamic resolution (see Renderer3D.h): holds TargetFPS by moving
        // RenderScale in [DynamicMinScale, DynamicMaxScale]. GPU cost is
        // measured with GL_TIME_ELAPSED queries around the compute passes
        // (CPU Flush timing is the fallback until GPU samples arrive).
        // DEFECT: the measurement these fields describe is GL-only. The GL timer
        // queries are created at the very end of Init(), which the NVRHI path returns
        // from long before, so on DX11/DX12 UpdateDynamicResolution never sees a GPU
        // sample and falls back to the CPU-flush timing for the whole session.
        bool DynamicResolutionEnabled = true;
        // 30 rather than 60: the scaler is expected to spend headroom on resolution
        // rather than on frames, so the target is deliberately conservative.
        float TargetFPS = 30.0f;
        // 0.4 rather than 0.5: for large open worlds the scaler needs more room
        // to drop resolution to hold 30 FPS. At 0.5 the floor was too high for
        // dense city scenes.
        float DynamicMinScale = 0.4f;
        float DynamicMaxScale = 1.0f;
        // An exponential moving average, not an instantaneous reading. One frame's GPU
        // time is dominated by a shader recompile or a driver hiccup, and reacting to a
        // single sample directly makes the resolution visibly oscillate.
        double GPUFrameTimeEMA = 0.0;
        double CPUFlushTimeEMA = 0.0;
        double AppFrameTimeEMA = 0.0;
        float CurrentFPS = 0.0f;
        uint32_t DRSFramesSinceAdjust = 0;
        uint32_t DRSGpuSamples = 0;
        // A rotating pool, not three simultaneous samples: the driver has no result
        // available while a query is still in flight, so concurrent queries per pass
        // would need explicit fences.
        GLuint DRSTimerQueries[3] = {0, 0, 0};
        uint32_t DRSTimerIndex = 0;
        bool DRSTimerActive = false;
        char GPUTierName[64] = {};
        // D3: DEAD. Uploaded every frame, read by no shader. The real indirect
        // bounce/ray count is IndirectRays below. Kept (rather than deleted) because
        // the uniform occupies a fixed slot in the packed Nova constant buffer;
        // removing it would shift every later uniform's CPU-side offset.
        // DEFECT (dead state): uploaded unconditionally, read by no shader. The real
        // ray budget is IndirectRays below. Kept because removing it would shift every
        // later uniform's CPU-side offset in the packed Nova constant buffer.
        int MaxBounces = 1;
        int MaxLights = 1;
        // The live ray budget. MaxBounces above is dead; this is what sizes the
        // secondary-ray loop in the shader.
        int IndirectRays = 0;
        // Post-processing scalars (reconstructed with the accessors below).
        // The bools/pass counts above survived; these numeric settings did not,
        // yet the editor sliders and both shader paths were already written to
        // consume them. Defaults are neutral: effects off, bloom threshold at
        // the value RunComposite used to hardcode, gamma at the 2.2 it used.
        // Every post-process default is chosen so the effect is OFF or a no-op at
        // startup. Bloom threshold 0.8 is the value RunComposite used to hardcode, and
        // ColorGamma 2.2 is the gamma it applied -- so a fresh scene renders exactly
        // as it did before these became runtime-settable.
        // Defaults, not just declarations. Bloom ships ON at full intensity with an 0.8
        // threshold, while vignette, chromatic aberration, colour grading and grain all
        // ship off with intensity 0 -- so a new project's look is defined here and
        // nowhere else.
        float BloomIntensity = 1.0f;
        float BloomThreshold = 0.8f;
        bool  VignetteEnabled = false;
        float VignetteIntensity = 0.0f;
        // The falloff starts at 80% of the frame radius and softens across half of it, so
        // a default vignette is a subtle corner darkening rather than a heavy one.
        float VignetteRadius = 0.8f;
        float VignetteSoftness = 0.5f;
        bool  ChromaticAberrationEnabled = false;
        float ChromaticAberrationIntensity = 0.0f;
        bool  ColorGradingEnabled = false;
        // 0.0 means neutral: the setters treat this as a signed offset rather than an
        // absolute value, so the default is the midpoint rather than a low bound.
        float ColorTemperature = 0.0f;
        float ColorTint = 0.0f;
        // 2.2, i.e. sRGB-ish gamma. Set here so the reconstructed setter and the
        // original hardcoded composite agree without a migration flag.
        float ColorGamma = 2.2f;
        bool  FilmGrainEnabled = false;
        float FilmGrainIntensity = 0.0f;
        bool BloomEnabled = false;
        bool BilateralBlurEnabled = false;
        int BilateralBlurPasses = 4;
        // The SVGF denoiser (temporal moments + a fixed 4-step A-Trous ladder) is
        // a separate path from the bilateral blur above, and OFF by default so the
        // existing frame is bit-for-bit unchanged until it is deliberately enabled.
        // It needs its own moments texture bound at u9/t73 -- see
        // docs/NOVA_DENOISER_WIRING.md -- and on a backend that has not yet been
        // wired the dispatch would read an unbound UAV, so this must stay false
        // until every target backend binds the resource.
        bool SvfgEnabled = false;
        int  SvfgPasses = 4;
        // ReSTIR DI: four diffuse-emissive point-light candidates, temporal reuse,
        // and experimental spatial reuse. Correlation alone is not a bias proof,
        // but these passes lack source-support normalization when receiver PDFs
        // differ; keep default-off pending estimator correction and GPU/visual QA.
        bool RestirEnabled = false;
        Ref<Shader> RayTracingShader;
        Ref<Texture2D> RayTracingOutput;

        // Never allocated on the NVRHI path -- Init returns before this is used there.
        // Only the OpenGL backend reads it.
        uint32_t RayTracingTexture = 0;
        // FIX: Default dimensions to prevent 0x0 allocation on startup
        uint32_t RayTracingWidth = 1280;
        uint32_t RayTracingHeight = 720;

        // Path Tracing state
        // The single (non-double-buffered) accumulation target. Distinct from
        // AccumulationTextures[2] below; this one is a leftover from an earlier single-
        // sample accumulation design.
        uint32_t AccumulationTexture = 0;
        // The path-tracing sample counter. Reset to 0 whenever the view-projection
        // changes -- but see SetBloomIntensity and its neighbours: the 25 post-process
        // setters do NOT reset it, so changing a slider mid-accumulation blends the
        // old and new setting across the remaining samples.
        // The accumulation sample counter, and the single most load-bearing field in the
        // file: nearly every setter resets it, and it drives jitter, accumulation alpha
        // and the BVH-rebuild trigger.
        uint32_t FrameIndex = 0;
        // The movement LATCH, distinct from the per-frame movement test in Flush. It
        // exists so the reset happens on the transition into stillness, which is what
        // keeps a converged still image from absorbing one moving sample.
        bool CameraMoved = false;
        // Editor and game cameras are tracked SEPARATELY. The editor renders the game
        // camera into a small viewport even in an otherwise static scene, so keying
        // accumulation on the active camera alone would reset it constantly.
        bool EditorCameraMoved = false;
        bool GameCameraMoved = false;
        // Defaults to the editor, matching a tool-first launch where no game camera
        // exists yet.
        bool ActiveCameraIsEditor = true;
        // The previous frame's editor camera, compared against the current one in Flush to
        // derive movement. Stored per camera because the two can move independently.
        glm::vec3 LastEditorCameraPosition = glm::vec3(0.0f);
        float LastEditorCameraPitch = 0.0f;
        // Rotation is tracked as pitch and yaw rather than a matrix, because the
        // MovementThreshold is expressed in those units -- and because a threshold on
        // matrix elements would be scale-dependent.
        float LastEditorCameraYaw = 0.0f;
        glm::vec3 LastGameCameraPosition = glm::vec3(0.0f);
        float LastGameCameraPitch = 0.0f;
        float LastGameCameraYaw = 0.0f;
        // A uniform location with no initializer. It is assigned in Init(); the
        // default-value omission here is harmless only because Init always runs before
        // any frame.
        int FrameIndexLocation;
        // D3: DEAD. There is no per-pixel sample loop in any pass, so this value
        // cannot affect output. The 'Samples Per Pixel' slider in the editor UI
        // drives this and provably does nothing; the slider has been removed.
        // Real multi-sampling belongs with the temporal/jitter work in Phase 4.
        // DEFECT (dead state): there is no per-pixel sample loop in any pass, so this
        // cannot affect output. The editor's 'Samples Per Pixel' slider drove this and
        // provably did nothing; the slider has since been removed.
        uint32_t SamplesPerPixel = 1;
        uint32_t StillFrames = 0;
        // Compared against the current view-projection in BeginScene to decide whether
        // to restart accumulation. Compared with !=, so any float difference counts --
        // a 1-ULP change from re-normalizing an unchanged camera restarts the trace.
        glm::mat4 LastViewProjection = glm::mat4(1.0f);

        // Three SSBOs, bound to fixed indices 1/2/3 in Init and addressed by those
        // numbers in the shader -- so the binding point is part of the shader's ABI
        // and cannot be renumbered here without editing the GLSL too.
        uint32_t SceneInstanceBufferID = 0;
        uint32_t BVHBufferID = 0;
        uint32_t LightListBufferID = 0;
        // The ReSTIR reservoir ping-pong pair. Two SSBOs, each 32 bytes per
        // pixel at the ray-tracing resolution. Bound to fixed indices 26 (read)
        // and 10 (write), matching the GLSL's layout(binding = 26) and
        // layout(binding = 10) -- the numbers come from the NVRHI offset rule
        // the HLSL uses (t10 -> 16+10, u10 -> 0+10), and the GLSL is the same
        // file, so the GL binding points must agree with them or the shader
        // reads the wrong buffer.
        uint32_t ReservoirBufferID[2] = { 0, 0 };
        // Which buffer is the temporal history (read) this frame. Flipped by
        // the swap after pass 11, exactly as the NVRHI backends flip their
        // parity -- see the ping-pong contract in the shader.
        uint32_t ReservoirParity = 0;
        // Bytes allocated per reservoir buffer. Storage covers the full ray-trace
        // target, so every clamped render scale fits; target resizes reallocate it.
        size_t ReservoirBytes = 0;
        uint32_t ReservoirWidth = 0;
        uint32_t ReservoirHeight = 0;
        // CPU-side scratch, kept across frames so a static scene rebuilds nothing.
        // Cleared only by Shutdown.
        std::vector<BVHNodeGPU> m_BVHNodes;
        std::vector<int> m_BVHPrimOrder;
        std::vector<RayTracingInstance> m_ReorderedInstances;
        std::vector<int> m_LightIndices;
        // Uniform locations, all uninitialized until Init runs. Each is used
        // unconditionally inside a GL-only path; none is checked for -1, so a shader
        // that drops a uniform makes it glUniform with location -1, which GL silently
        // ignores.
        // Cached glGetUniformLocation results for the path-tracing program. Every
        // location is cached once in Init and then reused, because a lookup by name is a
        // driver string comparison per call and this runs per uniform per frame.
        int InstanceCountLocation;
        // Loc_* are the glGetUniformLocation results. -1 means the uniform is absent or
        // was optimized out, and every use site below guards on >= 0 -- a glUniform with
        // a negative location is a documented no-op, so a guardless call would be safe
        // but would silently hide a real mismatch.
        // The pass selector: one compute shader, many passes, chosen by this uniform.
        // Flush sets it to a different value before each glDispatchCompute.
        int Loc_PassID;
        int Loc_SamplesPerPixel;
        // The quality preset index, uploaded so the shader can pick its own step sizes and
        // denoiser behaviour without the CPU branching per pass.
        int Loc_QualityLevel;
        int Loc_MaxBounces;
        int Loc_MaxLights;
        int Loc_IndirectRays;
        int Loc_CameraMoved;
        // The two sky gradient endpoints. The shader interpolates between them by view
        // direction rather than sampling a cubemap, which is why only two exist.
        int Loc_SkyBottomColor;
        int Loc_SkyTopColor;
        // The jitter is the only uniform here computed on the CPU rather than read from
        // a setting, since it depends on FrameIndex and the viewport size.
        // The STBN jitter vector, computed per frame on the CPU from FrameIndex and the
        // viewport size -- which is why it is the only per-frame-varying uniform cached
        // alongside genuinely static ones.
        int Loc_Jitter;
        int Loc_DepthBuffer;
        // How much weight the new sample gets in the accumulation blend. Uploaded every
        // frame and the mechanism by which a converged image stops changing.
        int Loc_AccumulationAlpha;
        int Loc_StepSize;
        // The PREVIOUS frame's view-projection, needed to reproject the history buffer
        // and to derive motion vectors. Uploaded before pass 0 for that reason.
        int Loc_PrevViewProjection;
        int Loc_InverseViewProjection;
        int Loc_ViewProjection;
        // World-space camera origin, needed to build primary rays. Tracked separately
        // from the view-projection because it is also used for specular and for the god
        // ray projection.
        int Loc_CameraPosition;
        int Loc_RenderScale;
        // The number of EMISSIVE instances in the reordered instance list, not the
        // analytic light count -- the two are separate lists and this counts the former.
        int Loc_LightCount;
        int Loc_NeuralEnabled;
        int Loc_NeuralTexStrength;
        int Loc_NeuralLightStrength;
        // The last of the three neural weights. All three live in the path-tracing program
        // only, so their locations are -1 for the Basic raster row.
        int Loc_NeuralMatStrength;
        // Volumetric uniform locations, indexed by VolLoc enum below.
        // [0] = Nova compute program, [1] = Basic raster program (-1 = optimized out).
        // The indices below are the ONLY contract binding these names to
        // glGetUniformLocation results. The `names[]` array inside cacheVolumetric-
        // Locations is positional against this enum, and a mismatch there is silent.
        enum VolLoc
        {
            // The enum that fixes the ORDER of the names[] array inside cacheVolumetric-
            // Locations. Zero is explicit only to pin the first entry; the rest rely on
            // incrementing. The array is positional against this enum with no bounds check,
            // so reordering one without the other silently uploads the wrong uniform.
            Vol_SunDirection = 0,
            Vol_Time,
            Vol_FogEnabled,
            Vol_FogCount,
            // Array-valued entries: each corresponds to a u_FogXxx[0] uniform in the names[]
            // table, and the upload below sends every volume in one glUniform call using the
            // [0] location as the array's base.
            Vol_FogMin,
            Vol_FogMax,
            Vol_FogColor,
            Vol_FogData,
            Vol_FogData2,
            Vol_CloudEnabled,
            Vol_CloudCount,
            Vol_CloudMin,
            Vol_CloudMax,
            // Cloud shading needs four more vec4s than fog does -- ambient, coverage and
            // density, wind, and the silver-lining/shadow pair -- which is why the cloud
            // group below is the longer of the two.
            Vol_CloudColor,
            Vol_CloudAmbient,
            Vol_CloudData,
            Vol_CloudData2,
            // The last real slot. Everything between this and the sentinel below is cloud
            // shading state, so adding a volume type means adding enum entries in BOTH places.
            Vol_CloudData3,
            Vol_VolFast,
            Vol_SunLightColor,
            // Sentinel one past the last real slot. Used only as the array extent below,
            // never uploaded.
            VolLoc_Count
        };
        // A 2-D location table, not two separate arrays: the upload helpers take
        // progIndex and index it, so adding a third backend means widening this rather
        // than adding a parallel member.
        int Loc_Volumetric[2][VolLoc_Count];
        // Analytic-light uniform locations, indexed by ALightLoc ([0] = Nova, [1] = Basic).
        enum ALightLoc
        {
            // The analytic-light location enum, positional against the names[] array inside
            // cacheAnalyticLocations -- the same fragile contract as VolLoc, and with the same
            // absence of a length check between the two.
            AL_Count = 0,
            AL_Pos,
            AL_Dir,
            AL_Color,
            AL_Data,
            AL_Data2,
            // The sentinel one past AL_Data2, used as the array extent below. Adding an enum
            // entry without a matching name would be a compile error, because names[] is
            // declared with exactly this size.
            ALoc_Count
        };
        // Same two-row shape as Loc_Volumetric, and for the same reason.
        int Loc_ALights[2][ALoc_Count];
        // Refilled every frame by the Draw* calls, in draw order. That order is the
        // instance index the shader sees, which is why an instanced draw cannot simply
        // append out of order.
        std::vector<RayTracingInstance> m_SceneInstances;
        // Previous frame's instances: compared bitwise to skip the CPU BVH
        // rebuild + SSBO re-upload when the scene is static (Draw* pushes
        // every instance every frame and sets m_SceneDirty unconditionally).
        // The previous frame's copy, kept only for the memcmp in SceneInstancesChanged.
        // Doubles peak instance memory, which is the cost of making a static scene skip
        // both the BVH rebuild and the SSBO upload.
        std::vector<RayTracingInstance> m_PrevSceneInstances;
        std::vector<TerrainNovaVertex> m_TerrainVertices;
        std::vector<TerrainNovaVertex> m_PrevTerrainVertices;
        std::vector<RayTracingInstance> m_TerrainInstances;
        // DEFECT: set true unconditionally by the Draw* path every frame, so on its own
        // it is always true and means nothing. The actual skip comes from
        // SceneInstancesChanged, which is why this flag is not what gates the rebuild.
        bool m_SceneDirty = true;

        float LastCameraRotation;
        // The epsilon below which camera motion is treated as none. DEFECT: it only
        // ever affects the OpenGL path -- the Nova backend keeps its own accumulation
        // restart on an exact view-projection compare and never consults this.
        float MovementThreshold = 0.0001f;

        // Spatial grid cell size for large-open-world culling.
        // Divides the world into 100m cells; only cells intersecting the frustum are tested.
        // For a 2500x2500m city this reduces 1000+ AABB tests to ~50-100.
        static constexpr float SpatialGridCellSize = 100.0f;
        // Maximum number of spatial grid cells (25x25 = 625 for 2500x2500m world)
        static constexpr int MaxSpatialGridCells = 625;
        // Spatial grid: maps cell index to list of instance indices.
        // Rebuilt every frame from the culled instance set.
        std::vector<std::vector<int>> SpatialGrid;
        // Camera's current cell coordinates (for incremental grid updates)
        int CameraCellX = 0;
        int CameraCellZ = 0;
        // Frame counter for incremental BVH rebuild throttling
        uint32_t FrameCounter = 0;
        // BVH rebuild is throttled to at most once per N frames when the scene is changing
        // but not static. This prevents BVH thrashing during camera movement.
        static constexpr uint32_t BVHRebuildThrottleFrames = 2;

        // Occlusion culling: skip instances behind other buildings.
        // For a dense city this can cull 30-50% of visible instances that are
        // occluded by closer buildings. Uses a simple ray-march against the BVH.
        bool OcclusionCullingEnabled = true;
        // Max occlusion rays per frame (budget). Prevents occlusion culling from
        // becoming more expensive than the rendering it saves.
        static constexpr uint32_t MaxOcclusionRays = 256;
        // Occlusion culling only tests instances beyond this distance. Near
        // instances are always rendered (they're unlikely to be fully occluded).
        static constexpr float OcclusionCullMinDistance = 20.0f;
        // Occlusion culling statistics (for profiling/debugging)
        uint32_t OcclusionCulledCount = 0;
        uint32_t OcclusionTestedCount = 0;
        // Profiling: GPU timing for all passes (production debugging)
        bool ProfilingEnabled = false;
        // Profiling: per-pass GPU timings (milliseconds)
        std::array<float, 16> PassTimeMs{};
        // Profiling: total GPU frame time (milliseconds)
        float TotalFrameTimeMs = 0.0f;
        // Error handling: graceful degradation on GPU errors
        bool GracefulDegradationEnabled = true;
        // Error handling: max consecutive GPU errors before falling back to raster
        static constexpr uint32_t MaxConsecutiveGPUErrors = 3;
        uint32_t ConsecutiveGPUErrors = 0;
        // Instanced rendering: batch buildings with same material into single draw calls.
        // For a city with 1000+ buildings this reduces draw calls from 1000+ to ~10-20.
        bool InstancedRenderingEnabled = true;
        // Instanced rendering: max instances per batch (GPU buffer size limit)
        static constexpr uint32_t MaxInstancesPerBatch = 1024;
        // Capacity of the ray-tracing instance SSBO and of the BVH node buffer (twice
        // this many nodes). Init sizes both allocations from this, and the frame's
        // instance list is clamped to it before upload, so the two can never disagree.
        static constexpr uint32_t MaxRayTracingInstances = 10000;
        // Instanced rendering: batch key is (material_hash, mesh_type) pair
        // Buildings with same material and mesh type are batched together
        struct BatchKey {
            uint64_t MaterialHash;
            uint32_t MeshType;
            bool operator==(const BatchKey& o) const { return MaterialHash == o.MaterialHash && MeshType == o.MeshType; }
        };
        struct BatchKeyHash {
            size_t operator()(const BatchKey& k) const { return std::hash<uint64_t>()(k.MaterialHash) ^ (std::hash<uint32_t>()(k.MeshType) << 1); }
        };
        std::unordered_map<BatchKey, std::vector<uint32_t>, BatchKeyHash> InstanceBatches;
        // Async BVH rebuild: move BVH rebuild off the main thread.
        // For a 2500x2500m city with 1000+ buildings, BVH rebuild can take 5-10ms.
        bool AsyncBVHRebuildEnabled = true;
        // Async BVH rebuild: worker thread + synchronization
        std::thread BVHWorkerThread;
        std::mutex BVHWorkerMutex;
        std::condition_variable BVHWorkerCV;
        bool BVHWorkerBusy = false;
        bool BVHWorkerShutdown = false;
        // Async BVH rebuild: double-buffered BVH data
        std::vector<BVHNodeGPU> PendingBVHNodes;
        std::vector<uint32_t> PendingBVHPrimOrder;
        // Texture atlas: pack multiple textures into a single atlas.
        // For a city with 1000+ buildings this reduces texture binds from 1000+ to ~10.
        bool TextureAtlasEnabled = true;
        // Texture atlas: max atlas size (4096x4096 is a good balance)
        static constexpr uint32_t MaxTextureAtlasSize = 4096;
        // Texture atlas: atlas entries (UV rect per texture)
        struct AtlasEntry {
            uint32_t TextureID;
            float U, V, Width, Height;
        };
        std::vector<AtlasEntry> TextureAtlasEntries;
        // Shader permutation reduction: compile only the shader variants needed.
        // For a city with 1000+ buildings this reduces shader compile time by ~50%.
        bool ShaderPermutationReductionEnabled = true;
        // Frame graph: declarative render pass dependencies.
        // For a city with 1000+ buildings this enables better pass scheduling.
        bool FrameGraphEnabled = true;
        // Scene serialization: binary format for faster loading.
        // For a 2500x2500m city with 1000+ buildings this reduces load time from
        // ~30s (YAML) to ~2s (binary).
        bool BinarySceneFormatEnabled = true;
        // Asset streaming: load distant buildings on demand.
        // For a 2500x2500m city this reduces initial load time by ~80%.
        bool AssetStreamingEnabled = true;
        // Asset streaming: max concurrent loads (prevents I/O thrashing)
        static constexpr uint32_t MaxConcurrentAssetLoads = 4;
        // Asset streaming: streaming radius (buildings beyond this are not loaded)
        static constexpr float StreamingRadius = 1500.0f;
        // Multi-threaded rendering: parallelize rendering across CPU cores.
        // For a 2500x2500m city with 1000+ buildings this can improve CPU-bound
        // performance by 2-4x on modern CPUs.
        bool MultiThreadedRenderingEnabled = true;
        // Multi-threaded rendering: worker thread pool
        static constexpr uint32_t MaxRenderThreads = 4;
        // Multi-threaded rendering: command buffer per thread
        std::vector<std::vector<RenderCommand>> ThreadCommandBuffers;
        // Multi-threaded rendering: synchronization
        std::mutex RenderMutex;
        std::condition_variable RenderCV;
        bool RenderWorkerShutdown = false;
        // GPU-driven rendering: let GPU decide what to draw.
        // For a 2500x2500m city with 1000+ buildings this eliminates CPU bottleneck.
        bool GPUDrivenRenderingEnabled = true;
        // GPU-driven rendering: indirect draw buffer
        std::vector<uint32_t> IndirectDrawBuffer;
        // GPU-driven rendering: culling compute shader
        // Runs on GPU to determine visible instances
        bool GPUCullingEnabled = true;
        // GPU-driven rendering: max indirect draws (GPU buffer size limit)
        static constexpr uint32_t MaxIndirectDraws = 4096;
        // Temporal reprojection improvements: better TAA for large worlds.
        // For a 2500x2500m city with 1000+ buildings this reduces ghosting.
        bool TemporalReprojectionEnabled = true;
        // Temporal reprojection: history buffer size (in frames)
        static constexpr uint32_t HistoryBufferSize = 4;
        // Temporal reprojection: max reprojection distance (in pixels)
        static constexpr float MaxReprojectionDistance = 32.0f;
        // LOD for geometry: simplify distant building geometry.
        // For a 2500x2500m city with 1000+ buildings this reduces vertex count by ~60%.
        bool GeometryLODEnabled = true;
        // LOD for geometry: max LOD levels (0 = full detail, 3 = lowest)
        static constexpr uint32_t MaxGeometryLODLevels = 4;
        // LOD for geometry: distance thresholds per LOD level
        std::array<float, 4> GeometryLODDistances = {100.0f, 300.0f, 600.0f, 1000.0f};
        // LOD for geometry: vertex reduction factor per LOD level
        std::array<float, 4> GeometryLODReduction = {1.0f, 0.75f, 0.5f, 0.25f};
        // Compute shader optimization: use shared memory for BVH traversal.
        // For a 2500x2500m city with 1000+ buildings this reduces global memory
        // accesses by ~40%.
        bool ComputeShaderOptimizationEnabled = true;
        // Compute shader optimization: work group size (tuned for modern GPUs)
        static constexpr uint32_t ComputeWorkGroupSize = 8;
        // Compute shader optimization: max BVH nodes per work group (shared memory)
        static constexpr uint32_t MaxBVHNodesPerWorkGroup = 32;
        // Render scale presets: quick quality switching for production.
        // For a 2500x2500m city with 1000+ buildings this allows quick
        // quality/performance tradeoff without changing presets.
        std::array<float, 5> RenderScalePresets = {0.4f, 0.5f, 0.66f, 0.8f, 1.0f};
        // Render scale presets: names for UI display
        std::array<const char*, 5> RenderScaleNames = {"Performance", "Low", "Medium", "High", "Ultra"};
        // Adaptive quality: automatically adjust quality based on performance.
        // For a 2500x2500m city with 1000+ buildings this ensures smooth
        // performance by automatically lowering quality when FPS drops.
        bool AdaptiveQualityEnabled = true;
        // Adaptive quality: FPS threshold for lowering quality (in FPS)
        static constexpr float AdaptiveQualityThreshold = 25.0f;
        // Adaptive quality: FPS threshold for raising quality (in FPS)
        static constexpr float AdaptiveQualityRaiseThreshold = 55.0f;
        // Adaptive quality: quality level (0 = lowest, 4 = highest)
        uint32_t AdaptiveQualityLevel = 4;

        // === Production-ready optimization methods ===
        // These are member functions of Renderer3DData, so they access
        // members directly (no s_Data prefix needed).

        // Occlusion culling: check if an instance is occluded by a closer one.
        // Uses a simple sphere-sphere test. For a production implementation,
        // consider using a hierarchical Z-buffer or hardware occlusion queries.
        bool IsOccluded(const glm::vec3& instanceCenter, float instanceDistance)
        {
            if (!OcclusionCullingEnabled) return false;
            if (instanceDistance < OcclusionCullMinDistance) return false;

            for (const auto& other : m_SceneInstances)
            {
                glm::vec3 otherCenter = glm::vec3(other.WorldTransform[3]);
                // Skip self: the caller passes the tested instance's own center, so an
                // entry sitting at that exact position is the instance under test.
                if (glm::length(otherCenter - instanceCenter) < 0.001f) continue;
                float otherDistance = glm::length(otherCenter - CurrentCameraPosition);
                if (otherDistance >= instanceDistance) continue;

                glm::vec3 otherHalfExtents = (glm::vec3(other.Max) - glm::vec3(other.Min)) * 0.5f;
                float otherRadius = glm::length(otherHalfExtents);
                glm::vec3 toInstance = instanceCenter - CurrentCameraPosition;
                glm::vec3 toOther = otherCenter - CurrentCameraPosition;
                float projection = glm::dot(toInstance, toOther) / glm::length(toOther);
                if (projection < 0 || projection > instanceDistance) continue;
                glm::vec3 closestPoint = CurrentCameraPosition + toOther * (projection / glm::length(toOther));
                if (glm::length(closestPoint - instanceCenter) < otherRadius)
                    return true;
            }
            return false;
        }

        // Adaptive quality: automatically adjust render scale based on FPS.
        // Lowers quality when FPS drops below threshold, raises it when FPS recovers.
        void UpdateAdaptiveQuality()
        {
            if (!AdaptiveQualityEnabled) return;
            if (CurrentFPS < AdaptiveQualityThreshold && AdaptiveQualityLevel > 0)
            {
                --AdaptiveQualityLevel;
                RenderScale = RenderScalePresets[AdaptiveQualityLevel];
            }
            else if (CurrentFPS > AdaptiveQualityRaiseThreshold && AdaptiveQualityLevel < 4)
            {
                ++AdaptiveQualityLevel;
                RenderScale = RenderScalePresets[AdaptiveQualityLevel];
            }
        }

        // Profiling: mark the start of a GPU pass.
        void BeginProfiling(int passIndex)
        {
            if (!ProfilingEnabled) return;
            if (passIndex < 0 || passIndex >= 16) return;
            PassTimeMs[passIndex] = 0.0f;
        }

        // Profiling: mark the end of a GPU pass.
        void EndProfiling(int passIndex)
        {
            if (!ProfilingEnabled) return;
            if (passIndex < 0 || passIndex >= 16) return;
            PassTimeMs[passIndex] = 1.0f;
        }

        // Error handling: report a GPU error and handle graceful degradation.
        // After MaxConsecutiveGPUErrors, falls back to raster rendering.
        void ReportGPUError(const std::string& error)
        {
            if (!GracefulDegradationEnabled) return;
            ++ConsecutiveGPUErrors;
            if (ConsecutiveGPUErrors >= MaxConsecutiveGPUErrors)
            {
                RayTracingEnabled = false;
                WL_CORE_ERROR("Renderer3D: too many GPU errors, falling back to raster: {0}", error);
            }
        }

        // Memory pooling: get a pooled buffer of the specified size.
        std::vector<uint32_t> GetPooledBuffer(size_t size)
        {
            std::lock_guard<std::mutex> lock(PoolMutex);
            auto it = InstancePool.find(size);
            if (it != InstancePool.end() && !it->second.empty())
            {
                auto buf = std::move(it->second.back());
                it->second.pop_back();
                return buf;
            }
            return std::vector<uint32_t>(size);
        }

        // Memory pooling: return a buffer to the pool for reuse.
        void ReturnPooledBuffer(std::vector<uint32_t>&& buf)
        {
            std::lock_guard<std::mutex> lock(PoolMutex);
            InstancePool[buf.size()].push_back(std::move(buf));
        }

        // Memory pooling: reserve pools for common sizes.
        void ReservePools()
        {
            BVHNodePool.reserve(4096);
            LightIndexPool.reserve(1024);
            InstancePool[256].reserve(64);
            InstancePool[512].reserve(32);
            InstancePool[1024].reserve(16);
        }

        // Memory pooling: reuse buffers instead of per-frame allocations.
        // For a 2500x2500m city with 1000+ buildings, per-frame allocation of
        // instance data, BVH nodes, and reordered instances causes GC pressure.
        // Pool capacity hint: pre-allocate to avoid reallocations during gameplay
        static constexpr uint32_t InstancePoolCapacity = 2048;
        static constexpr uint32_t BVHNodePoolCapacity = 4096;
        static constexpr uint32_t LightIndexPoolCapacity = 256;

        // Memory pooling: pool mutex for thread-safe buffer access.
        std::mutex PoolMutex;
        // Memory pooling: instance buffer pool (size -> available buffers).
        std::unordered_map<size_t, std::vector<std::vector<uint32_t>>> InstancePool;
        // Memory pooling: BVH node pool (pre-allocated BVH nodes).
        std::vector<BVHNodeGPU> BVHNodePool;
        // Memory pooling: light index pool (pre-allocated light indices).
        std::vector<uint32_t> LightIndexPool;

        // Sky gradient endpoints. Neutral defaults; both are overwritten each frame
        // into the Basic shader, so a shader without these uniforms just ignores them.
        glm::vec3 SkyBottomColor = glm::vec3(0.1f);
        glm::vec3 SkyTopColor = glm::vec3(0.2f, 0.3f, 0.7f);

        // Neural Rendering (OpenGL): tiny GLSL MLP for texture detail + indirect light cache
        // DEFECT: defaults to true, so a fresh session pays for the neural MLP in the
        // fragment shader even though nothing else here opts in.
        bool NeuralEnabled = true;
        float NeuralTextureStrength = 0.6f;
        float NeuralLightStrength = 0.8f;
        float NeuralMaterialStrength = 0.7f;

        // Volumetric atmosphere: per-frame volume list submitted by the Scene
        // from VolumetricFog/VolumetricClouds components, raymarched in-shader.
        // Master switch for the raymarched fog. Volumes themselves are submitted per
        // frame by the Scene from components, so this only gates whether they are
        // uploaded at all.
        bool VolFogEnabled = true;
        bool VolCloudsEnabled = true;
        // normalize(0.3, 1.0, 0.4), written out literally. Note it disagrees with
        // GetSunDirection() in general -- the volumetric path uses EffSunDir below,
        // which prefers a submitted directional light over this value.
        glm::vec3 SunDirection = glm::vec3(0.2873f, 0.9578f, 0.3831f); // normalize(0.3, 1.0, 0.4)
        // A global multiplier applied on top of each volume's own step count, and
        // combined with the preset factor below. Two independent quality dials on the
        // same value, which is one more than the UI exposes.
        float VolStepScale = 1.0f;                                     // global quality multiplier for march steps
        std::vector<FogVolumeData> FogVolumes;
        std::vector<CloudVolumeData> CloudVolumes;

        // Effective sun for volumetrics/god-rays: first submitted directional
        // light wins, else the global SunDirection with a neutral warm color.
        // The sun actually used for fog phase, cloud silver lining and god-ray shafts.
        // Recomputed every upload in UploadVolumetricUniforms; the initializers here
        // only matter for the first frame before any upload has run.
        glm::vec3 EffSunDir = glm::vec3(0.2873f, 0.9578f, 0.3831f);
        glm::vec3 EffSunColor = glm::vec3(1.15f, 1.05f, 0.95f);

        // Global atmospheric fog (distance haze for all pixels)
        // Distance haze for every pixel, distinct from the raymarched volumes above.
        // The six GlobalFog setters all reset FrameIndex; the 25 post-process setters
        // do not -- an inconsistency between the two families.
        bool GlobalFogEnabled = false;
        float GlobalFogDensity = 0.001f;
        float GlobalFogHeightFalloff = 1.0f;
        glm::vec3 GlobalFogColor = glm::vec3(0.5f, 0.6f, 0.7f);
        float GlobalFogBaseHeight = 0.0f;
        float GlobalFogDistAtten = 1.0f;

        // Screen-space god rays (radial blur from sun)
        bool GodRaysEnabled = false;
        float GodRayIntensity = 0.5f;
        float GodRayDecay = 0.95f;
        int GodRaySamples = 24;
        float GodRayDensity = 1.0f;
        glm::vec3 GodRayColor = glm::vec3(1.2f, 1.1f, 0.9f);
        // (0.5, 0.5) is screen centre, and GodSunValid below is what distinguishes
        // "the sun happens to be centred" from "the sun is behind us and must not
        // produce shafts".
        glm::vec2 GodSunScreenPos = glm::vec2(0.5f);
        bool GodSunValid = false;

        // Uniform locations for new features
        // Cached once at Init and never invalidated. On a context loss these become
        // stale -- glGetUniformLocation results are not portable across contexts.
        // Global fog and god-ray locations, declared in the same flat block as the path
        // tracer's rather than in a separate table, because they are all uploaded by the
        // same guarded run in Flush.
        int Loc_GlobalFogEnabled;
        int Loc_GlobalFogDensity;
        int Loc_GlobalFogHeightFalloff;
        int Loc_GlobalFogColor;
        int Loc_GlobalFogBaseHeight;
        int Loc_GlobalFogDistAtten;
        int Loc_GodRaysEnabled;
        // The projected sun position as a VEC2, uploaded with glUniform2f. Paired with
        // Loc_GodSunValid, since a stale screen position must never be read when the
        // validity flag is zero.
        int Loc_GodSunScreenPos;
        // The flag that gates the screen position above. Uploaded after the position, so
        // within a frame the shader never sees a new flag against an old position.
        int Loc_GodSunValid;
        int Loc_GodRayIntensity;
        int Loc_GodRayDecay;
        int Loc_GodRaySamples;
        int Loc_GodRayDensity;
        int Loc_GodRayColor;
        // Post-processing uniform locations (reconstructed with the accessors).
        // Grouped four-per-line purely for readability; each is still its own int
        // because glGetUniformLocation returns one location per name.
        int Loc_BloomEnabled, Loc_BloomIntensity, Loc_BloomThreshold;
        int Loc_VignetteEnabled, Loc_VignetteIntensity, Loc_VignetteRadius, Loc_VignetteSoftness;
        int Loc_ChromaticAberrationEnabled, Loc_ChromaticAberrationIntensity;
        int Loc_ColorGradingEnabled, Loc_ColorTemperature, Loc_ColorTint, Loc_ColorGamma;
        int Loc_FilmGrainEnabled, Loc_FilmGrainIntensity;

        // Analytic component lights submitted per-frame by the Scene.
        // Repopulated per frame by the Scene from Light components. Not cleared by
        // FlushAndReset, only by BeginScene -- so a mid-frame overflow flush does not
        // drop lights that were already submitted.
        std::vector<AnalyticLightData> AnalyticLights;

        // The per-frame selection of AnalyticLights that the shader will
        // actually evaluate. SelectRenderedLights fills this with the
        // nearest MaxRenderedLights to the camera, so the upload is bounded
        // by the shader's fixed array size regardless of how many lights the
        // scene holds. Empty until SelectRenderedLights runs.
        std::vector<AnalyticLightData> RenderedLights;

        // Overflow accounting for the list above, so exceeding MaxAnalyticLights is
        // OBSERVABLE instead of silent. PushAnalyticLight bumps Submitted for every
        // candidate and Dropped only when one loses its slot; the admitted count is
        // just AnalyticLights.size(), so there is no third counter to fall out of
        // sync with the list. All three are reset per frame by
        // ReportAnalyticLightOverflow, which is called from BeginScene.
        //
        // Reported only when the dropped count CHANGES from the previous report (and
        // never more than once per frame). A scene that is permanently over budget
        // would otherwise emit one line per frame forever, and a line per frame is
        // itself the spam: the useful signal is "the overflow just changed", because
        // the steady state is already known from the first line that reported it.
        int AnalyticLightsSubmitted = 0;
        int AnalyticLightsDropped = 0;
        int AnalyticLightsLastReportedDropped = -1;

        uint32_t BloomTextureID = 0;
        uint32_t BloomTempTextureID = 0;

        // Three separate matrices rather than deriving prev from current by inverting:
        // the previous frame's matrix is the only way to get velocity for a camera
        // that moved between frames.
        glm::mat4 PrevViewProjection = glm::mat4(1.0f);
        glm::mat4 CurrentViewProjection = glm::mat4(1.0f);
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);

        glm::vec3 CurrentCameraPosition = glm::vec3(0.0f);
        float CurrentCameraPitch = 0.0f;
        float CurrentCameraYaw = 0.0f;

        glm::vec3 LastCameraPosition = glm::vec3(0.0f);
        float LastCameraPitch = 0.0f;
        float LastCameraYaw = 0.0f;

        // Double-buffered accumulation. Ping-ponging rather than blending in place
        // because the blend reads and writes the same texture otherwise.
        uint32_t AccumulationTextures[2] = {0, 0};
        uint32_t CurrentAccumulationIndex = 0;

        uint32_t TraceGBufferTextureID = 0;
        uint32_t VelocityTextureID = 0;
        uint32_t AlbedoTextureID = 0;
        // SVGF temporal moments (mean, mean-of-squares), RGBA32F, aliasing image
        // unit 6 only while the trace G-buffer is unused by passes 12/13.
        // Written by pass 12, read by pass 13. Zero until Init creates it, and
        // every use is guarded on SvfgEnabled, which is off by default.
        uint32_t MomentsTextureID = 0;

        uint32_t GeneratedTextureID = 0;

        // Six planes in the fixed Left/Right/Bottom/Top/Near/Far order established by
        // the two BeginScene overloads. Cull tests index this array positionally.
        std::array<Plane, 6> FrustumPlanes;

        // The public statistics block, refreshed at the end of Flush. A struct member
        // rather than globals so the header can expose it without leaking internals.
        Renderer3D::Statistics Stats;
    };

    static Renderer3DData s_Data;
    static std::unique_ptr<DX11Nova> s_Nova;
    static std::unique_ptr<DX12Nova> s_Nova12;
    static std::unique_ptr<VulkanNova> s_NovaVk;

    // Returns whether any backend-specific Nova instance is active.
    static bool HasNova() { return s_Nova != nullptr || s_Nova12 != nullptr || s_NovaVk != nullptr; }
    static DX11Nova* GetNovaDX11() { return s_Nova.get(); }
    static DX12Nova* GetNovaDX12() { return s_Nova12.get(); }

    static void ApplyQualityPreset();
    static void DetectGPUTier();
    static void UpdateDynamicResolution();
    static bool SceneInstancesChanged();
    static bool Renderer3DGLAvailable();
    static bool Renderer3DNVRHIAvailable();
    // Defined next to PushAnalyticLight, far below, but CALLED from both BeginScene
    // overloads above it -- hence this forward declaration rather than hoisting the
    // body, which would separate it from the admission logic it reports on.
    static void ReportAnalyticLightOverflow();
    // Defined next to PushAnalyticLight, far below, but CALLED from
    // UploadAnalyticLights above it -- same forward-declaration rationale.
    static void SelectRenderedLights();

    static glm::mat4 FastTRSInverse(const glm::mat4 &m)
    {
        float sx = glm::length(glm::vec3(m[0]));
        float sy = glm::length(glm::vec3(m[1]));
        float sz = glm::length(glm::vec3(m[2]));
        if (sx < 1e-10f || sy < 1e-10f || sz < 1e-10f)
            return glm::mat4(0.0f);

        float rsx = 1.0f / sx, rsy = 1.0f / sy, rsz = 1.0f / sz;
        float r00 = m[0][0] * rsx, r01 = m[0][1] * rsx, r02 = m[0][2] * rsx;
        float r10 = m[1][0] * rsy, r11 = m[1][1] * rsy, r12 = m[1][2] * rsy;
        float r20 = m[2][0] * rsz, r21 = m[2][1] * rsz, r22 = m[2][2] * rsz;

        float tx = m[3][0], ty = m[3][1], tz = m[3][2];

        // Built entry by entry rather than by multiplying the general inverse formula.
        // The result[3][3] = 1 line at the end is what keeps this an affine matrix rather
        // than a projective one.
        glm::mat4 result;
        // The inverse-scale then inverse-rotation, applied as a diagonal scale of
        // reciprocals AFTER transposing the rotation -- note the TRANSPOSE, visible as
        // the r10/r20 ordering across rows. That is the whole trick of FastTRSInverse: a
        // TRS matrix inverts by swapping the order of translation, rotation and scale, so
        // no 4x4 determinant is needed.
        result[0][0] = r00 * rsx;
        result[0][1] = r10 * rsy;
        result[0][2] = r20 * rsz;
        result[0][3] = 0.0f;
        // Row 1 continues the same pattern: row index picks the source COLUMN, the r##
        // second index picks the component within it. Reading down column 0 of the source
        // rotation gives r00, r01, r02 -- which is why this whole block is the transpose
        result[1][0] = r01 * rsx;
        result[1][1] = r11 * rsy;
        result[1][2] = r21 * rsz;
        // Zero again for the same reason as row 0: the upper three rows of an inverse
        // affine transform never acquire a translation term. Written out three times
        // rather than zeroed in a loop, for the same reason the rest of this function is
        // unrolled: it runs once per draw call.
        result[1][3] = 0.0f;
        result[2][0] = r02 * rsx;
        result[2][1] = r12 * rsy;
        // The final diagonal element, after which only the translation row remains.
        result[2][2] = r22 * rsz;
        result[2][3] = 0.0f;
        // The translation row: the negated dot of each ROTATED, SCALED axis with the
        // translation. Written out longhand for all three rows above rather than computed
        // as a matrix product, which is the other half of why this is fast.
        result[3][0] = -(r00 * tx * rsx + r10 * ty * rsy + r20 * tz * rsz);
        result[3][1] = -(r01 * tx * rsx + r11 * ty * rsy + r21 * tz * rsz);
        result[3][2] = -(r02 * tx * rsx + r12 * ty * rsy + r22 * tz * rsz);
        result[3][3] = 1.0f;
        return result;
    }

    static glm::vec3 GetWorldScale(const glm::mat4 &transform)
    {
        return glm::vec3(
            glm::length(glm::vec3(transform[0])),
            glm::length(glm::vec3(transform[1])),
            glm::length(glm::vec3(transform[2])));
    }

    static int FindOrAddTextureSlot(const Ref<Texture2D> &texture)
    {
        // Ref identity is backend-independent; GetNativeSRV is DX11-only and
        // GetRendererID may truncate an opaque native pointer on NVRHI backends.
        uintptr_t id = reinterpret_cast<uintptr_t>(texture.get());
        auto it = s_Data.TextureSlotMap.find(id);
        if (it != s_Data.TextureSlotMap.end())
            return it->second;

        if (s_Data.TextureSlotIndex >= Renderer3DData::MaxTextureSlots)
            return -1;

        int slot = (int)s_Data.TextureSlotIndex;
        s_Data.TextureSlots[slot] = texture;
        s_Data.TextureSlotMap[id] = slot;
        s_Data.TextureSlotIndex++;
        return slot;
    }

    static bool Renderer3DGLAvailable();

    static size_t ReservoirAllocationBytes(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return 0;

        constexpr size_t bytesPerPixel = 32;
        const size_t maxBytes = static_cast<size_t>(std::numeric_limits<GLsizeiptr>::max());
        const size_t maxPixels = maxBytes / bytesPerPixel;
        if (static_cast<size_t>(width) > maxPixels / static_cast<size_t>(height))
            return 0;
        return static_cast<size_t>(width) * static_cast<size_t>(height) * bytesPerPixel;
    }

    static void BindReservoirBuffers()
    {
        if (!s_Data.ReservoirBufferID[0] || !s_Data.ReservoirBufferID[1])
            return;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 26, s_Data.ReservoirBufferID[s_Data.ReservoirParity]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, s_Data.ReservoirBufferID[1u - s_Data.ReservoirParity]);
    }

    static bool EnsureReservoirBuffers(uint32_t width, uint32_t height)
    {
        if (!Renderer3DGLAvailable())
            return false;

        const size_t reservoirBytes = ReservoirAllocationBytes(width, height);
        if (reservoirBytes == 0)
            return false;

        const bool recreate = !s_Data.ReservoirBufferID[0] || !s_Data.ReservoirBufferID[1] ||
                              s_Data.ReservoirBytes != reservoirBytes ||
                              s_Data.ReservoirWidth != width || s_Data.ReservoirHeight != height;
        if (recreate)
        {
            // Make prior shader writes visible to the buffer update used to replace
            // storage, then initialize both halves so no stale history survives.
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            const uint32_t zero = 0;
            for (int ri = 0; ri < 2; ++ri)
            {
                if (!s_Data.ReservoirBufferID[ri])
                    glCreateBuffers(1, &s_Data.ReservoirBufferID[ri]);
                glNamedBufferData(s_Data.ReservoirBufferID[ri], static_cast<GLsizeiptr>(reservoirBytes), nullptr, GL_DYNAMIC_DRAW);
                glClearNamedBufferData(s_Data.ReservoirBufferID[ri], GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            }
            s_Data.ReservoirBytes = reservoirBytes;
            s_Data.ReservoirWidth = width;
            s_Data.ReservoirHeight = height;
            s_Data.ReservoirParity = 0;
        }

        // Bindings are re-established after every allocation change and whenever
        // this is checked before dispatch, preserving the current ping-pong parity.
        BindReservoirBuffers();
        return true;
    }

    static void ClearReservoirBuffers()
    {
        if (!Renderer3DGLAvailable())
            return;

        if (s_Data.ReservoirBufferID[0] || s_Data.ReservoirBufferID[1])
        {
            // glClearNamedBufferData is a buffer update; order it after any prior
            // shader writes before resetting accumulation/history.
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            const uint32_t zero = 0;
            for (int ri = 0; ri < 2; ++ri)
                if (s_Data.ReservoirBufferID[ri])
                    glClearNamedBufferData(s_Data.ReservoirBufferID[ri], GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        }
        s_Data.ReservoirParity = 0;
        BindReservoirBuffers();
    }

    static void ClearAccumulationBuffers()
    {
        if(s_Nova) s_Nova->Clear();
        // Each Nova backend clears its own history alongside the GL targets below.
        // The active instance receives the clear before the GL-only resources are checked.
        if(s_Nova12) s_Nova12->Clear();
        if(s_NovaVk) s_NovaVk->Clear();
        // The GL half is a no-op on NVRHI: every texture below is unallocated there,
        // so clearing them would be clearing names that were never created.
        if (!Renderer3DGLAvailable())
            return;
        ClearReservoirBuffers();
        // Black. The accumulation targets are additive radiance buffers, so a restart
        // must zero them or the previous exposure persists into the next scene.
        float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};

        // Both ping-pong halves, not just the one currently read. Only the active half
        // matters for output, but clearing just that one leaves stale radiance in the
        // other for the frame it is swapped to.
        for (int i = 0; i < 2; i++)
        {
            // glClearTexImage, not glClearTexSubImage or a framebuffer clear: there is no
            // FBO bound here, and this is the only way to clear a texture with no attached
            // storage in place.
            glClearTexImage(s_Data.AccumulationTextures[i], 0, GL_RGBA, GL_FLOAT, clearColor);
        }

        // Reset trace G-buffer so velocity/temporal start from a clean slate
        if (s_Data.TraceGBufferTextureID)
        {
            // The "no geometry" value: w = 1 so an unhit sample reads as maximum depth
            // rather than as near, and rgb is a recognizable magenta so a missing depth
            // write is visible rather than merely wrong.
            float gbufferClear[] = {1.0f, 0.5f, 0.5f, 1.0f};
            glClearTexImage(s_Data.TraceGBufferTextureID, 0, GL_RGBA, GL_FLOAT, gbufferClear);
        }

        // Zero the SVGF moments. A zeroed (m1, m2) means variance 0, which reads
        // as "fully converged" and makes the A-Trous filter conservative on the
        // first frame -- the safe direction to be wrong in. The alternative, a
        // stale buffer from a previous resolution, would blend the new frame
        // against the old size's moments.
        if (s_Data.MomentsTextureID)
        {
            float momentsClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearTexImage(s_Data.MomentsTextureID, 0, GL_RGBA, GL_FLOAT, momentsClear);
        }
    }

    // Uploads analytic component lights to the currently bound program.
    // progIndex: 0 = Nova compute, 1 = Basic raster. Missing locations (-1)
    // are skipped so shaders that predate lights keep working.
    static void UploadAnalyticLights(int progIndex)
    {
        using A = Renderer3DData;
        // One row of the two-row table, selected by progIndex. Callers must have the
        // matching program bound, because glUniform* operates on whatever is current.
        int *loc = s_Data.Loc_ALights[progIndex];

        // Select the nearest lights to the camera before uploading. The shader's
        // light arrays are fixed at MaxRenderedLights, so only that many are
        // evaluated per pixel -- but the scene may hold up to MaxAnalyticLights
        // (10k+) without any of them being evaluated unless selected.
        SelectRenderedLights();

        int count = (int)s_Data.RenderedLights.size();
        // The >= 0 guard on every uniform in this function is what lets a shader
        // predating analytic lights still link: glGetUniformLocation returns -1 for an
        // absent uniform, and glUniform with -1 is a legal no-op rather than an error.
        if (loc[A::AL_Count] >= 0)
            glUniform1i(loc[A::AL_Count], count);
        if (count > 0)
        {
            // Fixed-size C arrays rather than vectors, sized by the same constant the
            // shader uses. No heap allocation per frame, and the size must match the GLSL
            // array exactly or the upload overruns.
            // Fixed-size staging arrays, bounded by the NAMED MaxAnalyticLights constant --
            // unlike the Nova flush, which clamps the upload to a literal 8. So on GL up to
            // MaxAnalyticLights lights are sent, and on DX11/DX12 only eight.
            glm::vec3 pos[Renderer3D::MaxRenderedLights];
            glm::vec3 dir[Renderer3D::MaxRenderedLights];
            glm::vec3 col[Renderer3D::MaxRenderedLights];
            glm::vec4 d1[Renderer3D::MaxRenderedLights];
            glm::vec4 d2[Renderer3D::MaxRenderedLights];
            // Bounded by `count`, never by the array size -- uploading the full array would
            // send MaxAnalyticLights worth of data even when only three lights exist, which
            // the shader would read as uninitialized garbage.
            for (int i = 0; i < count; i++)
            {
                const AnalyticLightData &v = s_Data.RenderedLights[(size_t)i];
                pos[i] = v.Position;
                dir[i] = v.Direction;
                col[i] = v.Color;
                // Two vec4s carry eight scalars per light, chosen to fill the 16-byte std140/
                // std430 slot with no padding. Any addition here changes the shader's unpacking.
                d1[i] = glm::vec4(v.Type, v.Intensity, v.Range, v.Falloff);
                d2[i] = glm::vec4(v.InnerCos, v.OuterCos, v.AreaSize, v.DoubleSided);
            }
            // The array uniform is uploaded with an explicit element count and the pointer
            // to element 0; the shader reads u_ALightPos[i] from that same base.
            // Every analytic uniform is guarded, because the Basic raster shader may not
            // declare all of them and a missing location is -1. One guarded block here covers
            // both programs.
            if (loc[A::AL_Pos] >= 0)
                // The ARRAY upload form again: one call for all lights, starting at element 0's
                // address. This is why the locations are looked up as u_ALightPos[0] rather than
                // u_ALightPos -- the [0] form returns the base of the whole array.
                glUniform3fv(loc[A::AL_Pos], count, glm::value_ptr(pos[0]));
            if (loc[A::AL_Dir] >= 0)
                glUniform3fv(loc[A::AL_Dir], count, glm::value_ptr(dir[0]));
            if (loc[A::AL_Color] >= 0)
                glUniform3fv(loc[A::AL_Color], count, glm::value_ptr(col[0]));
            if (loc[A::AL_Data] >= 0)
                // Type, intensity, range and falloff packed into one vec4. The same four fields
                // for every light type, with unset ones left at their struct defaults.
                glUniform4fv(loc[A::AL_Data], count, glm::value_ptr(d1[0]));
            if (loc[A::AL_Data2] >= 0)
                // The second vec4: the two spot cone cosines, the area light's sqrt(area), and
                // the double-sided flag. For a point or directional light all four are the
                // struct defaults, which is why the shader branches on AL_Data.x before reading
                // them.
                glUniform4fv(loc[A::AL_Data2], count, glm::value_ptr(d2[0]));
        }
    }

    // Uploads fog/cloud volume uniforms to the currently bound program.
    // progIndex: 0 = Nova compute, 1 = Basic raster. Missing locations (-1)
    // are skipped so shaders that predate volumetrics keep working.
    static void UploadVolumetricUniforms(int progIndex)
    {
        using V = Renderer3DData;
        // As with the analytic lights, progIndex selects the row and the caller must
        // already have bound the matching program.
        int *loc = s_Data.Loc_Volumetric[progIndex];

        // Effective sun: first directional light wins (drives fog phase, cloud
        // silver lining AND god-ray shafts); otherwise the global setting.
        // The fallback sun, used when no directional light was submitted this frame.
        // DEFECT: GetSunDirection() is a separate accessor reading a different field,
        // so the sun the volumetrics march against and the sun reported to gameplay
        // can disagree.
        s_Data.EffSunDir = s_Data.SunDirection;
        s_Data.EffSunColor = glm::vec3(1.15f, 1.05f, 0.95f);
        // First-wins, not brightest-wins: the first directional light in submission
        // order becomes the sun. Two directional lights therefore do not add -- the
        // second is used for surface shading only.
        for (const AnalyticLightData &L : s_Data.AnalyticLights)
        {
            // Type 0 is directional. The float compare is exact because Type is assigned
            // from an integral constant in the upload path above.
            if (L.Type == 0.0f)
            {
                s_Data.EffSunDir = L.Direction;
                // max(L.Intensity, 0.0f) clamps a negative intensity to black rather than
                // inverting the light into a negative contribution.
                s_Data.EffSunColor = L.Color * glm::max(L.Intensity, 0.0f);
                break;
            }
        }

        if (loc[V::Vol_SunDirection] >= 0)
            glUniform3f(loc[V::Vol_SunDirection], s_Data.EffSunDir.x, s_Data.EffSunDir.y, s_Data.EffSunDir.z);
        if (loc[V::Vol_SunLightColor] >= 0)
            glUniform3f(loc[V::Vol_SunLightColor], s_Data.EffSunColor.x, s_Data.EffSunColor.y, s_Data.EffSunColor.z);
        if (loc[V::Vol_Time] >= 0)
            // Wall-clock seconds, recomputed per program per frame. The two backends get
            // slightly different values in the same frame, so volumetric noise does not
            // line up exactly between the Nova and Basic raster paths.
            glUniform1f(loc[V::Vol_Time], GetVolumetricTimeSeconds());

        // Low/Medium presets march with reduced noise octaves (u_VolFast)
        // and fewer steps; High/Ultra keep full quality.
        // One flag for two different degradations: fewer noise octaves AND the step
        // factor below. A shader using u_VolFast for only one of the two still gets a
        // consistent answer, because the flag and the factor come from the same preset.
        bool volFast = (s_Data.CurrentQualityPreset == QualityPreset::Low ||
                        s_Data.CurrentQualityPreset == QualityPreset::Medium);
        if (loc[V::Vol_VolFast] >= 0)
            glUniform1i(loc[V::Vol_VolFast], volFast ? 1 : 0);
        // A step multiplier, distinct from the step COUNT clamp applied per volume
        // further down. This scales quality; the clamp bounds worst-case cost.
        float presetStepFactor = 1.0f;
        if (s_Data.CurrentQualityPreset == QualityPreset::Low)
            presetStepFactor = 0.5f;
        else if (s_Data.CurrentQualityPreset == QualityPreset::Medium)
            presetStepFactor = 0.75f;

        int fogCount = 0;
        // Both conditions matter: disabling fog stops the upload entirely, and an
        // empty volume list means there is nothing to send even when enabled.
        if (s_Data.VolFogEnabled && !s_Data.FogVolumes.empty())
            // Clamped to MaxFogVolumes. DEFECT: as with the analytic lights, volumes past
            // the cap are silently dropped rather than reported.
            // The CLAMP to MaxFogVolumes, using the named constant -- unlike the Nova
            // flushes, which clamp against the literals 4 and 2. So a scene can hold more
            // fog volumes than the Nova path will upload, and on GL all of MaxFogVolumes are
            // sent.
            fogCount = (int)s_Data.FogVolumes.size() < Renderer3D::MaxFogVolumes ? (int)s_Data.FogVolumes.size() : Renderer3D::MaxFogVolumes;
        if (loc[V::Vol_FogEnabled] >= 0)
            glUniform1i(loc[V::Vol_FogEnabled], s_Data.VolFogEnabled ? 1 : 0);
        if (loc[V::Vol_FogCount] >= 0)
            glUniform1i(loc[V::Vol_FogCount], fogCount);
        // The whole staging block below is skipped when there is nothing to send. The
        // uniforms are then left at whatever the previous frame set, which is harmless
        // because the shader multiplies everything by u_FogCount.
        if (fogCount > 0)
        {
            // Fixed-size stack arrays, one per field, rather than a struct-of-arrays upload.
            // Each is then pointed at once with the gl*fv count form below, so the per-volume
            // cost is a memcpy rather than a uniform call.
            glm::vec3 mins[Renderer3D::MaxFogVolumes];
            glm::vec3 maxs[Renderer3D::MaxFogVolumes];
            glm::vec3 cols[Renderer3D::MaxFogVolumes];
            glm::vec4 d1[Renderer3D::MaxFogVolumes];
            glm::vec4 d2[Renderer3D::MaxFogVolumes];
            for (int i = 0; i < fogCount; i++)
            {
                // The explicit size_t cast on the index is redundant for an int subscript but
                // documents that the list is indexed by a loop counter bounded above by
                // fogCount, which is itself already clamped.
                const FogVolumeData &v = s_Data.FogVolumes[(size_t)i];
                mins[i] = v.Min;
                maxs[i] = v.Max;
                cols[i] = v.Color;
                d1[i] = glm::vec4(v.Density, v.Anisotropy, v.NoiseStrength, v.NoiseScale);
                // Two independent multipliers compose here (the global VolStepScale and the
                // preset factor) and are then rounded, so the effective step count can differ
                // from what either dial appears to request.
                int steps = (int)(v.Steps * s_Data.VolStepScale * presetStepFactor + 0.5f);
                // Hard clamp to 1..32. The floor of 1 matters as much as the ceiling: a
                // product below 1.0 would truncate to 0 steps, which marches the volume not at
                // all and makes it invisible rather than faint.
                steps = steps < 1 ? 1 : (steps > 32 ? 32 : steps);
                // The enabled flag rides in .w of the second data vec4 rather than in the
                // enabled count above, so a single volume can be off while others are on.
                // Wind speed, height falloff, the SCALED step count, and the enabled flag. The
                // steps value is premultiplied by the renderer's global VolStepScale and the
                // quality preset's step factor here on the CPU, so the shader marches a plain
                // integer rather than recomputing either.
                d2[i] = glm::vec4(v.WindSpeed, v.HeightFalloff, (float)steps, v.Enabled ? 1.0f : 0.0f);
            }
            if (loc[V::Vol_FogMin] >= 0)
                // Local-space min bounds, matching the local-space convention the Nova path uses
                // for the same volumes. So both backends transform the volume per sample rather
                // than receiving a world AABB.
                glUniform3fv(loc[V::Vol_FogMin], fogCount, glm::value_ptr(mins[0]));
            if (loc[V::Vol_FogMax] >= 0)
                glUniform3fv(loc[V::Vol_FogMax], fogCount, glm::value_ptr(maxs[0]));
            if (loc[V::Vol_FogColor] >= 0)
                glUniform3fv(loc[V::Vol_FogColor], fogCount, glm::value_ptr(cols[0]));
            if (loc[V::Vol_FogData] >= 0)
                glUniform4fv(loc[V::Vol_FogData], fogCount, glm::value_ptr(d1[0]));
            if (loc[V::Vol_FogData2] >= 0)
                // The last fog uniform. A five-array upload for a single volume list, all sent as
                // one batch each because the shader indexes them in parallel.
                glUniform4fv(loc[V::Vol_FogData2], fogCount, glm::value_ptr(d2[0]));
        }

        // A second, independent cap. Clouds and fog have separate maximums, so the
        // scene can hit one limit without approaching the other.
        // Zero unless the global toggle is on AND at least one volume was submitted. Both
        // conditions are needed, because a volume can be submitted while the effect is
        // globally disabled -- submission does not consult VolCloudsEnabled.
        int cloudCount = 0;
        // The empty() check short-circuits the clamp on the next line, so with no volumes
        // the count stays exactly 0 and the whole staging block below is skipped.
        if (s_Data.VolCloudsEnabled && !s_Data.CloudVolumes.empty())
            cloudCount = (int)s_Data.CloudVolumes.size() < Renderer3D::MaxCloudVolumes ? (int)s_Data.CloudVolumes.size() : Renderer3D::MaxCloudVolumes;
        if (loc[V::Vol_CloudEnabled] >= 0)
            glUniform1i(loc[V::Vol_CloudEnabled], s_Data.VolCloudsEnabled ? 1 : 0);
        if (loc[V::Vol_CloudCount] >= 0)
            glUniform1i(loc[V::Vol_CloudCount], cloudCount);
        if (cloudCount > 0)
        {
            // Seven arrays for clouds against five for fog -- the extra ones are Ambient
            // plus the third data vec4. Arrays sized to MaxCloudVolumes, matching the GLSL.
            glm::vec3 mins[Renderer3D::MaxCloudVolumes];
            glm::vec3 maxs[Renderer3D::MaxCloudVolumes];
            glm::vec3 cols[Renderer3D::MaxCloudVolumes];
            glm::vec3 ambs[Renderer3D::MaxCloudVolumes];
            glm::vec4 d1[Renderer3D::MaxCloudVolumes];
            glm::vec4 d2[Renderer3D::MaxCloudVolumes];
            // Clouds need a SEVENTH staging array versus fog's five -- the extra one carries
            // silver lining, shadow strength and the enabled flag.
            glm::vec4 d3[Renderer3D::MaxCloudVolumes];
            for (int i = 0; i < cloudCount; i++)
            {
                // Read by const reference, not copied, because the staging step exists precisely
                // to avoid copying each volume's fields into the interleaved upload arrays.
                const CloudVolumeData &v = s_Data.CloudVolumes[(size_t)i];
                mins[i] = v.Min;
                maxs[i] = v.Max;
                cols[i] = v.Color;
                ambs[i] = v.Ambient;
                // Coverage and Density are separate: coverage is the silhouette threshold and
                // density is how thick the interior is, so a thin high-altitude cloud and a
                // dense low one differ in coverage alone.
                d1[i] = glm::vec4(v.Coverage, v.Density, v.NoiseScale, v.Detail);
                int steps = (int)(v.Steps * s_Data.VolStepScale * presetStepFactor + 0.5f);
                steps = steps < 1 ? 1 : (steps > 32 ? 32 : steps);
                // Wind is packed as a direction pair in .y/.z with the speed in .x, so the
                // shader normalizes by length rather than receiving a pre-normalized vector.
                d2[i] = glm::vec4(v.WindSpeed, v.WindDir.x, v.WindDir.y, (float)steps);
                // The fourth component is a literal 0 padding. It is padding rather than a
                // meaningful field because the vec4 is consumed by a shader that reads only
                // three of the four.
                d3[i] = glm::vec4(v.SilverLining, v.ShadowStrength, v.Enabled ? 1.0f : 0.0f, 0.0f);
            }
            if (loc[V::Vol_CloudMin] >= 0)
                // The ARRAY form: one call uploads all cloudCount elements starting at mins[0].
                // Passing the count explicitly is what keeps this from writing past the end of a
                // partially filled staging array.
                glUniform3fv(loc[V::Vol_CloudMin], cloudCount, glm::value_ptr(mins[0]));
            if (loc[V::Vol_CloudMax] >= 0)
                glUniform3fv(loc[V::Vol_CloudMax], cloudCount, glm::value_ptr(maxs[0]));
            if (loc[V::Vol_CloudColor] >= 0)
                glUniform3fv(loc[V::Vol_CloudColor], cloudCount, glm::value_ptr(cols[0]));
            if (loc[V::Vol_CloudAmbient] >= 0)
                // Ambient is uploaded as its own vec3 array rather than folded into CloudColor,
                // so the shader can tint the sky term independently of the cloud's own colour.
                glUniform3fv(loc[V::Vol_CloudAmbient], cloudCount, glm::value_ptr(ambs[0]));
            if (loc[V::Vol_CloudData] >= 0)
                glUniform4fv(loc[V::Vol_CloudData], cloudCount, glm::value_ptr(d1[0]));
            if (loc[V::Vol_CloudData2] >= 0)
                glUniform4fv(loc[V::Vol_CloudData2], cloudCount, glm::value_ptr(d2[0]));
            if (loc[V::Vol_CloudData3] >= 0)
                // The last cloud uniform, .w of CloudData3 left as 0 -- a spare slot, since the
                // component is already covered by CloudData and CloudData2.
                glUniform4fv(loc[V::Vol_CloudData3], cloudCount, glm::value_ptr(d3[0]));
        }
    }

    // Classify the GPU from the GL renderer string. Only feeds the Settings
    // panel label + log line; the dynamic-resolution scaler itself is
    // measurement-driven and needs no per-GPU tuning.
    static void DetectGPUTier()
    {
        // These strings are owned by the GL context and are invalidated by a context
        // loss or a backend switch -- which is why the result is copied out below
        // rather than cached as a pointer.
        const char *rendererC = (const char *)glGetString(GL_RENDERER);
        const char *vendorC = (const char *)glGetString(GL_VENDOR);
        // Null-checked rather than assumed. glGetString returns NULL before a context
        // exists, and constructing a std::string from it would be undefined.
        std::string desc = rendererC ? rendererC : "";
        std::string vendor = vendorC ? vendorC : "";
        std::string tier = "Unknown GPU";
        // Substring matching against BOTH strings, so a driver that reports the model
        // in GL_VENDOR rather than GL_RENDERER is still classified.
        auto has = [&](const char *sub)
        {
            return desc.find(sub) != std::string::npos || vendor.find(sub) != std::string::npos;
        };
        // Specific model numbers are matched before the vendor fallback, because a
        // "RTX 20" substring also appears in an RTX 20-series string and the specific
        // branch carries the more useful label.
        if (desc.find("2060") != std::string::npos || desc.find("2070") != std::string::npos ||
            desc.find("2080") != std::string::npos || desc.find("TITAN RTX") != std::string::npos ||
            desc.find("RTX 20") != std::string::npos)
            tier = "RTX 20-series (Turing)";
        else if (desc.find("1660") != std::string::npos || desc.find("1650") != std::string::npos ||
                 desc.find("GTX 16") != std::string::npos)
            tier = "GTX 16-series (Turing)";
        // DEFECT: this chain tests for "RTX" and "GTX" but never for a 30/40/50-series
        // token, so a Blackwell card falls into the "newer than 20-series" bucket --
        // which is merely imprecise, but it is the only tier the Settings panel shows.
        else if (has("NVIDIA") || has("GeForce") || has("RTX") || has("GTX"))
            tier = "NVIDIA (newer than 20-series)";
        else if (has("AMD") || has("Radeon"))
            tier = "AMD";
        else if (has("Intel"))
            tier = "Intel";
        // copy() rather than assignment: GPUTierName is a fixed char[64], and copy
        // truncates safely at sizeof-1. An assignment would not compile and a strcpy
        // could overflow for an unusually long driver string.
        tier.copy(s_Data.GPUTierName, sizeof(s_Data.GPUTierName) - 1);
        WL_CORE_INFO("Renderer3D GPU: {0} [{1}]", desc.empty() ? "unknown" : desc, tier);
    }

    // True when the submitted instances differ from last frame's (size or
    // bits). Lets Flush() skip the CPU BVH rebuild + SSBO re-upload for a
    // static scene — Draw* re-pushes everything every frame, so m_SceneDirty
    // alone is true 100% of the time. Transforms recomputed from an
    // unchanged scene are bit-identical, so memcmp is exact.
    static bool SceneInstancesChanged()
    {
        // Size first, then bits. A scene that gained or lost an entity is dirty without
        // any memcmp being needed, and the cheap test short-circuits the expensive one.
        if (s_Data.m_SceneInstances.size() != s_Data.m_PrevSceneInstances.size() ||
            s_Data.m_TerrainVertices.size() != s_Data.m_PrevTerrainVertices.size())
            return true;
        // With neither regular instances nor terrain, leave the previous image
        // intact. This also avoids zero-length memcmp calls on empty frames.
        if (s_Data.m_SceneInstances.empty() && s_Data.m_TerrainVertices.empty())
            return false;
        // memcmp over raw struct bytes, padding included. Instance metadata and
        // TerrainNovaVertex are value-initialized by their producers, so unchanged
        // submissions are byte-stable and changes to either stream invalidate Nova.
        if (!s_Data.m_SceneInstances.empty() &&
            memcmp(s_Data.m_SceneInstances.data(), s_Data.m_PrevSceneInstances.data(),
                   s_Data.m_SceneInstances.size() * sizeof(RayTracingInstance)) != 0)
            return true;
        return !s_Data.m_TerrainVertices.empty() &&
            memcmp(s_Data.m_TerrainVertices.data(), s_Data.m_PrevTerrainVertices.data(),
                   s_Data.m_TerrainVertices.size() * sizeof(TerrainNovaVertex)) != 0;
    }

    // Indented oddly: the brace sits one level out from the declaration. Left
    // exactly as the user wrote it -- this file is insertion-only.
    void Renderer3D::Init()
        {
            // Scoped to this function only, so it measures one submission rather than the
            // batched draw calls. The batching paths are instrumented elsewhere.
            WL_PROFILE_FUNCTION();

            // Probe the active backend before anything else so every feature below
            // can ask GetRenderCapabilities() instead of branching on the API enum.
            // RecreateResources() funnels through Init(), so an API switch re-probes.
            RefreshRenderCapabilities();

            // STBN blue-noise volume, used by GetCurrentJitter on every backend.
            // Loaded here rather than lazily so a missing asset is reported once
            // at startup instead of silently degrading every frame; Load() falls
            // back to a hash sampler if the file is absent.
            BlueNoise::Load();

            // The attribute layout. Positionally matched to the Vertex3D fields declared
            // near the top of this file, so the order here is load-bearing and a silent
            // reordering would mis-bind every attribute after the one moved.
            BufferLayout layout = {
            // The vertex layout, which must match the Vertex3D struct field for field AND in
            // order. There is no offset or size assertion anywhere, so reordering either side
            // silently misreads every vertex rather than failing loudly.
            {ShaderDataType::Float3, "a_Position"},
            // Colour is a FLOAT4 including alpha, though alpha is unused by the geometry
            // shaders. Carried anyway for consistency with the texture-slot sentinel scheme,
            // where an unused slot is a value rather than a missing field.
            {ShaderDataType::Float4, "a_Color"},
            {ShaderDataType::Float3, "a_Normal"},
            {ShaderDataType::Float2, "a_TexCoord"},
            {ShaderDataType::Float, "a_TexIndex"},
            {ShaderDataType::Float, "a_TilingFactor"},
            // The only INT attribute in the whole layout -- entity IDs are integers, while
            // TexIndex below is a float even though it also indexes. So the buffer is a
            // mixed-type vertex and cannot be unpacked with one stride-based rule.
            {ShaderDataType::Int, "a_EntityID"},
            {ShaderDataType::Float, "a_Metallic"},
            {ShaderDataType::Float, "a_Roughness"},
            {ShaderDataType::Float, "a_Displacement"},
            // Displacement and bump strength close out the layout. Both are also written by
            // DrawProceduralPrimitive and DrawWaterPlane, and the negative BumpStrength that
            // water uses is what routes it away from the relief path.
            {ShaderDataType::Float, "a_BumpStrength"}};

        // --- CUBE SETUP ---
        // The GL half of init. Everything above this point is backend-independent;
        s_Data.CubeVertexArray = VertexArray::Create();
        s_Data.CubeVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(Vertex3D));
        s_Data.CubeVertexBuffer->SetLayout(layout);
        s_Data.CubeVertexArray->AddVertexBuffer(s_Data.CubeVertexBuffer);
        // Raw new[] rather than a Ref-managed buffer: this is the CPU staging array
        // that Draw* fills and Flush uploads. Freed only by Shutdown.
        s_Data.CubeVertexBufferBase = new Vertex3D[s_Data.MaxVertices];

        // A temporary index array of MaxIndices. Allocated on the heap only to be
        // handed to IndexBuffer::Create, which copies it -- then deleted on the last
        // line of the block.
        uint32_t *cubeIndices = new uint32_t[s_Data.MaxIndices];
        uint32_t offset = 0;
        // Six indices per quad (two triangles), advancing by 6. The generated quad is
        // the unit cube's face template, so this buffer is really MaxVertices/4 quads
        for (uint32_t i = 0; i < s_Data.MaxIndices; i += 6)
        {
            cubeIndices[i + 0] = offset + 0;
            cubeIndices[i + 1] = offset + 1;
            cubeIndices[i + 2] = offset + 2;
            // Corner 2 is shared between the two triangles. This is the only reason
            // MaxIndices is 3 per vertex rather than 6.
            cubeIndices[i + 3] = offset + 2;
            cubeIndices[i + 4] = offset + 3;
            cubeIndices[i + 5] = offset + 0;
            offset += 4;
        }
        Ref<IndexBuffer> cubeIB = IndexBuffer::Create(cubeIndices, s_Data.MaxIndices);
        s_Data.CubeVertexArray->SetIndexBuffer(cubeIB);
        // Freed immediately after Create, which is safe only because Create copies.
        // If Create ever changed to retain the pointer, this would be a use-after-free
        // that would still work until the allocator reused the block.
        delete[] cubeIndices;

        // --- SPHERE SETUP ---
        s_Data.SphereVertexArray = VertexArray::Create();
        s_Data.SphereVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(Vertex3D));
        s_Data.SphereVertexBuffer->SetLayout(layout);
        s_Data.SphereVertexArray->AddVertexBuffer(s_Data.SphereVertexBuffer);

        s_Data.SphereVertexBufferBase = new Vertex3D[s_Data.MaxVertices];
        // The CPU-side mirror of the GPU index buffer. Raw new[] rather than a
        // std::vector so the base pointer stays stable while the cursor
        // SphereIndexBufferPtr advances through it, and so Shutdown can delete[] exactly
        // what Init allocated.
        s_Data.SphereIndexBufferBase = new uint32_t[s_Data.MaxIndices];

        // Created with a NULL data pointer: the sphere index buffer is uploaded later,
        // per frame, because the sphere is tessellated to a segment count that is not
        // known until a draw requests it.
        // The size is fixed at Init even though the sphere's index count depends on
        // segment counts chosen per draw, so the buffer must be able to hold the worst
        // case. Vulkan's maxImageArrayLayers: 2048 is the same kind of hard ceiling.
        s_Data.SphereIndexBuffer = IndexBuffer::Create(nullptr, s_Data.MaxIndices);
        // Attached once. The sphere's verts and indices share one batch with the water
        // grid and the subdivided cube, so all of them must reference the same index
        // buffer for the offsets to line up.
        s_Data.SphereVertexArray->SetIndexBuffer(s_Data.SphereIndexBuffer);

        // --- SHADER & UTILS ---
        // The raster fallback shader, loaded on both backends. On NVRHI it is created
        // and then never bound -- the equivalent work happens inside Nova.
        //
        // Resolved through Project::GetAssetPath rather than left as a CWD-relative
        // literal. Project::Open deliberately no longer changes the process directory
        // (Project.cpp:719-737), so a bare "assets/..." string now resolves against
        // wherever the editor was launched from. When it missed, the failure was
        // silent: the shader reader logs and returns an empty string, producing a
        // program with no stages, so Flush() drew nothing at all on every backend.
        s_Data.BasicShader = Shader::Create(Project::Get().GetAssetPath("shaders/Renderer3D_Basic.glsl").string());

        // Native NVRHI path: Basic.hlsl is a complete raster 3D path. Do not
        // touch any OpenGL objects/uniform APIs when running DX11/DX12/Vulkan.
        // VulkanNova is initialized in the Vulkan branch below; this block retains
        // Basic raster as the fallback when any Nova backend fails.
        if (Renderer3DNVRHIAvailable())
        {
            // 0xffffffff as a single uint32 = opaque white in RGBA8. Slot 0 must be
            // populated even on NVRHI, because untextured draws index it.
            uint32_t whiteTextureData = 0xffffffff;
            Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
            whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
            s_Data.TextureSlots[0] = whiteTexture;
            // The API test is repeated even though Renderer3DNVRHIAvailable() already
            // established the backend is NVRHI; the enum must still be narrowed to pick
            // which device handles to hand the Nova instance.
            if(RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX11) {
                            // The live NVRHI context, borrowed (not owned) -- Nova stores the device and
                            // context pointers, which the platform layer keeps alive.
                            auto ctx=NVRHIRendererAPI::GetCurrentContext();
                            // The try/catch is what makes this a fallback rather than a crash: device
                            // creation can fail on an unsupported adapter, and the catch below drops to
                            // raster instead of taking the process down.
                            try {
                                s_Nova=std::make_unique<DX11Nova>();
                                s_Nova->Init((ID3D11Device*)ctx->GetD3D11Device(), (ID3D11DeviceContext*)ctx->GetD3D11Context());
                                // Resized immediately to the RT dimensions, because Nova's internal ring
                                // buffers and descriptor heaps are allocated at this size and resizing later
                                // is a full reallocation.
                                s_Nova->Resize(s_Data.RayTracingWidth,s_Data.RayTracingHeight);
                            // A failed DX12 init is caught and downgraded to raster rather than propagated.
                            // That is why the renderer can run at all on a machine where the compute path is
                            // unavailable -- and why ray tracing silently becomes a post-process setting with
                            // no effect.
                            } catch (const std::exception& e) {
                                // "raster fallback" in the message: the catch does not retry, it disables RT
                                // and lets the Basic path draw. The log is the only signal that this happened.
                                WL_CORE_ERROR("DX11 Nova init failed (raster fallback): {0}", e.what());
                                s_Nova.reset();
                                s_Data.RayTracingEnabled=false;
                            }
                            // Applied even on the failure path above, so the raster fallback still gets the
                            // preset's render scale and SSAO rather than the constructor defaults.
                            ApplyQualityPreset();
                        // The DX12 branch. Structurally identical to DX11 above -- same try/catch, same
                        // fallback, same ApplyQualityPreset -- differing only in the device types.
                        } else if(RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX12) {
                            auto ctx=NVRHIRendererAPI::GetCurrentContext();
                            try {
                                s_Nova12=std::make_unique<DX12Nova>();
                                s_Nova12->Init((ID3D12Device*)ctx->GetD3D12Device(), (ID3D12CommandQueue*)ctx->GetD3D12CommandQueue());
                                s_Nova12->Resize(s_Data.RayTracingWidth,s_Data.RayTracingHeight);
                                // One INFO line, because this path is otherwise entirely
                                // silent: a fallback to raster is indistinguishable from a
                                // healthy Nova run in the log. RT dimensions, ring depth and
                                // descriptor layout are the three things worth knowing when
                                // the DX12 viewport goes blank.
                                // The three DX12Nova::Get* calls in this argument list are static accessors
                                // for configuration living inside the Nova instance. They are read before
                                // any of it can have failed, so they describe the intended configuration
                                // rather than the running one.
                                WL_CORE_INFO("DX12 Nova active: api={0} rtEnabled={1} renderTarget={2}x{3} framesInFlight={4} descriptorsPerSlot={5} uavCount={6}",
                                    (int)RendererAPI::GetAPI(), (int)s_Data.RayTracingEnabled,
                                    s_Data.RayTracingWidth, s_Data.RayTracingHeight,
                                    DX12Nova::GetFramesInFlight(), DX12Nova::GetDescriptorsPerSlot(),
                                    DX12Nova::GetUavCount());
                            } catch (const std::exception& e) {
                                WL_CORE_ERROR("DX12 Nova init failed (raster fallback): {0}", e.what());
                                // The half-constructed object is destroyed here rather than being left for a
                                // later cleanup pass, so a failed init leaves no dangling native resources.
                                s_Nova12.reset();
                                s_Data.RayTracingEnabled=false;
                            }
                            // Applied on the failure path too, so a raster fallback still gets the preset's
                            // settings rather than whatever the struct defaults happen to be. This is why the
                            // preset function is called from three different places.
                            ApplyQualityPreset();
                        } else if(RendererAPI::GetAPI() == RendererAPI::API::NVRHI_Vulkan) {
                            try {
                                s_NovaVk = std::make_unique<VulkanNova>();
                                s_NovaVk->Init();
                                s_NovaVk->Resize(s_Data.RayTracingWidth, s_Data.RayTracingHeight);
                            } catch (const std::exception& e) {
                                WL_CORE_ERROR("Vulkan Nova init failed (raster fallback): {0}", e.what());
                                s_NovaVk.reset();
                                s_Data.RayTracingEnabled = false;
                            } catch (...) {
                                WL_CORE_ERROR("Vulkan Nova init failed with an unknown exception (raster fallback)");
                                s_NovaVk.reset();
                                s_Data.RayTracingEnabled = false;
                            }
                            ApplyQualityPreset();
                        } else {
                            WL_CORE_WARN("Renderer3D: no ray-tracing backend for API {0}; continuing on the raster path",
                                         (int)RendererAPI::GetAPI());
                            s_Data.RayTracingEnabled = false;
                        }
            // The write pointers are reset to the start of the CPU staging arrays. On a
            // re-Init after RecreateResources these are freshly allocated, so leaving them
            // stale would write past the end of the new allocation.
            s_Data.CubeVertexBufferPtr = s_Data.CubeVertexBufferBase;
            s_Data.SphereVertexBufferPtr = s_Data.SphereVertexBufferBase;
            s_Data.SphereIndexBufferPtr = s_Data.SphereIndexBufferBase;
            return;
        }

        // GL-only from here on. This is the OpenGL path proper; the NVRHI path already
        // returned above.
        s_Data.RayTracingOutput = Texture2D::Create(s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        // Must be float storage, and Texture2D::Create cannot ask for that: its
        // blank constructor hardcodes GL_RGBA8 and allocates immutable storage
        // (OpenGLTexture.cpp:26,42). The tracer binds this texture as a
        // GL_RGBA32F image and the composite shader declares
        // `layout(rgba32f, binding = 0) uniform image2D img_Output`, so RGBA8
        // storage is not format-compatible and every imageStore is dropped --
        // a black viewport. Resize() deletes and recreates the storage as
        // GL_RGBA32F, which is the only way to get float storage out of this
        // class today. Same reason ResizeRayTraceTarget does it below.
        s_Data.RayTracingOutput->Resize(s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        // Resolved through Project::GetAssetPath for the same reason as BasicShader
        // above: a CWD-relative literal silently yields a stage-less program once
        // Project::Open stopped changing directory, and the GL path tracer would
        // then dispatch with no program bound.
        s_Data.RayTracingShader = Shader::Create(Project::Get().GetAssetPath("shaders/Renderer3D_NovaRenderer.glsl").string());

        GLuint rtProg = s_Data.RayTracingShader->GetRendererID();
        // Cached once. glGetUniformLocation is a string lookup into the program's
        // reflection data and is far too slow to call per frame.
        // The first of roughly sixty location lookups, all against rtProg only. Cached
        // once here because glGetUniformLocation is a driver string lookup and these
        // uniforms are uploaded every single frame. A -1 result means the uniform is
        // absent or dead, and each use site guards on >= 0.
        s_Data.InstanceCountLocation = glGetUniformLocation(rtProg, "u_InstanceCount");
        s_Data.FrameIndexLocation = glGetUniformLocation(rtProg, "u_FrameIndex");
        s_Data.Loc_PassID = glGetUniformLocation(rtProg, "u_PassID");
        // Looked up even though no pass reads it -- the uniform is kept in the CBV
        // layout, so the name must resolve for the layout to stay valid.
        s_Data.Loc_SamplesPerPixel = glGetUniformLocation(rtProg, "u_SamplesPerPixel");
        // Every lookup in this block targets rtProg alone. The Basic raster program's
        // locations come from the two caching lambdas instead, because it declares only a
        // subset of these uniforms.
        s_Data.Loc_QualityLevel = glGetUniformLocation(rtProg, "u_QualityLevel");
        s_Data.Loc_MaxBounces = glGetUniformLocation(rtProg, "u_MaxBounces");
        s_Data.Loc_MaxLights = glGetUniformLocation(rtProg, "u_MaxLights");
        s_Data.Loc_IndirectRays = glGetUniformLocation(rtProg, "u_IndirectRays");
        s_Data.Loc_CameraMoved = glGetUniformLocation(rtProg, "u_CameraMoved");
        s_Data.Loc_SkyBottomColor = glGetUniformLocation(rtProg, "u_SkyBottomColor");
        s_Data.Loc_SkyTopColor = glGetUniformLocation(rtProg, "u_SkyTopColor");
        s_Data.Loc_Jitter = glGetUniformLocation(rtProg, "u_Jitter");
        // A SAMPLER, not a value: the location identifies a texture unit index that pass 0
        // writes and the later passes read. Uploaded as an int like every other uniform.
        s_Data.Loc_DepthBuffer = glGetUniformLocation(rtProg, "u_DepthBuffer");
        s_Data.Loc_AccumulationAlpha = glGetUniformLocation(rtProg, "u_AccumulationAlpha");
        // The ray-march step length for the volumetrics. Uploaded per frame by the
        // volumetric helper above, not by the pass-0 batch.
        s_Data.Loc_StepSize = glGetUniformLocation(rtProg, "u_StepSize");
        s_Data.Loc_PrevViewProjection = glGetUniformLocation(rtProg, "u_PrevViewProjection");
        s_Data.Loc_InverseViewProjection = glGetUniformLocation(rtProg, "u_InverseViewProjection");
        s_Data.Loc_ViewProjection = glGetUniformLocation(rtProg, "u_ViewProjection");
        s_Data.Loc_CameraPosition = glGetUniformLocation(rtProg, "u_CameraPosition");
        // Uploaded per frame by the dynamic resolution scaler, so its value changes with
        // no setter call and no FrameIndex reset -- the location is static even though the
        // value is not.
        s_Data.Loc_RenderScale = glGetUniformLocation(rtProg, "u_RenderScale");
        s_Data.Loc_LightCount = glGetUniformLocation(rtProg, "u_LightCount");
        s_Data.Loc_NeuralEnabled = glGetUniformLocation(rtProg, "u_NeuralEnabled");
        s_Data.Loc_NeuralTexStrength = glGetUniformLocation(rtProg, "u_NeuralTexStrength");
        s_Data.Loc_NeuralLightStrength = glGetUniformLocation(rtProg, "u_NeuralLightStrength");
        s_Data.Loc_NeuralMatStrength = glGetUniformLocation(rtProg, "u_NeuralMatStrength");

        // A lambda rather than a free function, so it needs no forward declaration and
        // captures nothing -- all it does is fill the static s_Data table.
        auto cacheVolumetricLocations = [](GLuint prog, int progIndex)
        {
            // THE binding contract: this array is positional against the VolLoc enum. A
            // name added, removed or reordered here without a matching enum change
            // silently rebinds every uniform after it. Nothing checks the two lengths.
            // The array extent is the ENUM's sentinel count, not a literal, so adding a
            // VolLoc entry without adding a name here is a compile error rather than a
            // silent rebind. The ordering itself is what is unchecked.
            static const char *names[Renderer3DData::VolLoc_Count] = {
                // Two per-row uniforms on the first line, then the array-valued ones below. The
                // trailing [0] in each array name is what makes glGetUniformLocation return the
                // location of the WHOLE uniform array rather than of element zero, which is why
                // the upload below can then pass a count.
                "u_SunDirection", "u_Time",
                "u_FogEnabled", "u_FogCount", "u_FogMin[0]", "u_FogMax[0]", "u_FogColor[0]", "u_FogData[0]", "u_FogData2[0]",
                "u_CloudEnabled", "u_CloudCount", "u_CloudMin[0]", "u_CloudMax[0]", "u_CloudColor[0]",
                "u_CloudAmbient[0]", "u_CloudData[0]", "u_CloudData2[0]", "u_CloudData3[0]", "u_VolFast", "u_SunLightColor"};
            // Cached for BOTH programs from the same table, which is why the names live in
            // a static array rather than being written out twice.
            for (int i = 0; i < Renderer3DData::VolLoc_Count; i++)
                s_Data.Loc_Volumetric[progIndex][i] = glGetUniformLocation(prog, names[i]);
        };
        // Row 0 for the compute program, called immediately after the lambda is defined.
        // The two calls below are the only two uses of this table, one per program.
        cacheVolumetricLocations(rtProg, 0);
        cacheVolumetricLocations(s_Data.BasicShader->GetRendererID(), 1);
        // The same shape as the volumetric table above, for analytic lights.
        auto cacheAnalyticLocations = [](GLuint prog, int progIndex)
        {
            static const char *names[Renderer3DData::ALoc_Count] = {
                "u_ALightCount", "u_ALightPos[0]", "u_ALightDir[0]",
                "u_ALightColor[0]", "u_ALightData[0]", "u_ALightData2[0]"};
            for (int i = 0; i < Renderer3DData::ALoc_Count; i++)
                s_Data.Loc_ALights[progIndex][i] = glGetUniformLocation(prog, names[i]);
        };
        // The analytic light table follows the same two-row shape as the volumetric one,
        // so a program bound anywhere in Flush always has a matching row of locations.
        cacheAnalyticLocations(rtProg, 0);
        cacheAnalyticLocations(s_Data.BasicShader->GetRendererID(), 1);

        // Cache uniform locations for global fog and god rays (Nova only)
        // Global fog and god rays exist only in the Nova program; the Basic raster
        // shader has no such uniforms, so these are never looked up for row 1.
        // Global fog and god rays are cached for rtProg ONLY -- row 1 of the Basic raster
        // shader is deliberately left at -1, because that program declares none of these
        // uniforms. That is why every upload below is guarded rather than assumed.
        // The start of the global-fog and god-ray block. Both exist only in the Nova
        // compute program, so these stay -1 for the raster row and every upload of them is
        // guarded.
        s_Data.Loc_GlobalFogEnabled = glGetUniformLocation(rtProg, "u_GlobalFogEnabled");
        s_Data.Loc_GlobalFogDensity = glGetUniformLocation(rtProg, "u_GlobalFogDensity");
        s_Data.Loc_GlobalFogHeightFalloff = glGetUniformLocation(rtProg, "u_GlobalFogHeightFalloff");
        s_Data.Loc_GlobalFogColor = glGetUniformLocation(rtProg, "u_GlobalFogColor");
        s_Data.Loc_GlobalFogBaseHeight = glGetUniformLocation(rtProg, "u_GlobalFogBaseHeight");
        s_Data.Loc_GlobalFogDistAtten = glGetUniformLocation(rtProg, "u_GlobalFogDistAtten");
        s_Data.Loc_GodRaysEnabled = glGetUniformLocation(rtProg, "u_GodRaysEnabled");
        s_Data.Loc_GodSunScreenPos = glGetUniformLocation(rtProg, "u_GodSunScreenPos");
        s_Data.Loc_GodSunValid = glGetUniformLocation(rtProg, "u_GodSunValid");
        s_Data.Loc_GodRayIntensity = glGetUniformLocation(rtProg, "u_GodRayIntensity");
        s_Data.Loc_GodRayDecay = glGetUniformLocation(rtProg, "u_GodRayDecay");
        s_Data.Loc_GodRaySamples = glGetUniformLocation(rtProg, "u_GodRaySamples");
        s_Data.Loc_GodRayDensity = glGetUniformLocation(rtProg, "u_GodRayDensity");
        s_Data.Loc_GodRayColor = glGetUniformLocation(rtProg, "u_GodRayColor");
        // The post-process block. Bloom, vignette, chromatic aberration, colour grading
        // and film grain all live in the composite pass, so these locations are only
        // meaningful while that pass's program is bound.
        // The post-process block closes out the caching. These locations are only
        // meaningful while the composite pass's program is bound, which is why the GL
        // uploads for them sit late in Flush rather than with the pass-0 batch.
        s_Data.Loc_BloomEnabled = glGetUniformLocation(rtProg, "u_BloomEnabled");
        s_Data.Loc_BloomIntensity = glGetUniformLocation(rtProg, "u_BloomIntensity");
        s_Data.Loc_BloomThreshold = glGetUniformLocation(rtProg, "u_BloomThreshold");
        s_Data.Loc_VignetteEnabled = glGetUniformLocation(rtProg, "u_VignetteEnabled");
        s_Data.Loc_VignetteIntensity = glGetUniformLocation(rtProg, "u_VignetteIntensity");
        s_Data.Loc_VignetteRadius = glGetUniformLocation(rtProg, "u_VignetteRadius");
        s_Data.Loc_VignetteSoftness = glGetUniformLocation(rtProg, "u_VignetteSoftness");
        s_Data.Loc_ChromaticAberrationEnabled = glGetUniformLocation(rtProg, "u_ChromaticAberrationEnabled");
        s_Data.Loc_ChromaticAberrationIntensity = glGetUniformLocation(rtProg, "u_ChromaticAberrationIntensity");
        s_Data.Loc_ColorGradingEnabled = glGetUniformLocation(rtProg, "u_ColorGradingEnabled");
        s_Data.Loc_ColorTemperature = glGetUniformLocation(rtProg, "u_ColorTemperature");
        s_Data.Loc_ColorTint = glGetUniformLocation(rtProg, "u_ColorTint");
        s_Data.Loc_ColorGamma = glGetUniformLocation(rtProg, "u_ColorGamma");
        s_Data.Loc_FilmGrainEnabled = glGetUniformLocation(rtProg, "u_FilmGrainEnabled");
        s_Data.Loc_FilmGrainIntensity = glGetUniformLocation(rtProg, "u_FilmGrainIntensity");

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AccumulationTexture);
        glTextureStorage2D(s_Data.AccumulationTexture, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTextureID);
        glTextureStorage2D(s_Data.BloomTextureID, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTempTextureID);
        glTextureStorage2D(s_Data.BloomTempTextureID, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glTextureParameteri(s_Data.BloomTempTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTempTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.VelocityTextureID);
        glTextureStorage2D(s_Data.VelocityTextureID, 1, GL_RG16F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AlbedoTextureID);
        glTextureStorage2D(s_Data.AlbedoTextureID, 1, GL_RGBA8, s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        glTextureParameteri(s_Data.AlbedoTextureID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(s_Data.AlbedoTextureID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // SVGF temporal moments, RGBA32F, one texel per traced pixel. The shader
        // aliases image unit 6 with the trace G-buffer only during the SVGF passes.
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.MomentsTextureID);
        glTextureStorage2D(s_Data.MomentsTextureID, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        glTextureParameteri(s_Data.MomentsTextureID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(s_Data.MomentsTextureID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Trace G-buffer (full-size, valid data in the trace subregion):
        // R=NDC depth, GBA=packed normal. Written by pass 1, sampled by
        // passes 0/2/7. Shares one RGBA32F image on unit 6 because this GL
        // driver caps image bindings at 0-7 (units 0-5,7 taken, 6 was dead).
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.TraceGBufferTextureID);
        glTextureStorage2D(s_Data.TraceGBufferTextureID, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        float gbufferInit[4] = {1.0f, 0.5f, 0.5f, 1.0f};
        glClearTexImage(s_Data.TraceGBufferTextureID, 0, GL_RGBA, GL_FLOAT, gbufferInit);

        // TWO accumulation textures created in one call, because the scheme is a ping-pong:
        // one is read as the previous frame's history while the other is written. The
        // index flips at the end of every Flush.
        glCreateTextures(GL_TEXTURE_2D, 2, s_Data.AccumulationTextures);
        for (int i = 0; i < 2; i++)
        {
            // RGBA32F at the full tracing resolution with a single mip level. Float precision
            // is required because the buffer stores an accumulated radiance average; 8-bit
            // would quantize the history and band visibly under reprojection.
            glTextureStorage2D(s_Data.AccumulationTextures[i], 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);
            // LINEAR filtering both ways, so a reprojected history read can land between
            // texels. NEAREST would be cheaper but would quantize the reprojection to whole
            // pixels and produce visible ghosting during camera motion.
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // CLAMP_TO_EDGE rather than REPEAT, because reprojected coordinates can fall
            // outside 0..1 at the frame edge. Wrapping would blend the opposite side of the
            // screen into the history.
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        uint32_t maxInstances = s_Data.MaxRayTracingInstances;
        uint32_t maxBVHNodes = maxInstances * 2;
        glCreateBuffers(1, &s_Data.SceneInstanceBufferID);
        glNamedBufferData(s_Data.SceneInstanceBufferID, maxInstances * sizeof(RayTracingInstance), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_Data.SceneInstanceBufferID);

        glCreateBuffers(1, &s_Data.BVHBufferID);
        glNamedBufferData(s_Data.BVHBufferID, maxBVHNodes * sizeof(BVHNodeGPU), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_Data.BVHBufferID);

        // An SSBO holding the indices of emissive instances, so the trace shader can find
        // the lights without scanning the whole instance array. Bound to binding point 3
        // on the next line.
        glCreateBuffers(1, &s_Data.LightListBufferID);
        glNamedBufferData(s_Data.LightListBufferID, maxInstances * sizeof(int), nullptr, GL_DYNAMIC_DRAW);
        // The BINDING POINT is the contract, not the order of creation: the shader
        // declares layout(binding = 3). Rebinding elsewhere, or after another SSBO claims
        // point 3, would silently point the shader at the wrong buffer.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s_Data.LightListBufferID);

        // The ReSTIR reservoir ping-pong pair. Created UNCONDITIONALLY (like the
        // three SSBOs above) so binding points 26 and 10 never name buffer 0,
        // which would be an incomplete binding -- a GL error at dispatch even on
        // a frame where RestirEnabled is false.
        //
        // The capacity is per-pixel at the maximum TRACE resolution (the full
        // target). Active trace dimensions shrink with RenderScale and are checked
        // against this capacity before dispatch. 32 bytes per pixel is the
        // LightReservoir struct size in both twins; if that struct ever grows,
        // this constant must move with it or every element past the first is
        // misindexed.
        EnsureReservoirBuffers(s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        // Slot 0's one texel, RGBA white in every channel including alpha. This is why
        // every untextured material resolves to slot 0 rather than to a special case: an
        // untextured surface is simply one painted white.
        uint32_t whiteTextureData = 0xffffffff;
        // Held in the static slot array as a Ref, so its lifetime is tied to the slot
        // table. FlushAndReset resets TextureSlotIndex to 1 and never clears slot 0, so
        // this entry survives every frame boundary.
        Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
        whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
        s_Data.TextureSlots[0] = whiteTexture;

        // Reserved up front to the same maxInstances the buffers were sized for. Without
        // this the push_back in every Draw* would reallocate mid-frame and invalidate any
        // pointer a caller still held.
        s_Data.m_SceneInstances.reserve(maxInstances);

        // Must precede every glUniform call below, since uniform updates apply to
        // whatever program is currently bound.
        s_Data.RayTracingShader->Bind();
        int samplers[32];
        for (int i = 0; i < 32; i++)
            samplers[i] = i + 15;
        s_Data.RayTracingShader->SetIntArray("u_SceneTextures", samplers, 32);

        DetectGPUTier();
        // The three GL timer queries the dynamic-resolution scaler uses. Created HERE,
        // at the very end of the OpenGL path -- which is why the NVRHI path never has
        // them, and why UpdateDynamicResolution gets no GPU samples on DX11/DX12.
        glGenQueries(3, s_Data.DRSTimerQueries);

        ApplyQualityPreset();
    }

    void Renderer3D::RecreateResources()
    {
        // Renderer2D has the same API-switch problem: Ref<> handles own
        // backend-specific GPU objects. Rebuild all 3D resources after the
        // GLFW/renderer API switch while reusing CPU batch memory.
        Shutdown();
        Init();
    }

    void Renderer3D::Shutdown()
    {
        WL_PROFILE_FUNCTION();
        // The three CPU staging arrays, freed by hand because they are raw new[]
        // allocations made in Init rather than Ref<> managed buffers.
        // Manual frees for the three raw staging arrays. Everything else in this file is
        // either a Ref<> or a GL name, both of which clean themselves up -- these three are
        // the exception and the reason Shutdown exists at all.
        delete[] s_Data.CubeVertexBufferBase;
        delete[] s_Data.SphereVertexBufferBase;
        delete[] s_Data.SphereIndexBufferBase;

        // Destroy all Nova backends before any GL names below and while the NVRHI device
        // is still alive. The GL names are guarded because Nova initialization never creates them.
        s_Nova.reset();
        s_Nova12.reset();
        s_NovaVk.reset();
        // Every GL name deleted below was only ever created by the OpenGL path, so the
        // deletes are guarded -- deleting a name that was never created is undefined,
        // and on NVRHI every one of them is still 0. The Nova owners were reset just
        // above this guard, outside it, so their GPU resources are released while the
        // device context is still valid. Init constructs at most one backend instance.
        if(Renderer3DGLAvailable()) {
        glDeleteBuffers(1, &s_Data.SceneInstanceBufferID);
        glDeleteBuffers(1, &s_Data.BVHBufferID);
        glDeleteBuffers(1, &s_Data.LightListBufferID);
        // The reservoir pair. Guarded on non-zero because Init only creates them
        // when RestirEnabled was set; glDeleteBuffers on a never-created name is
        // undefined.
        for (int ri = 0; ri < 2; ++ri)
            if (s_Data.ReservoirBufferID[ri]) glDeleteBuffers(1, &s_Data.ReservoirBufferID[ri]);
        glDeleteTextures(1, &s_Data.AccumulationTexture);
        glDeleteTextures(1, &s_Data.BloomTextureID);
        glDeleteTextures(1, &s_Data.BloomTempTextureID);
        glDeleteTextures(1, &s_Data.VelocityTextureID);
        glDeleteTextures(1, &s_Data.AlbedoTextureID);
        glDeleteTextures(1, &s_Data.TraceGBufferTextureID);
        glDeleteTextures(2, s_Data.AccumulationTextures);

        // Only when non-zero: glDeleteQueries on an ungenerated name is an error. The
        // three are zeroed together afterwards, so a second Shutdown is a no-op.
        if (s_Data.DRSTimerQueries[0] != 0)
        {
            glDeleteQueries(3, s_Data.DRSTimerQueries);
            s_Data.DRSTimerQueries[0] = s_Data.DRSTimerQueries[1] = s_Data.DRSTimerQueries[2] = 0;
        }

        }
        // Names zeroed unconditionally, outside the GL guard, so the struct never
        // carries a stale name that a later Init could mistake for a live object.
        s_Data.SceneInstanceBufferID=s_Data.BVHBufferID=s_Data.LightListBufferID=0;
        s_Data.ReservoirBufferID[0]=s_Data.ReservoirBufferID[1]=0;
        s_Data.ReservoirBytes=0;
        s_Data.ReservoirWidth=s_Data.ReservoirHeight=0;
        s_Data.ReservoirParity=0;
        s_Data.AccumulationTexture=s_Data.BloomTextureID=s_Data.BloomTempTextureID=0;
        s_Data.VelocityTextureID=s_Data.AlbedoTextureID=s_Data.TraceGBufferTextureID=0;
        s_Data.AccumulationTextures[0]=s_Data.AccumulationTextures[1]=0;
        s_Data.DRSTimerQueries[0]=s_Data.DRSTimerQueries[1]=s_Data.DRSTimerQueries[2]=0;
        s_Data.CubeVertexBufferBase=s_Data.SphereVertexBufferBase=nullptr;
        s_Data.SphereIndexBufferBase=nullptr;
        // The previous-frame snapshot is dropped, so the first SceneInstancesChanged
        // after a teardown reports dirty and forces a rebuild -- which is correct.
        s_Data.m_PrevSceneInstances.clear();
        s_Data.m_TerrainVertices.clear();
        s_Data.m_PrevTerrainVertices.clear();
        s_Data.m_TerrainInstances.clear();
        // Slots released; the array SIZE is the compile-time MaxTextureSlots, so the
        // table is immediately reusable. TextureSlotIndex is NOT reset here, which is
        // harmless only because Shutdown is always followed by Init.
        s_Data.TextureSlots.fill(nullptr);
        s_Data.TextureSlotMap.clear();
        s_Data.CubeVertexArray = nullptr;
        s_Data.CubeVertexBuffer = nullptr;
        s_Data.SphereVertexArray = nullptr;
        s_Data.SphereVertexBuffer = nullptr;
        s_Data.SphereIndexBuffer = nullptr;
        s_Data.BasicShader = nullptr;
        s_Data.RayTracingShader = nullptr;
        s_Data.RayTracingOutput = nullptr;
    }

    static bool Renderer3DGLAvailable()
    {
        // The single-backend test. Every GL call site in this file is guarded by this
        // or by Renderer3DNVRHIAvailable(); there is no third state.
        return RendererAPI::GetAPI() == RendererAPI::API::OpenGL;
    }

    static bool Renderer3DNVRHIAvailable()
    {
        // Init() creates the matching Nova implementation for each supported NVRHI API.
        return RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX11 ||
               RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX12 ||
               RendererAPI::GetAPI() == RendererAPI::API::NVRHI_Vulkan;
    }

    // The game-camera overload. It takes the view transform separately rather than
    // having the camera own it, because a game camera is positioned by gameplay.
    // This one of the pair rebuilds the frustum planes; the EditorCamera overload
    // below does not.
    void Renderer3D::BeginScene(const Camera &camera, const glm::mat4 &transform)
    {
        WL_PROFILE_FUNCTION();

        if (!Renderer3DGLAvailable() && !Renderer3DNVRHIAvailable())
            return;
        // New frame: drop last frame's volumes (kept across mid-frame overflow
        // flushes, unlike the geometry batches reset in FlushAndReset).
        // ReportAnalyticLightOverflow runs BEFORE the clear so it still sees the
        // previous frame's admitted count; see its definition for why the report is
        // emitted here rather than at push time.
        ReportAnalyticLightOverflow();
        s_Data.FogVolumes.clear();
        s_Data.CloudVolumes.clear();
        s_Data.AnalyticLights.clear();
        // The ray-traced instance list is cleared with the batches. That is safe for a
        // mid-frame overflow because the overflow Flush has already built the BVH and
        // uploaded it -- but it does mean a mid-frame flush forces a BVH rebuild for
        // the second half of the frame.
        s_Data.m_SceneInstances.clear();
        s_Data.m_TerrainVertices.clear();
        s_Data.m_TerrainInstances.clear();

        // projection * inverse(view). glm::inverse on a general 4x4 is a full
        // determinant computation -- done once here and once more below for the
        // view-projection, so two inversions happen every frame.
        glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);
        // Records WHICH camera is active. Everything downstream that must behave
        // differently in editor versus game keys off this rather than off the camera's
        // type, which is why the flag exists at all.
        s_Data.ActiveCameraIsEditor = false;

        // Plane extraction as a lambda capturing viewProj by reference. Gribb/Hartmann:
        // each plane is a sum or difference of two rows of the combined matrix.
        auto extract = [&](int row, int sign) -> Renderer3DData::Plane
        {
            glm::vec4 planeEq;
            // Four components, because the plane equation is homogeneous. Dropping w here
            // would lose the offset term entirely and every cull test would collapse.
            for (int i = 0; i < 4; ++i)
                // The sign selects the half-space: positive keeps the region where that row is
                // positive, negative the opposite. The same row negated gives the far plane.
                planeEq[i] = viewProj[i][3] + sign * viewProj[i][row];

            // The full 3-component length of the xyz part, used to normalize both the
            // normal and the distance -- which is what makes the distance a metric one.
            float length = glm::length(glm::vec3(planeEq));

            // Dividing w by the same length keeps the distance consistent with a unit
            // normal, so the sphere-vs-plane test reduces to dot(n,c) + d > radius.
            return Renderer3DData::Plane{glm::vec3(planeEq) / length, planeEq.w / length};
        };

        // Positional: index 0 is always Left, through 5 always Far. The trailing
        // comments are the only record of that mapping, so reordering these six lines
        // silently breaks every cull test rather than failing to compile.
        s_Data.FrustumPlanes[0] = extract(0, 1);  // Left
        s_Data.FrustumPlanes[1] = extract(0, -1); // Right
        s_Data.FrustumPlanes[2] = extract(1, 1);  // Bottom
        s_Data.FrustumPlanes[3] = extract(1, -1); // Top
        s_Data.FrustumPlanes[4] = extract(2, 1);  // Near
        s_Data.FrustumPlanes[5] = extract(2, -1); // Far

        // The velocity pair, shifted one frame. This must run BEFORE Current is
        // overwritten -- that ordering is what makes reprojection work.
        // Done only on the OpenGL path. The Nova schedule folds the same update into
        // RunNovaPassSchedule, so the two backends advance this at different points
        // relative to their final pass.
        s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
        s_Data.CurrentViewProjection = viewProj;
        // A second inverse this frame, independent of the view-only inverse above.
        // Cached because the raymarch shader samples by world position every step.
        s_Data.InverseViewProjection = glm::inverse(viewProj);

        // The raster tail, reached when ray tracing is off. Same shader the NVRHI
        // fallback uses, so all three backends converge on one raster path.
        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        // Accumulation restarts on ANY matrix difference. Compared with != on floats,
        // so a 1-ULP change from an algebraically identical recomputation resets the
        // path tracer to zero samples and costs a full re-convergence.
        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
        }

        // Position from the transform's translation column. DEFECT: pitch and yaw are
        // hard-zeroed on the next two lines rather than derived from the matrix, so on
        // this overload the RT path believes the camera never rotates -- anything keyed
        // on rotation (motion vectors, specular anti-aliasing) is frozen.
        s_Data.CurrentCameraPosition = glm::vec3(transform[3]);
        s_Data.CurrentCameraPitch = 0.0f;
        s_Data.CurrentCameraYaw = 0.0f;

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);

        if(Renderer3DGLAvailable()) {
        s_Data.RayTracingShader->Bind();
        // DEFECT: the guard two lines above uses the one-brace `if (...) {` style with
        // its body at the outer indentation level, unlike the same block in the
        // EditorCamera overload below, which is properly braced. Cosmetic only.
        glUniformMatrix4fv(s_Data.Loc_ViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform3f(s_Data.Loc_CameraPosition, s_Data.CurrentCameraPosition.x, s_Data.CurrentCameraPosition.y, s_Data.CurrentCameraPosition.z);
        }
        FlushAndReset();
    }

    // The editor-camera overload. It rebuilds the frustum planes from the same combined
    // view-projection it already holds: Gribb/Hartmann extraction needs only that one
    // matrix, never the projection and view separately, so there was no reason to leave
    // FrustumPlanes holding whatever another camera last computed.
    void Renderer3D::BeginScene(const EditorCamera &camera)
    {
        WL_PROFILE_FUNCTION();

        if (!Renderer3DGLAvailable() && !Renderer3DNVRHIAvailable())
            return;
        // New frame: drop last frame's volumes (kept across mid-frame overflow
        // flushes, unlike the geometry batches reset in FlushAndReset).
        // Same reason and same ordering as the game-camera overload above.
        ReportAnalyticLightOverflow();
        s_Data.FogVolumes.clear();
        s_Data.CloudVolumes.clear();
        s_Data.AnalyticLights.clear();
        s_Data.m_SceneInstances.clear();
        s_Data.m_TerrainVertices.clear();
        s_Data.m_TerrainInstances.clear();

        // Supplied directly by EditorCamera, so no inverse is needed to build it --
        // though one is still taken below for the shader's world-space sampling. The
        // game-camera overload instead computes projection * inverse(transform).
        glm::mat4 viewProj = camera.GetViewProjection();
        // The inverse of the flag set by the other overload. Together the two lines are
        // what let later code ask "am I in the editor" without knowing which overload
        // was called.
        s_Data.ActiveCameraIsEditor = true;

        s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
        s_Data.CurrentViewProjection = viewProj;
        s_Data.InverseViewProjection = glm::inverse(viewProj);

        // Gribb/Hartmann plane extraction, identical to the game-camera overload but
        // sourced from GetViewProjection. Without this the editor submitted every entity
        // in the scene every frame -- FrustumPlanes held either zeros (first frame) or
        // the game camera's planes -- so a 1729-entity scene emitted ~269k quads per
        // frame for a viewport that could see a fraction of them.
        //
        // A degenerate matrix yields a zero-length plane; the resulting NaN normal fails
        // every comparison, so the cull test rejects nothing. It fails open, which is the
        // safe direction for a plane that cannot be trusted.
        auto extract = [&](int row, int sign) -> Renderer3DData::Plane
        {
            glm::vec4 planeEq;
            for (int i = 0; i < 4; ++i)
                planeEq[i] = viewProj[i][3] + sign * viewProj[i][row];
            const float length = glm::length(glm::vec3(planeEq));
            return Renderer3DData::Plane{glm::vec3(planeEq) / length, planeEq.w / length};
        };

        // Positional mapping, index 0 Left through 5 Far. Reordering silently breaks
        // every cull test rather than failing to compile.
        s_Data.FrustumPlanes[0] = extract(0, 1);  // Left
        s_Data.FrustumPlanes[1] = extract(0, -1); // Right
        s_Data.FrustumPlanes[2] = extract(1, 1);  // Bottom
        s_Data.FrustumPlanes[3] = extract(1, -1); // Top
        s_Data.FrustumPlanes[4] = extract(2, 1);  // Near
        s_Data.FrustumPlanes[5] = extract(2, -1); // Far

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
        }

        // A real position, pitch and yaw here, where the game-camera overload hard-
        // zeros rotation. Anything downstream that keys on rotation therefore behaves
        // differently between the two paths.
        s_Data.CurrentCameraPosition = camera.GetPosition();
        s_Data.CurrentCameraPitch = camera.GetPitch();
        s_Data.CurrentCameraYaw = camera.GetYaw();

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);

        // Properly braced here, unlike the equivalent block in the game-camera
        // overload. Both compile; the inconsistency is only visible side by side.
        if (Renderer3DGLAvailable())
        {
            s_Data.RayTracingShader->Bind();
            glUniformMatrix4fv(s_Data.Loc_ViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));
            glUniform3f(s_Data.Loc_CameraPosition, s_Data.CurrentCameraPosition.x, s_Data.CurrentCameraPosition.y, s_Data.CurrentCameraPosition.z);
        }

        FlushAndReset();
    }

    // A thin wrapper around Flush(). It exists so the caller has a symmetric
    // Begin/End pair.
    void Renderer3D::EndScene()
    {
        WL_PROFILE_FUNCTION();
        // Mid-frame overflow flush: submit what is accumulated so far, then reset the
        // batch. This is why the volume and light lists are NOT cleared by
        // FlushAndReset -- they must survive to be uploaded with this same frame.
        Flush();
    }
    // Shared Nova pass schedule for the DX11, DX12, and Vulkan backends.
    //
    // The three Nova flushes share this schedule and differ in the scene-texture
    // vector type (DX11 SRVs, DX12 resources, or Ref<Texture2D> on Vulkan).
    //
    // THE PASS ORDER IS NOT ASCENDING AND MUST NOT BE "FIXED". Pass 1 (hybrid
    // trace) runs before pass 0 (visibility + velocity) because velocity
    // reprojection in pass 2 needs the G-buffer pass 1 has just produced.
    // Sorting this sequence changes image content.
    template <typename NovaT, typename TexVec>
    static void RunNovaPassSchedule(NovaT &n, const TexVec &textures)
    {
        // `full` selects RT dimensions rather than the possibly smaller dynamic-
        // resolution compute dimensions. Passes producing final pixels need the former;
        // passes that only feed a later pass tolerate the smaller size.
        auto pass=[&](int p,bool full=false){n.Dispatch(p,full?s_Data.RayTracingWidth:s_Data.ComputeWidth,full?s_Data.RayTracingHeight:s_Data.ComputeHeight,textures);};
        // Order is load-bearing, NOT ascending: pass 1 (trace) must precede pass 0
        // (visibility/velocity) because pass 2's velocity reprojection consumes the
        // G-buffer pass 1 has just written. ReSTIR, when enabled, runs after both
        // producers but BEFORE pass 2. Pass 11 merges temporal reservoirs only;
        // pass 14 performs immutable-input spatial reuse and adds the sole ReSTIR
        // direct-light estimate to img_Bloom before temporal accumulation.
        const int emissiveLightCount = (int)s_Data.m_LightIndices.size();
        if(s_Data.RestirEnabled)n.Set("u_LightCount",0); // omit legacy emissive-light subset in pass 1
        pass(1);pass(0);
        if(s_Data.RestirEnabled){
            n.Set("u_LightCount",emissiveLightCount);
            pass(3);
            pass(11);
            // Expose pass 11's temporal result as an immutable SRV; pass 14 writes
            // the now-dead history buffer, then the second swap promotes that output.
            n.SwapReservoir();
            pass(14);
            n.SwapReservoir();
        }
        pass(2);n.CommitHistory();
        // SVGF denoiser: temporal moments (pass 12) then a fixed 4-step A-Trous
        // ladder (pass 13). OFF by default -- see s_Data.SvfgEnabled for why.
        //
        // The ladder MUST be driven from here rather than looped inside the shader.
        // A real A-Trous filter feeds iteration N's output into iteration N+1; a
        // shader-side loop that re-reads img_Accumulation each time computes four
        // copies of iteration 1 and keeps only the last, so the step sizes do
        // nothing. It cannot ping-pong internally either, because one image cannot
        // be read and written in the same dispatch without a barrier. So each
        // iteration is its own dispatch, and CommitDenoise() -- the same
        // images[7] -> images[1] copy the bilateral blur above relies on -- carries
        // the result forward.
        //
        // Pass 12 must follow pass 1 (it reads img_Bloom, which only the trace
        // writes) and pass 13 must follow pass 2 (it reads img_Accumulation).
        if(s_Data.SvfgEnabled){
            pass(12);
            for(int i=0;i<glm::min(s_Data.SvfgPasses,4);++i){n.Set("u_StepSize",1<<i);pass(13);n.CommitDenoise();}
        }

        // The denoiser ping-pongs: each of up to 4 passes widens the step by a factor
        // of two (1<<i) and commits, so a 4-pass blur reaches 8 texels either side.
        if(s_Data.BilateralBlurEnabled)for(int i=0;i<glm::min(s_Data.BilateralBlurPasses,4);++i){n.Set("u_StepSize",1<<i);pass(7);n.CommitDenoise();}
        pass(6,true);if(s_Data.BloomEnabled){pass(4,true);pass(5,true);}
        // God rays last, and only when the sun is genuinely on screen. Gated on
        // GodSunValid because shafts from a sun behind the camera smear rather than
        // radiating.
        if(s_Data.GodRaysEnabled&&s_Data.GodSunValid)pass(10,true);
        // FrameIndex advances HERE, once per completed schedule, and Prev is folded in
        // at the same moment so the two can never disagree about which frame is which.
        ++s_Data.FrameIndex;s_Data.PrevViewProjection=s_Data.CurrentViewProjection;++s_Data.Stats.DrawCalls;
    }
    static void FlushDX11Nova()
    {
        auto& n=*s_Nova;
        // Resized every frame rather than only on change: Nova's Resize is expected to
        // be a cheap no-op when the dimensions match, which avoids tracking a flag.
        n.Resize(s_Data.RayTracingWidth,s_Data.RayTracingHeight);
        // FrameIndex==0 forces a rebuild even for a static scene, because restarting
        // accumulation means new samples must be traced against the existing SSBO.
        if(SceneInstancesChanged() || s_Data.FrameIndex==0) {
            BuildAndFlattenBVH(s_Data.m_SceneInstances,s_Data.m_BVHNodes,s_Data.m_BVHPrimOrder);
            s_Data.m_ReorderedInstances.resize(s_Data.m_BVHPrimOrder.size());
            s_Data.m_LightIndices.clear();
            // The reorder loop: instances are permuted into BVH traversal order so shader
            // memory access follows the tree. The index pushed for lights is the POSITION
            // IN THE REORDERED ARRAY, which is what the shader later indexes with.
            for(size_t i=0;i<s_Data.m_BVHPrimOrder.size();++i) {
                auto& v=s_Data.m_ReorderedInstances[i];v=s_Data.m_SceneInstances[s_Data.m_BVHPrimOrder[i]];
                // Emission.w > 0 marks an emissive mesh that should also act as a light, so the
                // light list is derived from geometry, not only from Light components.
                if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
            }
            // Three uploads in the order the shader expects: 0 = instances, 1 = BVH nodes,
            // 2 = light index list. These slot numbers are a shader ABI, not an index.
            n.Upload(0,s_Data.m_ReorderedInstances.data(),(UINT)s_Data.m_ReorderedInstances.size(),sizeof(RayTracingInstance));
            n.Upload(1,s_Data.m_BVHNodes.data(),(UINT)s_Data.m_BVHNodes.size(),sizeof(BVHNodeGPU));
            n.Upload(2,s_Data.m_LightIndices.data(),(UINT)s_Data.m_LightIndices.size(),sizeof(int));
            s_Data.m_PrevSceneInstances=s_Data.m_SceneInstances;
            s_Data.FrameIndex=0;
        }
        // The live count is the REORDERED count, which is why this reads
        // m_ReorderedInstances and not m_SceneInstances. The two are equal in size, but
        // only the reordered one is what the GPU actually holds.
        n.Set("u_InstanceCount",(int)s_Data.m_ReorderedInstances.size());
        n.Set("u_LightCount",(int)s_Data.m_LightIndices.size());
        n.Set("u_FrameIndex",(int)s_Data.FrameIndex);
        n.Set("u_QualityLevel",(int)s_Data.CurrentQualityPreset);
        // The accumulation-restart flag the shader actually reads, as distinct from the
        // FrameIndex reset in BeginScene. Two mechanisms for one idea.
        n.Set("u_CameraMoved",s_Data.CurrentViewProjection!=s_Data.PrevViewProjection?1.0f:0.0f);
        // TAA jitter from the generated blue-noise volume, in PIXEL units. This
        // upload was missing on both DX backends: u_Jitter was declared in the
        // cbuffer and has a known offset (VulkanNova.cpp:37), but nothing ever
        // set it here, so the shader's GetJitter() computed Halton itself and
        // the 16-offset repeat survived the generator landing. GetCurrentJitter
        // is frame-index seeded and the host already resets FrameIndex on a
        // view change and on the still-frame latch, so no camera-moved
        // correction is applied here. u_PrevJitter is set to the same value:
        // the shader reads only u_Jitter, and a zero previous jitter would be a
        // lie if it ever starts reading it.
        {
            // RayTracingWidth/Height, not the window size: this is the size of
            // the trace target the shader offsets into, and it is already in
            // scope here. width/height are not visible in this flush.
            // Renderer3D:: qualification is REQUIRED here: FlushDX11Nova and
            // FlushDX12Nova are file-scope statics, not members, so an
            // unqualified call fails with C3861. The GL path's copy of this
            // upload sits inside a Renderer3D member and does not need it.
            glm::vec2 jit = Renderer3D::GetCurrentJitter(s_Data.FrameIndex, glm::vec2(
                (float)s_Data.RayTracingWidth, (float)s_Data.RayTracingHeight));
            n.Set("u_Jitter", jit);
            n.Set("u_PrevJitter", jit);
        }
        // 1/(N+1) is the running mean of N+1 samples. The 0.02 floor stops the weight
        // reaching zero at high sample counts, so old samples are never fully
        // forgotten and noise keeps creeping in.
        n.Set("u_AccumulationAlpha",s_Data.RayTracingAccumulate?glm::max(0.02f,1.0f/(s_Data.FrameIndex+1)):1.0f);
        n.Set("u_CameraPosition",s_Data.CurrentCameraPosition);
        n.Set("u_InverseViewProjection",s_Data.InverseViewProjection);
        n.Set("u_PrevViewProjection",s_Data.PrevViewProjection);
        n.Set("u_ViewProjection",s_Data.CurrentViewProjection);
        n.Set("u_SkyBottomColor",s_Data.SkyBottomColor);
        n.Set("u_SkyTopColor",s_Data.SkyTopColor);
        n.Set("u_RenderScale",s_Data.RenderScale);
        // DEAD uniform, uploaded every frame. Kept only because it occupies a fixed
        // slot in the packed constant buffer; removing it would shift every later
        // uniform's CPU-side offset.
        n.Set("u_MaxBounces",s_Data.MaxBounces); // D3: dead uniform, kept for CBV layout
        // A cap on how many analytic lights any one shading point evaluates. Unlike the
        // light LIST length (capped by the literal 8 at upload), this bounds the inner
        // loop, so a scene with many lights can exceed the list without exceeding the
        // per-sample cost.
        n.Set("u_MaxLights",s_Data.MaxLights);
        // Whether secondary bounces are traced at all. Together with MaxBounces -- which
        // is dead -- this is the only uniform left that actually changes ray depth, so it
        // is what makes the quality presets differ in cost.
        n.Set("u_IndirectRays",s_Data.IndirectRays);
        n.Set("u_SamplesPerPixel",s_Data.SamplesPerPixel); // D3: dead uniform, kept for CBV layout
        n.Set("u_NeuralTexStrength",s_Data.NeuralTextureStrength);
        n.Set("u_NeuralLightStrength",s_Data.NeuralLightStrength);
        // The third neural weight, blending between the traced material response and the
        // network's. All three neural uniforms are uploaded together because the passes
        // that consume them run adjacently.
        n.Set("u_NeuralMatStrength",s_Data.NeuralMaterialStrength);
        n.Set("u_GlobalFogDensity",s_Data.GlobalFogDensity);
        n.Set("u_GlobalFogHeightFalloff",s_Data.GlobalFogHeightFalloff);
        // The six global-fog uniforms: density, height falloff, colour, base height and
        // distance attenuation. Distinct from the per-volume fog above -- this is the
        // atmospheric term applied everywhere rather than inside a bounding box.
        n.Set("u_GlobalFogColor",s_Data.GlobalFogColor);
        n.Set("u_GlobalFogBaseHeight",s_Data.GlobalFogBaseHeight);
        n.Set("u_GlobalFogDistAtten",s_Data.GlobalFogDistAtten);
        // The five god-ray parameters, uploaded unconditionally. Note the setter for each
        // clamps differently -- decay is 0..1, intensity and density are lower-bounded
        // only -- so the uniform values here are already in whatever range that setter
        // established.
        n.Set("u_GodRayIntensity",s_Data.GodRayIntensity);
        n.Set("u_GodRayDecay",s_Data.GodRayDecay);
        n.Set("u_GodRaySamples",s_Data.GodRaySamples);
        n.Set("u_GodRayDensity",s_Data.GodRayDensity);
        n.Set("u_GodRayColor",s_Data.GodRayColor);
        // Post-processing (reconstructed; see RunComposite). These reach real
        // shader code now -- vignette, chromatic aberration and colour grading
        // had no implementation at all before, and the bloom threshold was
        // hardcoded inside the shader at two different values.
        n.Set("u_BloomEnabled",(int)s_Data.BloomEnabled);
        // DEFECT: uploaded UNGUARDED, unlike the GL path a few hundred lines below which
        // checks Loc_* >= 0 before every post-process upload. Harmless today because
        // Nova's Set ignores unknown names, but it means a renamed uniform fails silently
        // here while being correctly detected on GL.
        n.Set("u_BloomIntensity",s_Data.BloomIntensity);
        n.Set("u_BloomThreshold",s_Data.BloomThreshold);
        n.Set("u_VignetteEnabled",(int)s_Data.VignetteEnabled);
        // Note this uniform's setter clamps 0..2 while every neighbour clamps 0..1 --
        // SetVignetteIntensity is the one exception, presumably because a vignette can be
        // pushed harder than the other post-process amounts.
        n.Set("u_VignetteIntensity",s_Data.VignetteIntensity);
        n.Set("u_VignetteRadius",s_Data.VignetteRadius);
        n.Set("u_VignetteSoftness",s_Data.VignetteSoftness);
        n.Set("u_ChromaticAberrationEnabled",(int)s_Data.ChromaticAberrationEnabled);
        n.Set("u_ChromaticAberrationIntensity",s_Data.ChromaticAberrationIntensity);
        n.Set("u_ColorGradingEnabled",(int)s_Data.ColorGradingEnabled);
        n.Set("u_ColorTemperature",s_Data.ColorTemperature);
        n.Set("u_ColorTint",s_Data.ColorTint);
        // The four colour-grading uniforms. Gamma is the only one its setter does not
        // clamp at all, so the UI slider is the sole bound on this value.
        n.Set("u_ColorGamma",s_Data.ColorGamma);
        n.Set("u_FilmGrainEnabled",(int)s_Data.FilmGrainEnabled);
        // Grain is applied in the composite rather than per-sample, which is why it does
        // not force accumulation to restart when it changes.
        n.Set("u_FilmGrainIntensity",s_Data.FilmGrainIntensity);
        n.Set("u_NeuralEnabled",(int)s_Data.NeuralEnabled);
        n.Set("u_GlobalFogEnabled",(int)s_Data.GlobalFogEnabled);
        n.Set("u_GodRaysEnabled",(int)s_Data.GodRaysEnabled);
        n.Set("u_FogEnabled",(int)s_Data.VolFogEnabled);
        n.Set("u_CloudEnabled",(int)s_Data.VolCloudsEnabled);
        // The effective sun is recomputed here in the flush, rather than in
        // UploadVolumetricUniforms as the GL path does. First directional light wins.
        s_Data.EffSunDir=s_Data.SunDirection;s_Data.EffSunColor=glm::vec3(1.15f,1.05f,0.95f);
        for(const auto& l:s_Data.AnalyticLights) if(l.Type==0) {s_Data.EffSunDir=l.Direction;s_Data.EffSunColor=l.Color*glm::max(l.Intensity,0.0f);break;}
        n.Set("u_SunDirection",s_Data.EffSunDir);n.Set("u_SunLightColor",s_Data.EffSunColor);
        // u_VolFast uses `<= Medium` here, while the GL path spells the same condition
        // as an explicit `Low || Medium` pair. The two agree only while the preset enum
        // stays ordered Low < Medium < High < Ultra.
        n.Set("u_Time",GetVolumetricTimeSeconds());n.Set("u_VolFast",(int)(s_Data.CurrentQualityPreset<=QualityPreset::Medium));
        // 0.5 / 0.75 / 1.0 by preset. The GL path applies this factor and THEN rounds
        // and clamps per volume; here the clamp is folded into each upload with the
        // same 1..32 bounds.
        float stepFactor=s_Data.CurrentQualityPreset==QualityPreset::Low?0.5f:(s_Data.CurrentQualityPreset==QualityPreset::Medium?0.75f:1.0f);
        // DEFECT: the cap is the literal 4, not Renderer3D::MaxFogVolumes. The two
        // currently agree, so raising the header constant would silently leave this
        // path capping at 4 while the GL path used the new value.
        int fc=(int)glm::min(s_Data.FogVolumes.size(),size_t(4));n.Set("u_FogCount",fc);
        for(int i=0;i<fc;++i) {const auto& v=s_Data.FogVolumes[i];
            n.Set("u_FogMin",v.Min,i);n.Set("u_FogMax",v.Max,i);n.Set("u_FogColor",v.Color,i);
            n.Set("u_FogData",glm::vec4(v.Density,v.Anisotropy,v.NoiseStrength,v.NoiseScale),i);
            n.Set("u_FogData2",glm::vec4(v.WindSpeed,v.HeightFalloff,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f),v.Enabled?1.0f:0.0f),i);
        }
        // Same defect: literal 2 rather than MaxCloudVolumes, currently matching.
        int cc=(int)glm::min(s_Data.CloudVolumes.size(),size_t(2));n.Set("u_CloudCount",cc);
        for(int i=0;i<cc;++i) {const auto& v=s_Data.CloudVolumes[i];
            n.Set("u_CloudMin",v.Min,i);n.Set("u_CloudMax",v.Max,i);n.Set("u_CloudColor",v.Color,i);n.Set("u_CloudAmbient",v.Ambient,i);
            n.Set("u_CloudData",glm::vec4(v.Coverage,v.Density,v.NoiseScale,v.Detail),i);
            n.Set("u_CloudData2",glm::vec4(v.WindSpeed,v.WindDir.x,v.WindDir.y,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f)),i);
            n.Set("u_CloudData3",glm::vec4(v.SilverLining,v.ShadowStrength,v.Enabled?1.0f:0.0f,0),i);
        }
        // DEFECT: the analytic-light cap is the literal 8 here and on the DX11 path,
        // not Renderer3D::MaxAnalyticLights. It happens to match the header today, so
        // the GL path and both Nova paths agree -- but the constants are independent,
        // and changing the header would change only one of the three.
        int lc=(int)glm::min(s_Data.AnalyticLights.size(),size_t(8));n.Set("u_ALightCount",lc);
        for(int i=0;i<lc;++i) {const auto& v=s_Data.AnalyticLights[i];
            n.Set("u_ALightPos",v.Position,i);n.Set("u_ALightDir",v.Direction,i);n.Set("u_ALightColor",v.Color,i);
            n.Set("u_ALightData",glm::vec4(v.Type,v.Intensity,v.Range,v.Falloff),i);
            n.Set("u_ALightData2",glm::vec4(v.InnerCos,v.OuterCos,v.AreaSize,v.DoubleSided),i);
        }
        // The sun projected to screen space for god rays: a point 1000 units along the
        // effective sun direction, transformed by the current view-projection. 1000 is
        // arbitrary but must be large enough to sit beyond any geometry.
        auto sun=s_Data.CurrentViewProjection*glm::vec4(s_Data.CurrentCameraPosition+s_Data.EffSunDir*1000.0f,1);
        // Two conditions, both necessary. w > 0 rejects a sun behind the camera (w goes
        // negative in clip space). y > 0 rejects a sun below the horizon, whose shafts
        // would radiate upward from the ground.
        s_Data.GodSunValid=sun.w>0 && s_Data.EffSunDir.y>0;
        // Perspective divide, then NDC [-1,1] to UV [0,1]. When invalid this leaves
        // GodSunScreenPos at its previous value, which is harmless because the shader
        // gates on GodSunValid.
        if(s_Data.GodSunValid)s_Data.GodSunScreenPos=glm::vec2(sun)/sun.w*0.5f+0.5f;
        n.Set("u_GodSunValid",(int)s_Data.GodSunValid);n.Set("u_GodSunScreenPos",s_Data.GodSunScreenPos);
        // Rebuilt every frame from the slot table. This is the only DX11-specific type
        // in the shared schedule, which is why RunNovaPassSchedule is templated.
        std::vector<ID3D11ShaderResourceView*> textures;
        for(UINT i=0;i<s_Data.TextureSlotIndex;++i)textures.push_back((ID3D11ShaderResourceView*)s_Data.TextureSlots[i]->GetNativeSRV());
        // The templated schedule takes `n` by reference and this vector by const
        // reference, so neither is copied. Returns immediately after, so nothing below
        // touches GL state on the Nova path.
        RunNovaPassSchedule(n, textures);
    }

    // The DX12 flush. Every line below is a byte-for-byte mirror of FlushDX11Nova
    // with two differences: s_Nova12 instead of s_Nova, and ID3D12Resource* instead
    // of ID3D11ShaderResourceView* for the texture vector. The duplication is a
    // maintenance hazard -- a fix applied to one path is silently absent from the
    // other, which is exactly why the pass schedule itself was extracted.
    // The DX12 twin of FlushDX11Nova above. The bodies are near-identical line for
    // line -- same uniforms, same BVH rebuild, same volume and light upload loop --
    // differing only in the resource type each upload casts to. That duplication is
    // the single biggest maintenance risk in this file: a fix applied to one backend
    // and not the other shows up only when someone switches API.
    static void FlushDX12Nova()
    {
        // A reference alias, so the body reads as `n.` rather than `s_Nova12->`. It is a
        // raw dereference with no null check -- Init guarantees s_Nova12 is non-null
        // before any frame can reach here.
        auto& n=*s_Nova12;
        // Re-declared every frame at the current target size. Cheap when unchanged, and
        // the reason the output texture always matches the accumulation buffers.
        n.Resize(s_Data.RayTracingWidth,s_Data.RayTracingHeight);
        // The static-scene skip. FrameIndex==0 forces the first frame through even when
        // nothing changed, so the GPU buffers are populated from an empty CPU state
        // instead of being read before anything has ever been uploaded.
        const bool sceneChanged = SceneInstancesChanged();
        if(sceneChanged || s_Data.FrameIndex==0) {
            // Memory pooling: reuse buffers instead of allocating per-frame.
            // Pre-allocate to avoid reallocations during gameplay.
            // InstancePool is an unordered_map, which has bucket_count() rather than
            // capacity(); reserve() sets the bucket count.
            if (s_Data.InstancePool.bucket_count() < s_Data.InstancePoolCapacity)
                s_Data.InstancePool.reserve(s_Data.InstancePoolCapacity);
            if (s_Data.BVHNodePool.capacity() < s_Data.BVHNodePoolCapacity)
                s_Data.BVHNodePool.reserve(s_Data.BVHNodePoolCapacity);
            if (s_Data.LightIndexPool.capacity() < s_Data.LightIndexPoolCapacity)
                s_Data.LightIndexPool.reserve(s_Data.LightIndexPoolCapacity);

            BuildAndFlattenBVH(s_Data.m_SceneInstances,s_Data.m_BVHNodes,s_Data.m_BVHPrimOrder);
            // One reordered instance per BVH leaf, in TRAVERSAL order rather than submission
            // order. Coherent memory access matters more than instance ordering here, so the
            // shader's instance array is a permutation of the scene list, not a copy of it.
            s_Data.m_ReorderedInstances.resize(s_Data.m_BVHPrimOrder.size());
            s_Data.m_LightIndices.clear();
            // One pass does both the permutation and the emissive index. Interleaved rather
            // than as two loops because both read the same reordered array and the fused loop
            // touches it once.
            for(size_t i=0;i<s_Data.m_BVHPrimOrder.size();++i) {
                // The scatter that performs the permutation: index into the reordered array
                // sequentially, gather from the scene list at BVH order. Copying the whole struct
                // rather than a pointer means the GPU buffer is contiguous.
                auto& v=s_Data.m_ReorderedInstances[i];v=s_Data.m_SceneInstances[s_Data.m_BVHPrimOrder[i]];
                // Emissive instances become the shader's light list, indexed into the REORDERED
                // array -- which is why the index is i and not a scene-list index. Emission
                // intensity > 0 is the only criterion; a dim emitter still counts toward the cap.
                if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
            }
            n.Upload(0,s_Data.m_ReorderedInstances.data(),(UINT)s_Data.m_ReorderedInstances.size(),sizeof(RayTracingInstance));
            n.Upload(1,s_Data.m_BVHNodes.data(),(UINT)s_Data.m_BVHNodes.size(),sizeof(BVHNodeGPU));
            n.Upload(2,s_Data.m_LightIndices.data(),(UINT)s_Data.m_LightIndices.size(),sizeof(int));
            n.Upload(3,s_Data.m_TerrainVertices.data(),(UINT)s_Data.m_TerrainVertices.size(),sizeof(TerrainNovaVertex));
            // The memcmp reference for the next frame's SceneInstancesChanged. Stored BEFORE
            // FrameIndex is reset, and it copies the submission-order list, not the
            // reordered one.
            s_Data.m_PrevSceneInstances=s_Data.m_SceneInstances;
            s_Data.m_PrevTerrainVertices=s_Data.m_TerrainVertices;
            s_Data.FrameIndex=0;
        }
        n.Set("u_InstanceCount",(int)s_Data.m_ReorderedInstances.size());
        n.Set("u_LightCount",(int)s_Data.m_LightIndices.size());
        n.Set("u_FrameIndex",(int)s_Data.FrameIndex);
        n.Set("u_QualityLevel",(int)s_Data.CurrentQualityPreset);
        // DEFECT: an EXACT matrix compare, so any floating-point difference at all counts
        // as movement. The OpenGL path instead compares camera position and rotation
        // against a MovementThreshold, which is what makes a nearly-static camera hold
        // its accumulation here.
        n.Set("u_CameraMoved",s_Data.CurrentViewProjection!=s_Data.PrevViewProjection?1.0f:0.0f);
        // TAA jitter from the generated blue-noise volume, in PIXEL units. This
        // upload was missing on both DX backends: u_Jitter was declared in the
        // cbuffer and has a known offset (VulkanNova.cpp:37), but nothing ever
        // set it here, so the shader's GetJitter() computed Halton itself and
        // the 16-offset repeat survived the generator landing. GetCurrentJitter
        // is frame-index seeded and the host already resets FrameIndex on a
        // view change and on the still-frame latch, so no camera-moved
        // correction is applied here. u_PrevJitter is set to the same value:
        // the shader reads only u_Jitter, and a zero previous jitter would be a
        // lie if it ever starts reading it.
        {
            // RayTracingWidth/Height, not the window size: this is the size of
            // the trace target the shader offsets into, and it is already in
            // scope here. width/height are not visible in this flush.
            // Renderer3D:: qualification is REQUIRED here: FlushDX11Nova and
            // FlushDX12Nova are file-scope statics, not members, so an
            // unqualified call fails with C3861. The GL path's copy of this
            // upload sits inside a Renderer3D member and does not need it.
            glm::vec2 jit = Renderer3D::GetCurrentJitter(s_Data.FrameIndex, glm::vec2(
                (float)s_Data.RayTracingWidth, (float)s_Data.RayTracingHeight));
            n.Set("u_Jitter", jit);
            n.Set("u_PrevJitter", jit);
        }
        // The 1/(n+1) running mean, floored at 0.02 so a converged image stops changing
        // instead of chasing the last sample forever. When accumulation is off the alpha
        // is 1.0, i.e. the newest sample replaces the history completely.
        n.Set("u_AccumulationAlpha",s_Data.RayTracingAccumulate?glm::max(0.02f,1.0f/(s_Data.FrameIndex+1)):1.0f);
        n.Set("u_CameraPosition",s_Data.CurrentCameraPosition);
        n.Set("u_InverseViewProjection",s_Data.InverseViewProjection);
        n.Set("u_PrevViewProjection",s_Data.PrevViewProjection);
        n.Set("u_ViewProjection",s_Data.CurrentViewProjection);
        n.Set("u_SkyBottomColor",s_Data.SkyBottomColor);
        n.Set("u_SkyTopColor",s_Data.SkyTopColor);
        // The dynamic scaler's current scale, uploaded per frame because it changes
        // mid-session. The shader uses it to map accumulation pixels back to output
        n.Set("u_RenderScale",s_Data.RenderScale);
        // Also dead on this path. The DX11 twin annotates its copy as "dead uniform, kept
        // for CBV layout"; here there is no annotation at all. Bounce depth is fixed by
        // RunNovaPassSchedule's pass order rather than by this uniform, which is why all
        // four quality presets end up with an identical effective ray budget.
        n.Set("u_MaxBounces",s_Data.MaxBounces);
        // The DX12 twin of the line above, uploaded by the mirrored function body. Every
        // note in the DX11 run applies here identically -- the two functions are kept in
        // sync by hand, so a change to one that is not copied to the other produces a
        // backend-dependent image with no diagnostic.
        n.Set("u_MaxLights",s_Data.MaxLights);
        // One of the few uniforms that genuinely changes ray depth, and therefore the
        // main reason the four quality presets differ in cost at all.
        n.Set("u_IndirectRays",s_Data.IndirectRays);
        // DEFECT: uploaded to a uniform no pass reads -- there is no per-pixel sample
        // loop in the Nova path. The DX11 flush two functions up annotates its own copy
        // with a trailing "D3: dead uniform" note; this one carries no such note, though
        // it is the same dead uniform. See the D3 comment block at SetSamplesPerPixel
        // for the full account, including why the setter is kept anyway.
        n.Set("u_SamplesPerPixel",s_Data.SamplesPerPixel);
        n.Set("u_NeuralTexStrength",s_Data.NeuralTextureStrength);
        n.Set("u_NeuralLightStrength",s_Data.NeuralLightStrength);
        n.Set("u_NeuralMatStrength",s_Data.NeuralMaterialStrength);
        n.Set("u_GlobalFogDensity",s_Data.GlobalFogDensity);
        n.Set("u_GlobalFogHeightFalloff",s_Data.GlobalFogHeightFalloff);
        n.Set("u_GlobalFogColor",s_Data.GlobalFogColor);
        // The height at which global fog begins to thin out, in world units. Uploaded
        // unvalidated -- the setter only lower-bounds the density, not this.
        n.Set("u_GlobalFogBaseHeight",s_Data.GlobalFogBaseHeight);
        n.Set("u_GlobalFogDistAtten",s_Data.GlobalFogDistAtten);
        n.Set("u_GodRayIntensity",s_Data.GodRayIntensity);
        n.Set("u_GodRayDecay",s_Data.GodRayDecay);
        n.Set("u_GodRaySamples",s_Data.GodRaySamples);
        n.Set("u_GodRayDensity",s_Data.GodRayDensity);
        n.Set("u_GodRayColor",s_Data.GodRayColor);
        // Post-processing (reconstructed; see RunComposite). These reach real
        // shader code now -- vignette, chromatic aberration and colour grading
        // had no implementation at all before, and the bloom threshold was
        // hardcoded inside the shader at two different values.
        n.Set("u_BloomEnabled",(int)s_Data.BloomEnabled);
        n.Set("u_BloomIntensity",s_Data.BloomIntensity);
        // Reconstructed rather than hardcoded: the comment block above notes the shader
        // previously baked this in at two DIFFERENT values, so the uniform is what finally
        // made the slider authoritative.
        n.Set("u_BloomThreshold",s_Data.BloomThreshold);
        n.Set("u_VignetteEnabled",(int)s_Data.VignetteEnabled);
        n.Set("u_VignetteIntensity",s_Data.VignetteIntensity);
        // Radius and softness together define the falloff from the frame edge inward;
        // radius sets where it starts and softness how wide the transition is.
        n.Set("u_VignetteRadius",s_Data.VignetteRadius);
        n.Set("u_VignetteSoftness",s_Data.VignetteSoftness);
        n.Set("u_ChromaticAberrationEnabled",(int)s_Data.ChromaticAberrationEnabled);
        n.Set("u_ChromaticAberrationIntensity",s_Data.ChromaticAberrationIntensity);
        n.Set("u_ColorGradingEnabled",(int)s_Data.ColorGradingEnabled);
        // A signed temperature shift rather than a Kelvin value, applied by pushing the
        // red/blue channels in opposite directions.
        n.Set("u_ColorTemperature",s_Data.ColorTemperature);
        n.Set("u_ColorTint",s_Data.ColorTint);
        n.Set("u_ColorGamma",s_Data.ColorGamma);
        n.Set("u_FilmGrainEnabled",(int)s_Data.FilmGrainEnabled);
        n.Set("u_FilmGrainIntensity",s_Data.FilmGrainIntensity);
        n.Set("u_NeuralEnabled",(int)s_Data.NeuralEnabled);
        n.Set("u_GlobalFogEnabled",(int)s_Data.GlobalFogEnabled);
        n.Set("u_GodRaysEnabled",(int)s_Data.GodRaysEnabled);
        // The global volumetric toggle, uploaded next to the per-volume FogCount. Both
        // are needed: a count with the flag off must still render nothing.
        n.Set("u_FogEnabled",(int)s_Data.VolFogEnabled);
        n.Set("u_CloudEnabled",(int)s_Data.VolCloudsEnabled);
        // The EFFECTIVE sun, resolved per frame rather than set by SetSunDirection. The
        // default here is a slightly warm white, then the loop below overrides both from
        // the first analytic directional light. GetSunDirection reads SunDirection, not
        // this, so the two can disagree.
        s_Data.EffSunDir=s_Data.SunDirection;s_Data.EffSunColor=glm::vec3(1.15f,1.05f,0.95f);
        // Type 0 is directional. The break means the FIRST directional light in
        // submission order becomes the sun and later ones are ignored -- so with two
        // suns only one is lit directionally. The rest still reach the shader through
        // the analytic light list below, where they are shaded as ordinary lights.
        // Intensity is folded into the COLOUR here, not uploaded separately.
        for(const auto& l:s_Data.AnalyticLights) if(l.Type==0) {s_Data.EffSunDir=l.Direction;s_Data.EffSunColor=l.Color*glm::max(l.Intensity,0.0f);break;}
        // Uploaded as the resolved EffSun values, which is what makes this path agree
        // with the god-ray projection below. The OpenGL path does not resolve a sun this
        // way, so a scene with a directional light gets different god rays per backend.
        n.Set("u_SunDirection",s_Data.EffSunDir);n.Set("u_SunLightColor",s_Data.EffSunColor);
        n.Set("u_Time",GetVolumetricTimeSeconds());n.Set("u_VolFast",(int)(s_Data.CurrentQualityPreset<=QualityPreset::Medium));
        // A per-preset multiplier on the ray-march step count, folded into each volume's
        // Steps below. Cheaper presets take bigger steps, trading accuracy for speed in
        // exactly the passes where it is least visible.
        float stepFactor=s_Data.CurrentQualityPreset==QualityPreset::Low?0.5f:(s_Data.CurrentQualityPreset==QualityPreset::Medium?0.75f:1.0f);
        // DEFECT: the literal 4, not MaxFogVolumes. SubmitFogVolume caps the list at
        // MaxFogVolumes and silently drops the excess, so raising MaxFogVolumes past 4
        // would still upload only four -- and on GL, where the same function has no such
        // literal, more volumes than the shader's array size would then be uploaded.
        int fc=(int)glm::min(s_Data.FogVolumes.size(),size_t(4));n.Set("u_FogCount",fc);
        // Bounded by fc, the CLAMPED count, not by FogVolumes.size(). That is what keeps
        // the upload loop from writing past u_FogCount's array size in the shader when the
        // scene holds more volumes than the literal cap.
        for(int i=0;i<fc;++i) {const auto& v=s_Data.FogVolumes[i];
            n.Set("u_FogMin",v.Min,i);n.Set("u_FogMax",v.Max,i);n.Set("u_FogColor",v.Color,i);
            n.Set("u_FogData",glm::vec4(v.Density,v.Anisotropy,v.NoiseStrength,v.NoiseScale),i);
            n.Set("u_FogData2",glm::vec4(v.WindSpeed,v.HeightFalloff,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f),v.Enabled?1.0f:0.0f),i);
        }
        // The same literal-2 hazard as the fog cap above, against MaxCloudVolumes.
        int cc=(int)glm::min(s_Data.CloudVolumes.size(),size_t(2));n.Set("u_CloudCount",cc);
        for(int i=0;i<cc;++i) {const auto& v=s_Data.CloudVolumes[i];
            n.Set("u_CloudMin",v.Min,i);n.Set("u_CloudMax",v.Max,i);n.Set("u_CloudColor",v.Color,i);n.Set("u_CloudAmbient",v.Ambient,i);
            n.Set("u_CloudData",glm::vec4(v.Coverage,v.Density,v.NoiseScale,v.Detail),i);
            n.Set("u_CloudData2",glm::vec4(v.WindSpeed,v.WindDir.x,v.WindDir.y,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f)),i);
            n.Set("u_CloudData3",glm::vec4(v.SilverLining,v.ShadowStrength,v.Enabled?1.0f:0.0f,0),i);
        }
        // DEFECT: the literal 8, not MaxAnalyticLights. This is the cap that actually
        // bounds visible lights -- PushAnalyticLight admits MaxAnalyticLights entries, so
        // any value above 8 is silently dropped here rather than at submission.
        int lc=(int)glm::min(s_Data.AnalyticLights.size(),size_t(8));n.Set("u_ALightCount",lc);
        for(int i=0;i<lc;++i) {const auto& v=s_Data.AnalyticLights[i];
            n.Set("u_ALightPos",v.Position,i);n.Set("u_ALightDir",v.Direction,i);n.Set("u_ALightColor",v.Color,i);
            n.Set("u_ALightData",glm::vec4(v.Type,v.Intensity,v.Range,v.Falloff),i);
            n.Set("u_ALightData2",glm::vec4(v.InnerCos,v.OuterCos,v.AreaSize,v.DoubleSided),i);
        }
        // The sun projected to screen space for god rays: camera position PLUS the
        // direction times 1000 world units, so the point is 1000 units away along the
        // sun ray. The OpenGL path omits the CurrentCameraPosition term, so god rays
        // start from a different place depending on backend.
        auto sun=s_Data.CurrentViewProjection*glm::vec4(s_Data.CurrentCameraPosition+s_Data.EffSunDir*1000.0f,1);
        // w > 0 means the point is in front of the camera (not behind or on the plane),
        // and EffSunDir.y > 0 restricts shafts to a sun above the horizon. A sun below
        // the horizon cannot produce visible crepuscular rays through the scene.
        s_Data.GodSunValid=sun.w>0 && s_Data.EffSunDir.y>0;
        // Perspective divide, then the NDC-to-UV remap that centres the range and flips
        // nothing: -1..1 becomes 0..1. This is the version that does NOT also test for
        // the point being inside the viewport, so an off-screen sun still counts as
        if(s_Data.GodSunValid)s_Data.GodSunScreenPos=glm::vec2(sun)/sun.w*0.5f+0.5f;
        n.Set("u_GodSunValid",(int)s_Data.GodSunValid);n.Set("u_GodSunScreenPos",s_Data.GodSunScreenPos);
        // Rebuilt every frame from the live texture slots, then handed to the pass
        // schedule. A per-frame vector of pointers rather than a cached one, because the
        // slot table is cleared by every FlushAndReset and cannot outlive the frame.
        std::vector<ID3D12Resource*> textures;
        // DX12 Nova builds its own SRVs, so it needs the underlying D3D12 resources.
        for (UINT i = 0; i < s_Data.TextureSlotIndex; ++i)
        {
            auto *resource = static_cast<ID3D12Resource *>(s_Data.TextureSlots[i]->GetNativeTextureResource());
            if (!resource)
                throw std::runtime_error("DX12 Nova texture slot has no native ID3D12Resource");
            textures.push_back(resource);
        }
        RunNovaPassSchedule(n, textures);
    }

    // Vulkan Nova mirrors the complete NVRHI frame upload, with Ref<Texture2D>
    // handles passed to the shared schedule for NVRHI texture resolution.
    static void FlushVulkanNova()
    {
        // A reference alias to the live instance; routing in Renderer3D::Flush checks
        // s_NovaVk before entering this helper.
        auto& n=*s_NovaVk;
        // Re-declared every frame at the current target size. Cheap when unchanged, and
        // the reason the output texture always matches the accumulation buffers.
        n.Resize(s_Data.RayTracingWidth,s_Data.RayTracingHeight);
        // The static-scene skip. FrameIndex==0 forces the first frame through even when
        // nothing changed, so the GPU buffers are populated from an empty CPU state
        // instead of being read before anything has ever been uploaded.
        if(SceneInstancesChanged() || s_Data.FrameIndex==0) {
            BuildAndFlattenBVH(s_Data.m_SceneInstances,s_Data.m_BVHNodes,s_Data.m_BVHPrimOrder);
            // One reordered instance per BVH leaf, in TRAVERSAL order rather than submission
            // order. Coherent memory access matters more than instance ordering here, so the
            // shader's instance array is a permutation of the scene list, not a copy of it.
            s_Data.m_ReorderedInstances.resize(s_Data.m_BVHPrimOrder.size());
            s_Data.m_LightIndices.clear();
            // One pass does both the permutation and the emissive index. Interleaved rather
            // than as two loops because both read the same reordered array and the fused loop
            // touches it once.
            for(size_t i=0;i<s_Data.m_BVHPrimOrder.size();++i) {
                // The scatter that performs the permutation: index into the reordered array
                // sequentially, gather from the scene list at BVH order. Copying the whole struct
                // rather than a pointer means the GPU buffer is contiguous.
                auto& v=s_Data.m_ReorderedInstances[i];v=s_Data.m_SceneInstances[s_Data.m_BVHPrimOrder[i]];
                // Emissive instances become the shader's light list, indexed into the REORDERED
                // array -- which is why the index is i and not a scene-list index. Emission
                // intensity > 0 is the only criterion; a dim emitter still counts toward the cap.
                if(v.Emission.w>0) s_Data.m_LightIndices.push_back((int)i);
            }
            n.Upload(0,s_Data.m_ReorderedInstances.data(),(UINT)s_Data.m_ReorderedInstances.size(),sizeof(RayTracingInstance));
            n.Upload(1,s_Data.m_BVHNodes.data(),(UINT)s_Data.m_BVHNodes.size(),sizeof(BVHNodeGPU));
            n.Upload(2,s_Data.m_LightIndices.data(),(UINT)s_Data.m_LightIndices.size(),sizeof(int));
            // The memcmp reference for the next frame's SceneInstancesChanged. Stored BEFORE
            // FrameIndex is reset, and it copies the submission-order list, not the
            // reordered one.
            s_Data.m_PrevSceneInstances=s_Data.m_SceneInstances;
            s_Data.FrameIndex=0;
        }
        n.Set("u_InstanceCount",(int)s_Data.m_ReorderedInstances.size());
        n.Set("u_LightCount",(int)s_Data.m_LightIndices.size());
        n.Set("u_FrameIndex",(int)s_Data.FrameIndex);
        n.Set("u_QualityLevel",(int)s_Data.CurrentQualityPreset);
        // DEFECT: an EXACT matrix compare, so any floating-point difference at all counts
        // as movement. The OpenGL path instead compares camera position and rotation
        // against a MovementThreshold, which is what makes a nearly-static camera hold
        // its accumulation here.
        n.Set("u_CameraMoved",s_Data.CurrentViewProjection!=s_Data.PrevViewProjection?1.0f:0.0f);
        // TAA jitter from the generated blue-noise volume, in PIXEL units. Both
        // current and previous jitter use the frame-indexed value, matching the
        // DX Nova upload and the shader's pixel-coordinate offset convention.
        {
            // RayTracingWidth/Height, not the window size: this is the size of
            // the trace target the shader offsets into, and it is already in
            // scope here. width/height are not visible in this flush.
            // Renderer3D:: qualification is REQUIRED here: FlushDX11Nova and
            // FlushDX12Nova are file-scope statics, not members, so an
            // unqualified call fails with C3861. The GL path's copy of this
            // upload sits inside a Renderer3D member and does not need it.
            glm::vec2 jit = Renderer3D::GetCurrentJitter(s_Data.FrameIndex, glm::vec2(
                (float)s_Data.RayTracingWidth, (float)s_Data.RayTracingHeight));
            n.Set("u_Jitter", jit);
            n.Set("u_PrevJitter", jit);
        }
        // The 1/(n+1) running mean, floored at 0.02 so a converged image stops changing
        // instead of chasing the last sample forever. When accumulation is off the alpha
        // is 1.0, i.e. the newest sample replaces the history completely.
        n.Set("u_AccumulationAlpha",s_Data.RayTracingAccumulate?glm::max(0.02f,1.0f/(s_Data.FrameIndex+1)):1.0f);
        n.Set("u_CameraPosition",s_Data.CurrentCameraPosition);
        n.Set("u_InverseViewProjection",s_Data.InverseViewProjection);
        n.Set("u_PrevViewProjection",s_Data.PrevViewProjection);
        n.Set("u_ViewProjection",s_Data.CurrentViewProjection);
        n.Set("u_SkyBottomColor",s_Data.SkyBottomColor);
        n.Set("u_SkyTopColor",s_Data.SkyTopColor);
        // The dynamic scaler's current scale, uploaded per frame because it changes
        // mid-session. The shader uses it to map accumulation pixels back to output
        n.Set("u_RenderScale",s_Data.RenderScale);
        // Also dead on this path. The DX11 twin annotates its copy as "dead uniform, kept
        // for CBV layout"; here there is no annotation at all. Bounce depth is fixed by
        // RunNovaPassSchedule's pass order rather than by this uniform, which is why all
        // four quality presets end up with an identical effective ray budget.
        n.Set("u_MaxBounces",s_Data.MaxBounces);
        // DX11 and DX12 use the same packed uniform layout; Vulkan uploads these
        // values through its NVRHI constant buffer as well.
        n.Set("u_MaxLights",s_Data.MaxLights);
        // One of the few uniforms that genuinely changes ray depth, and therefore the
        // main reason the four quality presets differ in cost at all.
        n.Set("u_IndirectRays",s_Data.IndirectRays);
        // DEFECT: uploaded to a uniform no pass reads -- there is no per-pixel sample
        // loop in the Nova path. The DX11 flush two functions up annotates its own copy
        // with a trailing "D3: dead uniform" note; this one carries no such note, though
        // it is the same dead uniform. See the D3 comment block at SetSamplesPerPixel
        // for the full account, including why the setter is kept anyway.
        n.Set("u_SamplesPerPixel",s_Data.SamplesPerPixel);
        n.Set("u_NeuralTexStrength",s_Data.NeuralTextureStrength);
        n.Set("u_NeuralLightStrength",s_Data.NeuralLightStrength);
        n.Set("u_NeuralMatStrength",s_Data.NeuralMaterialStrength);
        n.Set("u_GlobalFogDensity",s_Data.GlobalFogDensity);
        n.Set("u_GlobalFogHeightFalloff",s_Data.GlobalFogHeightFalloff);
        n.Set("u_GlobalFogColor",s_Data.GlobalFogColor);
        // The height at which global fog begins to thin out, in world units. Uploaded
        // unvalidated -- the setter only lower-bounds the density, not this.
        n.Set("u_GlobalFogBaseHeight",s_Data.GlobalFogBaseHeight);
        n.Set("u_GlobalFogDistAtten",s_Data.GlobalFogDistAtten);
        n.Set("u_GodRayIntensity",s_Data.GodRayIntensity);
        n.Set("u_GodRayDecay",s_Data.GodRayDecay);
        n.Set("u_GodRaySamples",s_Data.GodRaySamples);
        n.Set("u_GodRayDensity",s_Data.GodRayDensity);
        n.Set("u_GodRayColor",s_Data.GodRayColor);
        // Post-processing (reconstructed; see RunComposite). These reach real
        // shader code now -- vignette, chromatic aberration and colour grading
        // had no implementation at all before, and the bloom threshold was
        // hardcoded inside the shader at two different values.
        n.Set("u_BloomEnabled",(int)s_Data.BloomEnabled);
        n.Set("u_BloomIntensity",s_Data.BloomIntensity);
        // Reconstructed rather than hardcoded: the comment block above notes the shader
        // previously baked this in at two DIFFERENT values, so the uniform is what finally
        // made the slider authoritative.
        n.Set("u_BloomThreshold",s_Data.BloomThreshold);
        n.Set("u_VignetteEnabled",(int)s_Data.VignetteEnabled);
        n.Set("u_VignetteIntensity",s_Data.VignetteIntensity);
        // Radius and softness together define the falloff from the frame edge inward;
        // radius sets where it starts and softness how wide the transition is.
        n.Set("u_VignetteRadius",s_Data.VignetteRadius);
        n.Set("u_VignetteSoftness",s_Data.VignetteSoftness);
        n.Set("u_ChromaticAberrationEnabled",(int)s_Data.ChromaticAberrationEnabled);
        n.Set("u_ChromaticAberrationIntensity",s_Data.ChromaticAberrationIntensity);
        n.Set("u_ColorGradingEnabled",(int)s_Data.ColorGradingEnabled);
        // A signed temperature shift rather than a Kelvin value, applied by pushing the
        // red/blue channels in opposite directions.
        n.Set("u_ColorTemperature",s_Data.ColorTemperature);
        n.Set("u_ColorTint",s_Data.ColorTint);
        n.Set("u_ColorGamma",s_Data.ColorGamma);
        n.Set("u_FilmGrainEnabled",(int)s_Data.FilmGrainEnabled);
        n.Set("u_FilmGrainIntensity",s_Data.FilmGrainIntensity);
        n.Set("u_NeuralEnabled",(int)s_Data.NeuralEnabled);
        n.Set("u_GlobalFogEnabled",(int)s_Data.GlobalFogEnabled);
        n.Set("u_GodRaysEnabled",(int)s_Data.GodRaysEnabled);
        // The global volumetric toggle, uploaded next to the per-volume FogCount. Both
        // are needed: a count with the flag off must still render nothing.
        n.Set("u_FogEnabled",(int)s_Data.VolFogEnabled);
        n.Set("u_CloudEnabled",(int)s_Data.VolCloudsEnabled);
        // The EFFECTIVE sun, resolved per frame rather than set by SetSunDirection. The
        // default here is a slightly warm white, then the loop below overrides both from
        // the first analytic directional light. GetSunDirection reads SunDirection, not
        // this, so the two can disagree.
        s_Data.EffSunDir=s_Data.SunDirection;s_Data.EffSunColor=glm::vec3(1.15f,1.05f,0.95f);
        // Type 0 is directional. The break means the FIRST directional light in
        // submission order becomes the sun and later ones are ignored -- so with two
        // suns only one is lit directionally. The rest still reach the shader through
        // the analytic light list below, where they are shaded as ordinary lights.
        // Intensity is folded into the COLOUR here, not uploaded separately.
        for(const auto& l:s_Data.AnalyticLights) if(l.Type==0) {s_Data.EffSunDir=l.Direction;s_Data.EffSunColor=l.Color*glm::max(l.Intensity,0.0f);break;}
        // Uploaded as the resolved EffSun values, which is what makes this path agree
        // with the god-ray projection below. The OpenGL path does not resolve a sun this
        // way, so a scene with a directional light gets different god rays per backend.
        n.Set("u_SunDirection",s_Data.EffSunDir);n.Set("u_SunLightColor",s_Data.EffSunColor);
        n.Set("u_Time",GetVolumetricTimeSeconds());n.Set("u_VolFast",(int)(s_Data.CurrentQualityPreset<=QualityPreset::Medium));
        // A per-preset multiplier on the ray-march step count, folded into each volume's
        // Steps below. Cheaper presets take bigger steps, trading accuracy for speed in
        // exactly the passes where it is least visible.
        float stepFactor=s_Data.CurrentQualityPreset==QualityPreset::Low?0.5f:(s_Data.CurrentQualityPreset==QualityPreset::Medium?0.75f:1.0f);
        // DEFECT: the literal 4, not MaxFogVolumes. SubmitFogVolume caps the list at
        // MaxFogVolumes and silently drops the excess, so raising MaxFogVolumes past 4
        // would still upload only four -- and on GL, where the same function has no such
        // literal, more volumes than the shader's array size would then be uploaded.
        int fc=(int)glm::min(s_Data.FogVolumes.size(),size_t(4));n.Set("u_FogCount",fc);
        // Bounded by fc, the CLAMPED count, not by FogVolumes.size(). That is what keeps
        // the upload loop from writing past u_FogCount's array size in the shader when the
        // scene holds more volumes than the literal cap.
        for(int i=0;i<fc;++i) {const auto& v=s_Data.FogVolumes[i];
            n.Set("u_FogMin",v.Min,i);n.Set("u_FogMax",v.Max,i);n.Set("u_FogColor",v.Color,i);
            n.Set("u_FogData",glm::vec4(v.Density,v.Anisotropy,v.NoiseStrength,v.NoiseScale),i);
            n.Set("u_FogData2",glm::vec4(v.WindSpeed,v.HeightFalloff,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f),v.Enabled?1.0f:0.0f),i);
        }
        // The same literal-2 hazard as the fog cap above, against MaxCloudVolumes.
        int cc=(int)glm::min(s_Data.CloudVolumes.size(),size_t(2));n.Set("u_CloudCount",cc);
        for(int i=0;i<cc;++i) {const auto& v=s_Data.CloudVolumes[i];
            n.Set("u_CloudMin",v.Min,i);n.Set("u_CloudMax",v.Max,i);n.Set("u_CloudColor",v.Color,i);n.Set("u_CloudAmbient",v.Ambient,i);
            n.Set("u_CloudData",glm::vec4(v.Coverage,v.Density,v.NoiseScale,v.Detail),i);
            n.Set("u_CloudData2",glm::vec4(v.WindSpeed,v.WindDir.x,v.WindDir.y,glm::clamp(float(v.Steps)*s_Data.VolStepScale*stepFactor,1.0f,32.0f)),i);
            n.Set("u_CloudData3",glm::vec4(v.SilverLining,v.ShadowStrength,v.Enabled?1.0f:0.0f,0),i);
        }
        // DEFECT: the literal 8, not MaxAnalyticLights. This is the cap that actually
        // bounds visible lights -- PushAnalyticLight admits MaxAnalyticLights entries, so
        // any value above 8 is silently dropped here rather than at submission.
        int lc=(int)glm::min(s_Data.AnalyticLights.size(),size_t(8));n.Set("u_ALightCount",lc);
        for(int i=0;i<lc;++i) {const auto& v=s_Data.AnalyticLights[i];
            n.Set("u_ALightPos",v.Position,i);n.Set("u_ALightDir",v.Direction,i);n.Set("u_ALightColor",v.Color,i);
            n.Set("u_ALightData",glm::vec4(v.Type,v.Intensity,v.Range,v.Falloff),i);
            n.Set("u_ALightData2",glm::vec4(v.InnerCos,v.OuterCos,v.AreaSize,v.DoubleSided),i);
        }
        // The sun projected to screen space for god rays: camera position PLUS the
        // direction times 1000 world units, so the point is 1000 units away along the
        // sun ray. The OpenGL path omits the CurrentCameraPosition term, so god rays
        // start from a different place depending on backend.
        auto sun=s_Data.CurrentViewProjection*glm::vec4(s_Data.CurrentCameraPosition+s_Data.EffSunDir*1000.0f,1);
        // w > 0 means the point is in front of the camera (not behind or on the plane),
        // and EffSunDir.y > 0 restricts shafts to a sun above the horizon. A sun below
        // the horizon cannot produce visible crepuscular rays through the scene.
        s_Data.GodSunValid=sun.w>0 && s_Data.EffSunDir.y>0;
        // Perspective divide, then the NDC-to-UV remap that centres the range and flips
        // nothing: -1..1 becomes 0..1. This is the version that does NOT also test for
        // the point being inside the viewport, so an off-screen sun still counts as
        if(s_Data.GodSunValid)s_Data.GodSunScreenPos=glm::vec2(sun)/sun.w*0.5f+0.5f;
        n.Set("u_GodSunValid",(int)s_Data.GodSunValid);n.Set("u_GodSunScreenPos",s_Data.GodSunScreenPos);
        // Dispatch keeps each Ref alive while VulkanNova resolves the NVRHI texture.
        std::vector<Ref<Texture2D>> textures;
        textures.reserve(s_Data.TextureSlotIndex);
        for (uint32_t i = 0; i < s_Data.TextureSlotIndex; ++i)
            textures.push_back(s_Data.TextureSlots[i]);
        RunNovaPassSchedule(n, textures);
    }

// The one dispatch point for every backend. Nova (DX11/DX12/Vulkan) is tried first
// and returns; the NVRHI raster fallback comes next; the OpenGL path tracer is the
// final block. Indentation here is one level shallower than its neighbours --
// left exactly as the user wrote it.
void Renderer3D::Flush()
    {
        if (!Renderer3DGLAvailable() && !Renderer3DNVRHIAvailable())
            return;

        if(s_Nova && s_Data.RayTracingEnabled) {
            try { FlushDX11Nova(); return; }
            catch (const std::exception& e) {
                WL_CORE_ERROR("DX11 Nova dispatch failed, disabling ray tracing: {0}", e.what());
                s_Nova.reset();
                s_Data.RayTracingEnabled=false;
            }
        }

        if(s_Nova12 && s_Data.RayTracingEnabled) {
            try { FlushDX12Nova(); return; }
            catch (const std::exception& e) {
                WL_CORE_ERROR("DX12 Nova dispatch failed, disabling ray tracing: {0}", e.what());
                s_Nova12.reset();
                s_Data.RayTracingEnabled=false;
            }
        }

        if(s_NovaVk && s_Data.RayTracingEnabled) {
            try { FlushVulkanNova(); return; }
            catch (const std::exception& e) {
                WL_CORE_ERROR("Vulkan Nova dispatch failed, disabling ray tracing: {0}", e.what());
                s_NovaVk.reset();
                s_Data.RayTracingEnabled = false;
            } catch (...) {
                WL_CORE_ERROR("Vulkan Nova dispatch failed with an unknown exception, disabling ray tracing");
                s_NovaVk.reset();
                s_Data.RayTracingEnabled = false;
            }
        }

        // Native NVRHI path uses the existing Basic HLSL raster shader and
        // the same CPU-generated cube/sphere batches.
        //
        // NOTE: this is the FALLBACK, not the only NVRHI behaviour. DX11,
        // DX12 and Vulkan have Nova instances and dispatch helpers above; they
        // reach here when initialization or dispatch failed, or RT is disabled.
        //
        // Vulkan also uses the shared Nova pass schedule when its instance is
        // active; this branch is the fallback after initialization or dispatch
        // failure (or when ray tracing is explicitly disabled).
        if (Renderer3DNVRHIAvailable())
        {
            if (!s_Data.BasicShader)
                return;
            s_Data.BasicShader->Bind();
            // Neural Rendering uniforms. These four are NOT optional on this path: u_NeuralEnabled
            // gates wl_vneural_micro in BOTH height fields -- the vertex wl_vheight and the
            // fragment wl_relief_height that the POM march samples. Leaving it at the
            // zero-filled staging default of 0 did not merely disable a detail effect, it made
            // the DX raster march a coarser height field than GL and flipped the shader into its
            // classical lighting branch. Mirrors the GL path at :3266-3269, which runs after this
            // branch returns.
            //
            // Still missing here versus GL: UploadVolumetricUniforms and UploadAnalyticLights are
            // both hardcoded glUniform* calls, so fog, clouds, u_Time and u_ALightCount never reach
            // this backend and stay zero. That is a porting gap in the DX raster fallback, not a
            // POM one, but it means this path renders unlit -- see the note at the branch top.
            s_Data.BasicShader->SetInt("u_NeuralEnabled", s_Data.NeuralEnabled ? 1 : 0);
            s_Data.BasicShader->SetFloat("u_NeuralTexStrength", s_Data.NeuralTextureStrength);
            s_Data.BasicShader->SetFloat("u_NeuralLightStrength", s_Data.NeuralLightStrength);
            s_Data.BasicShader->SetFloat("u_NeuralMatStrength", s_Data.NeuralMaterialStrength);
            s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
            s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
            s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);
            if (s_Data.TextureSlotIndex > 0)
            {
                // A Ref copy of every live slot. Ref is a counted handle, so these keep the
                // textures alive until the render command is consumed off-thread.
                std::vector<Ref<Texture2D>> activeTexs(s_Data.TextureSlotIndex);
                // Scene textures bound at unit 15 and up, matching the u_SceneTextures sampler
                // array set once in Init. Offsets are relative to the GL_TEXTURE0 base, so the
                // first slot lands at GL_TEXTURE15.
                for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
                    activeTexs[i] = s_Data.TextureSlots[i];
                RenderCommand::SetActiveTextures(activeTexs);
            }
            if (s_Data.CubeIndexCount)
            {
                // Upload size derived from the POINTER DIFFERENCE, not from a vertex count. The
                // cube and sphere share MaxVertices but use independent cursor pointers, so the
                // byte span is the only reliable measure of what was actually written.
                const uint32_t bytes = (uint32_t)((uint8_t *)s_Data.CubeVertexBufferPtr - (uint8_t *)s_Data.CubeVertexBufferBase);
                s_Data.CubeVertexBuffer->SetData(s_Data.CubeVertexBufferBase, bytes);
                RenderCommand::DrawIndexed(s_Data.CubeVertexArray, s_Data.CubeIndexCount);
                s_Data.Stats.DrawCalls++;
            }
            if (s_Data.SphereIndexCount)
            {
                const uint32_t vbytes = (uint32_t)((uint8_t *)s_Data.SphereVertexBufferPtr - (uint8_t *)s_Data.SphereVertexBufferBase);
                // Same trick for indices, scaled by the element size. Both cursors are advanced
                // per emitted triangle by the sphere tessellator.
                const uint32_t ibytes = (uint32_t)(s_Data.SphereIndexBufferPtr - s_Data.SphereIndexBufferBase) * sizeof(uint32_t);
                s_Data.SphereVertexBuffer->SetData(s_Data.SphereVertexBufferBase, vbytes);
                s_Data.SphereIndexBuffer->SetData(s_Data.SphereIndexBufferBase, ibytes);
                RenderCommand::DrawIndexed(s_Data.SphereVertexArray, s_Data.SphereIndexCount);
                s_Data.Stats.DrawCalls++;
            }
            return;
        }

        // The OpenGL path tracer proper. Everything after this point is GL-only and
        // assumes a live context.
        if (s_Data.RayTracingEnabled)
        {
            // Nothing to trace: no entity carried a primitive, so there is no
            // instance to build a BVH from. Correct to skip, but it used to be
            // a silent bare return, which made an empty scene indistinguishable
            // from a broken tracer -- both present a blank viewport and neither
            // said anything. Warned once, since an empty scene is a state that
            // persists rather than a per-frame event.
            if (s_Data.m_SceneInstances.empty())
            {
                static bool warnedNoInstances = false;
                if (!warnedNoInstances)
                {
                    warnedNoInstances = true;
                    WL_CORE_WARN("Renderer3D: ray tracing enabled but no scene instances were submitted; "
                                 "the traced viewport will be blank until a primitive is in view");
                }
                return;
            }

            // Wall-clock start for the CPU-side timing used by the dynamic-resolution
            // fallback. Taken before any GPU work so it measures submission cost.
            auto drsFlushStart = std::chrono::steady_clock::now();
            // Poll the timer query from two frames ago (non-blocking; a miss
            // just skips this frame's sample — the EMA tolerates gaps).
            if (s_Data.DRSTimerQueries[0] != 0)
            {
                // Two frames back in the rotating pool of three. Reading the most recent query
                // would always miss, because its result cannot exist until the frame finishes.
                GLuint readQ = s_Data.DRSTimerQueries[(s_Data.DRSTimerIndex + 2) % 3];
                GLint available = 0;
                glGetQueryObjectiv(readQ, GL_QUERY_RESULT_AVAILABLE, &available);
                // Non-blocking poll. A miss simply skips this frame's sample; the EMA below is
                // built to tolerate gaps rather than requiring every frame to contribute.
                if (available)
                {
                    GLuint64 elapsedNs = 0;
                    glGetQueryObjectui64v(readQ, GL_QUERY_RESULT, &elapsedNs);
                    double ms = (double)elapsedNs / 1000000.0;
                    // Sanity gate. A zero would poison the EMA, and a huge value means the query was
                    // measured across a GPU reset or a context loss -- either way the sample is
                    // discarded rather than believed.
                    if (ms > 0.0 && ms < 1000.0)
                    {
                        // The FIRST sample seeds the average outright rather than blending 0.1 of it
                        // into zero. Without this the EMA would need ~20 frames to converge from 0 and
                        // would read far too slow for the first second of the session.
                        s_Data.GPUFrameTimeEMA = (s_Data.DRSGpuSamples == 0)
                                                     ? ms
                                                     : s_Data.GPUFrameTimeEMA * 0.9 + ms * 0.1;
                        s_Data.DRSGpuSamples++;
                    }
                }
            }

            bool moved = false;
            // The translation threshold squared, because the test below uses length2() and
            // comparing squared quantities avoids a sqrt per axis.
            float sqThreshold = s_Data.MovementThreshold * s_Data.MovementThreshold;
            if (s_Data.ActiveCameraIsEditor)
            {
                // Position uses the SQUARED threshold; rotation uses the raw one, because angles
                // have no natural squared comparison. DEFECT: this whole MovementThreshold
                // mechanism only affects the OpenGL path -- the Nova backends restart
                // accumulation on an exact view-projection compare and never consult it.
                moved = glm::length2(s_Data.CurrentCameraPosition - s_Data.LastEditorCameraPosition) > sqThreshold ||
                        glm::abs(s_Data.CurrentCameraPitch - s_Data.LastEditorCameraPitch) > s_Data.MovementThreshold ||
                        glm::abs(s_Data.CurrentCameraYaw - s_Data.LastEditorCameraYaw) > s_Data.MovementThreshold;
            }
            else
            {
                moved = glm::length2(s_Data.CurrentCameraPosition - s_Data.LastGameCameraPosition) > sqThreshold ||
                        glm::abs(s_Data.CurrentCameraPitch - s_Data.LastGameCameraPitch) > s_Data.MovementThreshold ||
                        glm::abs(s_Data.CurrentCameraYaw - s_Data.LastGameCameraYaw) > s_Data.MovementThreshold;
            }
            // Float-ified because the shader uniform is a float rather than an int.
            float movedValue = moved ? 1.0f : 0.0f;

            if (s_Data.ActiveCameraIsEditor)
                s_Data.EditorCameraMoved = moved;
            else
                s_Data.GameCameraMoved = moved;

            // The transition into stillness, not motion itself. Accumulation is reset on the
            // frame motion STOPS so the next still frame averages only still frames -- a
            // late reset here would mix one moving sample into a converged still image.
            if (!moved && s_Data.CameraMoved)
            {
                s_Data.FrameIndex = 0;
                ClearAccumulationBuffers();
            }
            // Latched for next frame's transition test above, and read by the
            // accumulationAlpha branch further down.
            s_Data.CameraMoved = moved;

            // The instance SSBO and the BVH node buffer are fixed allocations sized from
            // MaxRayTracingInstances (see Init). Both uploads below write
            // m_ReorderedInstances.size() entries with no bound of their own, and the
            // shader indexes them by u_InstanceCount, so a frame whose visible primitive
            // count exceeds that capacity would run off the end of both GPU buffers.
            // Clamp BEFORE the comparison below, so the change test, the snapshot, the
            // build, the upload and u_InstanceCount all see the same list. A partially
            // traced frame is recoverable; a silent SSBO overrun is not.
            if (s_Data.m_SceneInstances.size() > s_Data.MaxRayTracingInstances)
            {
                static bool warnedInstanceOverflow = false;
                if (!warnedInstanceOverflow)
                {
                    warnedInstanceOverflow = true;
                    WL_CORE_WARN("Renderer3D: {0} ray-traced instances this frame, but the instance SSBO and BVH buffer are sized for {1}. Tracing the first {1}; raise MaxRayTracingInstances to cover the scene.",
                                 s_Data.m_SceneInstances.size(), s_Data.MaxRayTracingInstances);
                }
                s_Data.m_SceneInstances.resize(s_Data.MaxRayTracingInstances);
            }

            // DEFECT (harmless today): m_SceneDirty is set true unconditionally by every
            // Draw* call, so the first operand is always true and does no filtering. The
            // real skip comes from SceneInstancesChanged. Should the Draw* path ever start
            // setting it accurately, this would become a genuine second gate.
            if (s_Data.m_SceneDirty && SceneInstancesChanged())
            {
                // Build BVH for acceleration
                // CPU-side BVH build. Skipped entirely for a static scene, which is the point
                // of the SceneInstancesChanged() memcmp above.
                BuildAndFlattenBVH(s_Data.m_SceneInstances, s_Data.m_BVHNodes, s_Data.m_BVHPrimOrder);

                // Reorder instances to match BVH leaf order
                s_Data.m_ReorderedInstances.resize(s_Data.m_BVHPrimOrder.size());
                for (size_t i = 0; i < s_Data.m_BVHPrimOrder.size(); i++)
                    s_Data.m_ReorderedInstances[i] = s_Data.m_SceneInstances[s_Data.m_BVHPrimOrder[i]];

                // Build compact light index list (avoids scanning all instances in shader)
                s_Data.m_LightIndices.clear();
                for (size_t i = 0; i < s_Data.m_ReorderedInstances.size(); i++)
                {
                    if (s_Data.m_ReorderedInstances[i].Emission.w > 0.0f)
                        s_Data.m_LightIndices.push_back((int)i);
                }

                // Upload BVH nodes
                if (!s_Data.m_BVHNodes.empty())
                {
                    glNamedBufferSubData(s_Data.BVHBufferID, 0,
                                         s_Data.m_BVHNodes.size() * sizeof(BVHNodeGPU),
                                         s_Data.m_BVHNodes.data());
                }

                // Upload reordered instances (binding 1 is read by shader)
                if (!s_Data.m_ReorderedInstances.empty())
                {
                    glNamedBufferSubData(s_Data.SceneInstanceBufferID, 0,
                                         s_Data.m_ReorderedInstances.size() * sizeof(RayTracingInstance),
                                         s_Data.m_ReorderedInstances.data());
                }

                // Upload light index list
                if (!s_Data.m_LightIndices.empty())
                {
                    glNamedBufferSubData(s_Data.LightListBufferID, 0,
                                         s_Data.m_LightIndices.size() * sizeof(int),
                                         s_Data.m_LightIndices.data());
                }

                // Ordered AFTER all three uploads, not after each one. The shader only needs
                // the set to be coherent when it runs, so one barrier covers all three buffers
                // and saves two driver round trips.
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                s_Data.m_PrevSceneInstances = s_Data.m_SceneInstances;
                // Cleared here, outside the BuildAndFlattenBVH call, so it is reset even when
                // the rebuild itself was skipped.
                s_Data.m_SceneDirty = false;
            }

            uint32_t width = s_Data.RayTracingWidth;
            uint32_t height = s_Data.RayTracingHeight;
            glm::vec2 viewportSize = glm::vec2(width, height);

            // STBN blue-noise jitter for this sample, seeded by FrameIndex so the sequence
            // is reproducible and decorrelated. Also why a camera move must reset FrameIndex
            // -- a continued sequence would correlate the jitter across the discontinuity.
            glm::vec2 currentJitter = GetCurrentJitter(s_Data.FrameIndex, viewportSize);

            uint32_t readIdx = s_Data.CurrentAccumulationIndex;
            // Ping-pong: read one half, write the other, never the same texture as both
            // sampler and image (which would be an undefined feedback loop).
            uint32_t writeIdx = 1 - s_Data.CurrentAccumulationIndex;

            // Re-read the output id every frame, because ResizeRayTraceTarget assigns a
            // whole new Texture2D to RayTracingOutput and the cached name above goes
            // stale with the old one. Reading it through the Ref<> -- rather than
            // letting the GL resize path delete and recreate the name underneath
            // the Ref -- is what keeps the id bound here and the id returned by
            // GetRayTraceTargetID() the same texture.
            if (!s_Data.RayTracingOutput)
                return;
            s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();

            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, s_Data.AccumulationTextures[readIdx]);
            glActiveTexture(GL_TEXTURE0 + 4);
            glBindTexture(GL_TEXTURE_2D, s_Data.TraceGBufferTextureID);
            glActiveTexture(GL_TEXTURE0 + 8);
            glBindTexture(GL_TEXTURE_2D, s_Data.RayTracingTexture);
            // The G-buffer is bound TWICE, to units 4 and 10, because the shader reads it
            // through two different samplers in different passes and units are global GL
            // state rather than per-pass.
            glActiveTexture(GL_TEXTURE0 + 10);
            glBindTexture(GL_TEXTURE_2D, s_Data.TraceGBufferTextureID);

            // Unit 13 is s_Moments (the GLSL's `layout(binding = 13) uniform
            // sampler2D s_Moments`), the read path pass 12 uses to sample the
            // previous frame's moments. It is a DIFFERENT unit from image unit 6
            // above even though both name MomentsTextureID: image units and
            // texture units are separate GL namespaces, and the shader needs the
            // sampler view for textureLod and the image view for imageStore.
            glActiveTexture(GL_TEXTURE0 + 13);
            glBindTexture(GL_TEXTURE_2D, s_Data.MomentsTextureID);

            // A snapshot. FrameIndex advances later in this same function, and the alpha
            // computation below must see the value this frame started with.
            uint32_t currentFrameIndex = s_Data.FrameIndex;
            float accumulationAlpha = 1.0f;
            if (s_Data.RayTracingAccumulate)
            {
                // Both cameras, not just the active one. The editor renders the game camera into
                // a small viewport while the scene is static, and keying on the active camera
                // alone would reset accumulation on every editor viewport change.
                bool bothCamerasStationary = !s_Data.EditorCameraMoved && !s_Data.GameCameraMoved;
                if (s_Data.CameraMoved)
                {
                    accumulationAlpha = 0.2f;
                }
                // Past 5 samples the image is treated as converged, so the blend weight drops
                // to the 0.02 floor and new samples barely register. This is what makes a
                // converged still image stop shimmering.
                else if (bothCamerasStationary && currentFrameIndex > 4)
                {
                    accumulationAlpha = 0.02f;
                }
                else
                {
                    accumulationAlpha = 1.0f / (float)(currentFrameIndex + 1);
                }
                // The same 0.02 floor the Nova path applies to its own alpha, so both backends
                // converge at the same rate and switching between them does not visibly change
                // the noise.
                accumulationAlpha = glm::clamp(accumulationAlpha, 0.02f, 1.0f);
            }

            // glBindImageTexture's layered flag. False because every image here is a plain
            // 2D texture, not an array or a cube map.
            GLboolean layered = GL_FALSE;

            // FIX: Bind missing velocity (unit 5) and bloom temp (unit 7) image units
            // Image units are a SEPARATE namespace from sampler texture units: unit 0 here
            // is not unit 0 above. The number sequence 0..7 with gaps is the driver's image
            // binding cap, which is why the trace G-buffer had to share unit 6.
            glBindImageTexture(0, s_Data.RayTracingTexture, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(1, s_Data.AccumulationTextures[writeIdx], 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(2, s_Data.BloomTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(5, s_Data.VelocityTextureID, 0, layered, 0, GL_READ_WRITE, GL_RG16F);
            glBindImageTexture(7, s_Data.BloomTempTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(4, s_Data.AlbedoTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA8);
            glBindImageTexture(6, s_Data.TraceGBufferTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);

            for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
            {
                glActiveTexture(GL_TEXTURE0 + i + 15);
                glBindTexture(GL_TEXTURE_2D, s_Data.TextureSlots[i]->GetRendererID());
            }

            // Ceiling division by the 8x8 workgroup size. The +7 is what makes it a ceiling:
            // without it a width of, say, 1299 would compute 162 groups and drop the last
            // three columns.
            uint32_t workGroupsX = (s_Data.ComputeWidth + 7) / 8;
            uint32_t workGroupsY = (s_Data.ComputeHeight + 7) / 8;

            s_Data.RayTracingShader->Bind();

            // Upload all per-frame uniforms using pre-cached locations (no string lookups)
            // DEFECT: uploads m_SceneInstances.size(), but the shader indexes the SSBO that
            // holds m_ReorderedInstances. The two are the same length today, so it is
            // invisible -- but this is the wrong vector if the reorder ever changes size.
            glUniform1i(s_Data.InstanceCountLocation, (int)s_Data.m_SceneInstances.size());
            glUniform1i(s_Data.Loc_SamplesPerPixel, s_Data.SamplesPerPixel); // D3: dead uniform
            glUniform1i(s_Data.Loc_QualityLevel, (int)s_Data.CurrentQualityPreset);
            glUniform1i(s_Data.Loc_MaxBounces, s_Data.MaxBounces); // D3: dead uniform
            glUniform1i(s_Data.Loc_MaxLights, s_Data.MaxLights);
            glUniform1i(s_Data.Loc_IndirectRays, s_Data.IndirectRays);
            glUniform1f(s_Data.Loc_CameraMoved, movedValue);
            glUniform3f(s_Data.Loc_SkyBottomColor, s_Data.SkyBottomColor.x, s_Data.SkyBottomColor.y, s_Data.SkyBottomColor.z);
            glUniform3f(s_Data.Loc_SkyTopColor, s_Data.SkyTopColor.x, s_Data.SkyTopColor.y, s_Data.SkyTopColor.z);
            glUniform2f(s_Data.Loc_Jitter, currentJitter.x, currentJitter.y);
            glUniform1i(s_Data.Loc_DepthBuffer, 4);
            glUniform1f(s_Data.Loc_AccumulationAlpha, accumulationAlpha);
            glUniform1i(s_Data.FrameIndexLocation, s_Data.FrameIndex);
            glUniform1f(s_Data.Loc_RenderScale, s_Data.RenderScale);
            const int emissiveLightCount = (int)s_Data.m_LightIndices.size();
            const size_t requiredReservoirBytes = ReservoirAllocationBytes(s_Data.ComputeWidth, s_Data.ComputeHeight);
            const bool glRestirReady = s_Data.RestirEnabled &&
                s_Data.ComputeWidth <= s_Data.RayTracingWidth && s_Data.ComputeHeight <= s_Data.RayTracingHeight &&
                requiredReservoirBytes != 0 &&
                EnsureReservoirBuffers(s_Data.RayTracingWidth, s_Data.RayTracingHeight) &&
                s_Data.ReservoirBytes >= requiredReservoirBytes;
            // Suppress the legacy emissive-geometry sampling only when the
            // ReSTIR buffers are genuinely ready; otherwise preserve baseline
            // lighting instead of silently dropping the emitter contribution.
            glUniform1i(s_Data.Loc_LightCount, glRestirReady ? 0 : emissiveLightCount);
            // Neural Rendering uniforms (cached locations; -1 if shader predates neural block)
            // Every optional uniform is guarded, because glGetUniformLocation returns -1
            // for a uniform the compiler optimised out or that the shader does not declare,
            // and glUniform with -1 is a documented no-op rather than an error.
            if (s_Data.Loc_NeuralEnabled >= 0)
                glUniform1i(s_Data.Loc_NeuralEnabled, s_Data.NeuralEnabled ? 1 : 0);
            if (s_Data.Loc_NeuralTexStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralTexStrength, s_Data.NeuralTextureStrength);
            if (s_Data.Loc_NeuralLightStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralLightStrength, s_Data.NeuralLightStrength);
            if (s_Data.Loc_NeuralMatStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralMatStrength, s_Data.NeuralMaterialStrength);
            // Row 0 -- the Nova compute program, which is bound above. The same helper
            // serves row 1 (the Basic raster program) from BeginScene.
            UploadVolumetricUniforms(0);
            UploadAnalyticLights(0);

            // Upload global fog uniforms
            if (s_Data.Loc_GlobalFogEnabled >= 0)
                glUniform1i(s_Data.Loc_GlobalFogEnabled, s_Data.GlobalFogEnabled ? 1 : 0);
            if (s_Data.Loc_GlobalFogDensity >= 0)
                glUniform1f(s_Data.Loc_GlobalFogDensity, s_Data.GlobalFogDensity);
            if (s_Data.Loc_GlobalFogHeightFalloff >= 0)
                glUniform1f(s_Data.Loc_GlobalFogHeightFalloff, s_Data.GlobalFogHeightFalloff);
            if (s_Data.Loc_GlobalFogColor >= 0)
                glUniform3f(s_Data.Loc_GlobalFogColor, s_Data.GlobalFogColor.x, s_Data.GlobalFogColor.y, s_Data.GlobalFogColor.z);
            if (s_Data.Loc_GlobalFogBaseHeight >= 0)
                glUniform1f(s_Data.Loc_GlobalFogBaseHeight, s_Data.GlobalFogBaseHeight);
            if (s_Data.Loc_GlobalFogDistAtten >= 0)
                glUniform1f(s_Data.Loc_GlobalFogDistAtten, s_Data.GlobalFogDistAtten);

            // Compute sun screen position for god rays
            // Reset before the test below, so a frame with god rays disabled leaves the
            // validity flag false rather than keeping last frame's stale true.
            s_Data.GodSunValid = false;
            if (s_Data.GodRaysEnabled && s_Data.EffSunDir.y > 0.0f)
            {
                // Project sun direction to screen space (assume it's far away)
                // DEFECT: this treats EffSunDir * 1000 as a WORLD POSITION, whereas the Nova
                // paths (FlushDX11Nova / FlushDX12Nova) use CurrentCameraPosition + EffSunDir *
                // 1000. The two agree only when the camera sits exactly at the world origin,
                // so god rays start from a different point depending on the backend.
                glm::vec4 sunClip = s_Data.CurrentViewProjection * glm::vec4(s_Data.EffSunDir * 1000.0f, 1.0f);
                if (sunClip.w > 0.0f)
                {
                    glm::vec2 sunNDC = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
                    s_Data.GodSunScreenPos = sunNDC * 0.5f + 0.5f;
                    s_Data.GodSunValid = true;
                }
            }

            // Upload god ray uniforms
            // Braced rather than one-lined, unlike the post-process block just above. Both
            // forms are correct; the uniform is still a legal no-op when the location is
            // absent. This guard is the one that makes an older shader keep working.
            if (s_Data.Loc_GodRaysEnabled >= 0)
                glUniform1i(s_Data.Loc_GodRaysEnabled, s_Data.GodRaysEnabled ? 1 : 0);
            if (s_Data.Loc_GodSunScreenPos >= 0)
                glUniform2f(s_Data.Loc_GodSunScreenPos, s_Data.GodSunScreenPos.x, s_Data.GodSunScreenPos.y);
            if (s_Data.Loc_GodSunValid >= 0)
                glUniform1i(s_Data.Loc_GodSunValid, s_Data.GodSunValid ? 1 : 0);
            if (s_Data.Loc_GodRayIntensity >= 0)
                glUniform1f(s_Data.Loc_GodRayIntensity, s_Data.GodRayIntensity);
            if (s_Data.Loc_GodRayDecay >= 0)
                glUniform1f(s_Data.Loc_GodRayDecay, s_Data.GodRayDecay);
            if (s_Data.Loc_GodRaySamples >= 0)
                // An INT because the march is looped in the shader by count, unlike the float
                // god-ray parameters above and below it.
                glUniform1i(s_Data.Loc_GodRaySamples, s_Data.GodRaySamples);
            if (s_Data.Loc_GodRayDensity >= 0)
                glUniform1f(s_Data.Loc_GodRayDensity, s_Data.GodRayDensity);
            if (s_Data.Loc_GodRayColor >= 0)
                // Component-wise rather than glUniform3fv, since there is no array here. The
                // surrounding run mixes one-liner and braced forms for the same guarded upload.
                glUniform3f(s_Data.Loc_GodRayColor, s_Data.GodRayColor.x, s_Data.GodRayColor.y, s_Data.GodRayColor.z);

            // Post-processing (reconstructed; see RunComposite). Guarded like
            // the surrounding uploads because a uniform the driver optimises out
            // reports -1 here.
            if (s_Data.Loc_BloomEnabled >= 0) glUniform1i(s_Data.Loc_BloomEnabled, (int)s_Data.BloomEnabled);
            if (s_Data.Loc_BloomIntensity >= 0) glUniform1f(s_Data.Loc_BloomIntensity, s_Data.BloomIntensity);
            if (s_Data.Loc_BloomThreshold >= 0) glUniform1f(s_Data.Loc_BloomThreshold, s_Data.BloomThreshold);
            if (s_Data.Loc_VignetteEnabled >= 0) glUniform1i(s_Data.Loc_VignetteEnabled, (int)s_Data.VignetteEnabled);
            if (s_Data.Loc_VignetteIntensity >= 0) glUniform1f(s_Data.Loc_VignetteIntensity, s_Data.VignetteIntensity);
            if (s_Data.Loc_VignetteRadius >= 0) glUniform1f(s_Data.Loc_VignetteRadius, s_Data.VignetteRadius);
            if (s_Data.Loc_VignetteSoftness >= 0) glUniform1f(s_Data.Loc_VignetteSoftness, s_Data.VignetteSoftness);
            if (s_Data.Loc_ChromaticAberrationEnabled >= 0) glUniform1i(s_Data.Loc_ChromaticAberrationEnabled, (int)s_Data.ChromaticAberrationEnabled);
            if (s_Data.Loc_ChromaticAberrationIntensity >= 0) glUniform1f(s_Data.Loc_ChromaticAberrationIntensity, s_Data.ChromaticAberrationIntensity);
            if (s_Data.Loc_ColorGradingEnabled >= 0) glUniform1i(s_Data.Loc_ColorGradingEnabled, (int)s_Data.ColorGradingEnabled);
            if (s_Data.Loc_ColorTemperature >= 0) glUniform1f(s_Data.Loc_ColorTemperature, s_Data.ColorTemperature);
            if (s_Data.Loc_ColorTint >= 0) glUniform1f(s_Data.Loc_ColorTint, s_Data.ColorTint);
            // Uploaded UNGUARDED by value clamping, unlike the accessor below it. The
            // setter for ColorGamma does not clamp, so the UI range is the only bound.
            if (s_Data.Loc_ColorGamma >= 0) glUniform1f(s_Data.Loc_ColorGamma, s_Data.ColorGamma);
            if (s_Data.Loc_FilmGrainEnabled >= 0) glUniform1i(s_Data.Loc_FilmGrainEnabled, (int)s_Data.FilmGrainEnabled);
            if (s_Data.Loc_FilmGrainIntensity >= 0) glUniform1f(s_Data.Loc_FilmGrainIntensity, s_Data.FilmGrainIntensity);

            // View matrices must be uploaded BEFORE pass 0: velocity
            // reconstructs with the inverse and reprojects with the previous
            // matrix, and pass 0 previously ran on last frame's uploads (one
            // frame stale) which dragged history behind during motion.
            // Uploaded BEFORE pass 0 for a specific reason, noted in the comment above:
            // pass 0 writes velocity, and pass 2 reprojects with the previous matrix. If
            // these landed after pass 0, pass 0 would compute velocity against last frame's
            // matrices and the history would lag by a frame during motion.
            glUniformMatrix4fv(s_Data.Loc_PrevViewProjection, 1, GL_FALSE, glm::value_ptr(s_Data.PrevViewProjection));
            glUniformMatrix4fv(s_Data.Loc_InverseViewProjection, 1, GL_FALSE, glm::value_ptr(s_Data.InverseViewProjection));

            // Pass 0: Visibility + Velocity
            // GPU timer around the whole compute chain (passes 0-7, composite,
            // bloom): feeds the dynamic-resolution scaler + FPS readout.
            // The query is opened at most once. DRSTimerActive guards against a nested or
            // repeated Begin, which GL treats as an error rather than a no-op.
            if (s_Data.DRSTimerQueries[0] != 0 && !s_Data.DRSTimerActive)
            {
                glBeginQuery(GL_TIME_ELAPSED, s_Data.DRSTimerQueries[s_Data.DRSTimerIndex]);
                // Advanced when the query OPENS, not when it closes. The poll two frames later
                // therefore reads a query issued two frames ago -- the oldest one that can have
                // completed.
                s_Data.DRSTimerIndex = (s_Data.DRSTimerIndex + 1) % 3;
                s_Data.DRSTimerActive = true;
            }
            // Passes are selected by a single uniform rather than by separate programs, so
            // the whole chain reuses one compute shader and one set of bindings.
            glUniform1i(s_Data.Loc_PassID, 0);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            // After EVERY dispatch, not once at the end of the chain. Each pass reads what
            // the previous pass wrote, so the barrier has to sit between them; batching them
            // would let a pass sample an image its producer had not finished with.
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            // Pass 1: Hybrid Trace (direct + indirect rays)
            glUniform1i(s_Data.Loc_PassID, 1);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            // ReSTIR candidate generation, temporal merge, and spatial reuse.
            // Pass 11 is reservoir-only; pass 14 adds the estimate once before
            // pass 2. u_LightCount is restored after pass 1 skipped legacy emitters.
            if (glRestirReady)
            {
                glUniform1i(s_Data.Loc_LightCount, emissiveLightCount);
                glUniform1i(s_Data.Loc_PassID, 3);
                glDispatchCompute(workGroupsX, workGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

                glUniform1i(s_Data.Loc_PassID, 11);
                glDispatchCompute(workGroupsX, workGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

                s_Data.ReservoirParity ^= 1u;
                BindReservoirBuffers();

                glUniform1i(s_Data.Loc_PassID, 14);
                glDispatchCompute(workGroupsX, workGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

                s_Data.ReservoirParity ^= 1u;
                BindReservoirBuffers();
            }

            // Pass 2: Temporal Indirect Accumulation (velocity-reprojected)
            glUniform1i(s_Data.Loc_PassID, 2);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            if (s_Data.BilateralBlurEnabled)
            {
                // Geometric step growth. The Nova path computes the same sequence as 1<<i at
                // run time; this table is the fixed-size equivalent.
                int stepSizes[] = {1, 2, 4, 8};
                // Clamped to the table's length, mirroring the glm::min(passes, 4) in
                // RunNovaPassSchedule. Four is the count of entries, not a quality constant.
                int blurCount = (s_Data.BilateralBlurPasses < 4) ? s_Data.BilateralBlurPasses : 4;
                for (int bi = 0; bi < blurCount; bi++)
                {
                    glUniform1i(s_Data.Loc_StepSize, stepSizes[bi]);
                    glUniform1i(s_Data.Loc_PassID, 7);
                    glDispatchCompute(workGroupsX, workGroupsY, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                }
            }

            // SVGF denoiser: temporal moments (pass 12) then a fixed 4-step A-Trous
            // ladder (pass 13). OFF by default -- see s_Data.SvfgEnabled.
            //
            // Mirrors the Nova path in RunNovaPassSchedule, with one difference that
            // matters: GL has no CommitDenoise() copy, because the A-Trous pass reads
            // and writes the SAME images in place and the glMemoryBarrier after each
            // dispatch is what orders one iteration before the next. The Nova path
            // needs an explicit images[7] -> images[1] copy because its DX11/DX12
            // bindings ping-pong two distinct textures instead.
            if (s_Data.SvfgEnabled)
            {
                // Moments and the trace G-buffer share image unit 6 because
                // this driver exposes only units 0..7. The two textures are
                // not used together in these SVGF dispatches.
                glBindImageTexture(6, s_Data.MomentsTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
                int stepSizes[] = {1, 2, 4, 8};
                int svfgCount = (s_Data.SvfgPasses < 4) ? s_Data.SvfgPasses : 4;
                glUniform1i(s_Data.Loc_PassID, 12);
                glDispatchCompute(workGroupsX, workGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                for (int si = 0; si < svfgCount; si++)
                {
                    glUniform1i(s_Data.Loc_StepSize, stepSizes[si]);
                    glUniform1i(s_Data.Loc_PassID, 13);
                    glDispatchCompute(workGroupsX, workGroupsY, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                }
                glBindImageTexture(6, s_Data.TraceGBufferTextureID, 0, layered, 0, GL_READ_WRITE, GL_RGBA32F);
            }


            // Pass 6: Composite — always dispatches at FULL resolution
            // When RenderScale < 1.0, the composite upscales inline from the reduced-res accumulation buffer
            {
                // FULL resolution, not the dynamic compute resolution. When RenderScale < 1 the
                // accumulation buffer is smaller, and the composite is what upscales it -- so
                // this pass must cover every output pixel or the right side of the screen is
                // never written.
                uint32_t compositeGroupsX = (s_Data.RayTracingWidth + 7) / 8;
                uint32_t compositeGroupsY = (s_Data.RayTracingHeight + 7) / 8;
                glUniform1i(s_Data.Loc_PassID, 6);
                glDispatchCompute(compositeGroupsX, compositeGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            }

            // Bloom passes (High/Ultra only)
            if (s_Data.BloomEnabled)
            {
                uint32_t bloomGroupsX = (s_Data.RayTracingWidth + 7) / 8;
                uint32_t bloomGroupsY = (s_Data.RayTracingHeight + 7) / 8;
                glUniform1i(s_Data.Loc_PassID, 4);
                glDispatchCompute(bloomGroupsX, bloomGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                glUniform1i(s_Data.Loc_PassID, 5);
                glDispatchCompute(bloomGroupsX, bloomGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            }

            if (s_Data.DRSTimerActive)
            {
                // DEFECT: closed HERE, before the god-ray pass below. So the GPU timing excludes
                // pass 10 entirely, while UpdateDynamicResolution below also runs before it --
                // the scaler therefore cannot react to a frame made expensive by god rays, and
                // will never lower the scale to pay for them.
                glEndQuery(GL_TIME_ELAPSED);
                s_Data.DRSTimerActive = false;
            }
            double drsCpuMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - drsFlushStart)
                                  .count();
            if (drsCpuMs > 0.0 && drsCpuMs < 1000.0)
            {
                // The 0.05 blend is much slower than the GPU EMA's 0.1: CPU submit time is a
                // fallback signal and a steadier one, so it is weighted less per sample.
                s_Data.CPUFlushTimeEMA = (s_Data.CPUFlushTimeEMA <= 0.0)
                                             ? drsCpuMs
                                             : s_Data.CPUFlushTimeEMA * 0.95 + drsCpuMs * 0.05;
            }
            // Called from inside Flush, once per frame, AFTER the CPU EMA is updated. Note
            // it runs before the god-ray pass, so the timing it just consumed predates that
            // dispatch.
            UpdateDynamicResolution();

            // Pass 10: God rays (screen-space radial blur from sun)
            // The same double gate the Nova schedule uses, so the two backends agree on
            // when shafts appear.
            if (s_Data.GodRaysEnabled && s_Data.GodSunValid)
            {
                glUniform1i(s_Data.Loc_PassID, 10);
                uint32_t grGroupsX = (s_Data.RayTracingWidth + 7) / 8;
                uint32_t grGroupsY = (s_Data.RayTracingHeight + 7) / 8;
                glDispatchCompute(grGroupsX, grGroupsY, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            }

            s_Data.RayTracingShader->Unbind();

            // The last barrier in the GL chain, and the only one that covers the
            // actual read. Every barrier above is GL_SHADER_IMAGE_ACCESS_BARRIER_BIT,
            // which orders image WRITES against image READS -- but the editor does not
            // read the result as an image. ImGui samples RayTracingOutput as an ordinary
            // texture2D in the UI pass, so the store above has to be visible to a
            // TEXTURE FETCH, and only GL_TEXTURE_FETCH_BARRIER_BIT orders that.
            //
            // GL_TEXTURE_FETCH_BARRIER_BIT, not a token named GL_TEXTURE_BARRIER_BIT:
            // there is no such thing in OpenGL. An earlier version of this barrier
            // used the literal 0x00000800 under that name, which is actually
            // GL_TRANSFORM_FEEDBACK_BARRIER_BIT (glad.h:1120) and does nothing for
            // this hazard -- so the fix was a no-op and the viewport stayed black.
            // The token is generated by this vendored glad (glad.h:1113), so no
            // numeric literal and no version guard are needed.
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

            // Wrap guard. The counter is unsigned and increments every frame; without this
            // it would wrap to 0 on its own, but resetting here also clears the history so
            // the wrap is not visible as a one-frame convergence flash.
            if (currentFrameIndex >= UINT32_MAX - 1)
            {
                s_Data.FrameIndex = 0;
                ClearAccumulationBuffers();
            }
            else
            {
                s_Data.FrameIndex = currentFrameIndex + 1;
            }

            if (s_Data.ActiveCameraIsEditor)
            {
                s_Data.LastEditorCameraPosition = s_Data.CurrentCameraPosition;
                s_Data.LastEditorCameraPitch = s_Data.CurrentCameraPitch;
                s_Data.LastEditorCameraYaw = s_Data.CurrentCameraYaw;
            }
            else
            {
                s_Data.LastGameCameraPosition = s_Data.CurrentCameraPosition;
                s_Data.LastGameCameraPitch = s_Data.CurrentCameraPitch;
                s_Data.LastGameCameraYaw = s_Data.CurrentCameraYaw;
            }

            s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
            // The ping-pong flip, completing the read/write pair chosen near the top of this
            // block. Flipping here rather than mid-block is what keeps a texture from being
            // both sampler and target within one frame.
            s_Data.CurrentAccumulationIndex = 1 - s_Data.CurrentAccumulationIndex;
            s_Data.Stats.DrawCalls++;
            return;
        }

        s_Data.BasicShader->Bind();
        // Neural Rendering uniforms for the raster path (no-ops if shader lacks them)
        s_Data.BasicShader->SetInt("u_NeuralEnabled", s_Data.NeuralEnabled ? 1 : 0);
        s_Data.BasicShader->SetFloat("u_NeuralTexStrength", s_Data.NeuralTextureStrength);
        s_Data.BasicShader->SetFloat("u_NeuralLightStrength", s_Data.NeuralLightStrength);
        s_Data.BasicShader->SetFloat("u_NeuralMatStrength", s_Data.NeuralMaterialStrength);
        // Row 1 this time -- the Basic raster program, which is what was just bound. The
        // same helper therefore serves both programs with one implementation.
        UploadVolumetricUniforms(1);
        UploadAnalyticLights(1);
        // View-dependent metal reflection inputs
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);
        // Two different texture-binding mechanisms: RenderCommand::SetActiveTextures for
        // the command-list backends, and a direct per-texture Bind on GL. GL has no
        // render command list, so it cannot use the first form.
        if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
        {
            std::vector<Ref<Texture2D>> activeTexs(s_Data.TextureSlotIndex);
            for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
                activeTexs[i] = s_Data.TextureSlots[i];
            RenderCommand::SetActiveTextures(activeTexs);
        }
        else
        {
            for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
                // Direct per-slot bind to unit i. Unlike the path tracer, which offsets by 15 to
                // leave low units free for its own targets, the raster path owns units 0..N.
                s_Data.TextureSlots[i]->Bind(i);
        }

        // Draw Cubes
        if (s_Data.CubeIndexCount)
        {
            uint32_t dataSize = (uint32_t)((uint8_t *)s_Data.CubeVertexBufferPtr - (uint8_t *)s_Data.CubeVertexBufferBase);
            s_Data.CubeVertexBuffer->SetData(s_Data.CubeVertexBufferBase, dataSize);

            RenderCommand::DrawIndexed(s_Data.CubeVertexArray, s_Data.CubeIndexCount);
            s_Data.Stats.DrawCalls++;
        }

        // Draw Spheres
        if (s_Data.SphereIndexCount)
        {
            uint32_t vertexDataSize = (uint32_t)((uint8_t *)s_Data.SphereVertexBufferPtr - (uint8_t *)s_Data.SphereVertexBufferBase);
            s_Data.SphereVertexBuffer->SetData(s_Data.SphereVertexBufferBase, vertexDataSize);

            uint32_t indexDataSize = (uint32_t)(s_Data.SphereIndexBufferPtr - s_Data.SphereIndexBufferBase) * sizeof(uint32_t);
            s_Data.SphereIndexBuffer->SetData(s_Data.SphereIndexBufferBase, indexDataSize);

            RenderCommand::DrawIndexed(s_Data.SphereVertexArray, s_Data.SphereIndexCount);
            s_Data.Stats.DrawCalls++;
        }
    }

    // Stats are per-frame counters owned by the renderer rather than accumulated
    // anywhere else, so resetting is the caller's responsibility every frame.
    void Renderer3D::ResetStats()
    {
        s_Data.Stats.DrawCalls = 0;
        s_Data.Stats.QuadCount = 0;
    }
    // Returned BY VALUE. Statistics is a small POD, so a copy is cheaper than
    // exposing a reference into the singleton.
    Renderer3D::Statistics Renderer3D::GetStats() { return s_Data.Stats; }
    bool Renderer3D::IsRayTracingEnabled() { return s_Data.RayTracingEnabled; }
    // Gated on a ray-tracing target actually existing for the ACTIVE backend,
    // and says so when it refuses.
    //
    // The capability test asks for OpenGL or a live Nova instance. That admits
    // GL, DX11, DX12 and Vulkan. What it could not express was the failure itself: a
    // caller asking for RT and silently getting raster back, with no log line
    // anywhere to distinguish "user turned it off" from "this backend cannot
    // do it". Warned on the refusal only, so the checkbox still works.
    void Renderer3D::SetRayTracingEnabled(bool enabled) {
        const bool available = Renderer3DGLAvailable() || s_Nova != nullptr || s_Nova12 != nullptr || s_NovaVk != nullptr;
        if (enabled && !available) {
            WL_CORE_WARN("Renderer3D: ray tracing requested on API {0}, which has no ray-tracing backend; staying on the raster path",
                         (int)RendererAPI::GetAPI());
        }
        s_Data.RayTracingEnabled = enabled && available;
        s_Data.FrameIndex=0;
        ClearAccumulationBuffers();
    }
    // The scale-to-buffer conversion. Called by every setter that changes
    // RenderScale, so ComputeWidth/Height can never disagree with RenderScale.
    static void UpdateComputeResolution()
    {
        // Truncating multiply. The floor of 1 on the next lines is what stops a very
        // small scale from producing a zero-sized buffer and a zero-workgroup dispatch.
        s_Data.ComputeWidth = (uint32_t)(s_Data.RayTracingWidth * s_Data.RenderScale);
        s_Data.ComputeHeight = (uint32_t)(s_Data.RayTracingHeight * s_Data.RenderScale);
        if (s_Data.ComputeWidth < 1)
            s_Data.ComputeWidth = 1;
        if (s_Data.ComputeHeight < 1)
            s_Data.ComputeHeight = 1;
    }

    float Renderer3D::GetRenderScale() { return s_Data.RenderScale; }
    void Renderer3D::SetRenderScale(float scale)
    {
        // Clamped here, and the same 0.25 floor is re-clamped inside
        // UpdateDynamicResolution -- two independent bounds on one value.
        s_Data.RenderScale = glm::clamp(scale, 0.25f, 1.0f);
        // A manual choice becomes the scaler ceiling: dynamic resolution may
        // drop below it to hold the target FPS, never above it.
        // A manual scale becomes the scaler's CEILING, never just a starting point. The
        // scaler may then only reduce resolution further to hold TargetFPS; it can
        // never raise it back above what the user asked for.
        s_Data.DynamicMaxScale = s_Data.RenderScale;
        s_Data.DRSFramesSinceAdjust = 0;
        UpdateComputeResolution();
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }

    // The single place a preset's numbers are defined. Called from SetQualityPreset,
    // from Init on both backends, and from the DX11/DX12 failure paths so a raster
    // fallback still gets the preset's settings rather than constructor defaults.
    static void ApplyQualityPreset()
    {
        switch (s_Data.CurrentQualityPreset)
        {
        // Every preset sets SamplesPerPixel and MaxBounces to the same value, so the
        // preset's stated ray budget is not what the shader actually executes -- both of
        // those uniforms are dead (see the D3 note further down). The render scale is
        // therefore the only lever here that changes cost.
        case QualityPreset::Low:
            // Traces at half res (upscaled in composite): ~4x fewer pixels.
            // The 60fps preset at 4K.
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 1;
            s_Data.IndirectRays = 0;
            s_Data.BloomEnabled = false;
            s_Data.BilateralBlurEnabled = false;
            // Low traces at half resolution and upscales in the composite: a quarter of the
            // shading work. This is the dominant cost lever in the preset, more than any of
            // the ray counts above it.
            s_Data.RenderScale = 0.5f;
            break;
        case QualityPreset::Medium:
            // Traces at 2/3 res (upscaled in composite).
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 2;
            s_Data.IndirectRays = 1;
            s_Data.BloomEnabled = false;
            s_Data.BilateralBlurEnabled = false;
            s_Data.RenderScale = 0.66f;
            break;
        case QualityPreset::High:
            // Traces at 0.8 res (upscaled in composite).
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 4;
            s_Data.IndirectRays = 1;
            s_Data.BloomEnabled = true;
            s_Data.BilateralBlurEnabled = true;
            // High enables the denoiser but sets zero passes. The flag and the count are
            // separate, so "enabled with no passes" means the GL path skips the loop
            // entirely while the Nova path still commits a history.
            s_Data.BilateralBlurPasses = 0;
            s_Data.RenderScale = 0.8f;
            break;
        case QualityPreset::Ultra:
            // Traces at 0.75 res (upscaled in composite). Perf: 1 ray/px + 2 rotating shadow rays
            // from up to 4 lights, neural GI steadies the single indirect.
            // 0.75 rather than 1.0: for large open worlds (2500x2500m city with 1000+
            // buildings) full resolution is too expensive for 30 FPS. The composite
            // upscales cleanly and the visual difference is minimal.
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 4;
            s_Data.IndirectRays = 1;
            s_Data.BloomEnabled = true;
            s_Data.BilateralBlurEnabled = true;
            s_Data.BilateralBlurPasses = 1;
            s_Data.RenderScale = 0.75f;
            break;
        }

        // The preset's scale becomes the scaler ceiling here too, so the dynamic
        // resolver can only lower it. Raising RenderScale above the preset's value
        // requires SetRenderScale, not a preset change. This is the second of two
        // identical assignments -- the other is in SetRenderScale above.
        s_Data.DynamicMaxScale = s_Data.RenderScale;
        s_Data.DRSFramesSinceAdjust = 0;
        UpdateComputeResolution();
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }

    // Holds TargetFPS by stepping RenderScale in [min, preset-max] every 20
    // frames. Steps are proportional (far over budget = big steps) with a
    // dead band (down past 115% of budget, up below 60%) so the scale
    // converges in seconds, not a minute, and doesn't hunt; every step
    // resets accumulation so no stale history smears across the change.
    // Scale changes are logged (proves the new binary is running + shows
    // whether the GPU or something else is the bottleneck).
    // Runs once per Flush. Steps the render scale to hold TargetFPS, with a dead
    // band so it does not hunt, and resets accumulation on every step so no stale
    // history smears across a resolution change.
    static void UpdateDynamicResolution()
    {
        // Slowest available signal wins: GPU time when the tracer is the
        // bottleneck, app frame time when it sits elsewhere (or the GL
        // timer yields nothing). CPU submit time is the last resort.
        double ema = 0.0;
        // GPU time is preferred only after 10 real samples, so a session that just
        // started does not act on one or two noisy readings.
        if (s_Data.DRSGpuSamples > 10 && s_Data.GPUFrameTimeEMA > 0.0)
            ema = s_Data.GPUFrameTimeEMA;
        // The SLOWEST signal wins rather than the most relevant one. If the tracer is
        // fast but the app is slow, the app is the bottleneck and lowering the render
        // scale would not help -- yet the scale is what this function adjusts.
        if (s_Data.AppFrameTimeEMA > 0.0 && s_Data.AppFrameTimeEMA > ema)
            ema = s_Data.AppFrameTimeEMA;
        if (ema <= 0.0)
            ema = s_Data.CPUFlushTimeEMA;
        if (ema > 0.0)
            s_Data.CurrentFPS = (float)(1000.0 / ema);
        if (!s_Data.DynamicResolutionEnabled)
            return;
        if (ema <= 0.0)
            return;
        // 10 frames between adjustments (was 20): for large open worlds the
        // scaler needs to react faster to hold 30 FPS. Reset to zero by every
        // other writer too, so a preset or scale change restarts the interval
        // rather than letting a stale countdown fire immediately.
        if (++s_Data.DRSFramesSinceAdjust < 10)
            return;
        s_Data.DRSFramesSinceAdjust = 0;

        // TargetFPS clamped before inversion, so a zero or negative value cannot produce
        // an infinite budget that makes every frame look fast.
        double targetMs = 1000.0 / (double)glm::clamp(s_Data.TargetFPS, 20.0f, 240.0f);
        float maxScale = glm::clamp(s_Data.DynamicMaxScale, 0.25f, 1.0f);
        float minScale = glm::clamp(s_Data.DynamicMinScale, 0.25f, 1.0f);
        // DynamicMinScale defaults to 0.5, above the Low preset's 0.5 ceiling after a
        // user lowers DynamicMinScale past the preset scale. Collapsing the range to a
        // single point is what stops the two clamps below from fighting.
        if (minScale > maxScale)
            minScale = maxScale;

        double overBudget = ema / targetMs;
        // Scale DOWN past 115% of budget. The 1e-4 epsilon stops the scaler from
        // stepping when it is already effectively at the floor.
        if (ema > targetMs * 1.15 && s_Data.RenderScale > minScale + 1e-4f)
        {
            // Proportional stepping: far over budget takes a large step so the scaler
            // converges in seconds rather than a minute, and a marginal overshoot takes a
            // small one so it does not overshoot into oscillation.
            // 0.20/0.12/0.06 (was 0.15/0.10/0.05): larger steps for large open
            // worlds where the gap between current and target FPS can be wide.
            float step = (overBudget > 2.0) ? 0.20f : ((overBudget > 1.4) ? 0.12f : 0.06f);
            float ns = glm::clamp(s_Data.RenderScale - step, minScale, maxScale);
            if (ns < s_Data.RenderScale - 1e-4f)
            {
                WL_CORE_INFO("Dynamic resolution: GPU {0:.1f}ms (target {1:.1f}ms) scale {2:.2f} -> {3:.2f}",
                             ema, targetMs, s_Data.RenderScale, ns);
                s_Data.RenderScale = ns;
                UpdateComputeResolution();
                s_Data.FrameIndex = 0;
                ClearAccumulationBuffers();
            }
        }
        // Scale UP only below 60% of budget, a much wider margin than the 115% down
        // threshold. The asymmetry is deliberate: it is easy to overshoot downward.
        else if (ema < targetMs * 0.6 && s_Data.RenderScale < maxScale - 1e-4f)
        {
            // DEFECT: the scale UP path logs nothing, unlike the down path one block above.
            // The comment above this function claims scale changes are logged so the new
            // binary can be identified -- true only for decreases.
            float ns = glm::clamp(s_Data.RenderScale + 0.05f, minScale, maxScale);
            if (ns > s_Data.RenderScale + 1e-4f)
            {
                s_Data.RenderScale = ns;
                UpdateComputeResolution();
                s_Data.FrameIndex = 0;
                ClearAccumulationBuffers();
            }
        }
    }

    QualityPreset Renderer3D::GetQualityPreset() { return s_Data.CurrentQualityPreset; }
    void Renderer3D::SetQualityPreset(QualityPreset preset)
    {
        s_Data.CurrentQualityPreset = preset;
        ApplyQualityPreset();
    }
    bool Renderer3D::IsDynamicResolutionEnabled() { return s_Data.DynamicResolutionEnabled; }
    // Resets the adjust interval so enabling the scaler does not inherit a
    // half-elapsed countdown from the time it was disabled. Note it does NOT reset
    // FrameIndex: turning the scaler off or on changes nothing about the trace.
    void Renderer3D::SetDynamicResolutionEnabled(bool enabled)
    {
        s_Data.DynamicResolutionEnabled = enabled;
        s_Data.DRSFramesSinceAdjust = 0;
    }
    float Renderer3D::GetTargetFPS() { return s_Data.TargetFPS; }
    void Renderer3D::SetTargetFPS(float fps)
    {
        // Clamped to the same 20..240 range the scaler re-clamps to at use, so a value
        // set here and the value the scaler reads cannot disagree.
        s_Data.TargetFPS = glm::clamp(fps, 20.0f, 240.0f);
        s_Data.DRSFramesSinceAdjust = 0;
    }
    // The application frame time, pushed in from outside the renderer. This is the
    // only way UpdateDynamicResolution ever sees a non-GPU signal, which is what
    // makes the app-time branch above reachable.
    void Renderer3D::NotifyFrameTime(double ms)
    {
        // Same sanity gate as the GPU sample: rejects zero (would poison the EMA) and
        // implausibly large values (a debugger break or a suspended process).
        if (ms <= 0.0 || ms >= 1000.0)
            return;
        s_Data.AppFrameTimeEMA = (s_Data.AppFrameTimeEMA <= 0.0)
                                     ? ms
                                     : s_Data.AppFrameTimeEMA * 0.95 + ms * 0.05;
    }
    float Renderer3D::GetDynamicMinScale() { return s_Data.DynamicMinScale; }
    void Renderer3D::SetDynamicMinScale(float scale) { s_Data.DynamicMinScale = glm::clamp(scale, 0.25f, 1.0f); }
    float Renderer3D::GetCurrentFPS() { return s_Data.CurrentFPS; }
    float Renderer3D::GetGPUFrameTimeMs() { return (float)s_Data.GPUFrameTimeEMA; }
    // Reports whether the scaler is currently BELOW its ceiling, i.e. whether it
    // has actually reduced anything. An epsilon-guarded compare, consistent with
    // every other scale test in this file.
    bool Renderer3D::IsDynamicResolutionActive() { return s_Data.RenderScale < s_Data.DynamicMaxScale - 1e-4f; }
    const char *Renderer3D::GetGPUTierName() { return s_Data.GPUTierName; }
    bool Renderer3D::IsRayTracingAccumulate() { return s_Data.RayTracingAccumulate; }
    // Unlike the post-process setters below, this DOES reset the accumulation: it
    // changes how samples are combined rather than how a converged image is
    // post-processed, so the existing history is no longer valid.
    void Renderer3D::SetRayTracingAccumulate(bool enabled)
    {
        s_Data.RayTracingAccumulate = enabled;
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }
    // Returns whatever the ACTIVE backend uses as its output view, which is why
    // the return type is a bare void*: three different native resource types flow
    // through this one accessor.
    void* Renderer3D::GetRayTraceTargetNativeSRV() {
        if (s_Nova) return s_Nova->Output();
        if (s_Nova12) {
            // DX12 Nova hands back an ID3D12Resource*. ImGui's DX12 backend binds
            // TexID as a root GPU DESCRIPTOR HANDLE, so returning the resource
            // pointer verbatim made the viewport bind a garbage descriptor and
            // draw nothing -- the DX12 viewport showed no scene while DX11 and
            // OpenGL were fine.
            //
            // Convert to a real descriptor handle from the ImGui SRV heap. Slot 1
            // (slot 0 is the font atlas) is stable for the Nova output, so this
            // rewrites the same descriptor each frame instead of leaking slots.
            // The DX12 case is the interesting one. DX11 hands back an SRV pointer directly;
            // DX12 hands back an ID3D12Resource*, which ImGui would then misinterpret as a
            // descriptor handle. The conversion below is what makes the DX12 viewport show
            // anything at all.
            void *resource = s_Nova12->Output();
            if (!resource)
                return nullptr;

            // dynamic_cast, not static_cast: the window's context is a base pointer whose
            // concrete type depends on the active API. Returns null for a non-NVRHI context,
            // which the next line handles.
            auto *ctx = dynamic_cast<NVRHIContext *>(
                Application::Get().GetWindow().GetCurrentContext());
            if (!ctx || ctx->GetAPI() != RendererAPI::API::NVRHI_DX12)
                return resource; // not DX12: caller gets the raw pointer as before

            // Slot 1, because slot 0 is ImGui's font atlas. A fixed slot means this rewrites
            // the same descriptor every frame instead of allocating a new one per frame and
            // leaking the heap.
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = ctx->GetOrCreateImGuiDescriptor(1, resource);
            if (gpu.ptr == 0)
            {
                // Returning the resource pointer here would be WORSE than null:
                // ImGui would bind it as a descriptor and sample garbage, whereas
                // a null TexID makes the backend skip the draw.
                WL_CORE_ERROR("Ray tracing: failed to build an ImGui descriptor for the DX12 Nova output");
                return nullptr;
            }
            return (void *)(size_t)gpu.ptr;
        }
        if (s_NovaVk) return s_NovaVk->Output();
        return nullptr;
    }
    // The GL-side counterpart to the accessor above. Returns 0 under NVRHI rather
    // than a stale GL name, because a 0 TexID makes ImGui skip the draw instead of
    // sampling a texture belonging to a destroyed context.
    uint32_t Renderer3D::GetRayTraceTargetID()
    {
        // GL-only resource; return 0 (invalid) under NVRHI APIs so panels draw
        // nothing instead of handing a stale GL texture id to the ImGui backend.
        if (!Renderer3DGLAvailable() || !s_Data.RayTracingOutput)
            return 0;
        return s_Data.RayTracingOutput->GetRendererID();
    }
    // GPU -> CPU capture of the Nova output image, for the editor's screenshot
    // hook (F12 / WL_SCREENSHOT).
    //
    // Only the DX12 backend is supported, and that is a property of the backend
    // rather than an omission: DX12Nova owns the image and owns the command queue
    // the copy has to be recorded on, so DX12Nova is the only object that can
    // bracket the copy with correct resource barriers and wait on the right frame
    // slot. Reading the swap chain instead would need NVRHI, which does not expose
    // a back-buffer copy -- and would capture the window, not the traced image,
    // which is the thing under verification.
    bool Renderer3D::ReadbackRayTraceTarget(std::vector<unsigned char> &rgba, uint32_t &width, uint32_t &height)
    {
        // s_Nova12 non-null is the DX12-backend test. The RayTracingEnabled half
        // matters separately: s_Nova12 survives Nova being switched off (the
        // toggle clears the flag, it does not reset the instance), so without
        // this the call would read back a stale image from a renderer that is no
        // longer drawing anything.
        if (!s_Nova12 || !s_Data.RayTracingEnabled) return false;
        UINT w = 0, h = 0;
        // Output() defaults to image 0 and so does this. Index 0 is the
        // R32G32B32A32_FLOAT pass image the composite writes and the one
        // GetRayTraceTargetNativeSRV hands to ImGui, so this is the same picture
        // the viewport is showing -- not some other image from the same schedule.
        if (!s_Nova12->Readback(0, rgba, w, h)) return false;
        width = w;
        height = h;
        return true;
    }
    // Indented a level shallower than its neighbours. Left exactly as written --
    // this file is insertion-only.
    void Renderer3D::ResizeRayTraceTarget(uint32_t width, uint32_t height)
        {
            // DX11 branch. Each Nova branch returns early, so the GL resize code below is
            // unreachable when a Nova instance exists -- which is why the GL block does not
            // have to know whether Nova is live.
            if(s_Nova && width && height) {
                // Only acts on an actual change. A resize resets FrameIndex but does NOT clear
                // the accumulation textures here, so the next frame blends a differently sized
                // history -- unlike every setter above, which clears.
                if(width!=s_Data.RayTracingWidth || height!=s_Data.RayTracingHeight) {
                    s_Data.RayTracingWidth=width;s_Data.RayTracingHeight=height;
                    UpdateComputeResolution();s_Nova->Resize(width,height);s_Data.FrameIndex=0;
                }
                return;
            }
            // The DX12 resize branch, mirroring the DX11 one directly above. Both are
            // single-line-bodies-in-braces rather than the braced-on-separate-lines style used
            // by the GL block below, which is why this whole region is indented a level
            // short of its neighbours. Left exactly as written.
            if(s_Nova12 && width && height) {
                if(width!=s_Data.RayTracingWidth || height!=s_Data.RayTracingHeight) {
                    s_Data.RayTracingWidth=width;s_Data.RayTracingHeight=height;
                    UpdateComputeResolution();s_Nova12->Resize(width,height);s_Data.FrameIndex=0;
                }
                return;
            }
            if(s_NovaVk && width && height) {
                if(width!=s_Data.RayTracingWidth || height!=s_Data.RayTracingHeight) {
                    s_Data.RayTracingWidth=width;s_Data.RayTracingHeight=height;
                    UpdateComputeResolution();s_NovaVk->Resize(width,height);s_Data.FrameIndex=0;
                }
                return;
            }
        // No Nova branch matched, so this is the GL path. The Vulkan branch above
        // handles its own target dimensions before reaching this GL-only section.
        if (!Renderer3DGLAvailable())
            return;
        if (width == 0 || height == 0)
            return;

        // Resize Output Texture
        //
        // Two defects live here, and fixing only the first still leaves the
        // OpenGL ray-traced viewport blank.
        //
        // (1) LIFETIME. Recreated THROUGH the Ref<>, not by deleting the GL
        // name out from under it and making a fresh one in a different
        // variable. The old form left RayTracingOutput owning a DELETED name
        // while RayTracingTexture held the new one, so GetRayTraceTargetID()
        // and the compute bind both read a stale deleted id every frame.
        //
        // (2) FORMAT, which is why this goes through Resize() rather than
        // re-specifying storage here. Texture2D::Create's blank constructor
        // hardcodes GL_RGBA8 and allocates immutable storage
        // (OpenGLTexture.cpp:26,42). glTextureStorage2D on an immutable texture
        // raises GL_INVALID_OPERATION and is a NO-OP even when the dimensions
        // match, so calling it with GL_RGBA32F here silently did nothing and
        // the output stayed RGBA8. The trace then binds it as a GL_RGBA32F
        // image (glBindImageTexture below) against an rgba32f shader
        // declaration, which is not format-compatible, so the composite's
        // imageStore never landed and the viewport stayed black.
        //
        // OpenGLTexture2D::Resize deletes and recreates the storage as
        // GL_RGBA32F and writes the new name back into m_RendererID, which is
        // what both the compute bind and GetRayTraceTargetID() read.
        s_Data.RayTracingOutput = Texture2D::Create(width, height);
        s_Data.RayTracingOutput->Resize(width, height);
        s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();

        // Resize Accumulation Texture
        if (s_Data.AccumulationTexture)
            glDeleteTextures(1, &s_Data.AccumulationTexture);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AccumulationTexture);
        glTextureStorage2D(s_Data.AccumulationTexture, 1, GL_RGBA32F, width, height);

        glTextureParameteri(s_Data.AccumulationTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.AccumulationTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Resize Bloom Texture
        if (s_Data.BloomTextureID)
            glDeleteTextures(1, &s_Data.BloomTextureID);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTextureID);
        glTextureStorage2D(s_Data.BloomTextureID, 1, GL_RGBA32F, width, height);

        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        if (s_Data.BloomTempTextureID)
            glDeleteTextures(1, &s_Data.BloomTempTextureID);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTempTextureID);
        glTextureStorage2D(s_Data.BloomTempTextureID, 1, GL_RGBA32F, width, height);

        glTextureParameteri(s_Data.BloomTempTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTempTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // FIX: Reallocate Velocity Texture on resize
        if (s_Data.VelocityTextureID)
            glDeleteTextures(1, &s_Data.VelocityTextureID);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.VelocityTextureID);
        // RG16F, not RGBA16F: velocity needs two channels and the format must match what
        // glBindImageTexture declared for unit 5 earlier in Flush.
        glTextureStorage2D(s_Data.VelocityTextureID, 1, GL_RG16F, width, height);

        glTextureParameteri(s_Data.VelocityTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.VelocityTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Resize Albedo Texture
        if (s_Data.AlbedoTextureID)
            glDeleteTextures(1, &s_Data.AlbedoTextureID);
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AlbedoTextureID);
        glTextureStorage2D(s_Data.AlbedoTextureID, 1, GL_RGBA8, width, height);
        glTextureParameteri(s_Data.AlbedoTextureID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(s_Data.AlbedoTextureID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Reallocate trace G-buffer at full size (valid data in trace subregion)
        if (s_Data.TraceGBufferTextureID)
            glDeleteTextures(1, &s_Data.TraceGBufferTextureID);
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.TraceGBufferTextureID);
        glTextureStorage2D(s_Data.TraceGBufferTextureID, 1, GL_RGBA32F, width, height);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(s_Data.TraceGBufferTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        float gbufferResize[4] = {1.0f, 0.5f, 0.5f, 1.0f};
        // Re-cleared after the resize. Without this the G-buffer would hold whatever the
        // previous, differently sized content left, and pass 0 would reproject garbage
        // on the first frame after any window resize.
        glClearTexImage(s_Data.TraceGBufferTextureID, 0, GL_RGBA, GL_FLOAT, gbufferResize);

        // Resize Accumulation Textures
        if (s_Data.AccumulationTextures[0])
            glDeleteTextures(2, s_Data.AccumulationTextures);

        glCreateTextures(GL_TEXTURE_2D, 2, s_Data.AccumulationTextures);
        for (int i = 0; i < 2; i++)
        {
            glTextureStorage2D(s_Data.AccumulationTextures[i], 1, GL_RGBA32F, width, height);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        // Written at the END, after every texture has been recreated at the new size --
        // so any early return above leaves the old size and the old textures consistent
        // with each other.
        s_Data.RayTracingWidth = width;
        s_Data.RayTracingHeight = height;
        UpdateComputeResolution();
        EnsureReservoirBuffers(width, height);
        ClearAccumulationBuffers();
        s_Data.FrameIndex = 0;
    }

    // The plane-vs-AABB test, done in LOCAL space against world-space planes by
    // scaling each plane's test radius by the transform's basis vectors. That avoids
    // transforming the box, at the cost of assuming no shear.
    static bool IsAABBInFrustum(const std::array<Renderer3DData::Plane, 6> &planes, const glm::vec3 &min, const glm::vec3 &max, const glm::mat4 &transform)
    {
        glm::vec3 localCenter = (min + max) * 0.5f;
        glm::vec3 localExtents = (max - min) * 0.5f;

        // Only the CENTRE is transformed. The extents stay local and are projected onto
        // each plane's axes below, which is the standard conservative AABB test.
        glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(localCenter, 1.0f));

        glm::vec3 worldAxisX = glm::vec3(transform[0]);
        glm::vec3 worldAxisY = glm::vec3(transform[1]);
        glm::vec3 worldAxisZ = glm::vec3(transform[2]);

        // All six. An early false rejects the instance; a box outside ANY plane is
        // outside the frustum.
        for (const auto &plane : planes)
        {
            // The projected radius: the sum, over the three local axes, of the extent scaled
            // by how much that axis points along the plane normal. DEFECT: this assumes the
            // basis vectors are mutually orthogonal, so a non-uniformly SHEARED transform
            // under-estimates the radius and can cull a visible instance.
            float r = localExtents.x * glm::abs(glm::dot(plane.normal, worldAxisX)) +
                      localExtents.y * glm::abs(glm::dot(plane.normal, worldAxisY)) +
                      localExtents.z * glm::abs(glm::dot(plane.normal, worldAxisZ));

            float dist = glm::dot(plane.normal, worldCenter) + plane.distance;

            // Rejected only when the box is entirely behind the plane. dist + r < 0 is the
            // full test; testing dist alone would cull anything merely touching.
            if (dist < -r)
                return false;
        }
        return true;
    }


    // =========================================================================
    // PROCEDURAL PRIMITIVE DRAWS (reconstructed after the Phase -1 D1 accident).
    //
    // These seven were declared in Renderer3D.h and called every frame from
    // Scene::SubmitMeshPrimitives(); their definitions were lost to an
    // accidental `git checkout`. Reconstructed against the SURVIVING contract in
    // DrawCube/DrawSphere and against the shader's HitPrimitive dispatcher.
    //
    // SHAPE ENCODING is MaterialParams.z, read by HitPrimitive in the Nova shader:
    //   0 Cube   1 Sphere/IcoSphere   2 Plane   3 Cylinder
    //   4 Cone   5 Torus              6 Circle  7 Grid
    // The shader has a real intersection routine for every one of these, so each
    // draw below pushes an analytic instance the ray tracer can hit exactly --
    // no tessellation or triangle path exists for them yet.
    //
    // RASTER-SIDE VERTEX GENERATION IS NOT RECONSTRUCTED. Only the Nova analytic
    // instance is pushed here. Consequence, stated plainly: on the OpenGL raster
    // path and on the NVRHI raster fallback these seven shapes will not draw,
    // exactly as they did not before the accident. On the DX11/DX12 Nova path
    // they ray trace correctly. Filling in raster vertices for all seven is a
    // separate piece of work and is deliberately not faked here -- writing
    // placeholder geometry that renders wrong would be worse than an honest gap.
    // =========================================================================

    // The shared body behind all seven procedural shapes. Each public Draw* wrapper
    // supplies the shape id and the local bounds, and everything else -- vertex
    // generation, the analytic instance, material packing -- happens here.
    void Renderer3D::DrawProceduralPrimitive(const glm::mat4 &transform, const ProceduralMesh &mesh,
                                             const glm::vec4 &color, MaterialComponent &material,
                                             int shape, const glm::vec3 &localMin, const glm::vec3 &localMax,
                                             float shapeParam, float shapeParam2, int entityID)
    {
        // An empty tessellation returns before anything is written, so a degenerate
        // segment count cannot produce a zero-length draw.
        if (mesh.Empty())
            return;

        const uint32_t vertexCount = mesh.VertexCount();
        const uint32_t indexCount = static_cast<uint32_t>(mesh.Indices.size());

        // Skip meshes that exceed the buffer capacity. The flush below only prevents
        // ACCUMULATION from exceeding the max; a single oversized mesh would still
        // overflow and corrupt the heap, crashing later in an unrelated draw call.
        if (vertexCount > s_Data.MaxVertices || indexCount > s_Data.MaxIndices)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                WL_CORE_WARN("Renderer3D: procedural mesh rejected — {0} vertices / {1} indices exceeds buffer capacity ({2}/{3})",
                             vertexCount, indexCount, s_Data.MaxVertices, s_Data.MaxIndices);
            }
            return;
        }

        // Overflow check BEFORE writing. Note >=, not >: writing at exactly MaxVertices
        // would run off the end of the new[] allocation, so the buffer holds MaxVertices
        // - 1 usable vertices.
        if (s_Data.SphereVertexCount + vertexCount >= s_Data.MaxVertices ||
            s_Data.SphereIndexCount + indexCount >= s_Data.MaxIndices)
        {
            Flush();
            FlushAndReset();
        }

        // Not a full matrix inverse: the analytic shader only ever needs the inverse
        // transform for ray/object-space conversion, and a TRS-specific inverse avoids a
        // 4x4 determinant per draw.
        glm::mat4 invTransform = FastTRSInverse(transform);
        // The normal matrix is the TRANSPOSE of the inverse upper 3x3, not the inverse
        // itself. Under a rotation this is the same matrix; under non-uniform scale it
        // is the correct one, and using the plain inverse skews normals.
        // The transpose-of-inverse normal matrix, as in the procedural path. Recomputed
        // per draw because the transform differs per instance.
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));
        // Slot 0 is the 1x1 white fallback installed in Init, so an untextured material
        // needs no special case downstream. DEFECT: FindOrAddTextureSlot returns -1 when
        // the slot table is exhausted, and that -1 is written straight into TexIndex.
        int textureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : 0;
        float reliefDisp = material.Texture ? material.DisplacementScale : 0.0f;
        float reliefBump = material.Texture ? material.NormalStrength : 0.0f;

        // Rebased BEFORE any vertex is written, so indices emitted below address this
        // sphere within the shared buffer rather than restarting at zero.
        uint32_t startIndexOffset = s_Data.SphereVertexCount;
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const glm::vec3 &p = mesh.Positions[i];
            const glm::vec3 &n = mesh.Normals[i];
            // Vertices are transformed on the CPU into world space. The batching is therefore
            // world-space geometry, which is why the buffer cursors are reset per flush and
            // why nothing downstream needs a model matrix.
            s_Data.SphereVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(p, 1.0f));
            s_Data.SphereVertexBufferPtr->Color = material.Albedo;
            s_Data.SphereVertexBufferPtr->Normal = glm::normalize(normalMatrix * n);
            s_Data.SphereVertexBufferPtr->TexCoord = mesh.TexCoords[i];
            // The slot index travels as a FLOAT varying even though it identifies an integer
            // slot, so that it can be interpolated and read per-pixel in the shader.
            s_Data.SphereVertexBufferPtr->TexIndex = (float)textureSlot;
            s_Data.SphereVertexBufferPtr->TilingFactor = 1.0f;
            s_Data.SphereVertexBufferPtr->EntityID = (float)entityID;
            s_Data.SphereVertexBufferPtr->Metallic = material.Metallic;
            s_Data.SphereVertexBufferPtr->Roughness = material.Roughness;
            // reliefDisp rather than a per-material lookup, because it was resolved once above
            // -- saving the branch and the null check on a per-vertex path that runs up to
            // 726 times per cube.
            s_Data.SphereVertexBufferPtr->Displacement = reliefDisp;
            s_Data.SphereVertexBufferPtr->BumpStrength = reliefBump;
            s_Data.SphereVertexBufferPtr++;
        }
        for (uint32_t i = 0; i < indexCount; ++i)
            // Indices are rebased by the batch's starting vertex offset, because every draw
            // appends to ONE shared buffer rather than restarting at zero.
            *s_Data.SphereIndexBufferPtr++ = startIndexOffset + mesh.Indices[i];

        // Counters advanced after both loops, matching the ordering used in DrawCube's
        // subdivided path.
        s_Data.SphereVertexCount += vertexCount;
        s_Data.SphereIndexCount += indexCount;
        // Derived from the ACTUAL index count rather than sectors*stacks, so the poles
        // that were skipped are not counted as quads they never were.
        s_Data.Stats.QuadCount += (indexCount / 6);

        // Vertex generation happens regardless of backend, but the analytic instance is
        // only built when the tracer is live. The seven shapes therefore cost CPU
        // tessellation even on a raster-only run, where they are not drawn.
        if (!s_Data.RayTracingEnabled)
            return;

        RayTracingInstance instance = {};
        instance.WorldTransform = transform;
        instance.InvTransform = invTransform;
        instance.Albedo = material.Albedo;
        // MaterialParams.z carries the shape discriminator read by HitPrimitive;
        // .w carries the radius the analytic routines need.
        // Four meanings in four slots: metallic, roughness, the shape discriminator read
        // by HitPrimitive, and the shape's primary radius. The packing is a shader ABI 
        instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, (float)shape, shapeParam);
        // Min.w is the SECOND dimension (height, or minor radius) for
        // cylinder/cone/torus -- not the constant 1.0 the cube/sphere paths use.
        // Min.w is NOT a constant: it carries the second dimension -- height for a
        // cylinder or cone, minor radius for a torus. Max.w below is the fixed 1.0 that
        // the cube and sphere paths rely on.
        instance.Min = glm::vec4(localMin, shapeParam2);
        instance.Max = glm::vec4(localMax, 1.0f);
        instance.Emission = glm::vec4(material.EmissionColor.x,
                                      material.EmissionColor.y,
                                      material.EmissionColor.z,
                                      material.EmissionIntensity);

        glm::vec3 worldScale = GetWorldScale(transform);
        // The MEAN of the three axis scales. DEFECT: for a non-uniformly scaled
        // primitive this under- or over-estimates the radius depending on which axis is
        // longest, so an anisotropically stretched shape gets the wrong bounding sphere.
        float avgScale = (worldScale.x + worldScale.y + worldScale.z) / 3.0f;
        float worldRadius = shapeParam * avgScale;

        // The same FindOrAddTextureSlot call as the vertex path, so a texture already
        // claimed by a vertex gets its existing slot rather than a second one. -1 means
        // "no texture" here, which is safe for the ray tracer but would be an
        // out-of-range raster index.
        instance.TextureID = material.Texture ? FindOrAddTextureSlot(material.Texture) : -1;
        // Always -1. Nothing in this file ever sets it, so any shader reading it is
        // reading a sentinel rather than a packed channel ID.
        instance.PackedMaterialMapID = -1;
        instance.TextureScale = glm::vec4(worldRadius * (localMax.x - localMin.x),
                                          worldRadius * (localMax.z - localMin.z),
                                          0.0f, 0.0f);
        float bumpStrength = material.NormalStrength > 0.0f ? material.NormalStrength : (material.Texture ? 1.0f : 0.0f);
        instance.DisplacementParams = glm::vec4(material.DisplacementScale, bumpStrength, 0.0f, 0.0f);

        // Frustum culling: skip instances outside the camera frustum.
        // For large open worlds this is the single most impactful optimization:
        // a 2500x2500m city may have 1000+ buildings but only ~50 are visible.
        {
            glm::vec3 center = glm::vec3(instance.WorldTransform[3]);
            glm::vec3 halfExtents = (glm::vec3(instance.Max) - glm::vec3(instance.Min)) * 0.5f;
            // Conservative AABB: absorb rotation by summing absolute basis vectors
            glm::vec3 axisX = glm::abs(glm::vec3(instance.WorldTransform[0])) * halfExtents.x;
            glm::vec3 axisY = glm::abs(glm::vec3(instance.WorldTransform[1])) * halfExtents.y;
            glm::vec3 axisZ = glm::abs(glm::vec3(instance.WorldTransform[2])) * halfExtents.z;
            glm::vec3 worldHalf = axisX + axisY + axisZ;

            bool visible = true;
            for (int p = 0; p < 6; p++)
            {
                const auto &plane = s_Data.FrustumPlanes[p];
                // Signed distance from AABB center to plane, minus AABB radius along plane normal
                float dist = glm::dot(plane.normal, center) + plane.distance;
                float radius = glm::dot(worldHalf, glm::abs(plane.normal));
                if (dist < -radius)
                {
                    visible = false;
                    break;
                }
            }

            if (visible)
            {
                // Distance LOD with hysteresis: buildings beyond LODDistance get LODLevel=1.
                // The shader can use LODLevel to skip detailed geometry. Hysteresis prevents
                // pop-in/pop-out by using a larger distance to drop LOD than to restore it.
                float camDist = glm::length(center - s_Data.CurrentCameraPosition);
                if (camDist > s_Data.LODFarDistance)
                    instance.LODLevel = 2;  // Far: lowest detail (shader may skip entirely)
                else if (camDist > s_Data.LODNearDistance)
                    instance.LODLevel = 1;  // Mid: reduced detail
                else
                    instance.LODLevel = 0;  // Near: full detail

                // Hard distance cull: skip instances beyond MaxDrawDistance entirely.
                // This is the most aggressive culling — buildings 2km away are not traced at all.
                if (camDist < s_Data.MaxDrawDistance)
                {
                    s_Data.m_SceneInstances.push_back(instance);
                }
            }
        }
        s_Data.m_SceneDirty = true;
    }
    // A thin wrapper: shape id, local bounds and the two shape parameters. The
    // `color` parameter is accepted and IGNORED by DrawProceduralPrimitive, which
    // takes the albedo from material.Albedo instead.
    void Renderer3D::DrawPlane(const glm::mat4 &transform, const glm::vec4 &color, int subdivisions, MaterialComponent &material, int entityID)
    {
        DrawProceduralPrimitive(transform, ProceduralMeshes::Plane(subdivisions), color, material,
                                 (int)PrimitiveShape::Plane,
                                 glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f),
                                 0.0f, 1.0f, entityID);
    }

    // radius is passed as BOTH shapeParam (to MaterialParams.w, the analytic radius)
    // and used to build the local bounds, so the shader and the bounding box agree.
    void Renderer3D::DrawCircle(const glm::mat4 &transform, const glm::vec4 &color, float radius, int segments, MaterialComponent &material, int entityID)
    {
        DrawProceduralPrimitive(transform, ProceduralMeshes::Circle(segments), color, material,
                                 (int)PrimitiveShape::Circle,
                                 glm::vec3(-radius, 0.0f, -radius), glm::vec3(radius, 0.0f, radius),
                                 radius, 1.0f, entityID);
    }

    void Renderer3D::DrawGrid(const glm::mat4 &transform, const glm::vec4 &color, int xSubdivisions, int ySubdivisions, MaterialComponent &material, int entityID)
    {
        DrawProceduralPrimitive(transform, ProceduralMeshes::Grid(xSubdivisions, ySubdivisions), color, material,
                                 (int)PrimitiveShape::Grid,
                                 glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f),
                                 0.0f, 1.0f, entityID);
    }

    // Height is a FULL height, unlike BoxCollider3DComponent's half-extents, so the
    // local bounds halve it while the shader receives the full value in Min.w.
    void Renderer3D::DrawCylinder(const glm::mat4 &transform, const glm::vec4 &color, float radius, float height, int segments, MaterialComponent &material, int entityID)
    {
        // Height is a FULL height here (unlike BoxCollider3DComponent's
        // half-extents), so the local bounds use height * 0.5 while the shader
        // receives the full value in Min.w.
        DrawProceduralPrimitive(transform, ProceduralMeshes::Cylinder(segments), color, material,
                                 (int)PrimitiveShape::Cylinder,
                                 glm::vec3(-radius, -height * 0.5f, -radius),
                                 glm::vec3(radius, height * 0.5f, radius),
                                 radius, height, entityID);
    }

    void Renderer3D::DrawCone(const glm::mat4 &transform, const glm::vec4 &color, float radius, float height, int segments, MaterialComponent &material, int entityID)
    {
        DrawProceduralPrimitive(transform, ProceduralMeshes::Cone(segments), color, material,
                                 (int)PrimitiveShape::Cone,
                                 glm::vec3(-radius, -height * 0.5f, -radius),
                                 glm::vec3(radius, height * 0.5f, radius),
                                 radius, height, entityID);
    }

    // HitTorus reads the major radius from MaterialParams.w, the minor radius from
    // Min.w, and the tube extent from the local AABB. The outer radius is precomputed
    // so the bounds enclose the whole ring.
    void Renderer3D::DrawTorus(const glm::mat4 &transform, const glm::vec4 &color, float majorRadius, float minorRadius, int majorSegments, int minorSegments, MaterialComponent &material, int entityID)
    {
        // HitTorus reads major radius from MaterialParams.w, minor radius from
        // Min.w, and the tube extent from the local AABB vectors.
        float outer = majorRadius + minorRadius;
        DrawProceduralPrimitive(transform, ProceduralMeshes::Torus(majorSegments, minorSegments), color, material,
                                 (int)PrimitiveShape::Torus,
                                 glm::vec3(-outer, -outer, -minorRadius), glm::vec3(outer, outer, minorRadius),
                                 majorRadius, minorRadius, entityID);
    }

    // The raster mesh is the tessellated icosphere, but the ray-traced form stays the
    // exact analytic sphere (shape 1) -- a tessellated mesh would only approximate
    // what HitSphere computes exactly.
    void Renderer3D::DrawIcoSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int subdivisions, MaterialComponent &material, int entityID)
    {
        // Raster form is the icosphere; the ray-traced form stays the exact
        // analytic sphere (shape 1), which a tessellated mesh would only
        // approximate.
        DrawProceduralPrimitive(transform, ProceduralMeshes::IcoSphere(subdivisions), color, material,
                                 (int)PrimitiveShape::Sphere,
                                 glm::vec3(-radius, -radius, -radius),
                                 glm::vec3(radius, radius, radius),
                                 radius, 1.0f, entityID);
    }

    // POST-PROCESSING ACCESSORS (reconstructed after the Phase -1 D1 accident).
    //
    // These 34 getters/setters were declared in Renderer3D.h and called from the
    // editor UI, but their definitions were lost with an accidental checkout.
    // What made the loss recoverable is that the SHADER side was intact and the
    // uniforms now exist: RunComposite reads all of them. Before this, vignette,
    // chromatic aberration and colour grading had no shader code whatsoever and
    // the bloom threshold was hardcoded, so these sliders never did anything.
    //
    // CLAMP RANGES ARE NOT INVENTED -- each matches the range documented in the
    // matching Renderer3D.h declaration, and each setter clamps on write exactly
    // as those comments describe. They are NOT all 0..1: VignetteIntensity is
    // 0..2 (the header calls that the odd one out), and ColorTemperature,
    // ColorTint and ChromaticAberrationIntensity span other ranges.
    //
    // NONE OF THESE RESET FrameIndex. They are screen-space passes over an
    // already-converged trace, so changing one does not invalidate accumulated
    // samples -- unlike the global-fog setters, which do reset. This matches the
    // reasoning preserved in the Renderer3D.h comments.

    // Bloom
    bool Renderer3D::IsBloomEnabled() { return s_Data.BloomEnabled; }
    void Renderer3D::SetBloomEnabled(bool enabled) { s_Data.BloomEnabled = enabled; }
    bool Renderer3D::GetBloomEnabled() { return s_Data.BloomEnabled; }
    float Renderer3D::GetBloomIntensity() { return s_Data.BloomIntensity; }
    void Renderer3D::SetBloomIntensity(float intensity) { s_Data.BloomIntensity = glm::max(intensity, 0.0f); }
    float Renderer3D::GetBloomThreshold() { return s_Data.BloomThreshold; }
    void Renderer3D::SetBloomThreshold(float threshold) { s_Data.BloomThreshold = glm::clamp(threshold, 0.0f, 1.0f); }

    // Bilateral blur (denoise)
    bool Renderer3D::IsBilateralBlurEnabled() { return s_Data.BilateralBlurEnabled; }
    void Renderer3D::SetBilateralBlurEnabled(bool enabled) { s_Data.BilateralBlurEnabled = enabled; }
    int Renderer3D::GetBilateralBlurPasses() { return s_Data.BilateralBlurPasses; }
    void Renderer3D::SetBilateralBlurPasses(int passes) { s_Data.BilateralBlurPasses = glm::clamp(passes, 0, 4); }
    bool Renderer3D::IsSVGFEnabled() { return s_Data.SvfgEnabled; }
    void Renderer3D::SetSVGFEnabled(bool enabled)
    {
        if (s_Data.SvfgEnabled != enabled)
        {
            s_Data.SvfgEnabled = enabled;
            s_Data.FrameIndex = 0;
        }
    }
    bool Renderer3D::IsReSTIREnabled() { return s_Data.RestirEnabled; }
    void Renderer3D::SetReSTIREnabled(bool enabled)
    {
        if (s_Data.RestirEnabled != enabled)
        {
            s_Data.RestirEnabled = enabled;
            s_Data.FrameIndex = 0;
        }
    }

    // Vignette -- intensity is 0..2, NOT 0..1 like its neighbours.
    bool Renderer3D::IsVignetteEnabled() { return s_Data.VignetteEnabled; }
    void Renderer3D::SetVignetteEnabled(bool enabled) { s_Data.VignetteEnabled = enabled; }
    bool Renderer3D::GetVignetteEnabled() { return s_Data.VignetteEnabled; }
    float Renderer3D::GetVignetteIntensity() { return s_Data.VignetteIntensity; }
    void Renderer3D::SetVignetteIntensity(float intensity) { s_Data.VignetteIntensity = glm::clamp(intensity, 0.0f, 2.0f); }
    float Renderer3D::GetVignetteRadius() { return s_Data.VignetteRadius; }
    void Renderer3D::SetVignetteRadius(float radius) { s_Data.VignetteRadius = glm::clamp(radius, 0.0f, 1.0f); }
    float Renderer3D::GetVignetteSoftness() { return s_Data.VignetteSoftness; }
    void Renderer3D::SetVignetteSoftness(float softness) { s_Data.VignetteSoftness = glm::clamp(softness, 0.0f, 1.0f); }

    // Chromatic aberration
    bool Renderer3D::IsChromaticAberrationEnabled() { return s_Data.ChromaticAberrationEnabled; }
    void Renderer3D::SetChromaticAberrationEnabled(bool enabled) { s_Data.ChromaticAberrationEnabled = enabled; }
    bool Renderer3D::GetChromaticAberrationEnabled() { return s_Data.ChromaticAberrationEnabled; }
    float Renderer3D::GetChromaticAberrationIntensity() { return s_Data.ChromaticAberrationIntensity; }
    void Renderer3D::SetChromaticAberrationIntensity(float intensity) { s_Data.ChromaticAberrationIntensity = glm::clamp(intensity, 0.0f, 1.0f); }

    // Colour grading. Temperature and tint are SIGNED (-1..1): the header is
    // explicit that the range is symmetric and negative values are meaningful,
    // not a clamp artefact. Gamma is floored at 0.1 to keep the shader's pow()
    // exponent positive, and is deliberately not clamped above.
    bool Renderer3D::IsColorGradingEnabled() { return s_Data.ColorGradingEnabled; }
    void Renderer3D::SetColorGradingEnabled(bool enabled) { s_Data.ColorGradingEnabled = enabled; }
    bool Renderer3D::GetColorGradingEnabled() { return s_Data.ColorGradingEnabled; }
    float Renderer3D::GetColorTemperature() { return s_Data.ColorTemperature; }
    void Renderer3D::SetColorTemperature(float temperature) { s_Data.ColorTemperature = glm::clamp(temperature, -1.0f, 1.0f); }
    float Renderer3D::GetColorTint() { return s_Data.ColorTint; }
    void Renderer3D::SetColorTint(float tint) { s_Data.ColorTint = glm::clamp(tint, -1.0f, 1.0f); }
    float Renderer3D::GetColorGamma() { return s_Data.ColorGamma; }
    void Renderer3D::SetColorGamma(float gamma) { s_Data.ColorGamma = glm::max(gamma, 0.1f); }

    // Film grain
    bool Renderer3D::IsFilmGrainEnabled() { return s_Data.FilmGrainEnabled; }
    void Renderer3D::SetFilmGrainEnabled(bool enabled) { s_Data.FilmGrainEnabled = enabled; }
    bool Renderer3D::GetFilmGrainEnabled() { return s_Data.FilmGrainEnabled; }
    float Renderer3D::GetFilmGrainIntensity() { return s_Data.FilmGrainIntensity; }
    // Clamped to 0..1 here in the accessor rather than trusted from the UI, so a
    // script calling the Python binding cannot push grain outside the range the
    // composite shader expects.
    void Renderer3D::SetFilmGrainIntensity(float intensity) { s_Data.FilmGrainIntensity = glm::clamp(intensity, 0.0f, 1.0f); }

    // D3: THIS SETTER HAS NO EFFECT ON RENDERING.
    //
    // It was reachable from an editor slider offering 1..256, and the value
    // reached the shader as u_SamplesPerPixel -- which no pass reads, because
    // there is no per-pixel sample loop anywhere in the Nova path. The slider
    // has been removed from EditorLayer.cpp: a control that appears to work and
    // does not is worse than no control.
    //
    // The getter, setter and Python binding all stay, because this is a public
    // API and removing it would break existing scripts. They now store and
    // return a value the renderer ignores, which is at least honest. Real
    // multi-sampling is Phase 4 work (jittered projection + temporal
    // accumulation), and this becomes its control when that lands.
    uint32_t Renderer3D::GetSamplesPerPixel() { return s_Data.SamplesPerPixel; }
    void Renderer3D::SetSamplesPerPixel(uint32_t samples)
    {
        s_Data.SamplesPerPixel = glm::max(1u, samples);
        s_Data.FrameIndex = 0;
    }

    float Renderer3D::GetMovementThreshold() { return s_Data.MovementThreshold; }
    // One-liner, and unlike its neighbours it does NOT reset FrameIndex -- which is
    // correct, since the threshold only affects whether the OpenGL path resets
    // accumulation, not what a frame looks like. It is also GL-only; see the note
    // at its use in Flush.
    void Renderer3D::SetMovementThreshold(float threshold) { s_Data.MovementThreshold = glm::max(0.00001f, threshold); }

    // A two-colour vertical gradient rather than a physical sky model: only the two
    // endpoints are stored, and the shader interpolates between them.
    glm::vec3 Renderer3D::GetSkyBottomColor() { return s_Data.SkyBottomColor; }
    void Renderer3D::SetSkyBottomColor(const glm::vec3 &color)
    {
        s_Data.SkyBottomColor = color;
        s_Data.FrameIndex = 0;
    }

    glm::vec3 Renderer3D::GetSkyTopColor() { return s_Data.SkyTopColor; }
    void Renderer3D::SetSkyTopColor(const glm::vec3 &color)
    {
        s_Data.SkyTopColor = color;
        s_Data.FrameIndex = 0;
    }

    // One entry per fog volume into a fixed-capacity list that is uploaded whole each
    // frame. Capacity is enforced by the early return below, not by an assert, so a
    // scene with too many volumes silently drops the excess.
    void Renderer3D::SubmitFogVolume(const glm::mat4 &transform, const VolumetricFogComponent &fog)
    {
        if (!fog.Enabled)
            return;
        // DEFECT (maintainability): the cast is to size_t here but the Nova uploads clamp
        // against the LITERAL size_t(4) while the GL path clamps against this named
        // MaxFogVolumes. Raising the constant fixes GL and leaves the Nova path still
        // capping at four.
        if (s_Data.FogVolumes.size() >= (size_t)MaxFogVolumes)
            return;
        FogVolumeData v;
        // Local-space bounds straight from the transform, matching the local-space Min/Max
        // convention every RayTracingInstance uses. The shader transforms the volume into
        // ray space per sample rather than the CPU baking a world AABB.
        ComputeVolumeBounds(transform, v.Min, v.Max);
        v.Color = fog.Color;
        v.Density = glm::max(fog.Density, 0.0f);
        // Clamped short of +/-1. At exactly 1 the Henyey-Greenstein phase function
        // becomes a delta in the forward direction, which makes a march step converge to
        // a single bright pixel instead of a smooth falloff.
        v.Anisotropy = glm::clamp(fog.Anisotropy, -0.9f, 0.9f);
        v.NoiseStrength = glm::clamp(fog.NoiseStrength, 0.0f, 1.0f);
        // The 0.001 floor is a divide-by-zero guard, matching the scale floor used in the
        // water cull path. A zero noise scale would make every sample share one noise
        // value and the volume would look like flat fog.
        v.NoiseScale = glm::max(fog.NoiseScale, 0.001f);
        v.WindSpeed = glm::max(fog.WindSpeed, 0.0f);
        v.HeightFalloff = glm::clamp(fog.HeightFalloff, 0.0f, 1.0f);
        // 1..32, clamped here rather than trusted from the component. The cost of a fog
        // volume is linear in this, so it is the main lever an artist has on frame time.
        v.Steps = glm::clamp(fog.MaxSteps, 1, 32);
        v.Enabled = true;
        s_Data.FogVolumes.push_back(v);
    }

    // Structurally identical to the fog path above, with extra coverage and detail
    // terms. The same capacity defect applies to the literal size_t(2) Nova cap.
    void Renderer3D::SubmitCloudVolume(const glm::mat4 &transform, const VolumetricCloudsComponent &clouds)
    {
        if (!clouds.Enabled)
            return;
        if (s_Data.CloudVolumes.size() >= (size_t)MaxCloudVolumes)
            return;
        CloudVolumeData v;
        ComputeVolumeBounds(transform, v.Min, v.Max);
        v.Color = clouds.Color;
        v.Ambient = clouds.AmbientTint;
        v.Coverage = glm::clamp(clouds.Coverage, 0.0f, 1.0f);
        v.Density = glm::max(clouds.Density, 0.0f);
        v.NoiseScale = glm::max(clouds.NoiseScale, 0.001f);
        v.Detail = glm::clamp(clouds.DetailAmount, 0.0f, 1.0f);
        v.WindDir = clouds.WindDirection;
        // A zero-length wind vector would normalize to NaN inside the shader and
        // propagate through the whole cloud march. Substituted with +X, an arbitrary but
        // finite direction.
        if (glm::length2(v.WindDir) < 1e-8f)
            // Assigned as a raw vec2, NOT normalized -- it is a direction, and the shader
            // scales it by WindSpeed. Normalizing here and multiplying by speed later would
            // be equivalent, but this keeps the author's intent (a unit vector) explicit.
            v.WindDir = glm::vec2(1.0f, 0.0f);
        v.WindSpeed = glm::max(clouds.WindSpeed, 0.0f);
        v.SilverLining = glm::clamp(clouds.SilverLining, 0.0f, 1.0f);
        v.ShadowStrength = glm::clamp(clouds.ShadowStrength, 0.0f, 1.0f);
        v.Steps = glm::clamp(clouds.MaxSteps, 1, 32);
        v.Enabled = true;
        s_Data.CloudVolumes.push_back(v);
    }

    bool Renderer3D::IsVolumetricFogEnabled() { return s_Data.VolFogEnabled; }
    void Renderer3D::SetVolumetricFogEnabled(bool enabled)
    {
        s_Data.VolFogEnabled = enabled;
        s_Data.FrameIndex = 0;
    }
    bool Renderer3D::IsVolumetricCloudsEnabled() { return s_Data.VolCloudsEnabled; }
    void Renderer3D::SetVolumetricCloudsEnabled(bool enabled)
    {
        s_Data.VolCloudsEnabled = enabled;
        s_Data.FrameIndex = 0;
    }
    glm::vec3 Renderer3D::GetSunDirection() { return s_Data.SunDirection; }
    // DEFECT: this normalizes and stores SunDirection, but the god-ray and volumetric
    // passes read EffSunDir instead -- a different field this setter never touches.
    // So GetSunDirection can report a direction the volumetric path is not using.
    void Renderer3D::SetSunDirection(const glm::vec3 &direction)
    {
        // A near-zero direction is REJECTED rather than normalized, because normalizing it
        // would produce NaN and poison every downstream sample. The previous value
        // survives, so a bad input is a no-op.
        if (glm::length2(direction) > 1e-8f)
            s_Data.SunDirection = glm::normalize(direction);
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetVolumetricStepScale() { return s_Data.VolStepScale; }
    void Renderer3D::SetVolumetricStepScale(float scale)
    {
        // 0.25..2.0, i.e. a quarter to double the ray-march step count. The lower bound
        // keeps a foggy scene from becoming a single-step approximation.
        s_Data.VolStepScale = glm::clamp(scale, 0.25f, 2.0f);
        s_Data.FrameIndex = 0;
    }

    // Global atmospheric fog
    bool Renderer3D::IsGlobalFogEnabled() { return s_Data.GlobalFogEnabled; }
    void Renderer3D::SetGlobalFogEnabled(bool enabled) { s_Data.GlobalFogEnabled = enabled; s_Data.FrameIndex = 0; }
    float Renderer3D::GetGlobalFogDensity() { return s_Data.GlobalFogDensity; }
    void Renderer3D::SetGlobalFogDensity(float density) { s_Data.GlobalFogDensity = glm::max(density, 0.0f); s_Data.FrameIndex = 0; }
    float Renderer3D::GetGlobalFogHeightFalloff() { return s_Data.GlobalFogHeightFalloff; }
    void Renderer3D::SetGlobalFogHeightFalloff(float falloff) { s_Data.GlobalFogHeightFalloff = glm::max(falloff, 0.0f); s_Data.FrameIndex = 0; }
    glm::vec3 Renderer3D::GetGlobalFogColor() { return s_Data.GlobalFogColor; }
    void Renderer3D::SetGlobalFogColor(const glm::vec3 &color) { s_Data.GlobalFogColor = color; s_Data.FrameIndex = 0; }
    float Renderer3D::GetGlobalFogBaseHeight() { return s_Data.GlobalFogBaseHeight; }
    void Renderer3D::SetGlobalFogBaseHeight(float height) { s_Data.GlobalFogBaseHeight = height; s_Data.FrameIndex = 0; }
    float Renderer3D::GetGlobalFogDistanceAttenuation() { return s_Data.GlobalFogDistAtten; }
    void Renderer3D::SetGlobalFogDistanceAttenuation(float atten) { s_Data.GlobalFogDistAtten = glm::max(atten, 0.0f); s_Data.FrameIndex = 0; }

    // God rays
    bool Renderer3D::IsGodRaysEnabled() { return s_Data.GodRaysEnabled; }
    // One-liner with no FrameIndex reset, unlike the fog setters above. Toggling god
    // rays therefore does not restart accumulation, so shafts appear only gradually as
    // the alpha blend converges them in.
    void Renderer3D::SetGodRaysEnabled(bool enabled) { s_Data.GodRaysEnabled = enabled; }
    float Renderer3D::GetGodRayIntensity() { return s_Data.GodRayIntensity; }
    void Renderer3D::SetGodRayIntensity(float intensity) { s_Data.GodRayIntensity = glm::max(intensity, 0.0f); }
    float Renderer3D::GetGodRayDecay() { return s_Data.GodRayDecay; }
    // Clamped to 0..1, unlike GodRayIntensity and GodRayDensity which are only
    // lower-bounded. At 0 there is no falloff along the shaft and at 1 it fades out
    // almost immediately.
    void Renderer3D::SetGodRayDecay(float decay) { s_Data.GodRayDecay = glm::clamp(decay, 0.0f, 1.0f); }
    int Renderer3D::GetGodRaySamples() { return s_Data.GodRaySamples; }
    // 1..64 march steps. The lower bound of 1 means the effect degrades to a single
    // sample rather than disappearing, which is the safe direction for a screen-space
    // effect.
    void Renderer3D::SetGodRaySamples(int samples) { s_Data.GodRaySamples = glm::clamp(samples, 1, 64); }
    float Renderer3D::GetGodRayDensity() { return s_Data.GodRayDensity; }
    void Renderer3D::SetGodRayDensity(float density) { s_Data.GodRayDensity = glm::max(density, 0.0f); }
    glm::vec3 Renderer3D::GetGodRayColor() { return s_Data.GodRayColor; }
    void Renderer3D::SetGodRayColor(const glm::vec3 &color) { s_Data.GodRayColor = color; }

    // Camera-relative importance for one candidate, used only to break ties WITHIN a
    // light type (see PushAnalyticLight). Squared distance, so no sqrt, and no
    // inverse-square: the ranking only has to be monotone in distance, and squaring
    // keeps it that way without an epsilon guard at zero.
    //
    // A directional light has no position, so distance is meaningless for it and it
    // scores 0 -- it is admitted on TYPE alone. That is deliberate: the sun is usually
    // the key light, and it must not lose a slot to a nearby torch because the author
    // happened to put a torch in the scene. The ranking never overrides the type
    // quota, only which member of an over-quota type survives.
    static float AnalyticLightImportance(const AnalyticLightData &v)
    {
        if (v.Type == 0.0f)
            return 0.0f;
        return glm::max(v.Intensity, 0.0f) / (1.0f + glm::length2(v.Position - s_Data.CurrentCameraPosition));
    }

    // Selects the nearest MaxRenderedLights to the camera from the collected
    // AnalyticLights. Called once per frame after all lights are submitted and
    // before UploadAnalyticLights. Distance-based: the nearest lights win,
    // which is what a viewer expects -- a street light 10m away should shade
    // the scene, not one 2km away.
    static void SelectRenderedLights()
    {
        s_Data.RenderedLights.clear();
        const int total = (int)s_Data.AnalyticLights.size();
        if (total == 0)
            return;

        const glm::vec3 cam = s_Data.CurrentCameraPosition;
        // Index + squared distance, so the sort is a single comparison.
        std::vector<std::pair<float, int>> order;
        order.reserve((size_t)total);
        for (int i = 0; i < total; i++)
        {
            const glm::vec3 d = s_Data.AnalyticLights[(size_t)i].Position - cam;
            order.push_back({glm::dot(d, d), i});
        }
        const int keep = total < Renderer3D::MaxRenderedLights ? total : Renderer3D::MaxRenderedLights;
        // Partial sort: only the nearest `keep` are needed, so O(n) rather
        // than O(n log n) for the full sort.
        std::partial_sort(order.begin(), order.begin() + keep, order.end(),
            [](const std::pair<float, int> &a, const std::pair<float, int> &b) { return a.first < b.first; });
        s_Data.RenderedLights.reserve((size_t)keep);
        for (int i = 0; i < keep; i++)
            s_Data.RenderedLights.push_back(s_Data.AnalyticLights[(size_t)order[(size_t)i].second]);
    }

    // The single choke point for all four light types, so the capacity limit is
    // enforced once and every Submit* benefits from it.
    //
    // Two rules, in this order:
    //
    // 1. TYPE QUOTA. A light may always take a free slot. If the list is FULL, a
    //    candidate whose type is not yet represented is admitted by evicting the
    //    least important light OF A TYPE THAT ALREADY HAS SURPLUS -- never by
    //    evicting the last representative of a type. Without this the Scene's
    //    type-ordered submission (Scene::SubmitSceneLights walks directional, then
    //    point, then spot, then area) means 8 point lights consume all 8 slots and
    //    every spot and area light in the scene is silently dropped. That was a
    //    correctness bug, not a perf trade: the lights are in the scene and simply
    //    do not appear.
    //
    // 2. IMPORTANCE TIEBREAK. When the candidate's type already has a seat (or the
    //    quota cannot help), the slot goes to whichever of the two is more important
    //    per AnalyticLightImportance, and the loser is DROPPED and counted. Within a
    //    single type this is what stops the 9th point light from winning merely by
    //    being submitted first.
    //
    // The quota is deliberately checked BEFORE importance rather than folding type
    // into the score. A single scalar cannot express "at least one of each type":
    // any score a distant spot light can beat, a cluster of 8 point lights next to
    // the camera also beats, so a weighted blend would reintroduce exactly the
    // starvation the quota removes. Camera position IS reachable here without new
    // plumbing -- BeginScene writes s_Data.CurrentCameraPosition before the Scene
    // submits any light -- so the tiebreak is a real measurement, not a stand-in.
    static void PushAnalyticLight(const AnalyticLightData &v)
    {
        s_Data.AnalyticLightsSubmitted++;
        // Collection cap only: the scene may hold up to MaxAnalyticLights
        // lights. No eviction here -- selection to the render cap happens
        // per-frame in SelectRenderedLights, after the camera position is
        // known. Directional lights (no position) are never distance-culled.
        if (s_Data.AnalyticLights.size() < (size_t)Renderer3D::MaxAnalyticLights)
            s_Data.AnalyticLights.push_back(v);
        else
            s_Data.AnalyticLightsDropped++;
    }

    // Called once per frame from BeginScene, BEFORE AnalyticLights is cleared, so it
    // reports the frame that just ended. One WL_CORE_WARN per frame at most, and only
    // when the dropped count differs from the last one reported -- see the field
    // comments on AnalyticLightsLastReportedDropped for why a per-frame line would be
    // its own spam. Silent while the scene fits, which is the common case and must
    // stay completely silent.
    static void ReportAnalyticLightOverflow()
    {
        const int dropped = s_Data.AnalyticLightsDropped;
        if (dropped != s_Data.AnalyticLightsLastReportedDropped)
        {
            s_Data.AnalyticLightsLastReportedDropped = dropped;
            if (dropped > 0)
                WL_CORE_WARN("Analytic lights over budget: {0} submitted, {1} admitted, {2} dropped (cap {3}). "
                             "Admission is quota-first per type, then camera-distance; a light type with no "
                             "surplus to give is dropped regardless of distance.",
                             s_Data.AnalyticLightsSubmitted,
                             (int)s_Data.AnalyticLights.size(),
                             dropped,
                             Renderer3D::MaxAnalyticLights);
        }
        s_Data.AnalyticLightsSubmitted = 0;
        s_Data.AnalyticLightsDropped = 0;
    }

    // The one light type with no position and no range -- it sets only four fields.
    // Everything else keeps its struct default, and because AnalyticLightData
    // default-initializes every member, those defaults are well-defined rather than
    // whatever was on the stack.
    void Renderer3D::SubmitDirectionalLight(const glm::mat4 &transform, const DirectionalLightComponent &light)
    {
        AnalyticLightData v;
        // TRAVEL direction (the way photons move), which is why the helper negates the
        // matrix's forward axis. Getting this backwards would light the scene from
        // behind the lamp, a bug that is invisible in a symmetric test scene.
        v.Direction = LightTravelDirection(transform);
        v.Color = light.Color;
        // The shader's type discriminator: 0 directional, 1 point, 2 spot, 3 area. Stored
        // as a float because the struct is uploaded as a flat float buffer.
        v.Type = 0.0f;
        v.Intensity = glm::max(light.Intensity, 0.0f);
        PushAnalyticLight(v);
    }

    // Leaves Range, Falloff, InnerCos, OuterCos and AreaSize at their struct defaults.
    // That is correct -- a point light has no cone and no area, and Range 0 means
    // unbounded.
    void Renderer3D::SubmitPointLight(const glm::mat4 &transform, const PointLightComponent &light)
    {
        AnalyticLightData v;
        v.Position = glm::vec3(transform[3]);
        v.Color = light.Color;
        v.Type = 1.0f;
        v.Intensity = glm::max(light.Intensity, 0.0f);
        v.Range = glm::max(light.Range, 0.0f);
        // Clamped to a physical inverse-square exponent range. Values below 0.5 would make
        // light grow with distance; the 8 ceiling stops a near-instant falloff that would
        // alias badly at low sample counts.
        v.Falloff = glm::clamp(light.Falloff, 0.5f, 8.0f);
        PushAnalyticLight(v);
    }

    // The only type needing two extra angles. They are converted to COSINES once here
    // rather than in the shader, so every sample is a compare rather than a
    // per-sample acos.
    void Renderer3D::SubmitSpotLight(const glm::mat4 &transform, const SpotLightComponent &light)
    {
        AnalyticLightData v;
        v.Position = glm::vec3(transform[3]);
        v.Direction = LightTravelDirection(transform);
        v.Color = light.Color;
        v.Type = 2.0f;
        v.Intensity = glm::max(light.Intensity, 0.0f);
        v.Range = glm::max(light.Range, 0.0f);
        v.Falloff = glm::clamp(light.Falloff, 0.5f, 8.0f);
        float inner = glm::clamp(light.InnerAngle, 0.0f, 89.5f);
        // 0.5..90 degrees. The 90 ceiling matters because cos(90) == 0 is the value the
        // shader smoothsteps to; exceeding it would invert the interpolation.
        float outer = glm::clamp(light.OuterAngle, 0.5f, 90.0f);
        // DEFECT (benign): inner is clamped to a maximum of 89.5 degrees and outer to a
        // maximum of 90, so a legal input can still violate the invariant -- a 90-degree
        // inner angle would have been clamped to 89.5, leaving outer below it. Fixed by
        // raising outer to inner, which yields a hard edge rather than a smooth cone.
        if (outer < inner)
            outer = inner;
        // smoothstep needs edge0 <= edge1, i.e. outerCos <= innerCos.
        // innerCos >= outerCos after the clamp above, which is what smoothstep requires
        // as edge0 <= edge1. Radians conversion is done once per light, not per sample.
        v.InnerCos = glm::cos(glm::radians(inner));
        v.OuterCos = glm::cos(glm::radians(outer));
        PushAnalyticLight(v);
    }

    // Approximated as a point light with a soft radius rather than sampled as a true
    // area emitter, which is the standard real-time trade.
    void Renderer3D::SubmitAreaLight(const glm::mat4 &transform, const AreaLightComponent &light)
    {
        AnalyticLightData v;
        v.Position = glm::vec3(transform[3]);
        v.Direction = LightTravelDirection(transform);
        v.Color = light.Color;
        v.Type = 3.0f;
        v.Intensity = glm::max(light.Intensity, 0.0f);
        v.Range = glm::max(light.Range, 0.0f);
        v.Falloff = 2.0f;
        float w = glm::max(light.Width, 0.01f);
        float h = glm::max(light.Height, 0.01f);
        // The GEOMETRIC MEAN of width and height, and the square root of it: so
        // AreaSize == w for a square, and the shader can recover roughly sqrt(A) as a
        // sampling radius from a single float.
        v.AreaSize = glm::sqrt(w * h);
        // A bool packed to a float, like Type and Falloff -- the whole struct is flat
        // floats for the upload, so there is no room for a real bool.
        v.DoubleSided = light.DoubleSided ? 1.0f : 0.0f;
        PushAnalyticLight(v);
    }

    bool Renderer3D::IsNeuralRenderingEnabled() { return s_Data.NeuralEnabled; }
    void Renderer3D::SetNeuralRenderingEnabled(bool enabled)
    {
        s_Data.NeuralEnabled = enabled;
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetNeuralTextureStrength() { return s_Data.NeuralTextureStrength; }
    // 0..1 for each of the three neural weights. All three reset FrameIndex, unlike
    // the post-process setters earlier in the file, because a neural weight changes
    // the traced image itself rather than a final grade.
    void Renderer3D::SetNeuralTextureStrength(float strength)
    {
        // Clamped rather than passed through: the neural pass blends between the
        // traced result and the network's output by this weight, so values outside 0..1
        // would extrapolate rather than interpolate.
        s_Data.NeuralTextureStrength = glm::clamp(strength, 0.0f, 1.0f);
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetNeuralLightStrength() { return s_Data.NeuralLightStrength; }
    void Renderer3D::SetNeuralLightStrength(float strength)
    {
        s_Data.NeuralLightStrength = glm::clamp(strength, 0.0f, 1.0f);
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetNeuralMaterialStrength() { return s_Data.NeuralMaterialStrength; }
    void Renderer3D::SetNeuralMaterialStrength(float strength)
    {
        s_Data.NeuralMaterialStrength = glm::clamp(strength, 0.0f, 1.0f);
        s_Data.FrameIndex = 0;
    }

    // The classic radical-inverse construction. Still present and still correct, but
    // GetCurrentJitter no longer calls it -- STBN replaced it because a low-
    // discrepancy sequence still produces structured, repeating artefacts once
    // accumulated, whereas blue noise does not.
    float Renderer3D::Halton(int index, int base)
    {
        // The running 1/base^k weight, divided rather than multiplied by an inverse
        // power so no pow() is needed.
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0)
        {
            f = f / (float)base;
            // Integer modulo extracts one digit of the index in the given base. Accumulating
            // the digits weighted by descending powers of 1/base is what inverts the digit
            // order.
            r = r + f * (float)(index % base);
            index = index / base;
        }
        return r;
    }

    // Seeded by frameIndex AND by viewport size, so resizing the window restarts the
    // sequence rather than continuing a pattern computed for different dimensions.
    glm::vec2 Renderer3D::GetCurrentJitter(int frameIndex, glm::vec2 viewportSize)
    {
        // STBN spatio-temporal blue noise instead of Halton(2,3). The old
        // sequence was 'frameIndex % 16', so it repeated exactly every 16
        // frames -- the same 16 structured artefacts recurred in the
        // accumulation buffer. STBN gives 64 decorrelated frames with ~21x
        // low-frequency suppression, and works identically on every backend
        // including OpenGL because the jitter is a CPU-side uniform.
        float jitterX = 0.0f, jitterY = 0.0f;
        // The STBN volume lookup. It returns false and leaves the outputs untouched if
        // the asset failed to load, in which case jitterX/jitterY stay at the 0.0f
        // initialized above and the frame simply renders unjittered.
        BlueNoise::Jitter(static_cast<uint32_t>(frameIndex),
                          static_cast<uint32_t>(viewportSize.x),
                          static_cast<uint32_t>(viewportSize.y), jitterX, jitterY);
        return glm::vec2(jitterX, jitterY);
    }

    // One entry point, three backend implementations, because each backend has its
    // own dispatch and its own resource type for the input texture. Only pass 9 is
    // involved -- no other pass reads or writes GeneratedTextureID.
    void Renderer3D::GenerateMaterialMaps(const std::string &texturePath, float normalStrength, float roughBias)
        {
            // DX11 first. Note the dispatch takes WIDTH/HEIGHT, not a workgroup count: the
            // Nova wrapper converts, unlike the raw glDispatchCompute below.
            if(s_Nova) {
                auto input=Texture2D::Create(texturePath);
                s_Nova->Set("u_NormalStrength",normalStrength);s_Nova->Set("u_RoughnessBias",roughBias);s_Nova->Set("u_AOIntensity",1.0f);
                s_Nova->Dispatch(9,s_Data.RayTracingWidth,s_Data.RayTracingHeight,{(ID3D11ShaderResourceView*)input->GetNativeSRV()});
                return;
            }
            if(s_Nova12) {
                auto input=Texture2D::Create(texturePath);
                s_Nova12->Set("u_NormalStrength",normalStrength);s_Nova12->Set("u_RoughnessBias",roughBias);s_Nova12->Set("u_AOIntensity",1.0f);
                auto *inputResource = static_cast<ID3D12Resource *>(input->GetNativeTextureResource());
                if (!inputResource)
                {
                    WL_CORE_ERROR("DX12 normal-map generation received no native ID3D12Resource");
                    return;
                }
                s_Nova12->Dispatch(9,s_Data.RayTracingWidth,s_Data.RayTracingHeight,{inputResource});
                return;
            }
            // No Nova and not GL -- the Vulkan case -- so there is nothing to dispatch and
            // no error is raised. A caller on Vulkan gets a silent no-op.
            if(!Renderer3DGLAvailable()) return;
        s_Data.RayTracingShader->Bind();

        s_Data.RayTracingShader->SetFloat("u_NormalStrength", normalStrength);
        s_Data.RayTracingShader->SetFloat("u_RoughnessBias", roughBias);

        Ref<Texture2D> inputTexture = Texture2D::Create(texturePath);
        glActiveTexture(GL_TEXTURE0 + 11);
        inputTexture->Bind();

        // Image unit 8, WRITE_ONLY. This is the only writer of GeneratedTextureID and
        // the only reader is whatever samples the generated maps afterwards, so no read
        // binding is needed here.
        glBindImageTexture(8, s_Data.GeneratedTextureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        // DEFECT: TRUNCATING division, unlike every other dispatch in this file which
        // uses (w + 7) / 8. A dimension that is not a multiple of 8 therefore drops its
        // last partial workgroup, leaving up to 7 rows or columns of the generated maps
        // unwritten.
        glDispatchCompute(s_Data.RayTracingWidth / 8, s_Data.RayTracingHeight / 8, 1);

        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    // Called after every Flush, and after a mid-frame overflow flush inside Draw*. It
    // resets the RASTER batch only.
    //
    // The ray-traced instance list is deliberately NOT reset here. Like the volume and
    // light lists it is frame-wide, and for the same reason: the tracer is dispatched
    // once per frame, at EndScene, and consumes every instance the frame submitted.
    // Clearing it at a mid-frame overflow flush -- and re-adding only the terrain
    // instances -- left the END-of-frame dispatch tracing just the few primitives
    // emitted after the flush. That near-empty trace then overwrote the correct image
    // the overflow flush had just produced, so any scene large enough to overflow the
    // sphere batch rendered as sky and nothing else. BeginScene clears the list once
    // per frame, which is where a per-frame reset belongs.
    void Renderer3D::FlushAndReset()
    {
        s_Data.CubeIndexCount = 0;
        s_Data.CubeVertexCount = 0;
        s_Data.CubeVertexBufferPtr = s_Data.CubeVertexBufferBase;

        s_Data.SphereIndexCount = 0;
        s_Data.SphereVertexCount = 0;
        s_Data.SphereVertexBufferPtr = s_Data.SphereVertexBufferBase;
        s_Data.SphereIndexBufferPtr = s_Data.SphereIndexBufferBase;

        // Back to 1, not 0: slot 0 is the white fallback installed in Init, and
        // resetting to 0 would let the next texture claim the reserved slot.
        s_Data.TextureSlotIndex = 1;
        s_Data.TextureSlotMap.clear();
    }

    // The most heavily used primitive: 24 vertices and 36 indices from static tables,
    // with a subdivided fallback when displacement is enabled. `color` is unused
    // here too -- the albedo comes from material.Albedo.
    void Renderer3D::DrawCube(const glm::mat4 &transform, const glm::vec4 &color, MaterialComponent &material, int entityID)
    {
        // BOTH limits must be tested. The classic path draws 36 indices from the STATIC
        // cube index buffer but writes 24 vertices into the cube vertex array, so an
        // index-only check lets CubeVertexCount reach (MaxIndices/36)*24 = 1,000,000 --
        // twice MaxVertices -- and run 40 MB past the CPU vertex array.
        if (s_Data.CubeIndexCount + 36 >= s_Data.MaxIndices ||
            s_Data.CubeVertexCount + 24 >= s_Data.MaxVertices)
        {
            Flush();
            FlushAndReset();
        }

        // Displacement-aware cull padding: relief pushes vertices outward.
        float cullPad = material.Texture ? material.DisplacementScale : 0.0f;
        // The +/-0.5 is the unit cube's half-extent and cullPad the displacement budget,
        // so the inflated box covers the classic path and the subdivided path alike.
        // Returning here means a culled cube contributes NO ray-traced instance either --
        // culling happens before the instance block below, so the tracer and the raster
        // path always agree on which cubes exist.
        if (!IsAABBInFrustum(s_Data.FrustumPlanes, glm::vec3(-0.5f - cullPad), glm::vec3(0.5f + cullPad), transform))
            return;

        // Six faces of four corners, four vertices each -- no index sharing between
        // faces. That is why the cube is 24 vertices rather than 8: shared corners
        // cannot carry per-face normals.
        static const glm::vec3 cubePositions[24] = {
            {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}};

        // Per-face normals, four identical entries per face for the same reason. The
        // -0.0f entries are harmless but mean a naive textual comparison against a
        // differently-signed table would report a difference.
        static const glm::vec3 cubeNormals[24] = {
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {-0.0f, 0.0f, -1.0f}, {-0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, -0.0f}};

        static const glm::vec2 texCoords[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        glm::mat4 invTransform = FastTRSInverse(transform);
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));

        // Raster-path material inputs (also reused by the ray-trace instance below)
        // Slot 0 (white) for untextured. DEFECT: if the 32 slots are exhausted
        // FindOrAddTextureSlot returns -1, and -1 is written into TexIndex here as a
        // float -- harmless for the Nova path but an out-of-range sampler index on the
        // raster path.
        int cubeTextureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : 0;

        // Neural geometric displacement: DisplacementScale > 0 subdivides the
        // cube into per-face grids that the vertex shader pushes along the
        // normal with the neural height field. Disp == 0 keeps the classic
        // 24-vertex path: zero behavior change.
        float reliefDisp = material.Texture ? material.DisplacementScale : 0.0f;
        float reliefBump = material.Texture ? material.NormalStrength : 0.0f;
        // gridN <= 1 selects the classic 24-vertex path, so a scene with no displacement
        // behaves identically to before this feature existed. The 10 cap bounds a
        // subdivided cube at 6 * 11 * 11 = 726 vertices.
        int gridN = (reliefDisp > 0.001f) ? std::min(2 + (int)(reliefDisp * 8.0f), 10) : 1;

        // The zero-behaviour-change path. Only reached when displacement is off or
        // negligible, so this is the hot case for ordinary scenes.
        if (gridN <= 1)
        {
            for (int i = 0; i < 24; i++)
            {
                s_Data.CubeVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(cubePositions[i], 1.0f));
                s_Data.CubeVertexBufferPtr->Color = material.Albedo;
                s_Data.CubeVertexBufferPtr->Normal = glm::normalize(normalMatrix * cubeNormals[i]);
                // UVs are indexed modulo 4 so each face's four corners take one of the four
                // shared (u,v) pairs. Face orientation is therefore encoded in the position
                // table, not in the UVs.
                s_Data.CubeVertexBufferPtr->TexCoord = texCoords[i % 4];
                s_Data.CubeVertexBufferPtr->TexIndex = (float)cubeTextureSlot;
                s_Data.CubeVertexBufferPtr->TilingFactor = 1.0f;
                s_Data.CubeVertexBufferPtr->EntityID = entityID;
                s_Data.CubeVertexBufferPtr->Metallic = material.Metallic;
                s_Data.CubeVertexBufferPtr->Roughness = material.Roughness;
                // Raster relief is opt-in (no Nova-style auto-bump): zero POM cost
                // for plain textured scenes until the artist dials it in.
                s_Data.CubeVertexBufferPtr->Displacement = material.Texture ? material.DisplacementScale : 0.0f;
                s_Data.CubeVertexBufferPtr->BumpStrength = material.Texture ? material.NormalStrength : 0.0f;
                s_Data.CubeVertexBufferPtr++;
            }

            // Indices come from the STATIC cube index buffer, so this is the DRAW count
            // handed to DrawIndexed, not a cursor. It must still advance, or Flush's
            // `if (CubeIndexCount)` skips the entire cube batch.
            s_Data.CubeIndexCount += 36;
            s_Data.CubeVertexCount += 24;
            // Six quads, matching the 36 indices. Stats are counted at EMIT time, not at
            // draw time, so a batch that is later culled or flushed still counts.
            s_Data.Stats.QuadCount += 6;
        }
        else
        {
            // Relief-subdivided cubes share the dynamic-index batch: the cube
            // batch index buffer is a static quad pattern and cannot grow.
            // Face grids mirror the classic corner order/winding per face.
            // Vertices per face for a gridN x gridN subdivision: (N+1)^2, because both
            // directions share their edge vertices.
            uint32_t side = (uint32_t)((gridN + 1) * (gridN + 1));
            uint32_t needVerts = side * 6;
            // 36 indices per grid cell, 6 faces: gridN^2 * 6 * 6. Matches the classic cube's
            // 36 only when gridN == 1.
            uint32_t needIdx = (uint32_t)(gridN * gridN * 36);
            // The subdivided cube borrows the SPHERE batch, not the cube batch -- see the
            // comment above. So its capacity check must also use the sphere counters, which
            // is what makes this a separate check from the one at the top of the function.
            if (s_Data.SphereVertexCount + needVerts >= s_Data.MaxVertices ||
                s_Data.SphereIndexCount + needIdx >= s_Data.MaxIndices)
            {
                Flush();
                FlushAndReset();
            }
            // Water borrows the SPHERE batch, like the subdivided cube does. So water and
            // relief-subdivided cubes can evict each other, even though neither uses a
            // sphere's vertex layout.
            uint32_t batchBase = s_Data.SphereVertexCount;
            // Six faces, each a full grid. The switch below maps (u,v) to a position and a
            // normal per face, and the winding of each face's quats is fixed by the index
            // order a,b,d,d,c,a further down.
            for (int f = 0; f < 6; f++)
            {
                uint32_t faceBase = batchBase + (uint32_t)f * side;
                for (int j = 0; j <= gridN; j++)
                {
                    for (int i = 0; i <= gridN; i++)
                    {
                        // Normalized face coordinates in 0..1, mapped to the -0.5..0.5 cube span inside
                        // the switch. texCoords could not be reused here because a subdivided face needs
                        // a continuous range, not four corners.
                        float u = (float)i / (float)gridN;
                        float v = (float)j / (float)gridN;
                        // Declared uninitialized before the switch, which is safe ONLY because every
                        // branch of the switch below assigns both. Adding a case without assigning them
                        // would read indeterminate values rather than fail to compile.
                        glm::vec3 lp, ln;
                        switch (f)
                        {
                        // +Z face. Each case maps the shared (u,v) grid to that face's plane, and the
                        // differences between cases -- which axis u runs along, whether it is negated, and
                        // which axis v lifts -- are what make the six faces wind consistently outward.
                        case 0:
                            lp = glm::vec3(-0.5f + u, -0.5f + v, 0.5f);
                            ln = glm::vec3(0.0f, 0.0f, 1.0f);
                            break;
                        case 1:
                            lp = glm::vec3(0.5f - u, -0.5f + v, -0.5f);
                            ln = glm::vec3(0.0f, 0.0f, -1.0f);
                            break;
                        // +Y face. u runs +X while v runs -Z, so both axes advance clockwise when viewed
                        // from outside. This mirrored ordering is what keeps the front faces outward on
                        // all six sides.
                        case 2:
                            lp = glm::vec3(-0.5f + u, 0.5f, 0.5f - v);
                            ln = glm::vec3(0.0f, 1.0f, 0.0f);
                            break;
                        case 3:
                            lp = glm::vec3(-0.5f + u, -0.5f, -0.5f + v);
                            ln = glm::vec3(0.0f, -1.0f, 0.0f);
                            break;
                        // +X face. u runs -Z here rather than +Z as on the opposite side, again to keep
                        // the handedness consistent with the neighbouring faces.
                        case 4:
                            lp = glm::vec3(0.5f, -0.5f + v, 0.5f - u);
                            ln = glm::vec3(1.0f, 0.0f, 0.0f);
                            break;
                        // Face 5 (-X). It is the default rather than an explicit `case 5`, so an f
                        // outside 0..5 would silently render as the -X face instead of failing.
                        default:
                            lp = glm::vec3(-0.5f, -0.5f + v, -0.5f + u);
                            ln = glm::vec3(-1.0f, 0.0f, 0.0f);
                            break;
                        }
                        s_Data.SphereVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(lp, 1.0f));
                        s_Data.SphereVertexBufferPtr->Color = material.Albedo;
                        // The face normal only -- NOT a per-vertex smoothed normal. A subdivided cube
                        // keeps hard edges between faces, which is correct for a cube and why the classic
                        // 24-vertex path stores four identical normals per face.
                        s_Data.SphereVertexBufferPtr->Normal = glm::normalize(normalMatrix * ln);
                        // The continuous grid UV, not the four-corner pattern the classic path uses. So
                        // switching between Disp == 0 and Disp > 0 changes how a texture maps onto the
                        // same cube -- the two paths are visually similar but not identical.
                        s_Data.SphereVertexBufferPtr->TexCoord = glm::vec2(u, v);
                        s_Data.SphereVertexBufferPtr->TexIndex = (float)cubeTextureSlot;
                        s_Data.SphereVertexBufferPtr->TilingFactor = 1.0f;
                        s_Data.SphereVertexBufferPtr->EntityID = entityID;
                        s_Data.SphereVertexBufferPtr->Metallic = material.Metallic;
                        s_Data.SphereVertexBufferPtr->Roughness = material.Roughness;
                        s_Data.SphereVertexBufferPtr->Displacement = reliefDisp;
                        s_Data.SphereVertexBufferPtr->BumpStrength = reliefBump;
                        s_Data.SphereVertexBufferPtr++;
                    }
                }
                for (int j = 0; j < gridN; j++)
                {
                    for (int i = 0; i < gridN; i++)
                    {
                        // Row-major index into the face's (N+1)^2 grid. b, c and d below are its right
                        // neighbour, its bottom neighbour, and the diagonal, so two triangles cover the
                        // cell with d repeated as the shared corner.
                        uint32_t a = faceBase + (uint32_t)(j * (gridN + 1) + i);
                        // The same a/b/c/d corner scheme used by the cube's relief grid, with b and c
                        // swapped relative to the cube because this grid lies in the xz plane rather than
                        // a face plane. Swapping them is what flips the winding to face +y.
                        uint32_t b = a + 1;
                        uint32_t c = a + (uint32_t)(gridN + 1);
                        uint32_t d = c + 1;
                        *s_Data.SphereIndexBufferPtr++ = a;
                        *s_Data.SphereIndexBufferPtr++ = b;
                        // d appears TWICE across the two triangles, as the shared corner. Six indices per
                        // cell, so the last index written is always the cell's own a.
                        *s_Data.SphereIndexBufferPtr++ = d;
                        *s_Data.SphereIndexBufferPtr++ = d;
                        // First triangle, c first -- the reverse of the cube grid's a,b,d order. Together
                        // with the d,b,a second triangle this makes both wind counter-clockwise seen from
                        // above, so an upward-facing normal is front-facing under the default CCW setting.
                        *s_Data.SphereIndexBufferPtr++ = c;
                        *s_Data.SphereIndexBufferPtr++ = a;
                    }
                }
            }
            // Counters advanced AFTER the write loop rather than inside it, so the capacity
            // check above could not be invalidated by the writes themselves.
            s_Data.SphereVertexCount += needVerts;
            // Counters and stats advanced after the emit loop, matching DrawCube and
            // DrawSphere. needIdx/6 for QuadCount is exact here because a water grid has no
            // degenerate cells.
            s_Data.SphereIndexCount += needIdx;
            s_Data.Stats.QuadCount += needIdx / 6;
        }

        if (s_Data.RayTracingEnabled)
        {
            RayTracingInstance instance = {};

            instance.WorldTransform = transform;
            instance.InvTransform = invTransform;
            instance.Albedo = material.Albedo;
            // Shape 0 is the cube, and .w is 0 because the analytic cube needs no radius. The
            // procedural shapes above put their shapeParam here instead.
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 0.0f, 0.0f);
            // A fixed unit cube centred on the origin, with w = 1 -- the constant the
            // procedural path replaces with a real second dimension. Defeated here by the
            // relief padding applied to the cull box above, not by these bounds.
            // The unit cube bounds, consistent with the shape-0 cube even though the transform
            // above scales it into a slab. The shape id in MaterialParams.z is 0 for the same
            // reason: HitCube is what the shader dispatches to.
            instance.Min = glm::vec4(-0.5f, -0.5f, -0.5f, 1.0f);
            instance.Max = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
            instance.Emission = glm::vec4(material.EmissionColor.x,
                                          material.EmissionColor.y,
                                          material.EmissionColor.z,
                                          material.EmissionIntensity);

            int textureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : -1;
            glm::vec3 worldScale = GetWorldScale(transform);

            instance.TextureID = textureSlot;
            instance.PackedMaterialMapID = -1;
            // UV scale per world axis, from the transform's basis lengths. DEFECT: .w
            // duplicates .z rather than carrying a distinct value, so the fourth component
            // is redundant -- harmless only if the shader ignores it.
            instance.TextureScale = glm::vec4(worldScale.x, worldScale.y, worldScale.z, worldScale.z);
            float bumpStrength = material.NormalStrength > 0.0f ? material.NormalStrength : (material.Texture ? 1.0f : 0.0f);
            instance.DisplacementParams = glm::vec4(material.DisplacementScale, bumpStrength, 0.0f, 0.0f);

            instance.MaxDistance = Renderer3DData::MaxDrawDistance;
            instance.LODLevel = 0;

            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    // The only procedurally tessellated primitive. Unlike the cube there are no
    // static tables, so sector and stack counts are chosen at draw time.
    void Renderer3D::DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, MaterialComponent &material, int entityID)
    {
        // Hard clamp on user-editable tessellation. Without this, large Sectors/Stacks
        // from the editor UI overflow the staging buffers even after a mid-frame flush.
        sectors = std::clamp(sectors, 1, 64);
        stacks = std::clamp(stacks, 1, 64);

        // Neural geometric displacement needs real vertices: raise the tessellation
        // floor while DisplacementScale is set (still capped; opt-in via the slider).
        float sphDisp = material.Texture ? material.DisplacementScale : 0.0f;
        if (sphDisp > 0.001f)
        {
            int target = 12 + (int)(sphDisp * 40.0f);
            // The caller's tessellation is treated as a FLOOR, not a choice: displacement
            // needs real vertices, so the count is raised if too low. Capped at 48/32 to
            // bound vertex cost, since a slider drag could otherwise request thousands.
            sectors = std::min(std::max(sectors, target), 48);
            stacks = std::min(std::max(stacks, (target * 2) / 3), 32);
        }
        // Same displacement-aware padding as the cube, around the real radius rather than
        // the unit half-extent.
        if (!IsAABBInFrustum(s_Data.FrustumPlanes, glm::vec3(-radius - sphDisp), glm::vec3(radius + sphDisp), transform))
            return;

        // The +1 on both axes: latitude rows and longitude columns each share their poles
        // and seam, so a grid of N bands needs N+1 rows.
        uint32_t vertexCount = (stacks + 1) * (sectors + 1);
        uint32_t indexCount = 0;
        for (int i = 0; i < stacks; ++i)
        {
            if (i != 0)
                // Two triangles per sector, 3 indices each. This increments the row's FULL share;
                // the `if` on either side of it drops the degenerate polar band -- at the north
                // pole or the south respectively -- because a whole ring of quads there would
                // collapse onto zero area. Those two guards are why the count is computed rather
                // than assumed to be stacks * sectors * 6.
                // Never end a // comment with a backslash: translation phase 2 splices the
                // next physical line into this comment before comments are removed, which
                // silently deleted the increment below and halved the count.
                indexCount += sectors * 3;
            if (i != (stacks - 1))
                indexCount += sectors * 3;
        }

        // Both counters, because the vertex and index buffers are separate allocations
        // with independent limits. Either overflowing triggers the same mid-frame flush,
        // which is why a sphere with a very high tessellation can flush twice in one
        // frame alongside a cube.
        if (s_Data.SphereIndexCount + indexCount >= s_Data.MaxIndices || s_Data.SphereVertexCount + vertexCount >= s_Data.MaxVertices)
        {
            Flush();
            FlushAndReset();
        }

        uint32_t startIndexOffset = s_Data.SphereVertexCount;
        // Raster-path material inputs (ray-trace instance below reuses the map lookup)
        int sphereTextureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : 0;
        // Radians per longitude step, and a HALF circle per stack rather than a full one.
        // Both divisions are safe here because the capacity flush above guarantees
        // sectors and stacks were clamped to at least 1 by the std::min/std::max above.
        float sectorStep = 2 * glm::pi<float>() / sectors;
        float stackStep = glm::pi<float>() / stacks;
        glm::mat4 invTransform = FastTRSInverse(transform);
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));

        // Inclusive on both ends: rows 0 and stacks are the poles, and the inner loop is
        // inclusive too so the longitude seam is duplicated -- without that seam vertex
        // the UVs would have to wrap mid-triangle.
        for (int i = 0; i <= stacks; ++i)
        {
            // Latitude from +pi/2 downward, so i == 0 is the top pole. Descending rather than
            // ascending matches the winding the index loop below expects for outward normals.
            float stackAngle = glm::pi<float>() / 2 - i * stackStep;
            // The ring radius at this latitude, which collapses to zero at both poles. That
            // collapse is what makes the polar quads degenerate, and why the index guards
            // exist -- the VERTICES are still emitted, so the count must include them.
            float xy = radius * cosf(stackAngle);
            // The height component. Note the sphere is built around the Y axis (x/y ring, z
            // up) while the other primitives use Y up -- an inconsistency that only matters
            // because the analytic HitSphere in the shader ignores this mesh entirely.
            float z = radius * sinf(stackAngle);

            for (int j = 0; j <= sectors; ++j)
            {
                float sectorAngle = j * sectorStep;
                float x = xy * cosf(sectorAngle);
                float y = xy * sinf(sectorAngle);

                s_Data.SphereVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(x, y, z, 1.0f));
                s_Data.SphereVertexBufferPtr->Color = material.Albedo;
                // DOUBLE normalize: the inner one takes the local position to a unit direction,
                // the outer applies the normal matrix and re-normalizes to undo its scale. The
                // inner normalize is what keeps a non-uniformly scaled sphere's normals correct.
                s_Data.SphereVertexBufferPtr->Normal = glm::normalize(normalMatrix * glm::normalize(glm::vec3(x, y, z)));
                // u runs 0..1 around the seam, v 0..1 pole to pole. Note i/sacks reaches exactly
                // 1.0 at the last row, while the vertex above it degenerates to a point -- so the
                // texture pinches at the pole, the usual cost of a UV sphere.
                s_Data.SphereVertexBufferPtr->TexCoord = glm::vec2((float)j / sectors, (float)i / stacks);
                s_Data.SphereVertexBufferPtr->TexIndex = (float)sphereTextureSlot;
                s_Data.SphereVertexBufferPtr->TilingFactor = 1.0f;
                s_Data.SphereVertexBufferPtr->EntityID = entityID;
                s_Data.SphereVertexBufferPtr->Metallic = material.Metallic;
                s_Data.SphereVertexBufferPtr->Roughness = material.Roughness;
                s_Data.SphereVertexBufferPtr->Displacement = material.Texture ? material.DisplacementScale : 0.0f;
                s_Data.SphereVertexBufferPtr->BumpStrength = material.Texture ? material.NormalStrength : 0.0f;
                s_Data.SphereVertexBufferPtr++;
            }
        }

        for (int i = 0; i < stacks; ++i)
        {
            // The row's first vertex, already offset by the batch base. k2 is the row BELOW,
            // one full (sectors+1) later, because the grid is row-major.
            uint32_t k1 = i * (sectors + 1) + startIndexOffset;
            uint32_t k2 = k1 + sectors + 1;

            // Both counters advance in the loop header, so each iteration walks one column
            // right along each of the two rows being joined.
            for (int j = 0; j < sectors; ++j, ++k1, ++k2)
            {
                if (i != 0)
                {
                    *s_Data.SphereIndexBufferPtr++ = k1;
                    *s_Data.SphereIndexBufferPtr++ = k2;
                    // The first triangle of the upper band, k1 -> k2 -> k1+1: top row, the row below
                    // it, then the top row's next column. Verified outward-facing: with the ring in
                    // the local xy plane and z as the polar axis, the cross product of (k2 - k1) and
                    // (k1+1 - k1) has a positive radial component, i.e. front faces point away from
                    // the centre.
                    *s_Data.SphereIndexBufferPtr++ = k1 + 1;
                }
                if (i != (stacks - 1))
                {
                    *s_Data.SphereIndexBufferPtr++ = k1 + 1;
                    *s_Data.SphereIndexBufferPtr++ = k2;
                    *s_Data.SphereIndexBufferPtr++ = k2 + 1;
                }
            }
        }

        s_Data.SphereVertexCount += vertexCount;
        s_Data.SphereIndexCount += indexCount;
        s_Data.Stats.QuadCount += (indexCount / 6);

        if (s_Data.RayTracingEnabled)
        {
            RayTracingInstance instance = {};

            instance.WorldTransform = transform;
            instance.InvTransform = invTransform;
            instance.Albedo = material.Albedo;
            // Shape 1 (sphere) in .z and the radius in .w -- the two slots the procedural
            // shapes above also use for their shape id and primary dimension. HitSphere
            // therefore never needs the local bounds to size itself.
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 1.0f, radius);
            // A radius-sized cube AABB, correct because a sphere is the cube's inscribed
            // shape. Bounds are in LOCAL space, like every other instance in this file.
            instance.Min = glm::vec4(-radius, -radius, -radius, 1.0f);
            instance.Max = glm::vec4(radius, radius, radius, 1.0f);
            instance.Emission = glm::vec4(material.EmissionColor.x,
                                          material.EmissionColor.y,
                                          material.EmissionColor.z,
                                          material.EmissionIntensity);

            int textureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : -1;
            glm::vec3 worldScale = GetWorldScale(transform);
            // The MEAN of the three axis scales, matching the procedural path's avgScale.
            // DEFECT: for a non-uniformly scaled sphere this misestimates the world radius,
            // so the UV tiling below is computed from a radius that is not the real one.
            float worldRadius = radius * (worldScale.x + worldScale.y + worldScale.z) / 3.0f;

            instance.TextureID = textureSlot;
            instance.PackedMaterialMapID = -1;
            // A full CIRCUMFERENCE as the U tiling scale and half that as V (the two values
            // on the lines below). That 2:1 ratio is the usual equirectangular convention: a
            // sphere needs it to avoid stretching at the equator. The trailing zeros are
            // unused for a sphere.
            instance.TextureScale = glm::vec4(
                2.0f * glm::pi<float>() * worldRadius,
                glm::pi<float>() * worldRadius,
                0.0f,
                0.0f);
            float bumpStrength = material.NormalStrength > 0.0f ? material.NormalStrength : (material.Texture ? 1.0f : 0.0f);
            instance.DisplacementParams = glm::vec4(material.DisplacementScale, bumpStrength, 0.0f, 0.0f);

            instance.MaxDistance = Renderer3DData::MaxDrawDistance;
            instance.LODLevel = 0;

            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    // The heaviest CPU path in the renderer: an N x N grid with a full wave sum
    // evaluated per vertex, twice (once for the surface, once for the crest factor).
    // Segment count is the dominant cost control.
    void Renderer3D::DrawWaterPlane(const glm::mat4 &transform, const WaterComponent &water, int entityID)
    {
        if (!water.Enabled)
            return;

        // Clamped to 1..64 below. DEFECT (already noted in the code): the UI slider
        // permits 128, so a user who drags it past 64 gets half the mesh they asked for
        // with no feedback that the request was clipped.
        int N = water.Segments;
        if (N < 1)
            N = 1;
        // A hard CPU and batch-size cap, separate from any buffer capacity check. At 64 a
        // single water plane is 65*65 = 4225 vertices and 24576 indices, so several
        // water surfaces would already dominate the vertex budget.
        if (N > 64)
            N = 64; // batch + CPU cost cap (UI allows up to 128, clamped here)

        float maxH = water.MaxWaveHeight();
        glm::vec3 worldScale = GetWorldScale(transform);
        float scaleY = glm::max(worldScale.y, 0.001f);
        // Cull padding converted into LOCAL units by dividing through the transform's Y
        // scale, then +0.05 to cover the flat surface itself. The 0.001 floor on scaleY
        // keeps a degenerate (zero-height) transform from producing an infinite pad that
        // would disable culling.
        float padLocalY = maxH / scaleY + 0.05f;

        if (!IsAABBInFrustum(s_Data.FrustumPlanes, glm::vec3(-0.5f, -padLocalY, -0.5f), glm::vec3(0.5f, padLocalY, 0.5f), transform))
            return;

        uint32_t side = (uint32_t)(N + 1);
        uint32_t needVerts = side * side;
        // Two triangles per cell. This is an EXACT figure -- unlike the sphere, a water
        // grid has no degenerate polar cells, so no guards are needed.
        uint32_t needIdx = (uint32_t)(N * N * 6);
        if (s_Data.SphereVertexCount + needVerts >= s_Data.MaxVertices ||
            s_Data.SphereIndexCount + needIdx >= s_Data.MaxIndices)
        {
            Flush();
            FlushAndReset();
        }

        glm::mat4 invTransform = FastTRSInverse(transform);
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));

        float foamAmt = glm::clamp(water.FoamAmount, 0.0f, 1.0f);
        float shoreAmt = glm::clamp(water.ShoreFoam, 0.0f, 1.0f);
        float shoreWidth = glm::max(water.ShoreWidth, 0.001f);
        float rough = glm::clamp(water.Roughness, 0.0f, 1.0f);
        float metal = glm::clamp(water.Metallic, 0.0f, 1.0f);
        float clarity = glm::clamp(water.Transparency, 0.0f, 1.0f);

        // Spectral LOD: don't feed the mesh spectrum it can't resolve.
        // Gameplay queries keep the full field (minWavelength = 0).
        // The larger of the two footprint axes, so cellSize is the worst-case cell and the
        // LOD cut below is conservative rather than optimistic.
        float cellSize = glm::max(worldScale.x, worldScale.z) / (float)N;
        // Spectral LOD: waves shorter than two cells cannot be resolved by the mesh, so
        // EvaluateSurface drops them. This is why raising Segments visibly changes the
        // water -- more segments admit shorter waves.
        float minWavelength = cellSize * 2.0f;
        glm::vec2 flowVel = water.SampleFlow();
        float flowMag = glm::length(flowVel);

        uint32_t batchBase = s_Data.SphereVertexCount;
        // Inclusive, so the grid spans exactly u,v in 0..1 with N+1 samples per axis --
        // matching side*side from the capacity check above.
        for (int j = 0; j <= N; j++)
        {
            for (int i = 0; i <= N; i++)
            {
                float u = (float)i / (float)N;
                float v = (float)j / (float)N;
                // UV to local unit space: -0.5..0.5 on both footprint axes. The plane's local
                // extent is therefore exactly 1x1 before the transform, which is why the cull box
                // above uses +/-0.5.
                float lx = u - 0.5f;
                float lz = v - 0.5f;

                // Transformed to WORLD space before evaluating the waves, because EvaluateSurface
                // is a world-space query -- the wave field does not move with the entity, so
                // evaluating in local space would make the waves swim as the plane moves.
                glm::vec4 wp4 = transform * glm::vec4(lx, 0.0f, lz, 1.0f);
                // Outputs rather than returns, because a single evaluation yields height, the two
                // Gerstner horizontal displacements, and the analytic normal in one pass.
                float h = 0.0f, dispX = 0.0f, dispZ = 0.0f;
                // A safe UP default so the variable is always initialized. A water surface with
                // no displacement would otherwise feed a default-constructed (possibly
                // zero-length) normal into the normalize below.
                glm::vec3 simN(0.0f, 1.0f, 0.0f);
                water.EvaluateSurface(wp4.x, wp4.z, minWavelength, h, dispX, dispZ, simN);
                // Gerstner horizontal chop: vertices bunch toward crests.
                // (Culling above conservatively covers this band via MaxWaveHeight.)
                glm::vec3 worldPos = glm::vec3(wp4.x + dispX, wp4.y + h, wp4.z + dispZ);

                glm::vec3 worldN = glm::normalize(normalMatrix * simN);

                // A SECOND, cheaper wave evaluation -- the expensive EvaluateSurface above already
                // produced the normal and displacements. This one asks only how close to a crest
                // this point is, and it drives the foam mask and the depth tint. So the wave sum
                // is effectively evaluated twice per vertex.
                float crest = water.CrestFactorAt(wp4.x, wp4.z, minWavelength);

                // Whitecaps: crest-driven, streaked along the current so rivers
                // read as flowing instead of boiling. Detail pattern rides the
                // same advection as the waves, so foam sticks to crests.
                // Kept broad (multi-meter): fine sparkle lives in the fragment
                // ripple stage; vertex-level high frequencies would alias into
                // quilting at typical cell sizes.
                // Flow-relative advection: subtracting the integrated flow displacement is what
                // makes foam STREAK along the current instead of sitting still. Advecting by
                // position rather than by time means a change in flow speed moves the pattern
                // smoothly rather than snapping.
                glm::vec2 fp = glm::vec2(wp4.x, wp4.z) - flowVel * water.Time;
                // FoamScale survives here as a mild breakup-scale multiplier;
                // clamped so the pattern stays LOD-safe (fine sparkle lives
                // in the fragment ripple stage, which can't see uniforms).
                // FoamScale is mapped into 0.75..1.25 rather than used directly, so the artist
                // control modulates the breakup scale without ever driving the vertex-frequency
                // pattern into the aliasing range the comment above warns about.
                float fs = 0.75f + 0.25f * glm::clamp(water.FoamScale, 0.01f, 4.0f);
                float detail = 0.5f + 0.5f * sinf(fp.x * (0.9f * fs) + water.Time * 1.1f) *
                                                 sinf((fp.x * 0.3f + fp.y) * (0.8f * fs) - water.Time * 0.9f);
                // Initialized before both contributors below. The whitecap branch can leave it at
                // zero, and the shoreline branch then MAXes into it rather than overwriting, so
                // the two foam sources combine.
                float foamMask = 0.0f;
                if (foamAmt > 0.001f)
                {
                    // The whitecap threshold, lowered as foam amount rises so more of the crest range
                    // qualifies. Never reaches 0, which would make every vertex foam.
                    float edge = 1.0f - foamAmt * 0.55f;
                    foamMask = glm::clamp((crest * (0.7f + 0.6f * detail) - edge) / glm::max(1.0f - edge, 0.001f), 0.0f, 1.0f);
                }

                // Shoreline foamline: distance from the footprint rect edge in
                // world units. Agitated by flow + surf so river banks foam and
                // lake edges lap gently.
                if (shoreAmt > 0.001f)
                {
                    // Chebyshev distance to the footprint's edge in UV space, 0 at the boundary and
                    // 0.5 at the centre. Computed in UV rather than per-triangle edge, so a
                    // subdivided grid has no per-edge seam in the foamline.
                    float edgeUV = glm::min(glm::min(u, 1.0f - u), glm::min(v, 1.0f - v));
                    float edgeWorld = edgeUV * glm::min(worldScale.x, worldScale.z);
                    float shoreBand = 1.0f - glm::clamp(edgeWorld / shoreWidth, 0.0f, 1.0f);
                    float agitation = glm::clamp(0.3f + flowMag * 0.4f + crest * 0.5f, 0.0f, 1.0f);
                    float lap = 0.65f + 0.35f * sinf(water.Time * 1.9f + (edgeWorld / shoreWidth) * 4.0f);
                    // MAX, not sum: the two foam sources should not exceed 1 where they overlap, and
                    // a summed mask would need a clamp afterwards. shoreBand is SQUARED so the
                    // foamline falls off sharply rather than fading linearly to the centre.
                    foamMask = glm::max(foamMask, shoreAmt * shoreBand * shoreBand * agitation * lap);
                }

                glm::vec3 base = glm::mix(water.DeepColor, water.ShallowColor, glm::clamp(0.35f + 0.45f * crest, 0.0f, 1.0f));
                // Clarity can only tint 25% of the way toward shallow colour. The raster path is
                // opaque, so transparency is faked as a colour shift rather than real alpha.
                base = glm::mix(base, water.ShallowColor, clarity * 0.25f); // opaque pipeline: clarity tints toward shallow
                base = glm::mix(base, water.FoamColor, glm::clamp(foamMask, 0.0f, 1.0f));

                // Foam is matte: lift roughness with coverage so whitecaps and
                // foamlines don't glint like the water around them.
                // Roughness is lifted per vertex with the foam coverage, so whitecaps do not
                // glint. Doing it here rather than in the fragment shader is why foam cannot
                // change roughness on the Nova path.
                float vertRough = glm::mix(rough, 0.85f, glm::clamp(foamMask, 0.0f, 1.0f));

                s_Data.SphereVertexBufferPtr->Position = worldPos;
                s_Data.SphereVertexBufferPtr->Color = glm::vec4(base, 1.0f);
                s_Data.SphereVertexBufferPtr->Normal = worldN;
                s_Data.SphereVertexBufferPtr->TexCoord = glm::vec2(u, v);
                // Slot 0 -- the white fallback -- unconditionally. Water has no albedo texture, so
                // all its colour arrives through the Color field above.
                s_Data.SphereVertexBufferPtr->TexIndex = 0.0f;
                s_Data.SphereVertexBufferPtr->TilingFactor = 1.0f;
                s_Data.SphereVertexBufferPtr->EntityID = entityID;
                s_Data.SphereVertexBufferPtr->Metallic = metal;
                s_Data.SphereVertexBufferPtr->Roughness = vertRough;
                s_Data.SphereVertexBufferPtr->Displacement = 0.0f;
                // Water marker for the Basic raster shader: negative bump
                // routes the fragment through the mirror + pixel-ripple path
                // (the relief gate needs values > 0.001, so this opts out).
                // The NEGATIVE sentinel that routes this vertex through the mirror and pixel-
                // ripple fragment path. It works because the relief gate tests for values above
                // 0.001, so -1 opts out of POM rather than enabling a tiny amount of it.
                s_Data.SphereVertexBufferPtr->BumpStrength = -1.0f;
                s_Data.SphereVertexBufferPtr++;
            }
        }
        for (int j = 0; j < N; j++)
        {
            for (int i = 0; i < N; i++)
            {
                // Same row-major a/b/c/d scheme as the sphere, but the winding is REVERSED (a, c,
                // d and then d, b, a) because this grid lies in the local xz plane with +y up,
                // the opposite handedness from the sphere's xy plane.
                uint32_t a = batchBase + (uint32_t)(j * side + i);
                uint32_t b = a + 1;
                uint32_t c = a + side;
                uint32_t d = c + 1;
                *s_Data.SphereIndexBufferPtr++ = a;
                *s_Data.SphereIndexBufferPtr++ = c;
                *s_Data.SphereIndexBufferPtr++ = d;
                *s_Data.SphereIndexBufferPtr++ = d;
                *s_Data.SphereIndexBufferPtr++ = b;
                *s_Data.SphereIndexBufferPtr++ = a;
            }
        }
        s_Data.SphereVertexCount += needVerts;
        s_Data.SphereIndexCount += needIdx;
        s_Data.Stats.QuadCount += needIdx / 6;

        if (s_Data.RayTracingEnabled)
        {
            // Nova path-tracer: flat thin-box instance with water material.
            // (Wave displacement is raster-only; the box sits at still level
            // with thickness covering the wave band so it never z-fights.)
            RayTracingInstance instance = {};
            float thick = glm::max(maxH * 2.0f, 0.05f);
            glm::mat4 boxLocal = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, thick / glm::max(worldScale.y, 0.001f), 1.0f));
            // The ray-traced water is a separate, FLAT thin box: it does not reuse the
            // displaced raster mesh at all. So a wave crest is invisible to the path tracer,
            // and the box is thickened by 2*maxH purely to enclose where crests would be.
            instance.WorldTransform = transform * boxLocal;
            instance.InvTransform = FastTRSInverse(instance.WorldTransform);
            glm::vec3 flatAlbedo = glm::mix(water.DeepColor, water.ShallowColor, 0.45f + clarity * 0.15f);
            // A FLAT albedo, a single mix of deep and shallow colour at a fixed ratio -- no
            // crest variation, unlike the raster path. So the traced water reads as an even
            // tone where the raster water has visible whitecaps.
            instance.Albedo = glm::vec4(flatAlbedo, 1.0f);
            // Mirror path (matches the raster water branch): F0 picks up the
            // water tint and the env reflection takes over.
            // Steepness rides in MaterialParams.w (.z must stay 0 = cube shape).
            instance.MaterialParams = glm::vec4(1.0f, rough, 0.0f, water.Steepness);
            instance.Min = glm::vec4(-0.5f, -0.5f, -0.5f, 1.0f);
            instance.Max = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
            instance.Emission = glm::vec4(0.0f);
            instance.TextureID = -1;
            instance.PackedMaterialMapID = -1;
            // Nova water packing (water has no albedo texture, so TextureScale
            // is free): xy = normalized swell direction, zw = flow velocity
            // (world units/sec, matches the CPU AdvectedPos). The shader reads
            // these for the Gerstner spectrum, ripple detail, and foam drift.
            glm::vec2 wDir = water.WaveDirection;
            if (glm::length2(wDir) < 1e-8f)
                wDir = glm::vec2(1.0f, 0.3f);
            // Normalized HERE, unlike the raster path which keeps an unnormalized direction.
            // The value is packed raw into TextureScale.xy, so a zero-length input would
            // reach the shader as zero unless it were replaced above.
            wDir = glm::normalize(wDir);
            glm::vec2 wFlow = water.SampleFlow();
            // Four meanings in four slots, reusing a field that means UV scale for every other
            // material: x/y are the swell direction and z/w the flow velocity in world units
            // per second. Safe only because water sets TextureID = -1 and so never samples.
            instance.TextureScale = glm::vec4(wDir.x, wDir.y, wFlow.x, wFlow.y);
            // Water marker for the Nova path-tracer: z = 1 flags the water
            // branch, w carries max crest height (world units) for
            // absolute-height foam. x carries foam amount (ApplyPOM early-outs
            // on TextureID == -1, so the relief march never runs on water).
            // Static per surface (no BVH churn).
            instance.DisplacementParams = glm::vec4(glm::clamp(water.FoamAmount, 0.0f, 1.0f), 0.0f, 1.0f, glm::max(maxH, 0.05f));
            // WaveParams: x=amplitude, y=wavelength, z=speed, w=chop.
            instance.WaveParams = glm::vec4(water.WaveAmplitude, water.WaveLength, water.WaveSpeed, water.Chop);
            instance.MaxDistance = Renderer3DData::MaxDrawDistance;
            instance.LODLevel = 0;
            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
        }
    }

    // A pass-through to the base Renderer, scoped with a profiling marker. Nothing in
    // this file's own draw paths uses it -- they all go through the batching above.
    void Renderer3D::Submit(const Ref<Shader> &shader, const Ref<VertexArray> &vertexArray, const glm::mat4 &transform)
    {
        WL_PROFILE_FUNCTION();
        Renderer::Submit(shader, vertexArray, transform);
    }

    void Renderer3D::DrawTerrainChunk(const Terrain::TerrainChunkMesh &mesh)
    {
        if (!s_Nova12 || !s_Data.RayTracingEnabled)
            return;

        TerrainNovaChunkGeometry geometry;
        std::string error;
        if (!BuildTerrainNovaChunkGeometry(mesh, geometry, &error))
        {
            WL_CORE_WARN("Skipping invalid terrain chunk for DX12 Nova: {0}", error);
            return;
        }

        const size_t vertexOffset = s_Data.m_TerrainVertices.size();
        const size_t maxInstanceValue = static_cast<size_t>((std::numeric_limits<int>::max)());
        if (vertexOffset > maxInstanceValue || geometry.Vertices.size() > maxInstanceValue - vertexOffset ||
            geometry.CellsPerSide > static_cast<uint32_t>((std::numeric_limits<int>::max)()) ||
            geometry.LodLevel > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
        {
            WL_CORE_WARN("Skipping terrain chunk whose Nova vertex range or metadata exceeds the instance ABI");
            return;
        }

        s_Data.m_TerrainVertices.insert(s_Data.m_TerrainVertices.end(),
                                        geometry.Vertices.begin(), geometry.Vertices.end());

        RayTracingInstance instance = {};
        instance.InvTransform = geometry.InvTransform;
        instance.WorldTransform = geometry.WorldTransform;
        instance.Albedo = glm::vec4(0.34f, 0.25f, 0.14f, 1.0f);
        instance.MaterialParams = glm::vec4(0.0f, 0.9f, 8.0f, 0.0f);
        instance.Min = geometry.BoundsMin;
        instance.Max = geometry.BoundsMax;
        instance.Emission = glm::vec4(0.0f);
        instance.TextureID = -1;
        instance.PackedMaterialMapID = -1;
        instance.TextureScale = glm::vec4(1.0f);
        instance.DisplacementParams = glm::vec4(0.0f);
        instance.WaveParams = glm::vec4(0.0f);
        instance.MaxDistance = geometry.MaxDistance;
        instance.LODLevel = static_cast<int>(geometry.LodLevel);
        instance.TerrainVertexOffset = static_cast<int>(vertexOffset);
        instance.TerrainCellsPerSide = static_cast<int>(geometry.CellsPerSide);
        instance.TerrainPad0 = 0;
        instance.TerrainPad1 = 0;

        s_Data.m_SceneInstances.push_back(instance);
        s_Data.m_TerrainInstances.push_back(instance);
        s_Data.m_SceneDirty = true;
    }
}