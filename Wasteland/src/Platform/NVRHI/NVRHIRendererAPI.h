#pragma once

#include "Wasteland/Renderer/RendererAPI.h"
#include "Wasteland/Renderer/Shader.h"
#include <nvrhi/nvrhi.h>
#include <unordered_map>

namespace Wasteland {

	class NVRHIContext;
	class VertexArray;

	class NVRHIRendererAPI : public RendererAPI
	{
	public:
		NVRHIRendererAPI(RendererAPI::API api);
		virtual ~NVRHIRendererAPI() override;

		virtual void Init() override;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
		virtual void SetClearColor(const glm::vec4& color) override;
		virtual void Clear() override;

		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) override;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) override;
		virtual void SetLineWidth(float width) override;

		// NVRHI-specific methods
		NVRHIContext* GetContext() const { return m_Context; }
		void SetContext(NVRHIContext* context);

		// Static accessor for current context (used by NVRHI resource implementations)
		static NVRHIContext* GetCurrentContext() { return s_CurrentContext; }

		// Track current shader for pipeline creation
		void SetCurrentShader(const Ref<Shader>& shader) { m_CurrentShader = shader; }

		// Resource cleanup
		void ClearCachedResources();

	private:
		// Pipeline cache key (use pointer addresses)
		struct PipelineKey
		{
			const void* shaderPtr;
			const void* vertexArrayPtr;
			
			bool operator==(const PipelineKey& other) const
			{
				return shaderPtr == other.shaderPtr && vertexArrayPtr == other.vertexArrayPtr;
			}
		};

		struct PipelineKeyHash
		{
			size_t operator()(const PipelineKey& key) const
			{
				return std::hash<const void*>()(key.shaderPtr) ^ (std::hash<const void*>()(key.vertexArrayPtr) << 1);
			}
		};

		nvrhi::GraphicsPipelineHandle GetOrCreatePipeline(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray);
		nvrhi::BindingSetHandle GetOrCreateBindingSet(const Ref<Shader>& shader);

		RendererAPI::API m_API;
		glm::vec4 m_ClearColor;
		NVRHIContext* m_Context = nullptr;
		Ref<Shader> m_CurrentShader = nullptr;
		
		static NVRHIContext* s_CurrentContext;

		// Pipeline cache
		std::unordered_map<PipelineKey, nvrhi::GraphicsPipelineHandle, PipelineKeyHash> m_PipelineCache;
		
		// Binding set cache
		std::unordered_map<const void*, nvrhi::BindingSetHandle> m_BindingSetCache;
	};

}
