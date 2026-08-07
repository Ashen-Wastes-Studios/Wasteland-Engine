#include "wlpch.h"
#include "Framebuffer.h"

#include "Wasteland/Renderer/Renderer.h"

#include "Platform/OpenGL/OpenGLFramebuffer.h"
#include "Platform/NVRHI/NVRHIFramebuffer.h"

namespace Wasteland {

	Ref<Framebuffer> Framebuffer::Create(const FramebufferSpecification& spec)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLFramebuffer>(spec);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return CreateRef<NVRHIFramebuffer>(spec);
		}

		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
