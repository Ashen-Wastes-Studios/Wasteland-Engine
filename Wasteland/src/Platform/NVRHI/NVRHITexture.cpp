#include "wlpch.h"
#include "NVRHITexture.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"

#include <stb_image.h>

namespace Wasteland {

	// Helper to get the current NVRHI device
	static nvrhi::IDevice* GetDevice()
	{
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	}

	/////////////////////////////////////////////////////////////////////////////////////////////
	// NVRHITexture2D
	/////////////////////////////////////////////////////////////////////////////////////////////

	NVRHITexture2D::NVRHITexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height), m_Path("")
	{
		CreateTexture(width, height);
		CreateSampler();
	}

	NVRHITexture2D::NVRHITexture2D(const std::string& path)
		: m_Path(path)
	{
		int width, height, channels;
		stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 0);

		if (!data)
		{
			WL_CORE_ERROR("Failed to load texture: {0}", path);
			m_Width = 0;
			m_Height = 0;
			return;
		}

		m_Width = width;
		m_Height = height;

		CreateTexture(width, height);
		CreateSampler();

		// Upload texture data
		int bpp = channels;
		uint32_t size = width * height * bpp;
		SetData(data, size);

		stbi_image_free(data);
	}

	NVRHITexture2D::~NVRHITexture2D()
	{
		m_Sampler = nullptr;
		m_Texture = nullptr;
	}

	void NVRHITexture2D::CreateTexture(uint32_t width, uint32_t height)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHITexture2D: No NVRHI device available");
			return;
		}

		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.depth = 1;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.debugName = m_Path.empty() ? "Texture2D" : m_Path;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;

		m_Texture = device->createTexture(desc);
		if (!m_Texture)
		{
			WL_CORE_ERROR("NVRHITexture2D: Failed to create texture");
		}
	}

	void NVRHITexture2D::CreateSampler()
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHITexture2D: No NVRHI device available");
			return;
		}

		nvrhi::SamplerDesc desc;
		desc.minFilter = true;  // Linear
		desc.magFilter = true;  // Linear
		desc.mipFilter = true;  // Linear
		desc.addressU = nvrhi::SamplerAddressMode::Wrap;
		desc.addressV = nvrhi::SamplerAddressMode::Wrap;
		desc.addressW = nvrhi::SamplerAddressMode::Wrap;

		m_Sampler = device->createSampler(desc);
		if (!m_Sampler)
		{
			WL_CORE_ERROR("NVRHITexture2D: Failed to create sampler");
		}
	}

	uint32_t NVRHITexture2D::GetRendererID() const
	{
		// NVRHI doesn't have integer IDs, return a hash of the texture pointer
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_Texture.Get()));
	}

	void NVRHITexture2D::SetData(void* data, uint32_t size)
	{
		if (!m_Texture || !data || size == 0)
			return;

		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (!context)
		{
			WL_CORE_ERROR("NVRHITexture2D::SetData: No NVRHI context available");
			return;
		}

		nvrhi::ICommandList* cmd = context->GetCommandList();
		if (!cmd)
		{
			WL_CORE_ERROR("NVRHITexture2D::SetData: No command list available");
			return;
		}

		// Upload texture data (arraySlice=0, mipLevel=0)
		cmd->writeTexture(m_Texture, 0, 0, data, m_Width * 4); // Assuming RGBA8
	}

	void NVRHITexture2D::Bind(uint32_t slot) const
	{
		// Note: In NVRHI, textures are bound via binding arrays, not directly
		// This is handled in the shader/draw call implementation
		// For now, this is a no-op
	}

	void NVRHITexture2D::Resize(uint32_t width, uint32_t height)
	{
		if (width == m_Width && height == m_Height)
			return;

		m_Width = width;
		m_Height = height;

		// Recreate texture with new size
		m_Texture = nullptr;
		CreateTexture(width, height);
	}

}
