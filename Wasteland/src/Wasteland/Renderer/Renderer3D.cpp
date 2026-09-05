#include "wlpch.h"
#include "Renderer3D.h"

#include "VertexArray.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "Renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>

#include <chrono>

namespace Wasteland
{
    struct Vertex3D
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
        float TexIndex;
        float TilingFactor;
        int EntityID;
        float Metallic;
        float Roughness;
    };

    struct alignas(16) RayTracingInstance
    {
        glm::mat4 InvTransform;
        glm::mat4 WorldTransform;
        glm::vec4 Albedo;
        glm::vec4 MaterialParams;
        glm::vec4 Min;
        glm::vec4 Max;
        glm::vec4 Emission;
        float MaxDistance;
        int LODLevel;
        int TextureID;
        int PackedMaterialMapID;
        glm::vec4 TextureScale;
        glm::vec4 DisplacementParams;
    };

    // BVH node layout for GPU (std430 SSBO, 32 bytes per node)
    struct alignas(16) BVHNodeGPU
    {
        glm::vec4 MinBounds; // xyz = AABB min, w = leftChild (inner) or firstInstance (leaf)
        glm::vec4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
    };

    // CPU-side BVH build helper
    struct BVHBuildNode
    {
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        int leftChild;     // -1 for leaf
        int rightChild;    // -1 for leaf
        int firstInstance; // leaf only
        int instanceCount; // leaf only
    };

    struct BVHPrim
    {
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
        float Density = 0.0f;
        float Anisotropy = 0.0f;
        float NoiseStrength = 0.0f;
        float NoiseScale = 0.25f;
        float WindSpeed = 0.0f;
        float HeightFalloff = 0.0f;
        int Steps = 12;
        bool Enabled = true;
    };

    struct CloudVolumeData
    {
        glm::vec3 Min;
        glm::vec3 Max;
        glm::vec3 Color;
        glm::vec3 Ambient;
        float Coverage = 0.5f;
        float Density = 0.5f;
        float NoiseScale = 0.08f;
        float Detail = 0.5f;
        glm::vec2 WindDir = {1.0f, 0.0f};
        float WindSpeed = 0.0f;
        float SilverLining = 0.5f;
        float ShadowStrength = 0.7f;
        int Steps = 16;
        bool Enabled = true;
    };

    // World-space AABB of a unit-cube volume transformed by an entity matrix
    // (rotation is conservatively absorbed, same approach as the BVH builder).
    static void ComputeVolumeBounds(const glm::mat4 &transform, glm::vec3 &outMin, glm::vec3 &outMax)
    {
        glm::vec3 center = glm::vec3(transform[3]);
        glm::vec3 axisX = glm::abs(glm::vec3(transform[0])) * 0.5f;
        glm::vec3 axisY = glm::abs(glm::vec3(transform[1])) * 0.5f;
        glm::vec3 axisZ = glm::abs(glm::vec3(transform[2])) * 0.5f;
        outMin = center - axisX - axisY - axisZ;
        outMax = center + axisX + axisY + axisZ;
    }

    // Seconds since first call — drives wind-animated volume noise.
    static float GetVolumetricTimeSeconds()
    {
        static auto s_Start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - s_Start).count();
    }

    static const int BVH_LEAF_SIZE = 4;

    static glm::vec3 ComputeWorldAABBMin(const RayTracingInstance &inst)
    {
        glm::vec3 center = glm::vec3(inst.WorldTransform[3]);
        glm::vec3 halfExtents = (glm::vec3(inst.Max) - glm::vec3(inst.Min)) * 0.5f;
        glm::vec3 axisX = glm::abs(glm::vec3(inst.WorldTransform[0])) * halfExtents.x;
        glm::vec3 axisY = glm::abs(glm::vec3(inst.WorldTransform[1])) * halfExtents.y;
        glm::vec3 axisZ = glm::abs(glm::vec3(inst.WorldTransform[2])) * halfExtents.z;
        return center - axisX - axisY - axisZ;
    }

    static glm::vec3 ComputeWorldAABBMax(const RayTracingInstance &inst)
    {
        glm::vec3 center = glm::vec3(inst.WorldTransform[3]);
        glm::vec3 halfExtents = (glm::vec3(inst.Max) - glm::vec3(inst.Min)) * 0.5f;
        glm::vec3 axisX = glm::abs(glm::vec3(inst.WorldTransform[0])) * halfExtents.x;
        glm::vec3 axisY = glm::abs(glm::vec3(inst.WorldTransform[1])) * halfExtents.y;
        glm::vec3 axisZ = glm::abs(glm::vec3(inst.WorldTransform[2])) * halfExtents.z;
        return center + axisX + axisY + axisZ;
    }

    static int BuildBVHRecursive(std::vector<BVHBuildNode> &nodes,
                                 std::vector<BVHPrim> &prims,
                                 int start, int end)
    {
        int nodeIdx = (int)nodes.size();
        nodes.push_back({});

        // Compute bounds for this node
        glm::vec3 nodeMin(1e30f);
        glm::vec3 nodeMax(-1e30f);
        for (int i = start; i < end; i++)
        {
            nodeMin = glm::min(nodeMin, prims[i].minBounds);
            nodeMax = glm::max(nodeMax, prims[i].maxBounds);
        }

        int count = end - start;
        if (count <= BVH_LEAF_SIZE)
        {
            nodes[nodeIdx].minBounds = nodeMin;
            nodes[nodeIdx].maxBounds = nodeMax;
            nodes[nodeIdx].leftChild = -1;
            nodes[nodeIdx].rightChild = -1;
            nodes[nodeIdx].firstInstance = start;
            nodes[nodeIdx].instanceCount = count;
            return nodeIdx;
        }

        // Split on longest axis using median centroid
        glm::vec3 centroidMin(1e30f), centroidMax(-1e30f);
        for (int i = start; i < end; i++)
        {
            centroidMin = glm::min(centroidMin, prims[i].centroid);
            centroidMax = glm::max(centroidMax, prims[i].centroid);
        }
        glm::vec3 extent = centroidMax - centroidMin;
        int axis = 0;
        if (extent.y > extent.x)
            axis = 1;
        if (extent.z > extent[axis])
            axis = 2;

        int mid = (start + end) / 2;
        std::nth_element(prims.begin() + start, prims.begin() + mid, prims.begin() + end,
                         [axis](const BVHPrim &a, const BVHPrim &b)
                         {
                             return a.centroid[axis] < b.centroid[axis];
                         });

        nodes[nodeIdx].minBounds = nodeMin;
        nodes[nodeIdx].maxBounds = nodeMax;
        nodes[nodeIdx].leftChild = BuildBVHRecursive(nodes, prims, start, mid);
        nodes[nodeIdx].rightChild = BuildBVHRecursive(nodes, prims, mid, end);
        nodes[nodeIdx].firstInstance = 0;
        nodes[nodeIdx].instanceCount = 0;

        return nodeIdx;
    }

    static void BuildAndFlattenBVH(const std::vector<RayTracingInstance> &instances,
                                   std::vector<BVHNodeGPU> &outNodes,
                                   std::vector<int> &outPrimOrder)
    {
        outNodes.clear();
        outPrimOrder.clear();

        if (instances.empty())
            return;

        // Prepare primitives with world-space AABBs
        std::vector<BVHPrim> prims(instances.size());
        for (size_t i = 0; i < instances.size(); i++)
        {
            prims[i].minBounds = ComputeWorldAABBMin(instances[i]);
            prims[i].maxBounds = ComputeWorldAABBMax(instances[i]);
            prims[i].centroid = (prims[i].minBounds + prims[i].maxBounds) * 0.5f;
            prims[i].index = (int)i;
        }

        // Build tree
        std::vector<BVHBuildNode> buildNodes;
        buildNodes.reserve(instances.size() * 2);
        BuildBVHRecursive(buildNodes, prims, 0, (int)prims.size());

        // Flatten to GPU layout and compute reordered instance indices
        outNodes.resize(buildNodes.size());
        outPrimOrder.resize(prims.size());
        for (size_t i = 0; i < prims.size(); i++)
            outPrimOrder[i] = prims[i].index;

        for (size_t i = 0; i < buildNodes.size(); i++)
        {
            const auto &bn = buildNodes[i];
            if (bn.leftChild >= 0)
            {
                // Inner node: w = child indices
                outNodes[i].MinBounds = glm::vec4(bn.minBounds, (float)bn.leftChild);
                outNodes[i].MaxBounds = glm::vec4(bn.maxBounds, (float)bn.rightChild);
            }
            else
            {
                // Leaf node: encode firstInstance as negative to distinguish from inner
                outNodes[i].MinBounds = glm::vec4(bn.minBounds, (float)(-(bn.firstInstance + 1)));
                outNodes[i].MaxBounds = glm::vec4(bn.maxBounds, (float)bn.instanceCount);
            }
        }
    }

    struct Renderer3DData
    {
        struct Plane
        {
            glm::vec3 normal;
            float distance;
        };

        static const uint32_t MaxVertices = 100000;
        static const uint32_t MaxIndices = MaxVertices * 3;
        static const uint32_t MaxTextureSlots = 32;

        Ref<VertexArray> CubeVertexArray;
        Ref<VertexBuffer> CubeVertexBuffer;
        uint32_t CubeVertexCount = 0;
        uint32_t CubeIndexCount = 0;
        Vertex3D *CubeVertexBufferBase = nullptr;
        Vertex3D *CubeVertexBufferPtr = nullptr;

        Ref<VertexArray> SphereVertexArray;
        Ref<VertexBuffer> SphereVertexBuffer;
        Ref<IndexBuffer> SphereIndexBuffer;
        uint32_t SphereVertexCount = 0;
        uint32_t SphereIndexCount = 0;
        Vertex3D *SphereVertexBufferBase = nullptr;
        Vertex3D *SphereVertexBufferPtr = nullptr;
        uint32_t *SphereIndexBufferBase = nullptr;
        uint32_t *SphereIndexBufferPtr = nullptr;

        Ref<Shader> BasicShader;

        std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
        uint32_t TextureSlotIndex = 1;
        std::unordered_map<uint32_t, int> TextureSlotMap;

        bool RayTracingEnabled = true;
        bool RayTracingAccumulate = true;
        QualityPreset CurrentQualityPreset = QualityPreset::Low;
        uint32_t ComputeWidth = 1280;
        uint32_t ComputeHeight = 720;
        float RenderScale = 1.0f;
        int MaxBounces = 1;
        int MaxLights = 1;
        int IndirectRays = 0;
        bool BloomEnabled = false;
        bool BilateralBlurEnabled = false;
        int BilateralBlurPasses = 4;
        Ref<Shader> RayTracingShader;
        Ref<Texture2D> RayTracingOutput;

        uint32_t RayTracingTexture = 0;
        // FIX: Default dimensions to prevent 0x0 allocation on startup
        uint32_t RayTracingWidth = 1280;
        uint32_t RayTracingHeight = 720;

        // Path Tracing state
        uint32_t AccumulationTexture = 0;
        uint32_t FrameIndex = 0;
        bool CameraMoved = false;
        bool EditorCameraMoved = false;
        bool GameCameraMoved = false;
        bool ActiveCameraIsEditor = true;
        glm::vec3 LastEditorCameraPosition = glm::vec3(0.0f);
        float LastEditorCameraPitch = 0.0f;
        float LastEditorCameraYaw = 0.0f;
        glm::vec3 LastGameCameraPosition = glm::vec3(0.0f);
        float LastGameCameraPitch = 0.0f;
        float LastGameCameraYaw = 0.0f;
        int FrameIndexLocation;
        uint32_t SamplesPerPixel = 1;
        uint32_t StillFrames = 0;
        glm::mat4 LastViewProjection = glm::mat4(1.0f);

        uint32_t SceneInstanceBufferID = 0;
        uint32_t BVHBufferID = 0;
        uint32_t LightListBufferID = 0;
        std::vector<BVHNodeGPU> m_BVHNodes;
        std::vector<int> m_BVHPrimOrder;
        std::vector<RayTracingInstance> m_ReorderedInstances;
        std::vector<int> m_LightIndices;
        int InstanceCountLocation;
        int Loc_PassID;
        int Loc_SamplesPerPixel;
        int Loc_QualityLevel;
        int Loc_MaxBounces;
        int Loc_MaxLights;
        int Loc_IndirectRays;
        int Loc_CameraMoved;
        int Loc_SkyBottomColor;
        int Loc_SkyTopColor;
        int Loc_Jitter;
        int Loc_DepthBuffer;
        int Loc_AccumulationAlpha;
        int Loc_StepSize;
        int Loc_PrevViewProjection;
        int Loc_InverseViewProjection;
        int Loc_ViewProjection;
        int Loc_CameraPosition;
        int Loc_RenderScale;
        int Loc_LightCount;
        int Loc_NeuralEnabled;
        int Loc_NeuralTexStrength;
        int Loc_NeuralLightStrength;
        int Loc_NeuralMatStrength;
        // Volumetric uniform locations, indexed by VolLoc enum below.
        // [0] = Nova compute program, [1] = Basic raster program (-1 = optimized out).
        enum VolLoc
        {
            Vol_SunDirection = 0,
            Vol_Time,
            Vol_FogEnabled,
            Vol_FogCount,
            Vol_FogMin,
            Vol_FogMax,
            Vol_FogColor,
            Vol_FogData,
            Vol_FogData2,
            Vol_CloudEnabled,
            Vol_CloudCount,
            Vol_CloudMin,
            Vol_CloudMax,
            Vol_CloudColor,
            Vol_CloudAmbient,
            Vol_CloudData,
            Vol_CloudData2,
            Vol_CloudData3,
            Vol_VolFast,
            VolLoc_Count
        };
        int Loc_Volumetric[2][VolLoc_Count];
        std::vector<RayTracingInstance> m_SceneInstances;
        bool m_SceneDirty = true;

        float LastCameraRotation;
        float MovementThreshold = 0.0001f;

        glm::vec3 SkyBottomColor = glm::vec3(0.1f);
        glm::vec3 SkyTopColor = glm::vec3(0.2f, 0.3f, 0.7f);

        // Neural Rendering (OpenGL): tiny GLSL MLP for texture detail + indirect light cache
        bool NeuralEnabled = true;
        float NeuralTextureStrength = 0.6f;
        float NeuralLightStrength = 0.8f;
        float NeuralMaterialStrength = 0.7f;

        // Volumetric atmosphere: per-frame volume list submitted by the Scene
        // from VolumetricFog/VolumetricClouds components, raymarched in-shader.
        bool VolFogEnabled = true;
        bool VolCloudsEnabled = true;
        glm::vec3 SunDirection = glm::vec3(0.2873f, 0.9578f, 0.3831f); // normalize(0.3, 1.0, 0.4)
        float VolStepScale = 1.0f;                                     // global quality multiplier for march steps
        std::vector<FogVolumeData> FogVolumes;
        std::vector<CloudVolumeData> CloudVolumes;

        uint32_t BloomTextureID = 0;
        uint32_t BloomTempTextureID = 0;

        glm::mat4 PrevViewProjection = glm::mat4(1.0f);
        glm::mat4 CurrentViewProjection = glm::mat4(1.0f);
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);

        glm::vec3 CurrentCameraPosition = glm::vec3(0.0f);
        float CurrentCameraPitch = 0.0f;
        float CurrentCameraYaw = 0.0f;

        glm::vec3 LastCameraPosition = glm::vec3(0.0f);
        float LastCameraPitch = 0.0f;
        float LastCameraYaw = 0.0f;

        uint32_t AccumulationTextures[2] = {0, 0};
        uint32_t CurrentAccumulationIndex = 0;

        uint32_t TraceGBufferTextureID = 0;
        uint32_t VelocityTextureID = 0;
        uint32_t AlbedoTextureID = 0;

        uint32_t GeneratedTextureID = 0;

        std::array<Plane, 6> FrustumPlanes;

        Renderer3D::Statistics Stats;
    };

    static Renderer3DData s_Data;

    static void ApplyQualityPreset();

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

        glm::mat4 result;
        result[0][0] = r00 * rsx;
        result[0][1] = r10 * rsy;
        result[0][2] = r20 * rsz;
        result[0][3] = 0.0f;
        result[1][0] = r01 * rsx;
        result[1][1] = r11 * rsy;
        result[1][2] = r21 * rsz;
        result[1][3] = 0.0f;
        result[2][0] = r02 * rsx;
        result[2][1] = r12 * rsy;
        result[2][2] = r22 * rsz;
        result[2][3] = 0.0f;
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
        uint32_t id = texture->GetRendererID();
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

    static void ClearAccumulationBuffers()
    {
        float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};

        for (int i = 0; i < 2; i++)
        {
            glClearTexImage(s_Data.AccumulationTextures[i], 0, GL_RGBA, GL_FLOAT, clearColor);
        }

        // Reset trace G-buffer so velocity/temporal start from a clean slate
        if (s_Data.TraceGBufferTextureID)
        {
            float gbufferClear[] = {1.0f, 0.5f, 0.5f, 1.0f};
            glClearTexImage(s_Data.TraceGBufferTextureID, 0, GL_RGBA, GL_FLOAT, gbufferClear);
        }
    }

    // Uploads fog/cloud volume uniforms to the currently bound program.
    // progIndex: 0 = Nova compute, 1 = Basic raster. Missing locations (-1)
    // are skipped so shaders that predate volumetrics keep working.
    static void UploadVolumetricUniforms(int progIndex)
    {
        using V = Renderer3DData;
        int *loc = s_Data.Loc_Volumetric[progIndex];

        if (loc[V::Vol_SunDirection] >= 0)
            glUniform3f(loc[V::Vol_SunDirection], s_Data.SunDirection.x, s_Data.SunDirection.y, s_Data.SunDirection.z);
        if (loc[V::Vol_Time] >= 0)
            glUniform1f(loc[V::Vol_Time], GetVolumetricTimeSeconds());

        // Low/Medium presets march with reduced noise octaves (u_VolFast)
        // and fewer steps; High/Ultra keep full quality.
        bool volFast = (s_Data.CurrentQualityPreset == QualityPreset::Low ||
                        s_Data.CurrentQualityPreset == QualityPreset::Medium);
        if (loc[V::Vol_VolFast] >= 0)
            glUniform1i(loc[V::Vol_VolFast], volFast ? 1 : 0);
        float presetStepFactor = 1.0f;
        if (s_Data.CurrentQualityPreset == QualityPreset::Low)
            presetStepFactor = 0.5f;
        else if (s_Data.CurrentQualityPreset == QualityPreset::Medium)
            presetStepFactor = 0.75f;

        int fogCount = 0;
        if (s_Data.VolFogEnabled && !s_Data.FogVolumes.empty())
            fogCount = (int)s_Data.FogVolumes.size() < Renderer3D::MaxFogVolumes ? (int)s_Data.FogVolumes.size() : Renderer3D::MaxFogVolumes;
        if (loc[V::Vol_FogEnabled] >= 0)
            glUniform1i(loc[V::Vol_FogEnabled], s_Data.VolFogEnabled ? 1 : 0);
        if (loc[V::Vol_FogCount] >= 0)
            glUniform1i(loc[V::Vol_FogCount], fogCount);
        if (fogCount > 0)
        {
            glm::vec3 mins[Renderer3D::MaxFogVolumes];
            glm::vec3 maxs[Renderer3D::MaxFogVolumes];
            glm::vec3 cols[Renderer3D::MaxFogVolumes];
            glm::vec4 d1[Renderer3D::MaxFogVolumes];
            glm::vec4 d2[Renderer3D::MaxFogVolumes];
            for (int i = 0; i < fogCount; i++)
            {
                const FogVolumeData &v = s_Data.FogVolumes[(size_t)i];
                mins[i] = v.Min;
                maxs[i] = v.Max;
                cols[i] = v.Color;
                d1[i] = glm::vec4(v.Density, v.Anisotropy, v.NoiseStrength, v.NoiseScale);
                int steps = (int)(v.Steps * s_Data.VolStepScale * presetStepFactor + 0.5f);
                steps = steps < 1 ? 1 : (steps > 32 ? 32 : steps);
                d2[i] = glm::vec4(v.WindSpeed, v.HeightFalloff, (float)steps, v.Enabled ? 1.0f : 0.0f);
            }
            if (loc[V::Vol_FogMin] >= 0)
                glUniform3fv(loc[V::Vol_FogMin], fogCount, glm::value_ptr(mins[0]));
            if (loc[V::Vol_FogMax] >= 0)
                glUniform3fv(loc[V::Vol_FogMax], fogCount, glm::value_ptr(maxs[0]));
            if (loc[V::Vol_FogColor] >= 0)
                glUniform3fv(loc[V::Vol_FogColor], fogCount, glm::value_ptr(cols[0]));
            if (loc[V::Vol_FogData] >= 0)
                glUniform4fv(loc[V::Vol_FogData], fogCount, glm::value_ptr(d1[0]));
            if (loc[V::Vol_FogData2] >= 0)
                glUniform4fv(loc[V::Vol_FogData2], fogCount, glm::value_ptr(d2[0]));
        }

        int cloudCount = 0;
        if (s_Data.VolCloudsEnabled && !s_Data.CloudVolumes.empty())
            cloudCount = (int)s_Data.CloudVolumes.size() < Renderer3D::MaxCloudVolumes ? (int)s_Data.CloudVolumes.size() : Renderer3D::MaxCloudVolumes;
        if (loc[V::Vol_CloudEnabled] >= 0)
            glUniform1i(loc[V::Vol_CloudEnabled], s_Data.VolCloudsEnabled ? 1 : 0);
        if (loc[V::Vol_CloudCount] >= 0)
            glUniform1i(loc[V::Vol_CloudCount], cloudCount);
        if (cloudCount > 0)
        {
            glm::vec3 mins[Renderer3D::MaxCloudVolumes];
            glm::vec3 maxs[Renderer3D::MaxCloudVolumes];
            glm::vec3 cols[Renderer3D::MaxCloudVolumes];
            glm::vec3 ambs[Renderer3D::MaxCloudVolumes];
            glm::vec4 d1[Renderer3D::MaxCloudVolumes];
            glm::vec4 d2[Renderer3D::MaxCloudVolumes];
            glm::vec4 d3[Renderer3D::MaxCloudVolumes];
            for (int i = 0; i < cloudCount; i++)
            {
                const CloudVolumeData &v = s_Data.CloudVolumes[(size_t)i];
                mins[i] = v.Min;
                maxs[i] = v.Max;
                cols[i] = v.Color;
                ambs[i] = v.Ambient;
                d1[i] = glm::vec4(v.Coverage, v.Density, v.NoiseScale, v.Detail);
                int steps = (int)(v.Steps * s_Data.VolStepScale * presetStepFactor + 0.5f);
                steps = steps < 1 ? 1 : (steps > 32 ? 32 : steps);
                d2[i] = glm::vec4(v.WindSpeed, v.WindDir.x, v.WindDir.y, (float)steps);
                d3[i] = glm::vec4(v.SilverLining, v.ShadowStrength, v.Enabled ? 1.0f : 0.0f, 0.0f);
            }
            if (loc[V::Vol_CloudMin] >= 0)
                glUniform3fv(loc[V::Vol_CloudMin], cloudCount, glm::value_ptr(mins[0]));
            if (loc[V::Vol_CloudMax] >= 0)
                glUniform3fv(loc[V::Vol_CloudMax], cloudCount, glm::value_ptr(maxs[0]));
            if (loc[V::Vol_CloudColor] >= 0)
                glUniform3fv(loc[V::Vol_CloudColor], cloudCount, glm::value_ptr(cols[0]));
            if (loc[V::Vol_CloudAmbient] >= 0)
                glUniform3fv(loc[V::Vol_CloudAmbient], cloudCount, glm::value_ptr(ambs[0]));
            if (loc[V::Vol_CloudData] >= 0)
                glUniform4fv(loc[V::Vol_CloudData], cloudCount, glm::value_ptr(d1[0]));
            if (loc[V::Vol_CloudData2] >= 0)
                glUniform4fv(loc[V::Vol_CloudData2], cloudCount, glm::value_ptr(d2[0]));
            if (loc[V::Vol_CloudData3] >= 0)
                glUniform4fv(loc[V::Vol_CloudData3], cloudCount, glm::value_ptr(d3[0]));
        }
    }

    void Renderer3D::Init()
    {
        WL_PROFILE_FUNCTION();

        BufferLayout layout = {
            {ShaderDataType::Float3, "a_Position"},
            {ShaderDataType::Float4, "a_Color"},
            {ShaderDataType::Float3, "a_Normal"},
            {ShaderDataType::Float2, "a_TexCoord"},
            {ShaderDataType::Float, "a_TexIndex"},
            {ShaderDataType::Float, "a_TilingFactor"},
            {ShaderDataType::Int, "a_EntityID"},
            {ShaderDataType::Float, "a_Metallic"},
            {ShaderDataType::Float, "a_Roughness"}};

        // --- CUBE SETUP ---
        s_Data.CubeVertexArray = VertexArray::Create();
        s_Data.CubeVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(Vertex3D));
        s_Data.CubeVertexBuffer->SetLayout(layout);
        s_Data.CubeVertexArray->AddVertexBuffer(s_Data.CubeVertexBuffer);
        s_Data.CubeVertexBufferBase = new Vertex3D[s_Data.MaxVertices];

        uint32_t *cubeIndices = new uint32_t[s_Data.MaxIndices];
        uint32_t offset = 0;
        for (uint32_t i = 0; i < s_Data.MaxIndices; i += 6)
        {
            cubeIndices[i + 0] = offset + 0;
            cubeIndices[i + 1] = offset + 1;
            cubeIndices[i + 2] = offset + 2;
            cubeIndices[i + 3] = offset + 2;
            cubeIndices[i + 4] = offset + 3;
            cubeIndices[i + 5] = offset + 0;
            offset += 4;
        }
        Ref<IndexBuffer> cubeIB = IndexBuffer::Create(cubeIndices, s_Data.MaxIndices);
        s_Data.CubeVertexArray->SetIndexBuffer(cubeIB);
        delete[] cubeIndices;

        // --- SPHERE SETUP ---
        s_Data.SphereVertexArray = VertexArray::Create();
        s_Data.SphereVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(Vertex3D));
        s_Data.SphereVertexBuffer->SetLayout(layout);
        s_Data.SphereVertexArray->AddVertexBuffer(s_Data.SphereVertexBuffer);

        s_Data.SphereVertexBufferBase = new Vertex3D[s_Data.MaxVertices];
        s_Data.SphereIndexBufferBase = new uint32_t[s_Data.MaxIndices];

        s_Data.SphereIndexBuffer = IndexBuffer::Create(nullptr, s_Data.MaxIndices);
        s_Data.SphereVertexArray->SetIndexBuffer(s_Data.SphereIndexBuffer);

        // --- SHADER & UTILS ---
        s_Data.BasicShader = Shader::Create("assets/shaders/Renderer3D_Basic.glsl");

        s_Data.RayTracingOutput = Texture2D::Create(s_Data.RayTracingWidth, s_Data.RayTracingHeight);
        s_Data.RayTracingShader = Shader::Create("assets/shaders/Renderer3D_NovaRenderer.glsl");

        GLuint rtProg = s_Data.RayTracingShader->GetRendererID();
        s_Data.InstanceCountLocation = glGetUniformLocation(rtProg, "u_InstanceCount");
        s_Data.FrameIndexLocation = glGetUniformLocation(rtProg, "u_FrameIndex");
        s_Data.Loc_PassID = glGetUniformLocation(rtProg, "u_PassID");
        s_Data.Loc_SamplesPerPixel = glGetUniformLocation(rtProg, "u_SamplesPerPixel");
        s_Data.Loc_QualityLevel = glGetUniformLocation(rtProg, "u_QualityLevel");
        s_Data.Loc_MaxBounces = glGetUniformLocation(rtProg, "u_MaxBounces");
        s_Data.Loc_MaxLights = glGetUniformLocation(rtProg, "u_MaxLights");
        s_Data.Loc_IndirectRays = glGetUniformLocation(rtProg, "u_IndirectRays");
        s_Data.Loc_CameraMoved = glGetUniformLocation(rtProg, "u_CameraMoved");
        s_Data.Loc_SkyBottomColor = glGetUniformLocation(rtProg, "u_SkyBottomColor");
        s_Data.Loc_SkyTopColor = glGetUniformLocation(rtProg, "u_SkyTopColor");
        s_Data.Loc_Jitter = glGetUniformLocation(rtProg, "u_Jitter");
        s_Data.Loc_DepthBuffer = glGetUniformLocation(rtProg, "u_DepthBuffer");
        s_Data.Loc_AccumulationAlpha = glGetUniformLocation(rtProg, "u_AccumulationAlpha");
        s_Data.Loc_StepSize = glGetUniformLocation(rtProg, "u_StepSize");
        s_Data.Loc_PrevViewProjection = glGetUniformLocation(rtProg, "u_PrevViewProjection");
        s_Data.Loc_InverseViewProjection = glGetUniformLocation(rtProg, "u_InverseViewProjection");
        s_Data.Loc_ViewProjection = glGetUniformLocation(rtProg, "u_ViewProjection");
        s_Data.Loc_CameraPosition = glGetUniformLocation(rtProg, "u_CameraPosition");
        s_Data.Loc_RenderScale = glGetUniformLocation(rtProg, "u_RenderScale");
        s_Data.Loc_LightCount = glGetUniformLocation(rtProg, "u_LightCount");
        s_Data.Loc_NeuralEnabled = glGetUniformLocation(rtProg, "u_NeuralEnabled");
        s_Data.Loc_NeuralTexStrength = glGetUniformLocation(rtProg, "u_NeuralTexStrength");
        s_Data.Loc_NeuralLightStrength = glGetUniformLocation(rtProg, "u_NeuralLightStrength");
        s_Data.Loc_NeuralMatStrength = glGetUniformLocation(rtProg, "u_NeuralMatStrength");

        auto cacheVolumetricLocations = [](GLuint prog, int progIndex)
        {
            static const char *names[Renderer3DData::VolLoc_Count] = {
                "u_SunDirection", "u_Time",
                "u_FogEnabled", "u_FogCount", "u_FogMin[0]", "u_FogMax[0]", "u_FogColor[0]", "u_FogData[0]", "u_FogData2[0]",
                "u_CloudEnabled", "u_CloudCount", "u_CloudMin[0]", "u_CloudMax[0]", "u_CloudColor[0]",
                "u_CloudAmbient[0]", "u_CloudData[0]", "u_CloudData2[0]", "u_CloudData3[0]", "u_VolFast"};
            for (int i = 0; i < Renderer3DData::VolLoc_Count; i++)
                s_Data.Loc_Volumetric[progIndex][i] = glGetUniformLocation(prog, names[i]);
        };
        cacheVolumetricLocations(rtProg, 0);
        cacheVolumetricLocations(s_Data.BasicShader->GetRendererID(), 1);

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

        glCreateTextures(GL_TEXTURE_2D, 2, s_Data.AccumulationTextures);
        for (int i = 0; i < 2; i++)
        {
            glTextureStorage2D(s_Data.AccumulationTextures[i], 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(s_Data.AccumulationTextures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        uint32_t maxInstances = 10000;
        uint32_t maxBVHNodes = maxInstances * 2;
        glCreateBuffers(1, &s_Data.SceneInstanceBufferID);
        glNamedBufferData(s_Data.SceneInstanceBufferID, maxInstances * sizeof(RayTracingInstance), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_Data.SceneInstanceBufferID);

        glCreateBuffers(1, &s_Data.BVHBufferID);
        glNamedBufferData(s_Data.BVHBufferID, maxBVHNodes * sizeof(BVHNodeGPU), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_Data.BVHBufferID);

        glCreateBuffers(1, &s_Data.LightListBufferID);
        glNamedBufferData(s_Data.LightListBufferID, maxInstances * sizeof(int), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s_Data.LightListBufferID);

        uint32_t whiteTextureData = 0xffffffff;
        Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
        whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
        s_Data.TextureSlots[0] = whiteTexture;

        s_Data.m_SceneInstances.reserve(maxInstances);

        s_Data.RayTracingShader->Bind();
        int samplers[32];
        for (int i = 0; i < 32; i++)
            samplers[i] = i + 15;
        s_Data.RayTracingShader->SetIntArray("u_SceneTextures", samplers, 32);

        ApplyQualityPreset();
    }

    void Renderer3D::Shutdown()
    {
        WL_PROFILE_FUNCTION();
        delete[] s_Data.CubeVertexBufferBase;
        delete[] s_Data.SphereVertexBufferBase;
        delete[] s_Data.SphereIndexBufferBase;

        glDeleteBuffers(1, &s_Data.SceneInstanceBufferID);
        glDeleteBuffers(1, &s_Data.BVHBufferID);
        glDeleteBuffers(1, &s_Data.LightListBufferID);
        glDeleteTextures(1, &s_Data.AccumulationTexture);
        glDeleteTextures(1, &s_Data.BloomTextureID);
        glDeleteTextures(1, &s_Data.BloomTempTextureID);
        glDeleteTextures(1, &s_Data.VelocityTextureID);
        glDeleteTextures(1, &s_Data.AlbedoTextureID);
        glDeleteTextures(1, &s_Data.TraceGBufferTextureID);
        glDeleteTextures(2, s_Data.AccumulationTextures);

        s_Data.CubeVertexArray = nullptr;
        s_Data.CubeVertexBuffer = nullptr;
        s_Data.SphereVertexArray = nullptr;
        s_Data.SphereVertexBuffer = nullptr;
        s_Data.SphereIndexBuffer = nullptr;
        s_Data.BasicShader = nullptr;
        s_Data.RayTracingShader = nullptr;
        s_Data.RayTracingOutput = nullptr;
    }

    void Renderer3D::BeginScene(const Camera &camera, const glm::mat4 &transform)
    {
        WL_PROFILE_FUNCTION();

        // New frame: drop last frame's volumes (kept across mid-frame overflow
        // flushes, unlike the geometry batches reset in FlushAndReset).
        s_Data.FogVolumes.clear();
        s_Data.CloudVolumes.clear();
        s_Data.m_SceneInstances.clear();

        glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);
        s_Data.ActiveCameraIsEditor = false;

        auto extract = [&](int row, int sign) -> Renderer3DData::Plane
        {
            glm::vec4 planeEq;
            for (int i = 0; i < 4; ++i)
                planeEq[i] = viewProj[i][3] + sign * viewProj[i][row];

            float length = glm::length(glm::vec3(planeEq));

            return Renderer3DData::Plane{glm::vec3(planeEq) / length, planeEq.w / length};
        };

        s_Data.FrustumPlanes[0] = extract(0, 1);  // Left
        s_Data.FrustumPlanes[1] = extract(0, -1); // Right
        s_Data.FrustumPlanes[2] = extract(1, 1);  // Bottom
        s_Data.FrustumPlanes[3] = extract(1, -1); // Top
        s_Data.FrustumPlanes[4] = extract(2, 1);  // Near
        s_Data.FrustumPlanes[5] = extract(2, -1); // Far

        s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
        s_Data.CurrentViewProjection = viewProj;
        s_Data.InverseViewProjection = glm::inverse(viewProj);

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
        }

        s_Data.CurrentCameraPosition = glm::vec3(transform[3]);
        s_Data.CurrentCameraPitch = 0.0f;
        s_Data.CurrentCameraYaw = 0.0f;

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);

        s_Data.RayTracingShader->Bind();
        glUniformMatrix4fv(s_Data.Loc_ViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform3f(s_Data.Loc_CameraPosition, s_Data.CurrentCameraPosition.x, s_Data.CurrentCameraPosition.y, s_Data.CurrentCameraPosition.z);

        FlushAndReset();
    }

    void Renderer3D::BeginScene(const EditorCamera &camera)
    {
        WL_PROFILE_FUNCTION();

        // New frame: drop last frame's volumes (kept across mid-frame overflow
        // flushes, unlike the geometry batches reset in FlushAndReset).
        s_Data.FogVolumes.clear();
        s_Data.CloudVolumes.clear();
        s_Data.m_SceneInstances.clear();

        glm::mat4 viewProj = camera.GetViewProjection();
        s_Data.ActiveCameraIsEditor = true;

        s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
        s_Data.CurrentViewProjection = viewProj;
        s_Data.InverseViewProjection = glm::inverse(viewProj);

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
        }

        s_Data.CurrentCameraPosition = camera.GetPosition();
        s_Data.CurrentCameraPitch = camera.GetPitch();
        s_Data.CurrentCameraYaw = camera.GetYaw();

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);

        s_Data.RayTracingShader->Bind();
        glUniformMatrix4fv(s_Data.Loc_ViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform3f(s_Data.Loc_CameraPosition, s_Data.CurrentCameraPosition.x, s_Data.CurrentCameraPosition.y, s_Data.CurrentCameraPosition.z);

        FlushAndReset();
    }

    void Renderer3D::EndScene()
    {
        WL_PROFILE_FUNCTION();
        Flush();
    }

    void Renderer3D::Flush()
    {
        if (s_Data.RayTracingEnabled)
        {
            if (s_Data.m_SceneInstances.empty())
                return;

            bool moved = false;
            float sqThreshold = s_Data.MovementThreshold * s_Data.MovementThreshold;
            if (s_Data.ActiveCameraIsEditor)
            {
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
            float movedValue = moved ? 1.0f : 0.0f;

            if (s_Data.ActiveCameraIsEditor)
                s_Data.EditorCameraMoved = moved;
            else
                s_Data.GameCameraMoved = moved;

            if (!moved && s_Data.CameraMoved)
            {
                s_Data.FrameIndex = 0;
                ClearAccumulationBuffers();
            }
            s_Data.CameraMoved = moved;

            if (s_Data.m_SceneDirty)
            {
                // Build BVH for acceleration
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

                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                s_Data.m_SceneDirty = false;
            }

            uint32_t width = s_Data.RayTracingWidth;
            uint32_t height = s_Data.RayTracingHeight;
            glm::vec2 viewportSize = glm::vec2(width, height);

            glm::vec2 currentJitter = GetCurrentJitter(s_Data.FrameIndex, viewportSize);

            uint32_t readIdx = s_Data.CurrentAccumulationIndex;
            uint32_t writeIdx = 1 - s_Data.CurrentAccumulationIndex;

            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, s_Data.AccumulationTextures[readIdx]);
            glActiveTexture(GL_TEXTURE0 + 4);
            glBindTexture(GL_TEXTURE_2D, s_Data.TraceGBufferTextureID);
            glActiveTexture(GL_TEXTURE0 + 10);
            glBindTexture(GL_TEXTURE_2D, s_Data.TraceGBufferTextureID);

            uint32_t currentFrameIndex = s_Data.FrameIndex;
            float accumulationAlpha = 1.0f;
            if (s_Data.RayTracingAccumulate)
            {
                bool bothCamerasStationary = !s_Data.EditorCameraMoved && !s_Data.GameCameraMoved;
                if (s_Data.CameraMoved)
                {
                    accumulationAlpha = 0.2f;
                }
                else if (bothCamerasStationary && currentFrameIndex > 4)
                {
                    accumulationAlpha = 0.02f;
                }
                else
                {
                    accumulationAlpha = 1.0f / (float)(currentFrameIndex + 1);
                }
                accumulationAlpha = glm::clamp(accumulationAlpha, 0.02f, 1.0f);
            }

            s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();
            GLboolean layered = GL_FALSE;

            // FIX: Bind missing velocity (unit 5) and bloom temp (unit 7) image units
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

            uint32_t workGroupsX = (s_Data.ComputeWidth + 7) / 8;
            uint32_t workGroupsY = (s_Data.ComputeHeight + 7) / 8;

            s_Data.RayTracingShader->Bind();

            // Upload all per-frame uniforms using pre-cached locations (no string lookups)
            glUniform1i(s_Data.InstanceCountLocation, (int)s_Data.m_SceneInstances.size());
            glUniform1i(s_Data.Loc_SamplesPerPixel, s_Data.SamplesPerPixel);
            glUniform1i(s_Data.Loc_QualityLevel, (int)s_Data.CurrentQualityPreset);
            glUniform1i(s_Data.Loc_MaxBounces, s_Data.MaxBounces);
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
            glUniform1i(s_Data.Loc_LightCount, (int)s_Data.m_LightIndices.size());
            // Neural Rendering uniforms (cached locations; -1 if shader predates neural block)
            if (s_Data.Loc_NeuralEnabled >= 0)
                glUniform1i(s_Data.Loc_NeuralEnabled, s_Data.NeuralEnabled ? 1 : 0);
            if (s_Data.Loc_NeuralTexStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralTexStrength, s_Data.NeuralTextureStrength);
            if (s_Data.Loc_NeuralLightStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralLightStrength, s_Data.NeuralLightStrength);
            if (s_Data.Loc_NeuralMatStrength >= 0)
                glUniform1f(s_Data.Loc_NeuralMatStrength, s_Data.NeuralMaterialStrength);
            UploadVolumetricUniforms(0);

            // View matrices must be uploaded BEFORE pass 0: velocity
            // reconstructs with the inverse and reprojects with the previous
            // matrix, and pass 0 previously ran on last frame's uploads (one
            // frame stale) which dragged history behind during motion.
            glUniformMatrix4fv(s_Data.Loc_PrevViewProjection, 1, GL_FALSE, glm::value_ptr(s_Data.PrevViewProjection));
            glUniformMatrix4fv(s_Data.Loc_InverseViewProjection, 1, GL_FALSE, glm::value_ptr(s_Data.InverseViewProjection));

            // Pass 0: Visibility + Velocity
            glUniform1i(s_Data.Loc_PassID, 0);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            // Pass 1: Hybrid Trace (direct + indirect rays)
            glUniform1i(s_Data.Loc_PassID, 1);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            // Pass 2: Temporal Indirect Accumulation (velocity-reprojected)
            glUniform1i(s_Data.Loc_PassID, 2);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

            if (s_Data.BilateralBlurEnabled)
            {
                int stepSizes[] = {1, 2, 4, 8};
                int blurCount = (s_Data.BilateralBlurPasses < 4) ? s_Data.BilateralBlurPasses : 4;
                for (int bi = 0; bi < blurCount; bi++)
                {
                    glUniform1i(s_Data.Loc_StepSize, stepSizes[bi]);
                    glUniform1i(s_Data.Loc_PassID, 7);
                    glDispatchCompute(workGroupsX, workGroupsY, 1);
                    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                }
            }

            // Pass 6: Composite — always dispatches at FULL resolution
            // When RenderScale < 1.0, the composite upscales inline from the reduced-res accumulation buffer
            {
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

            s_Data.RayTracingShader->Unbind();

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
        UploadVolumetricUniforms(1);
        // View-dependent metal reflection inputs
        s_Data.BasicShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);
        s_Data.BasicShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
        s_Data.BasicShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);
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

    void Renderer3D::ResetStats()
    {
        s_Data.Stats.DrawCalls = 0;
        s_Data.Stats.QuadCount = 0;
    }
    Renderer3D::Statistics Renderer3D::GetStats() { return s_Data.Stats; }
    bool Renderer3D::IsRayTracingEnabled() { return s_Data.RayTracingEnabled; }
    void Renderer3D::SetRayTracingEnabled(bool enabled) { s_Data.RayTracingEnabled = enabled; }
    static void UpdateComputeResolution()
    {
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
        s_Data.RenderScale = glm::clamp(scale, 0.25f, 1.0f);
        UpdateComputeResolution();
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }

    static void ApplyQualityPreset()
    {
        switch (s_Data.CurrentQualityPreset)
        {
        case QualityPreset::Low:
            // Traces at half res (upscaled in composite): ~4x fewer pixels.
            // The 60fps preset at 4K.
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 1;
            s_Data.IndirectRays = 0;
            s_Data.BloomEnabled = false;
            s_Data.BilateralBlurEnabled = false;
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
            // Traces at 2/3 res (upscaled in composite).
            s_Data.SamplesPerPixel = 1;
            s_Data.MaxBounces = 1;
            s_Data.MaxLights = 4;
            s_Data.IndirectRays = 1;
            s_Data.BloomEnabled = true;
            s_Data.BilateralBlurEnabled = true;
            s_Data.BilateralBlurPasses = 0;
            s_Data.RenderScale = 0.66f;
            break;
        case QualityPreset::Ultra:
            // Traces at 3/4 res (upscaled in composite). Perf: 1 ray/px + 2 rotating shadow rays
            // from up to 4 lights, neural GI steadies the single indirect
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

        UpdateComputeResolution();
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }

    QualityPreset Renderer3D::GetQualityPreset() { return s_Data.CurrentQualityPreset; }
    void Renderer3D::SetQualityPreset(QualityPreset preset)
    {
        s_Data.CurrentQualityPreset = preset;
        ApplyQualityPreset();
    }
    bool Renderer3D::IsRayTracingAccumulate() { return s_Data.RayTracingAccumulate; }
    void Renderer3D::SetRayTracingAccumulate(bool enabled)
    {
        s_Data.RayTracingAccumulate = enabled;
        s_Data.FrameIndex = 0;
        ClearAccumulationBuffers();
    }
    uint32_t Renderer3D::GetRayTraceTargetID() { return s_Data.RayTracingOutput->GetRendererID(); }
    void Renderer3D::ResizeRayTraceTarget(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;

        // Resize Output Texture
        s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();

        if (s_Data.RayTracingTexture)
        {
            glDeleteTextures(1, &s_Data.RayTracingTexture);
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.RayTracingTexture);
        glTextureStorage2D(s_Data.RayTracingTexture, 1, GL_RGBA32F, width, height);

        glTextureParameteri(s_Data.RayTracingTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.RayTracingTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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

        s_Data.RayTracingWidth = width;
        s_Data.RayTracingHeight = height;
        UpdateComputeResolution();
        s_Data.FrameIndex = 0;
    }

    static bool IsAABBInFrustum(const std::array<Renderer3DData::Plane, 6> &planes, const glm::vec3 &min, const glm::vec3 &max, const glm::mat4 &transform)
    {
        glm::vec3 localCenter = (min + max) * 0.5f;
        glm::vec3 localExtents = (max - min) * 0.5f;

        glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(localCenter, 1.0f));

        glm::vec3 worldAxisX = glm::vec3(transform[0]);
        glm::vec3 worldAxisY = glm::vec3(transform[1]);
        glm::vec3 worldAxisZ = glm::vec3(transform[2]);

        for (const auto &plane : planes)
        {
            float r = localExtents.x * glm::abs(glm::dot(plane.normal, worldAxisX)) +
                      localExtents.y * glm::abs(glm::dot(plane.normal, worldAxisY)) +
                      localExtents.z * glm::abs(glm::dot(plane.normal, worldAxisZ));

            float dist = glm::dot(plane.normal, worldCenter) + plane.distance;

            if (dist < -r)
                return false;
        }
        return true;
    }

    uint32_t Renderer3D::GetSamplesPerPixel() { return s_Data.SamplesPerPixel; }
    void Renderer3D::SetSamplesPerPixel(uint32_t samples)
    {
        s_Data.SamplesPerPixel = glm::max(1u, samples);
        s_Data.FrameIndex = 0;
    }

    float Renderer3D::GetMovementThreshold() { return s_Data.MovementThreshold; }
    void Renderer3D::SetMovementThreshold(float threshold) { s_Data.MovementThreshold = glm::max(0.00001f, threshold); }

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

    void Renderer3D::SubmitFogVolume(const glm::mat4 &transform, const VolumetricFogComponent &fog)
    {
        if (!fog.Enabled)
            return;
        if (s_Data.FogVolumes.size() >= (size_t)MaxFogVolumes)
            return;
        FogVolumeData v;
        ComputeVolumeBounds(transform, v.Min, v.Max);
        v.Color = fog.Color;
        v.Density = glm::max(fog.Density, 0.0f);
        v.Anisotropy = glm::clamp(fog.Anisotropy, -0.9f, 0.9f);
        v.NoiseStrength = glm::clamp(fog.NoiseStrength, 0.0f, 1.0f);
        v.NoiseScale = glm::max(fog.NoiseScale, 0.001f);
        v.WindSpeed = glm::max(fog.WindSpeed, 0.0f);
        v.HeightFalloff = glm::clamp(fog.HeightFalloff, 0.0f, 1.0f);
        v.Steps = glm::clamp(fog.MaxSteps, 1, 32);
        v.Enabled = true;
        s_Data.FogVolumes.push_back(v);
    }

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
        if (glm::length2(v.WindDir) < 1e-8f)
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
    void Renderer3D::SetSunDirection(const glm::vec3 &direction)
    {
        if (glm::length2(direction) > 1e-8f)
            s_Data.SunDirection = glm::normalize(direction);
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetVolumetricStepScale() { return s_Data.VolStepScale; }
    void Renderer3D::SetVolumetricStepScale(float scale)
    {
        s_Data.VolStepScale = glm::clamp(scale, 0.25f, 2.0f);
        s_Data.FrameIndex = 0;
    }

    bool Renderer3D::IsNeuralRenderingEnabled() { return s_Data.NeuralEnabled; }
    void Renderer3D::SetNeuralRenderingEnabled(bool enabled)
    {
        s_Data.NeuralEnabled = enabled;
        s_Data.FrameIndex = 0;
    }
    float Renderer3D::GetNeuralTextureStrength() { return s_Data.NeuralTextureStrength; }
    void Renderer3D::SetNeuralTextureStrength(float strength)
    {
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

    float Renderer3D::Halton(int index, int base)
    {
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0)
        {
            f = f / (float)base;
            r = r + f * (float)(index % base);
            index = index / base;
        }
        return r;
    }

    glm::vec2 Renderer3D::GetCurrentJitter(int frameIndex, glm::vec2 viewportSize)
    {
        int index = (frameIndex % 16) + 1;

        float jitterX = Halton(index, 2) - 0.5f;
        float jitterY = Halton(index, 3) - 0.5f;

        return glm::vec2(jitterX * 2.0f / viewportSize.x, jitterY * 2.0f / viewportSize.y);
    }

    void Renderer3D::GenerateMaterialMaps(const std::string &texturePath, float normalStrength, float roughBias)
    {
        s_Data.RayTracingShader->Bind();

        s_Data.RayTracingShader->SetFloat("u_NormalStrength", normalStrength);
        s_Data.RayTracingShader->SetFloat("u_RoughnessBias", roughBias);

        Ref<Texture2D> inputTexture = Texture2D::Create(texturePath);
        glActiveTexture(GL_TEXTURE0 + 11);
        inputTexture->Bind();

        glBindImageTexture(8, s_Data.GeneratedTextureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        glDispatchCompute(s_Data.RayTracingWidth / 8, s_Data.RayTracingHeight / 8, 1);

        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    void Renderer3D::FlushAndReset()
    {
        s_Data.m_SceneInstances.clear();

        s_Data.CubeIndexCount = 0;
        s_Data.CubeVertexCount = 0;
        s_Data.CubeVertexBufferPtr = s_Data.CubeVertexBufferBase;

        s_Data.SphereIndexCount = 0;
        s_Data.SphereVertexCount = 0;
        s_Data.SphereVertexBufferPtr = s_Data.SphereVertexBufferBase;
        s_Data.SphereIndexBufferPtr = s_Data.SphereIndexBufferBase;

        s_Data.TextureSlotIndex = 1;
        s_Data.TextureSlotMap.clear();
    }

    void Renderer3D::DrawCube(const glm::mat4 &transform, const glm::vec4 &color, MaterialComponent &material, int entityID)
    {
        if (s_Data.CubeIndexCount + 36 >= s_Data.MaxIndices)
        {
            Flush();
            FlushAndReset();
        }

        if (!IsAABBInFrustum(s_Data.FrustumPlanes, glm::vec3(-0.5f), glm::vec3(0.5f), transform))
            return;

        static const glm::vec3 cubePositions[24] = {
            {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}};

        static const glm::vec3 cubeNormals[24] = {
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {-0.0f, 0.0f, -1.0f}, {-0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, -0.0f}};

        static const glm::vec2 texCoords[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        glm::mat4 invTransform = FastTRSInverse(transform);
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));

        // Raster-path material inputs (also reused by the ray-trace instance below)
        int cubeTextureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : 0;

        for (int i = 0; i < 24; i++)
        {
            s_Data.CubeVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(cubePositions[i], 1.0f));
            s_Data.CubeVertexBufferPtr->Color = material.Albedo;
            s_Data.CubeVertexBufferPtr->Normal = glm::normalize(normalMatrix * cubeNormals[i]);
            s_Data.CubeVertexBufferPtr->TexCoord = texCoords[i % 4];
            s_Data.CubeVertexBufferPtr->TexIndex = (float)cubeTextureSlot;
            s_Data.CubeVertexBufferPtr->TilingFactor = 1.0f;
            s_Data.CubeVertexBufferPtr->EntityID = entityID;
            s_Data.CubeVertexBufferPtr->Metallic = material.Metallic;
            s_Data.CubeVertexBufferPtr->Roughness = material.Roughness;
            s_Data.CubeVertexBufferPtr++;
        }

        s_Data.CubeIndexCount += 36;
        s_Data.CubeVertexCount += 24;
        s_Data.Stats.QuadCount += 6;

        if (s_Data.RayTracingEnabled)
        {
            RayTracingInstance instance = {};

            instance.WorldTransform = transform;
            instance.InvTransform = invTransform;
            instance.Albedo = material.Albedo;
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 0.0f, 0.0f);
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
            instance.TextureScale = glm::vec4(worldScale.x, worldScale.y, worldScale.z, worldScale.z);
            float bumpStrength = material.NormalStrength > 0.0f ? material.NormalStrength : (material.Texture ? 1.0f : 0.0f);
            instance.DisplacementParams = glm::vec4(material.DisplacementScale, bumpStrength, 0.0f, 0.0f);

            instance.MaxDistance = 1000.0f;
            instance.LODLevel = 0;

            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    void Renderer3D::DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, MaterialComponent &material, int entityID)
    {
        if (!IsAABBInFrustum(s_Data.FrustumPlanes, glm::vec3(-radius), glm::vec3(radius), transform))
            return;

        uint32_t vertexCount = (stacks + 1) * (sectors + 1);
        uint32_t indexCount = 0;
        for (int i = 0; i < stacks; ++i)
        {
            if (i != 0)
                indexCount += sectors * 3;
            if (i != (stacks - 1))
                indexCount += sectors * 3;
        }

        if (s_Data.SphereIndexCount + indexCount >= s_Data.MaxIndices || s_Data.SphereVertexCount + vertexCount >= s_Data.MaxVertices)
        {
            Flush();
            FlushAndReset();
        }

        uint32_t startIndexOffset = s_Data.SphereVertexCount;
        // Raster-path material inputs (ray-trace instance below reuses the map lookup)
        int sphereTextureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : 0;
        float sectorStep = 2 * glm::pi<float>() / sectors;
        float stackStep = glm::pi<float>() / stacks;
        glm::mat4 invTransform = FastTRSInverse(transform);
        glm::mat3 normalMatrix = glm::transpose(glm::mat3(invTransform));

        for (int i = 0; i <= stacks; ++i)
        {
            float stackAngle = glm::pi<float>() / 2 - i * stackStep;
            float xy = radius * cosf(stackAngle);
            float z = radius * sinf(stackAngle);

            for (int j = 0; j <= sectors; ++j)
            {
                float sectorAngle = j * sectorStep;
                float x = xy * cosf(sectorAngle);
                float y = xy * sinf(sectorAngle);

                s_Data.SphereVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(x, y, z, 1.0f));
                s_Data.SphereVertexBufferPtr->Color = material.Albedo;
                s_Data.SphereVertexBufferPtr->Normal = glm::normalize(normalMatrix * glm::normalize(glm::vec3(x, y, z)));
                s_Data.SphereVertexBufferPtr->TexCoord = glm::vec2((float)j / sectors, (float)i / stacks);
                s_Data.SphereVertexBufferPtr->TexIndex = (float)sphereTextureSlot;
                s_Data.SphereVertexBufferPtr->TilingFactor = 1.0f;
                s_Data.SphereVertexBufferPtr->EntityID = entityID;
                s_Data.SphereVertexBufferPtr->Metallic = material.Metallic;
                s_Data.SphereVertexBufferPtr->Roughness = material.Roughness;
                s_Data.SphereVertexBufferPtr++;
            }
        }

        for (int i = 0; i < stacks; ++i)
        {
            uint32_t k1 = i * (sectors + 1) + startIndexOffset;
            uint32_t k2 = k1 + sectors + 1;

            for (int j = 0; j < sectors; ++j, ++k1, ++k2)
            {
                if (i != 0)
                {
                    *s_Data.SphereIndexBufferPtr++ = k1;
                    *s_Data.SphereIndexBufferPtr++ = k2;
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
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 1.0f, radius);
            instance.Min = glm::vec4(-radius, -radius, -radius, 1.0f);
            instance.Max = glm::vec4(radius, radius, radius, 1.0f);
            instance.Emission = glm::vec4(material.EmissionColor.x,
                                          material.EmissionColor.y,
                                          material.EmissionColor.z,
                                          material.EmissionIntensity);

            int textureSlot = material.Texture ? FindOrAddTextureSlot(material.Texture) : -1;
            glm::vec3 worldScale = GetWorldScale(transform);
            float worldRadius = radius * (worldScale.x + worldScale.y + worldScale.z) / 3.0f;

            instance.TextureID = textureSlot;
            instance.PackedMaterialMapID = -1;
            instance.TextureScale = glm::vec4(
                2.0f * glm::pi<float>() * worldRadius,
                glm::pi<float>() * worldRadius,
                0.0f,
                0.0f);
            float bumpStrength = material.NormalStrength > 0.0f ? material.NormalStrength : (material.Texture ? 1.0f : 0.0f);
            instance.DisplacementParams = glm::vec4(material.DisplacementScale, bumpStrength, 0.0f, 0.0f);

            instance.MaxDistance = 1000.0f;
            instance.LODLevel = 0;

            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    void Renderer3D::Submit(const Ref<Shader> &shader, const Ref<VertexArray> &vertexArray, const glm::mat4 &transform)
    {
        WL_PROFILE_FUNCTION();
        Renderer::Submit(shader, vertexArray, transform);
    }
}