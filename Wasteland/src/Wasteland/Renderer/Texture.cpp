#include "wlpch.h"
#include "Texture.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLTexture.h"
#include "Platform/NVRHI/NVRHITexture.h"

namespace Wasteland {

	Ref<Texture2D> Texture2D::Create(uint32_t width, uint32_t height)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLTexture2D>(width, height);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return std::make_shared<NVRHITexture2D>(width, height);
		}

		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<Texture2D> Texture2D::Create(const std::string& path)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLTexture2D>(path);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return std::make_shared<NVRHITexture2D>(path);
		}

		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
