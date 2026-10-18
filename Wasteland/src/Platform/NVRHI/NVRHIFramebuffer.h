#pragma once

#include "Wasteland/Renderer/Framebuffer.h"
// NVRHI, the vendor-neutral RHI this backend is written against. Unlike the
// OpenGL sibling -- which keeps every GL enum in its .cpp and names only
// uint32_t handles in the header -- this one needs nvrhi types in the
// interface itself, because it exposes the NVRHI-specific accessors below.
// Any consumer of this header therefore inherits NVRHI's headers and its
// version-compatibility constraints.
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	// NVRHI implementation of a render target, covering DX11, DX12 and Vulkan
	// with one class -- NVRHI normalises the backend differences below this
	// layer, which is why a single Framebuffer subclass serves all three.
	//
	// The class is the one place in this codebase that can hand a caller a real
	// native view of an attachment: GetColorAttachmentNativeSRV() below is
	// what ImGui uses to sample an engine framebuffer's colour target directly.
	class NVRHIFramebuffer : public Framebuffer
	{
	public:
		// Takes the spec and, in the .cpp, creates the NVRHI framebuffer and its
		// textures. Spec is copied into m_Specification like the GL sibling.
		NVRHIFramebuffer(const FramebufferSpecification& spec);
		// Releases the NVRHI objects. Virtual to match the Framebuffer base's
		// virtual dtor, so deletion through a Framebuffer* is well-defined.
		virtual ~NVRHIFramebuffer();

		// Makes this framebuffer the NVRHI render target. The .cpp also sets the
		// viewport from m_Specification, mirroring the OpenGL sibling's Bind().
		virtual void Bind() override;
		virtual void Unbind() override;

		// Writes the new size into m_Specification and calls the private
		// Invalidate(), rebuilding every NVRHI object.
		//
		// DEFECT: no zero-size guard, the same gap as OpenGLFramebuffer::Resize.
		// Resize(0, 0) asks NVRHI for a zero-extent texture, which is invalid.
		virtual void Resize(uint32_t width, uint32_t height) override;
		// Blocking CPU readback of one texel.
		//
		// The .cpp does this through m_ReadbackCommandList and
		// m_ReadbackStagingTexture (see their comments below) rather than on the
		// immediate list, because the batched immediate command list must not be
		// opened twice in one frame.
		//
		// Readbacks are a GPU/CPU sync point; this is a debug/pick path, not
		// something to call per frame.
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) override;

		// Fills one colour attachment with a constant, as a command-list clear
		// rather than a CPU-side write -- the .cpp issues it on the immediate
		// command list, so it does not stall the pipeline the way ReadPixel does.
		virtual void ClearAttachment(uint32_t attachmentIndex, int value) override;

		// Out-of-line here, unlike the GL sibling's inline. It cannot be inline:
		// the return value is a plain uint32_t, but NVRHI texture handles are
		// opaque pointers that have no meaningful integer form to hand back.
		//
		// The .cpp therefore has to invent a number. Callers that genuinely need an
		// integer id -- legacy code paths -- get whatever the .cpp produces;
		// callers wanting the real object should use GetColorAttachment() instead.
		virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const override;
		// The native GPU view of a colour attachment, returned as a void*.
		//
		// On DX11 this is an ID3D11ShaderResourceView*, which is what ImGui's D3D11
		// backend needs to bind the framebuffer as a drawable image. On DX12 and
		// Vulkan there is no single equivalent pointer, so this returns nullptr and
		// callers fall back to the integer path.
		//
		// void* rather than a typed pointer because naming
		// ID3D11ShaderResourceView would drag the D3D11 headers into this header
		// for every backend.
		virtual void *GetColorAttachmentNativeSRV(uint32_t index = 0) const override;

		// The ImGui TexID for this attachment on the active NVRHI backend.
		//
		// This is the DX12 answer to "TexID must be a GPU descriptor handle, and
		// DX12 has no native SRV object to return", which is why it cannot live in
		// GetColorAttachmentNativeSRV: that one reports a native VIEW, and on DX12
		// there is no such object -- the descriptor lives in a heap, not in a
		// resource.
		//
		// DX11 returns the cached native SRV unchanged, so the DX11 caller keeps
		// working with no branch of its own.
		//
		// VULKAN RETURNS A DESCRIPTOR SET, built by ImGui_ImplVulkan_AddTexture from
		// three things the engine does own: the image view comes from the attachment
		// itself (getNativeView caches it on the texture, so its lifetime follows the
		// texture's), the sampler is NVRHIContext's, and the descriptor set layout is
		// created by ImGui's own CreateDeviceObjects. The pool the set is allocated
		// from is the engine's Vulkan ImGui pool. The set is cached per attachment and
		// rebuilt when either the texture or that pool changes -- see the members
		// below for why both are part of the key.
		virtual void *GetColorAttachmentImGuiTexID(uint32_t index = 0) const override;

		virtual const FramebufferSpecification& GetSpecification() const override { return m_Specification; }

		// Escape hatches past the abstract Framebuffer interface, exposed only
		// here. Code that needs the real NVRHI object calls these; code that should
		// stay backend-agnostic goes through the Framebuffer base instead.
		// NVRHI-specific accessors
		// The framebuffer object itself, for callers that must submit a custom pass
		// against it. Inline because it is a bare member read.
		//
		// Returns a handle, not an owning reference: NVRHI objects are refcounted
		// and this class does not expose a way to retain one, so the handle stays
		// valid only while this framebuffer lives. Invalidate() replaces it on
		// resize.
		nvrhi::FramebufferHandle GetFramebuffer() const { return m_Framebuffer; }
		// Out-of-line for the same reason as GetColorAttachmentRendererID: an
		// out-of-range index has to be checked, and the OpenGL sibling's inline
		// operator[] skips that check.
		//
		// Returns the real texture, which is what an NVRHI-native caller wants --
		// no integer id, no backend-specific cast.
		nvrhi::TextureHandle GetColorAttachment(uint32_t index) const;
		// The depth/stencil texture, inline. Singular because a framebuffer has at
		// most one depth attachment, matching the GL sibling's single
		// m_DepthAttachment member.
		//
		// Returns a default-constructed (null) handle when the spec asked for no
		// depth attachment, with no separate has-depth query to check first.
		nvrhi::TextureHandle GetDepthAttachment() const { return m_DepthTexture; }

	private:
		// PRIVATE -- the correct shape, and the contrast with the OpenGL sibling
		// matters: that class exposes its Invalidate() publicly, which lets a
		// caller rebuild the FBO behind the engine's back. Here it is reachable
		// only from the ctor and from Resize(), so an object can never be torn down
		// and recreated without its size being updated first.
		void Invalidate();
		// Returns the cached Vulkan descriptor set to ImGui's pool, if there is one.
		// Called from the destructor and from Invalidate() so a resize does not hold
		// a set against a texture that is about to die. Safe to call when nothing
		// was ever cached.
		void ReleaseVulkanImGuiDescriptorSet() const;

		FramebufferSpecification m_Specification;
		// The framebuffer object.
		//
		// DEFECT: not initialised to a null handle. Every handle member below is
		// likewise uninitialised, and the dtor releases them unconditionally.
		// There is exactly one ctor which creates all of them, so this is
		// currently correct -- but adding a defaulted ctor, or a creation path
		// that fails partway, would leave the dtor releasing indeterminate handles.
		nvrhi::FramebufferHandle m_Framebuffer;
		// Colour attachments, one per non-depth format in the spec, mirroring the
		// spec order so index i pairs with colour attachment slot i.
		std::vector<nvrhi::TextureHandle> m_ColorTextures;
		nvrhi::TextureHandle m_DepthTexture;
		// The readback path's two resources, and the reason the .cpp needs a
		// command list of its own.
		//
		// ReadPixel must block the CPU until the GPU has finished producing a
		// texel. The engine's normal work is submitted on a batched IMMEDIATE
		// command list which cannot be opened a second time within one frame -- so
		// opening it to do readback would trip that restriction. These two members
		// give readback its own list and its own staging texture to copy the
		// result through.
		//
		// Cost: a fence or wait per ReadPixel call. Acceptable for picking and
		// tests, ruinous if anything calls it every frame.
		// Dedicated non-immediate command list for blocking pixel readback
		// (the batched immediate list must not be opened twice per frame)
		// The private list opened and closed inside each ReadPixel call.
		nvrhi::CommandListHandle m_ReadbackCommandList;
		// CPU-visible landing zone for the copied texel, then read back into the
		// int ReadPixel returns. Its format must match the attachment being read,
		// so reallocating it is part of what Invalidate() has to do on resize.
		nvrhi::StagingTextureHandle m_ReadbackStagingTexture;
		// THE VULKAN DESCRIPTOR SET CACHE, three members because a set is only valid
		// for one (texture, pool) pair and neither half alone is enough.
		//
		// THE TEXTURE IS HALF THE KEY because Invalidate() rebuilds every attachment on
		// a viewport resize. A set cached against the old texture would name a
		// VkImageView that died with it, which is a descriptor pointing at freed memory
		// rather than a visible failure. Stored as a handle rather than a raw pointer so
		// this reference itself keeps the texture alive and the comparison is a plain
		// pointer test.
		//
		// THE POOL IS THE OTHER HALF, and it is the one that is easy to miss: the pool is
		// destroyed and recreated by a backend switch and by ImGui's own shutdown,
		// either of which invalidates every set allocated from it while the attachment
		// texture is untouched. Without the pool in the key the cache would hand ImGui a
		// set from a destroyed pool.
		//
		// THE SET ITSELF is a void* so this header does not have to include
		// <vulkan/vulkan.h>; the cast back to VkDescriptorSet happens in the one place
		// that returns it. The DX12 path above is deliberately a fixed slot, which works
		// there because a D3D12 descriptor heap entry is written in place -- a Vulkan set
		// is pool-allocated, so it has to be cached here.
		mutable nvrhi::TextureHandle m_VkImGuiCacheTexture;
		mutable void *m_VkImGuiCacheDescriptorSet = nullptr;
		mutable void *m_VkImGuiCachePool = nullptr;
	};

}
