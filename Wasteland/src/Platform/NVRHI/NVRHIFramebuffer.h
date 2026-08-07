#pragma once

#include "Wasteland/Renderer/Framebuffer.h"
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	class NVRHIFramebuffer : public Framebuffer
	{
	public:
		NVRHIFramebuffer(const FramebufferSpecification& spec);
		virtual ~NVRHIFramebuffer();

		virtual void Bind() override;
		virtual void Unbind() override;

		virtual void Resize(uint32_t width, uint32_t height) override;
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) override;

		virtual void ClearAttachment(uint32_t attachmentIndex, int value) override;

		virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const override;

		virtual const FramebufferSpecification& GetSpecification() const override { return m_Specification; }

		// NVRHI-specific accessors
		nvrhi::FramebufferHandle GetFramebuffer() const { return m_Framebuffer; }
		nvrhi::TextureHandle GetColorAttachment(uint32_t index) const;
		nvrhi::TextureHandle GetDepthAttachment() const { return m_DepthTexture; }

	private:
		void Invalidate();

		FramebufferSpecification m_Specification;
		nvrhi::FramebufferHandle m_Framebuffer;
		std::vector<nvrhi::TextureHandle> m_ColorTextures;
		nvrhi::TextureHandle m_DepthTexture;
	};

}
