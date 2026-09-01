#include "wlpch.h"
#include "NVRHIFramebuffer.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland
{

	// Helper to get the current NVRHI device
	static nvrhi::IDevice *GetDevice()
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context || context->GetAPI() != RendererAPI::GetAPI())
		{
			WL_CORE_ERROR("NVRHIFramebuffer: renderer API and NVRHI context do not match");
			return nullptr;
		}
		return context->GetDevice();
	}

	// Helper to convert framebuffer texture format to NVRHI format
	static nvrhi::Format FramebufferTextureFormatToNVRHI(FramebufferTextureFormat format)
	{
		switch (format)
		{
		case FramebufferTextureFormat::RGBA8:
			return nvrhi::Format::SRGBA8_UNORM;
		case FramebufferTextureFormat::RED_INTEGER:
			return nvrhi::Format::R32_SINT;
		case FramebufferTextureFormat::DEPTH24STENCIL8:
			return nvrhi::Format::D24S8;
		default:
			WL_CORE_ASSERT(false, "Unknown FramebufferTextureFormat!");
			return nvrhi::Format::UNKNOWN;
		}
	}

	NVRHIFramebuffer::NVRHIFramebuffer(const FramebufferSpecification &spec)
		: m_Specification(spec)
	{
		Invalidate();
	}

	NVRHIFramebuffer::~NVRHIFramebuffer()
	{
		m_Framebuffer = nullptr;
		m_ColorTextures.clear();
		m_DepthTexture = nullptr;
	}

	void NVRHIFramebuffer::Invalidate()
	{
		nvrhi::IDevice *device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIFramebuffer: No NVRHI device available");
			return;
		}

		// Clear old resources
		m_Framebuffer = nullptr;
		m_ColorTextures.clear();
		m_DepthTexture = nullptr;

		// Create color attachments
		for (const auto &attachmentSpec : m_Specification.Attachments.Attachments)
		{
			if (attachmentSpec.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8)
			{
				// Create depth-stencil texture
				nvrhi::TextureDesc desc;
				desc.width = m_Specification.Width;
				desc.height = m_Specification.Height;
				desc.depth = 1;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.format = FramebufferTextureFormatToNVRHI(attachmentSpec.TextureFormat);
				desc.isRenderTarget = true;
				desc.debugName = "Framebuffer_Depth";
				desc.initialState = nvrhi::ResourceStates::DepthWrite;
				desc.keepInitialState = true;

				m_DepthTexture = device->createTexture(desc);
			}
			else
			{
				// Create color texture
				nvrhi::TextureDesc desc;
				desc.width = m_Specification.Width;
				desc.height = m_Specification.Height;
				desc.depth = 1;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.format = FramebufferTextureFormatToNVRHI(attachmentSpec.TextureFormat);
				desc.isRenderTarget = true;
				desc.debugName = "Framebuffer_Color_" + std::to_string(m_ColorTextures.size());
				desc.initialState = nvrhi::ResourceStates::RenderTarget;
				desc.keepInitialState = true;

				auto texture = device->createTexture(desc);
				m_ColorTextures.push_back(texture);
			}
		}

		// Create framebuffer
		nvrhi::FramebufferDesc fbDesc;

		for (const auto &colorTexture : m_ColorTextures)
		{
			fbDesc.addColorAttachment(colorTexture);
		}

		if (m_DepthTexture)
		{
			fbDesc.setDepthAttachment(m_DepthTexture);
		}

		m_Framebuffer = device->createFramebuffer(fbDesc);
		if (!m_Framebuffer)
		{
			WL_CORE_ERROR("NVRHIFramebuffer: Failed to create framebuffer");
		}
	}

	void NVRHIFramebuffer::Bind()
	{
		// Note: In NVRHI, framebuffers are set via GraphicsState
		// This is handled in the Renderer implementation
		// For now, this is a no-op
	}

	void NVRHIFramebuffer::Unbind()
	{
		// No-op in NVRHI
	}

	void NVRHIFramebuffer::Resize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 ||
			width == m_Specification.Width && height == m_Specification.Height)
			return;

		m_Specification.Width = width;
		m_Specification.Height = height;

		Invalidate();
	}

	int NVRHIFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		WL_CORE_WARN("NVRHIFramebuffer::ReadPixel not yet implemented");
		// TODO: Implement pixel readback using staging texture
		return -1;
	}

	void NVRHIFramebuffer::ClearAttachment(uint32_t attachmentIndex, int value)
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHIFramebuffer::ClearAttachment: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList *cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHIFramebuffer::ClearAttachment: No command list available");
			return;
		}

		if (attachmentIndex < m_ColorTextures.size())
		{
			nvrhi::Color clearColor;
			clearColor.r = clearColor.g = clearColor.b = clearColor.a = static_cast<float>(value);
			cmd->clearTextureFloat(m_ColorTextures[attachmentIndex], nvrhi::TextureSubresourceSet(), clearColor);
		}
	}

	uint32_t NVRHIFramebuffer::GetColorAttachmentRendererID(uint32_t index) const
	{
		if (index < m_ColorTextures.size())
		{
			return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_ColorTextures[index].Get()));
		}
		return 0;
	}

	nvrhi::TextureHandle NVRHIFramebuffer::GetColorAttachment(uint32_t index) const
	{
		if (index < m_ColorTextures.size())
		{
			return m_ColorTextures[index];
		}
		return nullptr;
	}

}
