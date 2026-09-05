#pragma once

#include "Wasteland/Renderer/Camera.h"
#include "Wasteland/Renderer/EditorCamera.h"
#include "Wasteland/Renderer/Shader.h"
#include "Wasteland/Renderer/VertexArray.h"

#include "Wasteland/Renderer/Texture.h"
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Entity.h>

#include <glm/glm.hpp>

namespace Wasteland
{

    enum class QualityPreset : int
    {
        Low = 0,
        Medium = 1,
        High = 2,
        Ultra = 3
    };

    class Renderer3D
    {
    public:
        static void Init();
        static void Shutdown();

        static void BeginScene(const Camera &camera, const glm::mat4 &transform);
        static void BeginScene(const EditorCamera &camera);
        static void EndScene();
        static void Flush();

        // Primitive 3D drawing functions mimicking 2D
        static void DrawCube(const glm::mat4 &transform, const glm::vec4 &color, MaterialComponent &material, int entityID = -1);

        static void DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, MaterialComponent &material, int entityID = -1);

        static void Submit(const Ref<Shader> &shader, const Ref<VertexArray> &vertexArray, const glm::mat4 &transform = glm::mat4(1.0f));

        // Stats
        struct Statistics
        {
            uint32_t DrawCalls = 0;
            uint32_t QuadCount = 0;

            uint32_t GetTotalVertexCount() { return QuadCount * 4; }
            uint32_t GetTotalIndexCount() { return QuadCount * 6; }
        };
        static void ResetStats();
        static Statistics GetStats();
        static bool IsRayTracingEnabled();
        static void SetRayTracingEnabled(bool enabled);
        static QualityPreset GetQualityPreset();
        static void SetQualityPreset(QualityPreset preset);
        static bool IsRayTracingAccumulate();
        static void SetRayTracingAccumulate(bool enabled);
        static uint32_t GetRayTraceTargetID();
        static void ResizeRayTraceTarget(uint32_t width, uint32_t height);
        static float GetRenderScale();
        static void SetRenderScale(float scale);

        // Dynamic resolution: holds a target framerate (default 30fps) by
        // moving RenderScale between [DynamicMinScale, preset scale] based on
        // measured GPU time. This is what lets Ultra stay above 30fps on
        // older hardware (RTX 20-series): the preset stays at full quality,
        // the tracer transparently drops resolution when the GPU can't keep
        // up, and the composite upscale + shader perf tiers hide the change.
        static bool IsDynamicResolutionEnabled();
        static void SetDynamicResolutionEnabled(bool enabled);
        static float GetTargetFPS();
        static void SetTargetFPS(float fps);
        static float GetDynamicMinScale();
        static void SetDynamicMinScale(float scale);
        // Smoothed measured framerate / GPU frame cost. 0 until measured.
        static float GetCurrentFPS();
        static float GetGPUFrameTimeMs();
        // True while the scaler is holding RenderScale below the preset max.
        static bool IsDynamicResolutionActive();
        // Classified from the GL renderer string at Init
        // ("RTX 20-series", "RTX 30-series or newer", ...). Empty if unknown.
        static const char *GetGPUTierName();
        // Feeds the application's real frame time (ms) into the scaler.
        // Call once per frame (EditorLayer::OnUpdate does). The scaler uses
        // the slowest available signal (GPU timer vs app frame), so it still
        // converges when the bottleneck sits outside the compute chain.
        static void NotifyFrameTime(double ms);

        // Ray Tracing Settings
        static uint32_t GetSamplesPerPixel();
        static void SetSamplesPerPixel(uint32_t samples);
        static float GetMovementThreshold();
        static void SetMovementThreshold(float threshold);

        // Sky Settings
        static glm::vec3 GetSkyBottomColor();
        static void SetSkyBottomColor(const glm::vec3 &color);
        static glm::vec3 GetSkyTopColor();
        static void SetSkyTopColor(const glm::vec3 &color);

        // Volumetric atmosphere (fog + clouds). Volumes are submitted per-frame
        // by the Scene from VolumetricFog/VolumetricClouds components; the
        // entity Transform defines the volume box (center = Translation,
        // full size = Scale). Raymarched in both Nova and raster paths.
        static constexpr int MaxFogVolumes = 4;
        static constexpr int MaxCloudVolumes = 2;
        static void SubmitFogVolume(const glm::mat4 &transform, const VolumetricFogComponent &fog);
        static void SubmitCloudVolume(const glm::mat4 &transform, const VolumetricCloudsComponent &clouds);
        static bool IsVolumetricFogEnabled();
        static void SetVolumetricFogEnabled(bool enabled);
        static bool IsVolumetricCloudsEnabled();
        static void SetVolumetricCloudsEnabled(bool enabled);
        static glm::vec3 GetSunDirection();
        static void SetSunDirection(const glm::vec3 &direction);
        static float GetVolumetricStepScale();
        static void SetVolumetricStepScale(float scale);

        // Analytic lights (directional/spot/point/area components). Submitted
        // per-frame by the Scene; first MaxAnalyticLights win. Evaluated with
        // shadows in the Nova path alongside emissive-geometry lights.
        static constexpr int MaxAnalyticLights = 8;
        static void SubmitDirectionalLight(const glm::mat4 &transform, const DirectionalLightComponent &light);
        static void SubmitPointLight(const glm::mat4 &transform, const PointLightComponent &light);
        static void SubmitSpotLight(const glm::mat4 &transform, const SpotLightComponent &light);
        static void SubmitAreaLight(const glm::mat4 &transform, const AreaLightComponent &light);

        // Neural Rendering (OpenGL GLSL MLP: neural texture detail + neural indirect lighting)
        static bool IsNeuralRenderingEnabled();
        static void SetNeuralRenderingEnabled(bool enabled);
        static float GetNeuralTextureStrength();
        static void SetNeuralTextureStrength(float strength);
        static float GetNeuralLightStrength();
        static void SetNeuralLightStrength(float strength);
        static float GetNeuralMaterialStrength();
        static void SetNeuralMaterialStrength(float strength);

        static float Halton(int index, int base);
        static glm::vec2 GetCurrentJitter(int frameIndex, glm::vec2 viewportSize);

        static void GenerateMaterialMaps(const std::string &texturePath, float normalStrength, float roughBias);

    private:
        static void FlushAndReset();
    };

}