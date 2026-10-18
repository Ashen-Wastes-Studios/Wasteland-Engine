#pragma once

// Brings in the abstract Framebuffer base, FramebufferSpecification and the
// FramebufferTextureFormat enum. No GL header: this class names only
// uint32_t handles, so glad and the GL enums stay in the .cpp.
#include "Wasteland/Renderer/Framebuffer.h"

namespace Wasteland
{

	// GL implementation of a render target.
	//
	// The whole object is rebuilt rather than patched when the size changes --
	// Resize() just writes Width/Height into m_Specification and calls
	// Invalidate(), which deletes and re-creates the FBO and every attachment.
	// No GL object is ever resized in place. Simple, and cheap enough for the
	// once-per-resize frequency it runs at.
	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		// Copies the spec by value into m_Specification via the member init list,
		// then the .cpp partitions the attachment list: every non-depth format
		// goes into m_ColorAttachmentSpecifications in order, and a depth format
		// overwrites m_DepthAttachmentSpecification.
		//
		// The GL object itself is not created here directly -- the .cpp ends the
		// ctor with Invalidate(), which is also what Resize() calls.
		OpenGLFramebuffer(const FramebufferSpecification &spec);
		virtual ~OpenGLFramebuffer();

		// PUBLIC, and not an override. It is not part of the Framebuffer interface,
		// yet Invalidate() is public in the OpenGLFramebuffer header.
		//
		// DEFECT: exposing it publicly lets a caller destroy and silently rebuild
		// the FBO behind the engine's back -- with no resize, no rebind and no
		// width/height update -- invalidating any cached native handles a caller
		// may already hold. The NVRHI sibling declares its own Invalidate() as
		// private, which is the safe shape; here it should be too.
		void Invalidate();

		// Binds GL_FRAMEBUFFER and sets the viewport to the framebuffer's own size.
		// Folding the viewport into Bind means callers cannot forget it; the .cpp's
		// Unbind() restores binding 0 but deliberately leaves the viewport alone.
		virtual void Bind() override;
		// Binds framebuffer 0. Deliberately does not reset the viewport -- the
		// caller is expected to set its own before the next pass, as Bind() does
		// for this FBO.
		virtual void Unbind() override;

		// Mutates m_Specification in place, then Invalidate(). The spec object is
		// returned by reference from GetSpecification(), so this write is visible
		// to any caller still holding that reference.
		//
		// DEFECT: no zero-size guard. Resize(0, 0) reaches glTexImage2D with a zero
		// dimension, which is a GL error, and glCheckFramebufferStatus then trips
		// the 'Framebuffer is incomplete!' assert. A minimum-size clamp belongs
		// here.
		virtual void Resize(uint32_t width, uint32_t height) override;
		// CPU readback of one texel from a color attachment.
		//
		// The .cpp asserts the index is in range, then binds the attachment as the
		// read buffer and reads GL_RED_INTEGER/GL_INT unconditionally, rather than
		// consulting the attachment's stored format.
		//
		// DEFECT: that hardcoding only reads correctly for a RED_INTEGER
		// attachment. FramebufferSpecification also permits RGBA8, whose
		// AttachColorTexture creates a GL_RGBA8 texture, and a RED_INTEGER read
		// from one is a GL error. The format should come from
		// m_ColorAttachmentSpecifications[attachmentIndex].
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) override;

		// Fills one color attachment with a value using glClearTexImage.
		//
		// The .cpp asserts the index, looks up the internal format via
		// WastelandFBTextureFormatToGL(spec.TextureFormat), and passes GL_INT with
		// a pointer to the int parameter.
		//
		// DEFECT: the data is always 4 bytes, but the byte count glClearTexImage
		// needs is set by the internal format. For RED_INTEGER (GL_R32I) a single
		// component matches. For RGBA8 it implies four components, i.e. 16 bytes
		// of source, so only the first four bytes of the surrounding stack are
		// meaningfully initialised and the rest is an out-of-bounds read. The
		// parameter type would need to widen to match the format.
		virtual void ClearAttachment(uint32_t attachmentIndex, int value) override;

		// Inline accessor returning the GL texture name for one color attachment.
		//
		// DEFECT: operator[] with no bounds check. The .cpp's ReadPixel and
		// ClearAttachment both assert the index is in range; this inline does not,
		// which is an inconsistency within one class -- an out-of-range index here
		// is undefined behaviour instead of a clean assert.
		virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const override { return m_ColorAttachments[index]; }

		// Returns a reference to the live spec, not a copy, so callers see later
		// Resize() writes -- convenient, but it also means a caller can mutate the
		// framebuffer's size without Resize() running and with no FBO rebuild.
		virtual const FramebufferSpecification &GetSpecification() const override { return m_Specification; }

	private:
		// The GL framebuffer object name, zero-initialised. Doubles as the
		// 'already created' flag: Invalidate() in the .cpp only deletes the old
		// objects when m_RendererID is non-zero, so 0 must mean 'never created'.
		uint32_t m_RendererID = 0;
		// DEFECT: m_ColorAttachment is a DEAD FIELD. It is declared here but never
		// read or written anywhere -- the .cpp stores color attachments only in the
		// m_ColorAttachments vector and assigns m_DepthAttachment alone. The singular
		// name alongside the plural vector one line below is exactly the trap a
		// future edit falls into. Safe to delete.
		//
		// m_DepthAttachment is live: the .cpp's
		// glDeleteTextures(1, &m_DepthAttachment) depends on it being a single
		// uint32_t, and Invalidate() resets it to 0.
		uint32_t m_ColorAttachment = 0, m_DepthAttachment = 0;
		FramebufferSpecification m_Specification;

		// CPU-side mirror of the color attachment list, in creation order. Index i
		// here corresponds to GL_COLOR_ATTACHMENT0 + i in the .cpp.
		//
		// Copied rather than referenced so the original Framebuffer
		// AttachmentSpecification can be a temporary at the call site.
		std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
		// Single depth/stencil slot, defaulted to FramebufferTextureFormat::None,
		// which is the sentinel the .cpp's `!= None` test uses to skip depth
		// attachment creation entirely.
		//
		// Singular by design: a framebuffer has at most one depth attachment here.
		FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

		// The created GL texture names, resized and filled by glCreateTextures in
		// the .cpp's Invalidate(). Kept in sync with the specifications vector
		// above by index.
		//
		// DEFECT: nothing in the type system ties this vector's length to the
		// specifications vector's. The .cpp resizes both in the same block today,
		// so they agree, but the sizes are independent invariants rather than one.
		std::vector<uint32_t> m_ColorAttachments;
	};

}