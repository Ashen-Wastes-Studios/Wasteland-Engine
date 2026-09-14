#include "wlpch.h"
#include "NVRHITexture.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"

#include <stb_image.h>

#include <cstdint>
#include <vector>

namespace Wasteland {

	// Helper to get the current NVRHI device
	static nvrhi::IDevice* GetDevice()
	{
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	}

	// Full mip chain, mirroring OpenGLTexture2D's level computation
	// (loops while the largest dimension > 1).
	static uint32_t ComputeMipLevels(uint32_t width, uint32_t height)
	{
		uint32_t levels = 1;
		uint32_t extent = width > height ? width : height;
		while (extent > 1)
		{
			extent >>= 1;
			levels++;
		}
		return levels;
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
		// D3D UV origin is top-left; stb flip is a GL-bottom-left convention
		// (OpenGLTexture2D sets flip(1) before its own loads). Loading flipped
		// here would display every DX texture upside-down vs GL.
		stbi_set_flip_vertically_on_load(0);
		stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);

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

		WL_CORE_INFO("NVRHITexture2D loaded '{0}' ({1}x{2}, {3} mips, {4})",
			path, width, height, ComputeMipLevels((uint32_t)width, (uint32_t)height),
			!m_Texture ? "null" : (m_Texture->getDesc().format == nvrhi::Format::RGBA8_UNORM ? "RGBA8" : "other"));

		// Upload texture data
		int bpp = 4;
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
		// Linear RGBA8, matching OpenGLTexture2D's GL_RGBA8: the Nova shaders
		// were tuned on raw sRGB values. SRGBA8_UNORM would make D3D
		// auto-decode every sample to linear (dark/desaturated vs GL and wrong
		// luminance-derived height inputs for relief).
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.debugName = m_Path.empty() ? "Texture2D" : m_Path;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		// Full mip chain: Nova samples explicit LOD 0..4 (distance + grazing).
		// A single level leaves minification unfiltered (shimmer/blockiness).
		desc.mipLevels = ComputeMipLevels(width, height);

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
		desc.mipFilter = true;  // Linear (trilinear, now that textures carry mips)
		// Mirrored repeat, matching OpenGLTexture2D: photographic textures are
		// tiled ~1/m but not tileable, so plain repeat shows seams every tile.
		desc.addressU = nvrhi::SamplerAddressMode::Mirror;
		desc.addressV = nvrhi::SamplerAddressMode::Mirror;
		desc.addressW = nvrhi::SamplerAddressMode::Mirror;
		// 16x aniso like the GL path; >1 also selects the anisotropic D3D filter.
		desc.maxAnisotropy = 16.0f;

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

	// Uploads level 0 plus a CPU box-filtered mip chain. NVRHI has no
	// GenerateMips equivalent on this path, and Nova samples explicit LODs.
	void NVRHITexture2D::UploadMipChain(nvrhi::ICommandList* cmd, const void* baseData)
	{
		const uint8_t* src = static_cast<const uint8_t*>(baseData);
		cmd->writeTexture(m_Texture, 0, 0, src, size_t(m_Width) * 4);

		uint32_t levelW = m_Width, levelH = m_Height;
		std::vector<uint8_t> prev, next;
		prev.assign(src, src + size_t(levelW) * levelH * 4);

		uint32_t mipLevels = ComputeMipLevels(m_Width, m_Height);
		for (uint32_t level = 1; level < mipLevels; level++)
		{
			uint32_t nextW = levelW > 1 ? levelW / 2 : 1;
			uint32_t nextH = levelH > 1 ? levelH / 2 : 1;
			next.resize(size_t(nextW) * nextH * 4);

			for (uint32_t y = 0; y < nextH; y++)
			{
				for (uint32_t x = 0; x < nextW; x++)
				{
					uint32_t sx = x * 2, sy = y * 2;
					uint32_t ex = (sx + 1 < levelW) ? sx + 1 : sx;
					uint32_t ey = (sy + 1 < levelH) ? sy + 1 : sy;
					uint32_t count = (ex - sx + 1) * (ey - sy + 1);
					uint32_t sum[4] = {};
					for (uint32_t yy = sy; yy <= ey; yy++)
						for (uint32_t xx = sx; xx <= ex; xx++)
						{
							const uint8_t* p = prev.data() + (size_t(yy) * levelW + xx) * 4;
							sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
						}
					uint8_t* d = next.data() + (size_t(y) * nextW + x) * 4;
					d[0] = uint8_t(sum[0] / count); d[1] = uint8_t(sum[1] / count);
					d[2] = uint8_t(sum[2] / count); d[3] = uint8_t(sum[3] / count);
				}
			}

			cmd->writeTexture(m_Texture, 0, level, next.data(), size_t(nextW) * 4);
			prev.swap(next);
			levelW = nextW;
			levelH = nextH;
		}
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

		if (size != uint64_t(m_Width) * m_Height * 4)
		{
			WL_CORE_ERROR("NVRHITexture2D::SetData: Expected a full RGBA8 image");
			return;
		}
		const bool transient = !context->IsCommandListOpen();
		if (transient) cmd->open();
		UploadMipChain(cmd, data);
		if (transient)
		{
			cmd->close();
			context->GetDevice()->executeCommandList(cmd);
		}
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
