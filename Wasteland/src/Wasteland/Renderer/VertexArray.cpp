#include "wlpch.h"
#include "VertexArray.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"
#include "Platform/NVRHI/NVRHIVertexArray.h"

namespace Wasteland {

	Ref<VertexArray> VertexArray::Create()
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLVertexArray>();
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return std::make_shared<NVRHIVertexArray>();
		}

		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
