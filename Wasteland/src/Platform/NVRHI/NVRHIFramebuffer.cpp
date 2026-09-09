#include "wlpch.h"
#include "NVRHIFramebuffer.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <cstring>

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
		// In NVRHI, framebuffers are set via GraphicsState. Route that through
		// the context: while bound, GetCurrentFramebuffer() returns this
		// framebuffer and scene draws/clears target it instead of the swapchain.
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (context)
			context->SetRenderTargetFramebuffer(m_Framebuffer);
	}

	void NVRHIFramebuffer::Unbind()
	{
		auto *context = NVRHIRendererAPI::GetCurrentContext();
		if (context)
			context->SetRenderTargetFramebuffer(nullptr); // back to swapchain
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
		if (attachmentIndex >= m_ColorTextures.size() || !m_ColorTextures[attachmentIndex])
			return -1;

		auto *context = NVRHIRendererAPI::GetCurrentContext();
		nvrhi::IDevice *device = context ? context->GetDevice() : nullptr;
		if (!device)
			return -1;

		const nvrhi::TextureDesc &texDesc = m_ColorTextures[attachmentIndex]->getDesc();
		if (x < 0 || y < 0 || x >= (int)texDesc.width || y >= (int)texDesc.height)
			return -1;

		if (context->GetAPI() == RendererAPI::API::NVRHI_DX11)
		{
			// D3D11: NVRHI's backend does not support deferred command lists
			// (createCommandList errors and returns null), so do the readback on
			// the raw immediate context instead. On DX11 the immediate context
			// executes at record time, so ordering with the batched NVRHI list
			// is preserved without any command-list juggling.
			auto *d3dDevice = static_cast<ID3D11Device *>(context->GetD3D11Device());
			auto *d3dCtx = static_cast<ID3D11DeviceContext *>(context->GetD3D11Context());
			ID3D11Resource *srcResource = static_cast<ID3D11Resource *>(
				m_ColorTextures[attachmentIndex]->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
			if (!d3dDevice || !d3dCtx || !srcResource)
				return -1;

			DXGI_FORMAT dxgiFormat;
			switch (texDesc.format)
			{
			case nvrhi::Format::RGBA8_UNORM:  dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;     break;
			case nvrhi::Format::R32_SINT:     dxgiFormat = DXGI_FORMAT_R32_SINT;           break;
			case nvrhi::Format::RGBA16_FLOAT: dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
			default:
				return -1;
			}

			D3D11_TEXTURE2D_DESC sDesc = {};
			sDesc.Width = texDesc.width;
			sDesc.Height = texDesc.height;
			sDesc.MipLevels = 1;
			sDesc.ArraySize = 1;
			sDesc.Format = dxgiFormat;
			sDesc.SampleDesc.Count = 1;
			sDesc.Usage = D3D11_USAGE_STAGING;
			sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Texture2D *staging = nullptr;
			if (FAILED(d3dDevice->CreateTexture2D(&sDesc, nullptr, &staging)) || !staging)
				return -1;

			d3dCtx->CopySubresourceRegion(staging, 0, 0, 0, 0, srcResource, 0, nullptr);

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			int pixel = -1;
			if (SUCCEEDED(d3dCtx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData)
			{
				const uint8_t *row = reinterpret_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch;
				if (texDesc.format == nvrhi::Format::R32_SINT)
				{
					memcpy(&pixel, row + size_t(x) * 4, sizeof(int));
				}
				else
				{
					// RGBA8: reconstruct the entity id written as bytes
					const uint8_t *px = row + size_t(x) * 4;
					pixel = int(px[0]) | (int(px[1]) << 8) | (int(px[2]) << 16) | (int(px[3]) << 24);
				}
				d3dCtx->Unmap(staging, 0);
			}
			staging->Release();
			return pixel;
		}

		// DX12 / Vulkan: NVRHI staging readback with a deferred command list
		// (both backends support non-immediate lists).
		if (!m_ReadbackStagingTexture || m_ReadbackStagingTexture->getDesc().width != texDesc.width ||
			m_ReadbackStagingTexture->getDesc().height != texDesc.height)
		{
			nvrhi::TextureDesc stagingDesc;
			stagingDesc.width = texDesc.width;
			stagingDesc.height = texDesc.height;
			stagingDesc.depth = 1;
			stagingDesc.arraySize = 1;
			stagingDesc.mipLevels = 1;
			stagingDesc.sampleCount = 1;
			stagingDesc.format = texDesc.format;
			stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
			stagingDesc.debugName = "Framebuffer_ReadbackStaging";
			stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
			stagingDesc.keepInitialState = true;
			m_ReadbackStagingTexture = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		}
		if (!m_ReadbackStagingTexture)
			return -1;

		if (!m_ReadbackCommandList)
		{
			nvrhi::CommandListParameters params;
			params.setEnableImmediateExecution(false);
			m_ReadbackCommandList = device->createCommandList(params);
		}
		if (!m_ReadbackCommandList)
			return -1;

		m_ReadbackCommandList->open();
		m_ReadbackCommandList->copyTexture(m_ReadbackStagingTexture, nvrhi::TextureSlice(),
										   m_ColorTextures[attachmentIndex], nvrhi::TextureSlice());
		m_ReadbackCommandList->close();
		device->executeCommandList(m_ReadbackCommandList);

		size_t rowPitch = 0;
		void *mapped = device->mapStagingTexture(m_ReadbackStagingTexture,
												 nvrhi::TextureSlice(),
												 nvrhi::CpuAccessMode::Read,
												 &rowPitch);
		if (!mapped)
			return -1;

		int pixel = -1;
		const uint32_t bytesPerPixel = nvrhi::getFormatInfo(texDesc.format).bytesPerBlock;
		const uint8_t *row = reinterpret_cast<const uint8_t *>(mapped) + size_t(y) * rowPitch;
		if (texDesc.format == nvrhi::Format::R32_SINT)
		{
			memcpy(&pixel, row + size_t(x) * 4, sizeof(int));
		}
		else
		{
			const uint8_t *px = row + size_t(x) * bytesPerPixel;
			if (bytesPerPixel >= 4)
				pixel = int(px[0]) | (int(px[1]) << 8) | (int(px[2]) << 16) | (int(px[3]) << 24);
		}

		device->unmapStagingTexture(m_ReadbackStagingTexture);
		return pixel;
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

	void *NVRHIFramebuffer::GetColorAttachmentNativeSRV(uint32_t index) const
	{
		// NVRHI's D3D11 backend caches the SRV inside the texture (freed with it),
		// so requesting the native view is cheap and needs no local lifetime
		// management. Attachments were created with isShaderResource = true.
		if (index >= m_ColorTextures.size() || !m_ColorTextures[index])
			return nullptr;
		return m_ColorTextures[index]->getNativeView(nvrhi::ObjectTypes::D3D11_ShaderResourceView).pointer;
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
