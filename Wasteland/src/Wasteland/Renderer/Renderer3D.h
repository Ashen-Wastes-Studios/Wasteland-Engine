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
        static bool IsRayTracingLowQuality();
        static void SetRayTracingLowQuality(bool enabled);
        static bool IsRayTracingAccumulate();
        static void SetRayTracingAccumulate(bool enabled);
        static uint32_t GetRayTraceTargetID();
        static void ResizeRayTraceTarget(uint32_t width, uint32_t height);

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

        static float Halton(int index, int base);
        static glm::vec2 GetCurrentJitter(int frameIndex, glm::vec2 viewportSize);

        static void GenerateMaterialMaps(const std::string &texturePath, float normalStrength, float roughBias);

    private:
        static void FlushAndReset();
    };

}