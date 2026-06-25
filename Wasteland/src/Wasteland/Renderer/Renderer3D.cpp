#include "wlpch.h"
#include "Renderer3D.h"

#include "VertexArray.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "Renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>

namespace Wasteland
{

    struct Vertex3D
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
        int EntityID;
    };

    struct RayTracingInstance
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
        float Padding[2];
    };

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

        bool RayTracingEnabled = true;
        Ref<Shader> RayTracingShader;
        Ref<Texture2D> RayTracingOutput;

        uint32_t RayTracingTexture = 0;
        uint32_t RayTracingWidth = 0;
        uint32_t RayTracingHeight = 0;

        // Path Tracing state
        uint32_t AccumulationTexture = 0;
        uint32_t FrameIndex = 0;
        int FrameIndexLocation;
        uint32_t SamplesPerPixel = 4;
        uint32_t StillFrames = 0;
        glm::mat4 LastViewProjection = glm::mat4(1.0f);

        uint32_t SceneInstanceBufferID = 0;
        int InstanceCountLocation;
        std::vector<RayTracingInstance> m_SceneInstances;
        bool m_SceneDirty = true;

        float LastCameraRotation;
        float MovementThreshold = 0.0001f;

        glm::vec3 SkyBottomColor = glm::vec3(0.1f);
        glm::vec3 SkyTopColor = glm::vec3(0.2f, 0.3f, 0.7f);

        uint32_t BloomTextureID = 0;
        uint32_t BloomTempTextureID = 0;

        glm::mat4 PrevViewProjection = glm::mat4(1.0f);
        glm::mat4 CurrentViewProjection = glm::mat4(1.0f);

        glm::vec3 CurrentCameraPosition = glm::vec3(0.0f);
        float CurrentCameraPitch = 0.0f;
        float CurrentCameraYaw = 0.0f;

        glm::vec3 LastCameraPosition = glm::vec3(0.0f);
        float LastCameraPitch = 0.0f;
        float LastCameraYaw = 0.0f;

        std::vector<uint32_t> LightIndicies;

        uint32_t AccumulationTextures[2] = {0, 0};
        uint32_t CurrentAccumulationIndex = 0;

        std::array<Plane, 6> FrustumPlanes;

        Renderer3D::Statistics Stats;
    };

    static Renderer3DData s_Data;

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
            {ShaderDataType::Int, "a_EntityID"}};

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

        s_Data.InstanceCountLocation = glGetUniformLocation(s_Data.RayTracingShader->GetRendererID(), "u_InstanceCount");
        s_Data.FrameIndexLocation = glGetUniformLocation(s_Data.RayTracingShader->GetRendererID(), "u_FrameIndex");

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
        glCreateBuffers(1, &s_Data.SceneInstanceBufferID);
        glNamedBufferData(s_Data.SceneInstanceBufferID, maxInstances * sizeof(RayTracingInstance), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_Data.SceneInstanceBufferID);

        uint32_t whiteTextureData = 0xffffffff;
        Ref<Texture2D> whiteTexture = Texture2D::Create(1, 1);
        whiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
        s_Data.TextureSlots[0] = whiteTexture;

        s_Data.m_SceneInstances.reserve(maxInstances);
    }

    void Renderer3D::Shutdown()
    {
        WL_PROFILE_FUNCTION();
        delete[] s_Data.CubeVertexBufferBase;
        delete[] s_Data.SphereVertexBufferBase;
        delete[] s_Data.SphereIndexBufferBase;

        glDeleteBuffers(1, &s_Data.SceneInstanceBufferID);
        glDeleteTextures(1, &s_Data.AccumulationTexture);
        glDeleteTextures(1, &s_Data.BloomTextureID);
        glDeleteTextures(1, &s_Data.BloomTempTextureID);
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

        s_Data.m_SceneInstances.clear();

        glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

        // Helper lambda to extract planes cleanly using glm::vec4
        auto extract = [&](int row, int sign) -> Renderer3DData::Plane
        {
            glm::vec4 planeEq;
            // Calculate the plane equation (Ax + By + Cz + D = 0)
            // Row 3 is column 3 of the matrix (index 3), Row 0/1/2 are the components
            for (int i = 0; i < 4; ++i)
                planeEq[i] = viewProj[i][3] + sign * viewProj[i][row];

            // The length of the normal (A, B, C)
            float length = glm::length(glm::vec3(planeEq));

            // Return normalized Plane struct
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

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
            // Optionally clear the texture here if you have a clear function
        }

        s_Data.CurrentCameraPosition = glm::vec3(transform[3]);
        s_Data.CurrentCameraPitch = 0.0f;
        s_Data.CurrentCameraYaw = 0.0f;

        s_Data.RayTracingShader->Bind();
        s_Data.RayTracingShader->SetMat4("u_ViewProjection", viewProj);
        s_Data.RayTracingShader->SetMat4("u_InverseViewProjection", glm::inverse(viewProj));
        s_Data.RayTracingShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);

        FlushAndReset();
    }

    void Renderer3D::BeginScene(const EditorCamera &camera)
    {
        WL_PROFILE_FUNCTION();

        s_Data.m_SceneInstances.clear();

        glm::mat4 viewProj = camera.GetViewProjection();

        s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
        s_Data.CurrentViewProjection = viewProj;

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

        s_Data.RayTracingShader->Bind();
        s_Data.RayTracingShader->SetMat4("u_ViewProjection", viewProj);
        s_Data.RayTracingShader->SetMat4("u_InverseViewProjection", glm::inverse(viewProj));
        s_Data.RayTracingShader->SetFloat3("u_CameraPosition", s_Data.CurrentCameraPosition);

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

            bool moved = (s_Data.CurrentCameraPosition != s_Data.LastCameraPosition) ||
                         (s_Data.CurrentCameraPitch != s_Data.LastCameraPitch) ||
                         (s_Data.CurrentCameraYaw != s_Data.LastCameraYaw);
            float movedValue = moved ? 1.0f : 0.0f;

            if (s_Data.m_SceneDirty)
            {
                glNamedBufferSubData(s_Data.SceneInstanceBufferID, 0,
                                     s_Data.m_SceneInstances.size() * sizeof(RayTracingInstance),
                                     s_Data.m_SceneInstances.data());

                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

                s_Data.RayTracingShader->Bind();

                glUniform1i(s_Data.InstanceCountLocation, (int)s_Data.m_SceneInstances.size());
                glUniform1i(s_Data.FrameIndexLocation, (int)s_Data.FrameIndex);

                s_Data.m_SceneDirty = false;
            }

            s_Data.LightIndicies.clear();
            for (uint32_t i = 0; i < s_Data.m_SceneInstances.size(); i++)
            {
                if (s_Data.m_SceneInstances[i].Emission.w > 0.0f)
                    s_Data.LightIndicies.push_back(i);
            }

            uint32_t readIdx = s_Data.CurrentAccumulationIndex;
            uint32_t writeIdx = 1 - s_Data.CurrentAccumulationIndex;

            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, s_Data.AccumulationTextures[readIdx]);

            s_Data.RayTracingShader->Bind();
            s_Data.RayTracingShader->SetInt("u_InstanceCount", (int)s_Data.m_SceneInstances.size());
            s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();

            glBindImageTexture(0, s_Data.RayTracingTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(1, s_Data.AccumulationTextures[writeIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_READ_WRITE, GL_RGBA32F, 0, GL_READ_WRITE);

            uint32_t workGroupsX = (s_Data.RayTracingWidth + 7) / 8;
            uint32_t workGroupsY = (s_Data.RayTracingHeight + 7) / 8;

            s_Data.RayTracingShader->SetInt("u_SamplesPerPixel", s_Data.SamplesPerPixel);
            s_Data.RayTracingShader->SetFloat("u_CameraMoved", movedValue);
            s_Data.RayTracingShader->SetInt("u_FrameIndex", s_Data.FrameIndex);
            s_Data.RayTracingShader->SetFloat3("u_SkyBottomColor", s_Data.SkyBottomColor);
            s_Data.RayTracingShader->SetFloat3("u_SkyTopColor", s_Data.SkyTopColor);

            // 1. Ray Trace & Denoise
            s_Data.RayTracingShader->SetInt("u_PassID", 0);
            s_Data.RayTracingShader->SetMat4("u_PrevViewProjection", s_Data.PrevViewProjection);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            // 2. Spatial Bilateral Blur (Added for noise reduction)
            s_Data.RayTracingShader->SetInt("u_PassID", 5);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            // 3. Bloom Threshold
            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            s_Data.RayTracingShader->SetInt("u_PassID", 1);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            // 4. Bloom Blur
            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(7, s_Data.BloomTempTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            s_Data.RayTracingShader->SetInt("u_PassID", 2);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            glBindImageTexture(7, s_Data.BloomTempTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            s_Data.RayTracingShader->SetInt("u_PassID", 3);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            // 5. Composite
            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            s_Data.RayTracingShader->SetInt("u_PassID", 4);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT /*| GL_BUFFER_UPDATE_BARRIER_BIT*/);

            s_Data.RayTracingShader->Unbind();

            if (moved)
                s_Data.FrameIndex = 0;
            else
                s_Data.FrameIndex++;

            s_Data.LastCameraPitch = s_Data.CurrentCameraPitch;
            s_Data.LastCameraYaw = s_Data.CurrentCameraYaw;
            s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
            s_Data.LastCameraPosition = s_Data.CurrentCameraPosition;
            s_Data.CurrentAccumulationIndex = 1 - s_Data.CurrentAccumulationIndex;
            s_Data.Stats.DrawCalls++;
            return;
        }

        s_Data.BasicShader->Bind();
        for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
            s_Data.TextureSlots[i]->Bind(i);

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
    uint32_t Renderer3D::GetRayTraceTargetID() { return s_Data.RayTracingOutput->GetRendererID(); }
    void Renderer3D::ResizeRayTraceTarget(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;

        // Resize Output Texture
        s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID(); // Assuming your Texture2D handles this

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
        s_Data.FrameIndex = 0; // Reset accumulation on resize
    }
    static bool IsAABBInFrustum(const std::array<Renderer3DData::Plane, 6> &planes, const glm::vec3 &min, const glm::vec3 &max, const glm::mat4 &transform)
    {
        // 1. Calculate local extents
        glm::vec3 localCenter = (min + max) * 0.5f;
        glm::vec3 localExtents = (max - min) * 0.5f;

        // 2. Transform the center to world space
        glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(localCenter, 1.0f));

        // 3. Transform the basis vectors (columns of the matrix) to get world-space axes
        // We use these to project the local extents onto the plane normals
        glm::vec3 worldAxisX = glm::vec3(transform[0]);
        glm::vec3 worldAxisY = glm::vec3(transform[1]);
        glm::vec3 worldAxisZ = glm::vec3(transform[2]);

        for (const auto &plane : planes)
        {
            // Calculate the "radius" of the box projected onto the plane normal
            // Radius = sum(|N dot Axis_i| * extent_i)
            float r = localExtents.x * glm::abs(glm::dot(plane.normal, worldAxisX)) +
                      localExtents.y * glm::abs(glm::dot(plane.normal, worldAxisY)) +
                      localExtents.z * glm::abs(glm::dot(plane.normal, worldAxisZ));

            // Distance from center to plane
            float dist = glm::dot(plane.normal, worldCenter) + plane.distance;

            // If the box is completely behind the plane, cull it
            if (dist < -r)
                return false;
        }
        return true;
    }

    uint32_t Renderer3D::GetSamplesPerPixel() { return s_Data.SamplesPerPixel; }
    void Renderer3D::SetSamplesPerPixel(uint32_t samples)
    {
        s_Data.SamplesPerPixel = glm::max(1u, samples);
        s_Data.FrameIndex = 0; // Reset accumulation when changing samples
    }

    float Renderer3D::GetMovementThreshold() { return s_Data.MovementThreshold; }
    void Renderer3D::SetMovementThreshold(float threshold) { s_Data.MovementThreshold = glm::max(0.00001f, threshold); }

    glm::vec3 Renderer3D::GetSkyBottomColor() { return s_Data.SkyBottomColor; }
    void Renderer3D::SetSkyBottomColor(const glm::vec3 &color)
    {
        s_Data.SkyBottomColor = color;
        s_Data.FrameIndex = 0; // Reset accumulation when changing sky
    }

    glm::vec3 Renderer3D::GetSkyTopColor() { return s_Data.SkyTopColor; }
    void Renderer3D::SetSkyTopColor(const glm::vec3 &color)
    {
        s_Data.SkyTopColor = color;
        s_Data.FrameIndex = 0; // Reset accumulation when changing sky
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
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

        for (int i = 0; i < 24; i++)
        {
            s_Data.CubeVertexBufferPtr->Position = glm::vec3(transform * glm::vec4(cubePositions[i], 1.0f));
            s_Data.CubeVertexBufferPtr->Color = material.Albedo;
            s_Data.CubeVertexBufferPtr->Normal = glm::normalize(normalMatrix * cubeNormals[i]);
            s_Data.CubeVertexBufferPtr->TexCoord = texCoords[i % 4];
            s_Data.CubeVertexBufferPtr->EntityID = entityID;
            s_Data.CubeVertexBufferPtr++;
        }

        s_Data.CubeIndexCount += 36;
        s_Data.CubeVertexCount += 24;
        s_Data.Stats.QuadCount += 6;

        if (s_Data.RayTracingEnabled)
        {
            RayTracingInstance instance = {};

            instance.WorldTransform = transform;
            instance.InvTransform = glm::inverse(transform);
            instance.Albedo = material.Albedo;
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 0.0f, 0.0f);
            instance.Min = glm::vec4(-0.5, -0.5, -0.5, 1.0f);
            instance.Max = glm::vec4(0.5, 0.5, 0.5, 1.0f);
            instance.Emission = glm::vec4(material.EmissionColor.x,
                                          material.EmissionColor.y,
                                          material.EmissionColor.z,
                                          material.EmissionIntensity);

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
        float sectorStep = 2 * glm::pi<float>() / sectors;
        float stackStep = glm::pi<float>() / stacks;
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

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
                s_Data.SphereVertexBufferPtr->EntityID = entityID;
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
            instance.InvTransform = glm::inverse(transform);
            instance.Albedo = material.Albedo;
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 1.0f, radius);
            instance.Min = glm::vec4(-radius, -radius, -radius, 1.0f);
            instance.Max = glm::vec4(radius, radius, radius, 1.0f);
            instance.Emission = glm::vec4(material.EmissionColor.x,
                                          material.EmissionColor.y,
                                          material.EmissionColor.z,
                                          material.EmissionIntensity);

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