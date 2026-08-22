#pragma once

#include <glm/glm.hpp>
#include <vector>

#include "VertexArray.h"
#include "Texture.h"

namespace Wasteland {

	class RenderCommand; // Forward declaration

	class RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1,
			NVRHI_DX11 = 2,
			NVRHI_DX12 = 3,
			NVRHI_Vulkan = 4
		};
	public:
		virtual ~RendererAPI() = default;

		virtual void Init() = 0;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) = 0;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) = 0;

		virtual void SetLineWidth(float width) = 0;
		virtual void SetActiveTextures(const std::vector<Ref<Texture2D>>& textures) {}

		inline static API GetAPI() { return s_API; }
	private:
		friend class RenderCommand;
		static API s_API;
	};

}