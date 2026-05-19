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
        float TexIndex;
        float TilingFactor;
        int EntityID;
    };

    struct RayTracingInstance 
    {
        glm::mat4 InvTransform;      // 64 bytes
        glm::mat4 WorldTransform;    // 64 bytes
        glm::vec4 Color;             // 16 bytes
        glm::vec4 Properties;        // 16 bytes -> x = Type, y = Radius, z = EntityID, w = Unused
    }; // Total size = 96 bytes (Perfect 16-byte alignment stride for std430)

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

        bool RayTracingEnabled = false;
        Ref<Shader> RayTracingShader;
        Ref<Texture2D> RayTracingOutput;

        uint32_t SceneInstanceBufferID = 0;
        std::vector<RayTracingInstance> m_SceneInstances;

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

        s_Data.RayTracingOutput = Texture2D::Create(1280, 720); 
        s_Data.RayTracingShader = Shader::Create("assets/shaders/Renderer3D_RayTracing.glsl");

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

            glNamedBufferSubData(s_Data.SceneInstanceBufferID, 0,
                                  s_Data.m_SceneInstances.size() * sizeof(RayTracingInstance), 
                                  s_Data.m_SceneInstances.data());
            
            uint32_t textureID = s_Data.RayTracingOutput->GetRendererID();
            glBindImageTexture(0, textureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

            s_Data.RayTracingShader->Bind();
            s_Data.RayTracingShader->SetInt("u_InstanceCount", (int)s_Data.m_SceneInstances.size());

            uint32_t workGroupsX = (s_Data.RayTracingOutput->GetWidth() + 7) / 8;
            uint32_t workGroupsY = (s_Data.RayTracingOutput->GetHeight() + 7) / 8;

            glDispatchCompute(workGroupsX, workGroupsY, 1);

            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            s_Data.RayTracingShader->Unbind();

            s_Data.Stats.DrawCalls++;
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
        uint32_t textureID = s_Data.RayTracingOutput->GetRendererID();
        if (textureID)
        {
            glDeleteTextures(1, &textureID);
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Compute shaders writing to rgba32f require glTexStorage2D, NOT glTexImage2D!
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, width, height);

        glBindTexture(GL_TEXTURE_2D, 0); 
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

    void Renderer3D::DrawCube(const glm::vec3 &position, const glm::vec3 &size, const glm::vec4 &color)
    {
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), size);
        DrawCube(transform, color, 0, 1.0f, -1);
    }

    void Renderer3D::DrawCube(const glm::mat4 &transform, const glm::vec4 &color, int textureIndex, float tilingFactor, int entityID)
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
            s_Data.CubeVertexBufferPtr->Color = color;
            s_Data.CubeVertexBufferPtr->Normal = glm::normalize(normalMatrix * cubeNormals[i]); 
            s_Data.CubeVertexBufferPtr->TexCoord = texCoords[i % 4];
            s_Data.CubeVertexBufferPtr->TexIndex = (float)textureIndex;
            s_Data.CubeVertexBufferPtr->TilingFactor = tilingFactor;
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
            instance.Color = color;
            
            // x = Type (0.0 = Cube), y = Radius (0.0 for cube), z = EntityID
            instance.Properties = glm::vec4(0.0f, 0.0f, (float)entityID, 0.0f);
            
            s_Data.m_SceneInstances.push_back(instance);
            return;
        }
    }

    void Renderer3D::DrawSphere(const glm::vec3 &position, float radius, const glm::vec4 &color)
    {
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
        DrawSphere(transform, color, radius, 20, 20, 0, 1.0f, -1);
    }

    void Renderer3D::DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, int textureIndex, float tilingFactor, int entityID)
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
                s_Data.SphereVertexBufferPtr->Color = color;
                s_Data.SphereVertexBufferPtr->Normal = glm::normalize(normalMatrix * glm::normalize(glm::vec3(x, y, z)));
                s_Data.SphereVertexBufferPtr->TexCoord = glm::vec2((float)j / sectors, (float)i / stacks);
                s_Data.SphereVertexBufferPtr->TexIndex = (float)textureIndex;
                s_Data.SphereVertexBufferPtr->TilingFactor = tilingFactor;
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
            instance.Color = color;
            
            // x = Type (1.0 = Sphere), y = Base Radius, z = EntityID
            instance.Properties = glm::vec4(1.0f, radius, (float)entityID, 0.0f);
            
            s_Data.m_SceneInstances.push_back(instance);
            return;
        }
    }

    void Renderer3D::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
    {
        WL_PROFILE_FUNCTION();
        Renderer::Submit(shader, vertexArray, transform);
    }
}