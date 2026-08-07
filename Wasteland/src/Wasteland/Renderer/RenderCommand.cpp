#include "wlpch.h"
#include "RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/NVRHI/NVRHIRendererAPI.h"

namespace Wasteland {

	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI;

	void RenderCommand::SetAPI(RendererAPI::API api)
	{
		// Clean up old API
		if (s_RendererAPI)
		{
			delete s_RendererAPI;
			s_RendererAPI = nullptr;
		}

		// Create new API
		switch (api)
		{
		case RendererAPI::API::None:
			s_RendererAPI = nullptr;
			break;
		case RendererAPI::API::OpenGL:
			s_RendererAPI = new OpenGLRendererAPI;
			break;
		case RendererAPI::API::NVRHI_DX11:
		case RendererAPI::API::NVRHI_DX12:
		case RendererAPI::API::NVRHI_Vulkan:
			s_RendererAPI = new NVRHIRendererAPI(api);
			break;
		}

		RendererAPI::s_API = api;
	}

}