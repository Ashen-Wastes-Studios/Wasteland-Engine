#include "wlpch.h"
#include "Buffer.h"

#include "Renderer.h"

#include "Platform/OpenGL/OpenGLBuffer.h"
#include "Platform/NVRHI/NVRHIBuffer.h"

namespace Wasteland {

	Ref<VertexBuffer> VertexBuffer::Create(uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLVertexBuffer>(size);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return CreateRef<NVRHIVertexBuffer>(size);
		}

		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

	Ref<VertexBuffer> VertexBuffer::Create(float* vertices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLVertexBuffer>(vertices, size);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return CreateRef<NVRHIVertexBuffer>(vertices, size);
		}

		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

	Ref<IndexBuffer> IndexBuffer::Create(uint32_t* indices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
		case RendererAPI::API::OpenGL: return CreateRef<OpenGLIndexBuffer>(indices, size);
		case RendererAPI::API::NVRHI_DX11:
		case RendererAPI::API::NVRHI_DX12:
		case RendererAPI::API::NVRHI_Vulkan:
			return CreateRef<NVRHIIndexBuffer>(indices, size);
		}

		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

}
