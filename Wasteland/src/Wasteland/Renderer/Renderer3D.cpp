#include "wlpch.h"
#include "Renderer3D.h"

#include "VertexArray.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "Renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>

namespace Wasteland {

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
    }; 

    struct Renderer3DData
    {
        static const uint32_t MaxVertices = 100000; 
        static const uint32_t MaxIndices = MaxVertices * 3; 
        static const uint32_t MaxTextureSlots = 32; 

        Ref<VertexArray> CubeVertexArray;
        Ref<VertexBuffer> CubeVertexBuffer;
        uint32_t CubeVertexCount = 0;
        uint32_t CubeIndexCount = 0;
        Vertex3D* CubeVertexBufferBase = nullptr;
        Vertex3D* CubeVertexBufferPtr = nullptr;

        Ref<VertexArray> SphereVertexArray;
        Ref<VertexBuffer> SphereVertexBuffer;
        Ref<IndexBuffer> SphereIndexBuffer; 
        uint32_t SphereVertexCount = 0;
        uint32_t SphereIndexCount = 0;
        Vertex3D* SphereVertexBufferBase = nullptr;
        Vertex3D* SphereVertexBufferPtr = nullptr;
        uint32_t* SphereIndexBufferBase = nullptr;
        uint32_t* SphereIndexBufferPtr = nullptr;

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
        uint32_t SamplesPerPixel = 1;
        uint32_t StillFrames = 0;
        glm::mat4 LastViewProjection = glm::mat4(1.0f);

        uint32_t SceneInstanceBufferID = 0;
        std::vector<RayTracingInstance> m_SceneInstances;
        bool m_SceneDirty = true;

        glm::vec3 LastCameraPosition = glm::vec3(0.0f);
        glm::vec2 LastCameraRotation = glm::vec2(0.0f);
        float MovementThreshold = 0.0001f;

        uint32_t BloomTextureID = 0;

        glm::mat4 PrevViewProjection = glm::mat4(1.0f);
        glm::mat4 CurrentViewProjection = glm::mat4(1.0f);

        Renderer3D::Statistics Stats;
    };

    static Renderer3DData s_Data;

    void Renderer3D::Init()
    {
        WL_PROFILE_FUNCTION();

        BufferLayout layout = {
            { ShaderDataType::Float3, "a_Position"     },
            { ShaderDataType::Float4, "a_Color"        },
            { ShaderDataType::Float3, "a_Normal"       }, 
            { ShaderDataType::Float2, "a_TexCoord"     },
            { ShaderDataType::Float,  "a_TexIndex"     },
            { ShaderDataType::Float,  "a_TilingFactor" },
            { ShaderDataType::Int,    "a_EntityID"     }
        };

        // --- CUBE SETUP ---
        s_Data.CubeVertexArray = VertexArray::Create();
        s_Data.CubeVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(Vertex3D));
        s_Data.CubeVertexBuffer->SetLayout(layout);
        s_Data.CubeVertexArray->AddVertexBuffer(s_Data.CubeVertexBuffer);
        s_Data.CubeVertexBufferBase = new Vertex3D[s_Data.MaxVertices];

        uint32_t* cubeIndices = new uint32_t[s_Data.MaxIndices];
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

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AccumulationTexture);
        glTextureStorage2D(s_Data.AccumulationTexture, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTextureID);
        glTextureStorage2D(s_Data.BloomTextureID, 1, GL_RGBA32F, s_Data.RayTracingWidth, s_Data.RayTracingHeight);

        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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

        s_Data.CubeVertexArray = nullptr;
        s_Data.CubeVertexBuffer = nullptr;
        s_Data.SphereVertexArray = nullptr;
        s_Data.SphereVertexBuffer = nullptr;
        s_Data.SphereIndexBuffer = nullptr;
        s_Data.BasicShader = nullptr;
        s_Data.RayTracingShader = nullptr;
        s_Data.RayTracingOutput = nullptr;
    }

    void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        WL_PROFILE_FUNCTION();
        glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        if (viewProj != s_Data.LastViewProjection)
        {
            s_Data.FrameIndex = 0;
            s_Data.LastViewProjection = viewProj;
            // Optionally clear the texture here if you have a clear function
        }

        s_Data.RayTracingShader->Bind();
        s_Data.RayTracingShader->SetMat4("u_InverseViewProjection", glm::inverse(viewProj));
        s_Data.RayTracingShader->SetFloat3("u_CameraPosition", glm::vec3(transform[3])); 

        FlushAndReset();
    }

    void Renderer3D::BeginScene(const EditorCamera& camera)
    {
        WL_PROFILE_FUNCTION();
        glm::mat4 viewProj = camera.GetViewProjection();

        s_Data.BasicShader->Bind();
        s_Data.BasicShader->SetMat4("u_ViewProjection", viewProj);

        s_Data.RayTracingShader->Bind();
        s_Data.RayTracingShader->SetMat4("u_InverseViewProjection", glm::inverse(viewProj));
        s_Data.RayTracingShader->SetFloat3("u_CameraPosition", camera.GetPosition());

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
            if (s_Data.m_SceneInstances.empty()) return;

            EditorCamera editorCamera;

            bool moved = CheckForCameraMovement(editorCamera); // Compare camera pos/rotation to last frame

            if (moved)
            {
                s_Data.FrameIndex = 0; // Reset accumulation
                s_Data.SamplesPerPixel = 1; // Go into "Fast/Draft" mode
                s_Data.StillFrames = 0;
            }
            else
            {
                s_Data.StillFrames++;
                s_Data.SamplesPerPixel = (s_Data.StillFrames > 120) ? 8 : 1;
            }

            // Only upload if something changed
            if (s_Data.m_SceneDirty) 
            {
                glNamedBufferSubData(s_Data.SceneInstanceBufferID, 0, 
                                    s_Data.m_SceneInstances.size() * sizeof(RayTracingInstance), 
                                    s_Data.m_SceneInstances.data());
                s_Data.m_SceneDirty = false; // Reset flag
            }

            glNamedBufferSubData(s_Data.SceneInstanceBufferID, 0, 
                                s_Data.m_SceneInstances.size() * sizeof(RayTracingInstance), 
                                s_Data.m_SceneInstances.data());

            s_Data.RayTracingShader->Bind();
            s_Data.RayTracingShader->SetInt("u_InstanceCount", (int)s_Data.m_SceneInstances.size());
            s_Data.RayTracingShader->SetInt("u_FrameIndex", (int)s_Data.FrameIndex);

            s_Data.RayTracingTexture = s_Data.RayTracingOutput->GetRendererID();

            glBindImageTexture(0, s_Data.RayTracingTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            
            glBindImageTexture(1, s_Data.AccumulationTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

            glBindImageTexture(2, s_Data.BloomTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

            uint32_t workGroupsX = (s_Data.RayTracingWidth + 7) / 8;
            uint32_t workGroupsY = (s_Data.RayTracingHeight + 7) / 8;

            s_Data.RayTracingShader->Bind();
            s_Data.RayTracingShader->SetInt("u_SamplesPerPixel", s_Data.SamplesPerPixel);
            s_Data.RayTracingShader->SetInt("u_FrameIndex", s_Data.FrameIndex);

            // Ray Trace & Denoise
            s_Data.RayTracingShader->SetInt("u_PassID", 0);
            s_Data.RayTracingShader->SetMat4("u_PrevViewProjection", s_Data.PrevViewProjection);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            // Bloom Threshold (Extract bright pixels)
            s_Data.RayTracingShader->SetInt("u_PassID", 1);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            // Bloom Blur (Blur the extracted buffer)
            s_Data.RayTracingShader->SetInt("u_PassID", 2);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            // Composite (Add glow back to main image)
            s_Data.RayTracingShader->SetInt("u_PassID", 3);
            glDispatchCompute(workGroupsX, workGroupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            s_Data.RayTracingShader->Unbind();

            if (&editorCamera.GetPosition()) 
            {
                s_Data.FrameIndex = 0; // Reset accumulation
            }
            else 
            {
                s_Data.FrameIndex++;   // Accumulate
            }

            s_Data.PrevViewProjection = s_Data.CurrentViewProjection;
            s_Data.Stats.DrawCalls++;
            s_Data.FrameIndex++;
            return;
        }

        s_Data.BasicShader->Bind();
        for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
            s_Data.TextureSlots[i]->Bind(i);

        // Draw Cubes
        if (s_Data.CubeIndexCount)
        {
            uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.CubeVertexBufferPtr - (uint8_t*)s_Data.CubeVertexBufferBase);
            s_Data.CubeVertexBuffer->SetData(s_Data.CubeVertexBufferBase, dataSize);

            RenderCommand::DrawIndexed(s_Data.CubeVertexArray, s_Data.CubeIndexCount);
            s_Data.Stats.DrawCalls++;
        }

        // Draw Spheres
        if (s_Data.SphereIndexCount)
        {
            uint32_t vertexDataSize = (uint32_t)((uint8_t*)s_Data.SphereVertexBufferPtr - (uint8_t*)s_Data.SphereVertexBufferBase);
            s_Data.SphereVertexBuffer->SetData(s_Data.SphereVertexBufferBase, vertexDataSize);

            uint32_t indexDataSize = (uint32_t)(s_Data.SphereIndexBufferPtr - s_Data.SphereIndexBufferBase) * sizeof(uint32_t);
            s_Data.SphereIndexBuffer->SetData(s_Data.SphereIndexBufferBase, indexDataSize);

            RenderCommand::DrawIndexed(s_Data.SphereVertexArray, s_Data.SphereIndexCount);
            s_Data.Stats.DrawCalls++;
        }
    }

    void Renderer3D::ResetStats() { s_Data.Stats.DrawCalls = 0; s_Data.Stats.QuadCount = 0; }
    Renderer3D::Statistics Renderer3D::GetStats() { return s_Data.Stats; }
    bool Renderer3D::IsRayTracingEnabled() { return s_Data.RayTracingEnabled; }
    void Renderer3D::SetRayTracingEnabled(bool enabled) { s_Data.RayTracingEnabled = enabled; }
    uint32_t Renderer3D::GetRayTraceTargetID() { return s_Data.RayTracingOutput->GetRendererID(); }
    void Renderer3D::ResizeRayTraceTarget(uint32_t width, uint32_t height) 
    { 
        if (width == 0 || height == 0) return;

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
        if (s_Data.AccumulationTexture) glDeleteTextures(1, &s_Data.AccumulationTexture);
        
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.AccumulationTexture);
        glTextureStorage2D(s_Data.AccumulationTexture, 1, GL_RGBA32F, width, height);
        
        glTextureParameteri(s_Data.AccumulationTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.AccumulationTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Resize Bloom Texture
        if (s_Data.BloomTextureID) glDeleteTextures(1, &s_Data.BloomTextureID);
    
        glCreateTextures(GL_TEXTURE_2D, 1, &s_Data.BloomTextureID);
        glTextureStorage2D(s_Data.BloomTextureID, 1, GL_RGBA32F, width, height);
        
        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(s_Data.BloomTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        s_Data.RayTracingWidth = width;
        s_Data.RayTracingHeight = height;
        s_Data.FrameIndex = 0; // Reset accumulation on resize
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

    bool Renderer3D::CheckForCameraMovement(const EditorCamera &editorCamera)
    {
        glm::vec3 currentPos = editorCamera.GetPosition();
        glm::vec2 currentRot = glm::vec2(editorCamera.GetPitch(), editorCamera.GetYaw());

        // Calculate the difference between current and last frame
        float posDelta = glm::distance(currentPos, s_Data.LastCameraPosition);
        
        // For rotation, we just check if any component changed
        // (Euler angles are tricky, but this works for basic movement)
        float rotDelta = glm::distance(currentRot, s_Data.LastCameraRotation);

        // Check if the delta exceeds our tolerance
        bool moved = (posDelta > s_Data.MovementThreshold) || (rotDelta > s_Data.MovementThreshold);

        if (moved)
        {
            // Update the "Last" state for the next frame
            s_Data.LastCameraPosition = currentPos;
            s_Data.LastCameraRotation = currentRot;
        }

        return moved;
    }

    void Renderer3D::DrawCube(const glm::mat4& transform, const glm::vec4& color, MaterialComponent& material, int entityID)
    {
        if (s_Data.CubeIndexCount + 36 >= s_Data.MaxIndices)
        {
            Flush();
            FlushAndReset();
        }

        static const glm::vec3 cubePositions[24] = {
            { -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f }, {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f }, 
            {  0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f }, {  0.5f,  0.5f, -0.5f }, 
            { -0.5f,  0.5f,  0.5f }, {  0.5f,  0.5f,  0.5f }, {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f }, 
            { -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f,  0.5f }, { -0.5f, -0.5f,  0.5f }, 
            {  0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f, -0.5f }, {  0.5f,  0.5f, -0.5f }, {  0.5f,  0.5f,  0.5f }, 
            { -0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f, -0.5f }  
        };

        static const glm::vec3 cubeNormals[24] = {
            {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f,  1.0f }, 
            {  0.0f,  0.0f, -1.0f }, { -0.0f,  0.0f, -1.0f }, { -0.0f,  0.0f, -1.0f }, {  0.0f,  0.0f, -1.0f }, 
            {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f }, 
            {  0.0f, -1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f }, 
            {  1.0f,  0.0f,  0.0f }, {  1.0f,  0.0f,  0.0f }, {  1.0f,  0.0f,  0.0f }, {  1.0f,  0.0f,  0.0f }, 
            { -1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f, -0.0f }  
        };

        static const glm::vec2 texCoords[4] = { {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f} };
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
            RayTracingInstance instance;
            // Calculate the inverse matrix so the compute shader can trace a local axis-aligned unit cube
            instance.WorldTransform = transform;
            instance.InvTransform = glm::inverse(transform);

            instance.Albedo = material.Albedo;
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 0.0f, 0.0f);

            instance.Min = glm::vec4(-0.5f, -0.5f, -0.5f, 1.0f); 
            instance.Max = glm::vec4( 0.5f,  0.5f,  0.5f, 1.0f);

            instance.Emission = glm::vec4(material.EmissionColor.x, 
                                      material.EmissionColor.y, 
                                      material.EmissionColor.z, 
                                      material.EmissionIntensity);
            
            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    void Renderer3D::DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, MaterialComponent& material, int entityID)
    {
        uint32_t vertexCount = (stacks + 1) * (sectors + 1);
        uint32_t indexCount = 0;
        for (int i = 0; i < stacks; ++i) {
            if (i != 0) indexCount += sectors * 3;
            if (i != (stacks - 1)) indexCount += sectors * 3;
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
            RayTracingInstance instance;
            // Calculate the inverse matrix so the compute shader can trace a local unit sphere at (0,0,0)
            instance.WorldTransform = transform;
            instance.InvTransform = glm::inverse(transform);

            instance.Albedo = material.Albedo;
            instance.MaterialParams = glm::vec4(material.Metallic, material.Roughness, 1.0f, radius);

            instance.Min = glm::vec4(-radius, -radius, -radius, 1.0f);
            instance.Max = glm::vec4( radius,  radius,  radius, 1.0f);

            instance.Emission = glm::vec4(material.EmissionColor.x, 
                                      material.EmissionColor.y, 
                                      material.EmissionColor.z, 
                                      material.EmissionIntensity);
            
            s_Data.m_SceneInstances.push_back(instance);
            s_Data.m_SceneDirty = true;
            return;
        }
    }

    void Renderer3D::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
    {
        WL_PROFILE_FUNCTION();
        Renderer::Submit(shader, vertexArray, transform);
    }
}