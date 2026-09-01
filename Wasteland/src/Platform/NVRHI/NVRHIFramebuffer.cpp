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

		if (m_Specification.Width == 0 || m_Specification.Height == 0)
		{
			WL_CORE_WARN("NVRHIFramebuffer: Skipping Invalidate with zero size ({0}x{1})", m_Specification.Width, m_Specification.Height);
			return;
		}

		// Clear old resources
		m_Framebuffer = nullptr;
		m_ColorTextures.clear();
		m_DepthTexture = nullptr;

		// Create attachments
		for (const auto &attachmentSpec : m_Specification.Attachments.Attachments)
		{
			if (attachmentSpec.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8)
			{
				// Create depth-stencil texture
				// D3D11/D3D12 note: A depth texture that is both DSV and SRV must be typeless
				// (R24G8_TYPELESS + R24_UNORM_X8_TYPELESS / D24_UNORM_S8_UINT). NVRHI only uses
				// the typeless resource format when desc.isTypeless == true. Here we do NOT
				// need to sample depth in the editor viewport, so keep it DSV-only to avoid
				// the E_INVALIDARG from CreateTexture2D (D24 + BIND_SHADER_RESOURCE without typeless).
				nvrhi::TextureDesc desc;
				desc.width = m_Specification.Width;
				desc.height = m_Specification.Height;
				desc.depth = 1;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.format = FramebufferTextureFormatToNVRHI(attachmentSpec.TextureFormat);
				desc.isRenderTarget = true;
				desc.isShaderResource = false; // critical: no SRV for depth without typeless
				desc.isUAV = false;
				desc.isTypeless = false;
				desc.sampleCount = m_Specification.Samples > 0 ? m_Specification.Samples : 1;
				desc.debugName = "Framebuffer_Depth";
				desc.initialState = nvrhi::ResourceStates::DepthWrite;
				desc.keepInitialState = true;

				m_DepthTexture = device->createTexture(desc);
				if (!m_DepthTexture)
				{
					WL_CORE_ERROR("NVRHIFramebuffer: Failed to create depth texture ({0}x{1} format D24S8)", desc.width, desc.height);
				}
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
				desc.isShaderResource = true; // needed for ImGui viewport + entity picking readback
				desc.isUAV = false;
				desc.isTypeless = false;
				desc.sampleCount = m_Specification.Samples > 0 ? m_Specification.Samples : 1;
				desc.debugName = "Framebuffer_Color_" + std::to_string(m_ColorTextures.size());
				desc.initialState = nvrhi::ResourceStates::RenderTarget;
				desc.keepInitialState = true;

				auto texture = device->createTexture(desc);
				if (!texture)
				{
					WL_CORE_ERROR("NVRHIFramebuffer: Failed to create color texture {0} ({1}x{2})", m_ColorTextures.size(), desc.width, desc.height);
					continue;
				}
				m_ColorTextures.push_back(texture);
			}
		}

		// Create framebuffer — only add valid attachments
		nvrhi::FramebufferDesc fbDesc;

		for (const auto &colorTexture : m_ColorTextures)
		{
			if (colorTexture)
				fbDesc.addColorAttachment(colorTexture);
		}
		if (m_DepthTexture)
		{
			fbDesc.setDepthAttachment(m_DepthTexture);
		}

		m_Framebuffer = device->createFramebuffer(fbDesc);
		if (!m_Framebuffer)
		{
			WL_CORE_ERROR("NVRHIFramebuffer: Failed to create framebuffer ({0} color attachments, depth={1})", m_ColorTextures.size(), m_DepthTexture ? "yes" : "no");
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
