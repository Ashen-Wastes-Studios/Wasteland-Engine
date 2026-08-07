#pragma once

#include "Wasteland/Renderer/Texture.h"
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	class NVRHITexture2D : public Texture2D
	{
	public:
		NVRHITexture2D(uint32_t width, uint32_t height);
		NVRHITexture2D(const std::string& path);
		virtual ~NVRHITexture2D();

		virtual uint32_t GetWidth() const override { return m_Width; }
		virtual uint32_t GetHeight() const override { return m_Height; }
		virtual uint32_t GetRendererID() const override;

		virtual void SetData(void* data, uint32_t size) override;

		virtual void Bind(uint32_t slot = 0) const override;

		virtual void Resize(uint32_t width, uint32_t height) override;

		virtual bool operator==(const Texture& other) const override
		{
			const NVRHITexture2D* otherNVRHI = dynamic_cast<const NVRHITexture2D*>(&other);
			return otherNVRHI && m_Texture == otherNVRHI->m_Texture;
		}

		// NVRHI-specific accessors
		nvrhi::TextureHandle GetTexture() const { return m_Texture; }
		nvrhi::SamplerHandle GetSampler() const { return m_Sampler; }

	private:
		void CreateTexture(uint32_t width, uint32_t height);
		void CreateSampler();

		const std::string m_Path;
		uint32_t m_Width, m_Height;
		nvrhi::TextureHandle m_Texture;
		nvrhi::SamplerHandle m_Sampler;
	};

}
