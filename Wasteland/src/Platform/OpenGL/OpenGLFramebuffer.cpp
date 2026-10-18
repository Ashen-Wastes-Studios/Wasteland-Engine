#include "wlpch.h"
#include "OpenGLFramebuffer.h"
#include <glad/glad.h>

namespace Wasteland
{

	namespace Utils
	{

		// Maps the engine's boolean 'is this multisampled?' question onto the one
		// GL enum difference it actually causes: a multisample render target is
		// created with GL_TEXTURE_2D_MULTISAMPLE and cannot also be a 2D texture.
		// Every texture call below funnels through here so the target can never
		// disagree with the multisample flag the framebuffer was built with.
		static GLenum TextureTarget(bool multisampled)
		{
			// Plain 2D for every non-multisampled attachment -- including depth, which
			// is routed through the same helper despite being a different sort of
			// attachment; the format enum passed separately is what distinguishes it.
			return multisampled ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
		}

		// Wraps glCreateTextures purely to keep the target enum out of the call sites.
		// outID is written in place, so the caller must hand over writable storage
		// of at least `count` GL texture names -- the Invalidate() colour loop
		// passes m_ColorAttachments.data() after a resize() that sized it.
		static void CreateTextures(bool multisampled, uint32_t *outID, uint32_t count)
		{
			glCreateTextures(TextureTarget(multisampled), count, outID);
		}

		// glBindTexture writes to whichever texture unit happens to be active.
		// The target enum is mandatory on every call, so it is supplied from the
		// same helper rather than hardcoded -- getting it wrong on a multisample
		// target is a GL_INVALID_OPERATION, not a silent no-op.
		static void BindTextures(bool multisampled, uint32_t id)
		{
			glBindTexture(TextureTarget(multisampled), id);
		}

		// Single point of attachment for one colour texture: allocate storage, set
		// sampler parameters, then point GL_COLOR_ATTACHMENT0 + index at it.
		// The attachment slot is derived from `index`, so two colour attachments
		// created from otherwise identical specs land in different slots purely
		// because of their position in the specifications vector.
		static void AttachColorTexture(uint32_t id, int samples, GLenum internalFormat, GLenum format, uint32_t width, uint32_t height, int index)
		{
			// samples > 1 is the entire multisample test, and it is repeated here
			// rather than derived from the texture's actual target -- so a caller
			// passing samples == 1 gets the 2D path and everything downstream agrees.
			// samples == 0 or negative would also take the 2D branch, since the
			// comparison is signed int against the literal 1.
			bool multisampled = samples > 1;
			if (multisampled)
			{
				// Immutable storage: sized once here and the texture is complete, so no
				// glTexSubImage2D may ever target it. glTexStorage2DMultisample is also
				// the only allocation path that can create a multisample texture at all.
				// GL_FALSE is fixed-function-sRGB: the attachment stores linear values and
				// any encode/decode must be done by the shader.
				glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internalFormat, width, height, GL_FALSE);
			}
			else
			{
				// glTexImage2D is the mutable alternative to glTexStorage2DMultisample:
				// it names a format, a data format and a type, and because data is nullptr
				// it only allocates. Used on the single-sample path, where a 2D texture is
				// what downstream sampling code expects.
				// Note the asymmetry with the multisample branch: only glTexImage2D needs
				// the GL_UNSIGNED_BYTE data type here, because no data is uploaded.
				glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);

				// The one place sampler state is chosen for a colour attachment, and it is
				// chosen by internal format rather than by anything in the specification.
				// GL_R32I is a signed-integer texture and is not filterable, so GL_LINEAR
				// on it is a GL_INVALID_ENUM at draw time; GL_NEAREST is the only legal
				// filter and also the only one correct for exact texel readback.
				// Every other format gets GL_LINEAR. The specification has no field for
				// this choice -- see the TODO in FramebufferTextureSpecification -- so a
				// caller cannot request NEAREST on an RGBA8 attachment or REPEAT on any.
				GLenum filter = (internalFormat == GL_R32I) ? GL_NEAREST : GL_LINEAR;

				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			// Binding an attachment point does not imply a draw buffer: until
			// glDrawBuffers/glDrawBuffer runs further down, GL_COLOR_ATTACHMENT0 + index
			// is only where fragment output lands IF that slot is in the draw-buffer
			// list. That list is set once for the whole framebuffer, not per attachment.
			// Mip level 0 hardcoded -- these are never mipmapped.
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, TextureTarget(multisampled), id, 0);
		}

		// Depth counterpart of AttachColorTexture. The only difference that matters
		// is the attachment point, which arrives as a parameter (GL_DEPTH_STENCIL
		// _ATTACHMENT here) instead of being computed from an index.
		//
		// Its `samples > 1` test is written out again rather than shared with the
		// colour helper above, so the two copies of that rule can drift apart --
		// though the whole file keeps the identical spelling in both.
		static void AttachDepthTexture(uint32_t id, int samples, GLenum format, GLenum attachmentType, uint32_t width, uint32_t height)
		{
			bool multisampled = samples > 1;
			if (multisampled)
			{
				// GL_DEPTH24_STENCIL8 as the internal format: 24 bits of depth and 8 of
				// stencil in ONE texture, combined rather than separate. Anything that wants
				// stencil therefore gets depth with it -- there is no depth-only option in
				// this backend's format enum.
				glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, format, width, height, GL_FALSE);
			}
			else
			{
				// DEFECT: the internal format is `format` (GL_DEPTH24_STENCIL8) but the data
				// format and type are hardcoded to GL_DEPTH_STENCIL / GL_UNSIGNED_INT_24_8.
				// They agree today because IsDepthFormat admits exactly one format, so the
				// parameter cannot yet select anything else -- but the parameter is
				// misleading: it reads as if a second depth format would be honoured here.
				glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);

				// DEPTH24_STENCIL8 is filterable, so GL_LINEAR is legal here, unlike the
				// GL_NEAREST special case the colour path needs for GL_R32I. Sampling
				// depth linearly is only meaningful if a shader actually reads it.
				// CLAMP_TO_EDGE on all three axes matches the colour path, so depth
				// sampling at the framebuffer border clamps rather than wraps.
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glFramebufferTexture2D(GL_FRAMEBUFFER, attachmentType, TextureTarget(multisampled), id, 0);
		}

		// Classifies an attachment format as the depth slot. Deliberately a switch
		// over one enumerator rather than a comparison, so a future depth format
		// fails to compile here instead of being silently treated as colour.
		static bool IsDepthFormat(FramebufferTextureFormat format)
		{
			// No default case on purpose: an unhandled format falls through to the
			// `return false` below, which routes it into m_ColorAttachmentSpecifications
			// -- and then to the colour switch below, which also has no default. The
			// end result is a texture allocated but never attached, not a loud failure.
			// An exhaustive switch (or a default asserting here) would catch it sooner.
			switch (format)
			{
			case FramebufferTextureFormat::DEPTH24STENCIL8:
				return true;
			}
			// Fallthrough for every format that is not DEPTH24STENCIL8. `Depth` is an
			// alias of the same value, so it never appears as its own case.
			return false;
		}

		// Internal format for glClearTexImage and for texture creation.
		// GL_RED_INTEGER is the *internal* format here -- a sized internal format
		// -- whereas RED_INTEGER without a size is a data format. Mixing the two
		// up is a common source of GL_INVALID_OPERATION in this code.
		static GLenum WastelandFBTextureFormatToGL(FramebufferTextureFormat format)
		{
			switch (format)
			{
			case FramebufferTextureFormat::RGBA8:
				return GL_RGBA8;
			case FramebufferTextureFormat::RED_INTEGER:
				return GL_RED_INTEGER;
			}

			// Unreachable for every format FramebufferSpecification can currently
			// produce, but it is the safety net for a new enumerator. WL_CORE_ASSERT
			// compiles out in Release, so in a shipping build the `return 0` below is
			// what actually happens: glClearTexImage then receives internalFormat 0.
			WL_CORE_ASSERT(false, "Unknown FramebufferTextureFormat!");
			return 0;
		}

		// Intended to pick the glReadPixels type from the format. DEFECT: DEAD CODE
		// -- nothing in this file or elsewhere calls GLDataType; the readback below
		// hardcodes GL_INT instead, which is why the RGBA8 mismatch documented in
		// OpenGLFramebuffer.h still exists despite this helper being present.
		// Safe to delete, or to start routing ReadPixel through it.
		//
		// Were it called, it would also be wrong for RED_INTEGER: it returns
		// GL_RED_INTEGER, which is a *format* enum, where GL_INT is the type. The
		// two fallthroughs below share this function's assert-then-return-0 shape,
		// and in a Release build -- where WL_CORE_ASSERT compiles out -- the
		// `return 0` is what actually reaches the caller, handing glClearTexImage or
		// glReadPixels an internal format of 0.
		static GLenum GLDataType(FramebufferTextureFormat format)
		{
			switch (format)
			{
			case FramebufferTextureFormat::RGBA8:
				return GL_UNSIGNED_BYTE;
			case FramebufferTextureFormat::RED_INTEGER:
				return GL_RED_INTEGER;
			}

			WL_CORE_ASSERT(false, "Unknown FramebufferTextureFormat!");
			return 0;
		}

	}

	// Spec copied by value, so the caller's object can be a temporary or be
	// destroyed immediately after construction. Everything below reads only
	// m_Specification, never the parameter again.
	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpecification &spec)
		: m_Specification(spec)
	{
		// Partitions the flat attachment list by format. Colour entries keep their
		// relative order -- that order IS the GL_COLOR_ATTACHMENT slot number, set
		// later by the index parameter of AttachColorTexture. Depth entries all
		// collapse into one slot, so a spec with two depth attachments keeps only
		// the last, silently discarding the first.
		for (auto format : m_Specification.Attachments.Attachments)
		{
			// IsDepthFormat is the partition predicate, so colour/depth routing is
			// decided entirely by the enum value and not by position in the vector.
			if (!Utils::IsDepthFormat(format.TextureFormat))
				m_ColorAttachmentSpecifications.emplace_back(format);
			else
				// Assignment, not emplace: a second depth attachment overwrites the first
				// with no diagnostic. Since the engine's format enum admits one depth
				// format this only bites on a hand-built spec with duplicates.
				m_DepthAttachmentSpecification = format;
		}

		// No GL object is created in the ctor body itself -- Invalidate() is the
		// single place that builds the FBO, and Resize() calls the same function.
		// One construction path means the ctor and a resize cannot diverge.
		Invalidate();
	}

	// Deletes every GL object this framebuffer owns. Order matters and is
	// correct as written: the FBO goes first, while its attachment references
	// are still resolvable, then the textures those references pointed at.
	// Deleting textures first would leave the FBO briefly pointing at freed
	// names, which is legal but is the wrong order to leave behind.
	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		// Count is passed explicitly because the deletion API takes (n, ptr), not a
		// span. m_RendererID is a single uint32_t, so the address-of is correct.
		glDeleteFramebuffers(1, &m_RendererID);
		// DEFECT (minor): on a framebuffer with no colour attachments this calls
		// glDeleteTextures(0, ...). The spec permits n == 0, but the pointer is
		// then still evaluated -- it is valid (a valid vector), so no fault, just
		// a pointless call.
		glDeleteTextures(m_ColorAttachments.size(), m_ColorAttachments.data());
		glDeleteTextures(1, &m_DepthAttachment);
	}

	void OpenGLFramebuffer::Invalidate()
	{
		// m_RendererID doubles as the 'has been created' flag, so the first
		// Invalidate() from the ctor skips the delete block entirely -- there is
		// nothing to delete yet, and glDeleteFramebuffers(1, &0) would target the
		// name 0, which is reserved rather than a real object.
		// DEFECT: the color vector is cleared but m_RendererID is not reset to 0
		// inside this block; it is immediately overwritten by glCreateFramebuffers
		// below, so it is harmless today but makes the flag's meaning subtle.
		if (m_RendererID)
		{
			glDeleteFramebuffers(1, &m_RendererID);
			glDeleteTextures(m_ColorAttachments.size(), m_ColorAttachments.data());
			glDeleteTextures(1, &m_DepthAttachment);

			// Cleared so the resize path below starts from an empty vector; the
			// specifications vector is deliberately NOT cleared, since it is rebuilt
			// only in the ctor and Invalidate() must keep working from it on a resize.
			m_ColorAttachments.clear();
			m_DepthAttachment = 0;
		}

		// glCreateFramebuffers leaves the new object unbound; the bind on the next
		// line is what makes the attachment calls below apply to THIS framebuffer
		// rather than to whatever was bound by the caller. Invalidate() therefore
		// clobbers GL_FRAMEBUFFER and restores it to 0 at the end -- a caller who
		// had a framebuffer bound before calling Resize() does not get it back.
		glCreateFramebuffers(1, &m_RendererID);
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

		// One multisample decision for the whole framebuffer, taken from the spec.
		// It drives BOTH the texture target and the storage call in the helpers,
		// so an attachment can never end up on the 2D path while the FBO expects a
		// multisample texture.
		bool multisample = m_Specification.Samples > 1;

		// Attachments
		if (m_ColorAttachmentSpecifications.size())
		{
			// resize() first, because glCreateTextures writes `count` names into the
			// vector's storage -- the vector must already own that many uint32_t
			// before the call, or CreateTextures writes out of bounds.
			m_ColorAttachments.resize(m_ColorAttachmentSpecifications.size());
			// DEFECT: the specifications vector's length and this vector's length are
			// independent invariants; the loop below indexes both with the same i and
			// trusts that they agree. They do, because both are sized from the same
			// source in the ctor -- but nothing enforces it, and a Resize() that
			// rebuilt only one of them would read past the end of the other.
			Utils::CreateTextures(multisample, m_ColorAttachments.data(), (uint32_t)m_ColorAttachments.size());

			// Size, not the specifications vector, drives the count -- so the loop bound
			// is the GL names actually created, while the body indexes the spec vector.
			// Index i therefore means 'GL_COLOR_ATTACHMENT0 + i' on the GL side and
			// 'the i-th non-depth entry of the spec' on the C++ side. Those agree only
			// because the ctor partitioned in order and nothing has reordered since.
			for (size_t i = 0; i < m_ColorAttachments.size(); i++)
			{
				// Storage must be bound before glTexImage2D / glTexStorage2DMultisample can
				// describe it -- hence the explicit bind before each attachment's setup,
				// inside the loop rather than once for all of them.
				Utils::BindTextures(multisample, m_ColorAttachments[i]);
				// Format dispatch happens per attachment, so a mixed list (RGBA8 then
				// RED_INTEGER) gives attachment 0 an RGBA8 texture and attachment 1 an
				// R32I one. No default case: a format with no arm below still gets its
				// texture bound and created (the two lines above run unconditionally) but
				// is never attached or given sampler state, leaving a framebuffer that
				// reports incomplete at the status check below.
				switch (m_ColorAttachmentSpecifications[i].TextureFormat)
				{
				case FramebufferTextureFormat::RGBA8:
					// GL_RGBA8 internal / GL_RGBA data format: a normalized colour attachment.
					// The final argument is the slot, cast from size_t to int because
					// AttachColorTexture takes an int -- a spec with more than INT_MAX colour
					// attachments would wrap, which the 4-attachment assert below rules out.
					Utils::AttachColorTexture(m_ColorAttachments[i], m_Specification.Samples, GL_RGBA8, GL_RGBA, m_Specification.Width, m_Specification.Height, (int)i);
					break;
				case FramebufferTextureFormat::RED_INTEGER:
					// GL_R32I internal format paired with GL_RED_INTEGER as the data format --
					// the picking path, one signed-integer channel. AttachColorTexture forces
					// GL_NEAREST on it because GL_R32I is not filterable.
					Utils::AttachColorTexture(m_ColorAttachments[i], m_Specification.Samples, GL_R32I, GL_RED_INTEGER, m_Specification.Width, m_Specification.Height, (int)i);
					break;
				}
			}
		}

		// Depth is optional, keyed on the sentinel the specifications default to.
		// A framebuffer with no depth attachment is legal; the `else` further down
		// then has to select the draw-buffer state for that case.
		if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			// Single texture, single name: CreateTextures with count 1 writing directly
			// into the member, which is why the depth slot is a uint32_t rather than a
			// one-element vector.
			Utils::CreateTextures(multisample, &m_DepthAttachment, 1);
			Utils::BindTextures(multisample, m_DepthAttachment);
			// One depth attachment max, so a plain switch rather than a loop. Same
			// missing-default caveat as the colour switch above.
			switch (m_DepthAttachmentSpecification.TextureFormat)
			{
			case FramebufferTextureFormat::DEPTH24STENCIL8:
				// GL_DEPTH24_STENCIL8 as the internal format and GL_DEPTH_STENCIL_ATTACHMENT
				// as the attachment point: one texture carrying both depth and stencil, so
				// the stencil ops in the renderer work without a second attachment.
				Utils::AttachDepthTexture(m_DepthAttachment, m_Specification.Samples, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT, m_Specification.Width, m_Specification.Height);
				break;
			}
		}

		// The draw-buffer list is a FRAMEBUFFER property, not a per-attachment one,
		// so it is configured once here from the number of colour textures that
		// were actually created -- after all of them have been attached above.
		// Skipping this leaves GL_COLOR_ATTACHMENT0 as the only enabled buffer,
		// which happens to be right for a single attachment and wrong for more.
		if (m_ColorAttachments.size() > 0)
		{
			// DEFECT: the assert is compiled out in Release, so nothing at all stops a
			// framebuffer with five colour attachments: glDrawBuffers below would then
			// be called with n = 5 and buffers pointing at a 4-element array -- an
			// out-of-bounds read of GLenum values handed straight to GL.
			WL_CORE_ASSERT(m_ColorAttachments.size() <= 4, "Too many color attachments!");
			// Fixed 4-entry array of the maximum draw-buffer count the GLES/desktop
			// core profile guarantees. The local array exists only to satisfy the
			// (count, list) signature of glDrawBuffers -- it is never indexed past
			// `count`, which the assert above is meant to bound.
			GLenum buffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
			// Note the deliberate mismatch with the assert: the ASSERT says `<= 4` and
			// the ARRAY has 4 slots, but the count passed here is the real texture
			// count. The pair is consistent only while the assert is compiled in.
			glDrawBuffers((GLsizei)m_ColorAttachments.size(), buffers);
		}
		else
		{
			// Depth-only framebuffer: explicitly disable fragment colour output.
			// Without this, the default draw buffer of GL_COLOR_ATTACHMENT0 would stay
			// enabled and the depth pass would also write to a non-existent colour
			// target -- which the framebuffer status check would then report.
			// Only depth-pass
			glDrawBuffer(GL_NONE);
		}

		// Completeness check, and the ONLY validation this function performs.
		// DEFECT: WL_CORE_ASSERT is a debug-only construct, so in a Release build
		// this compiles to nothing: an incomplete framebuffer is created, bound and
		// used, and the failure surfaces much later as silently missing geometry
		// rather than as an error here. A GL error check or an unconditional
		// failure path would be needed to catch it in shipping builds.
		WL_CORE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Framebuffer is incomplete!");

		// Restores GL_FRAMEBUFFER to 0 rather than to whatever was bound before.
		// Defensible -- this function is called from the ctor and from Resize(),
		// neither of which has a caller framebuffer to preserve -- but it is an
		// unconditional state change on a shared context.
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	// Bind also sets the viewport, so a caller cannot forget it. The viewport is
	// therefore left pointing at this framebuffer's size after Unbind(); see the
	// header note about Unbind() deliberately not restoring it.
	void OpenGLFramebuffer::Bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);
		glViewport(0, 0, m_Specification.Width, m_Specification.Height);
	}

	// Unbind restores only the binding. It does NOT reset the viewport, and
	// does not reset glDrawBuffers either -- the draw-buffer list belongs to
	// the framebuffer object, so it travels with it rather than with the
	// binding, and no state needs clearing on the default framebuffer here.
	void OpenGLFramebuffer::Unbind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	// Size is written into the spec BEFORE Invalidate() rebuilds, so the helpers
	// below read the new dimensions from m_Specification. Nothing is resized
	// in place: every texture is deleted and re-created at the new size.
	//
	// DEFECT: no guard against width == 0 or height == 0. glTexImage2D with a
	// zero dimension is a GL error, and the completeness assert in Invalidate()
	// then fires with a misleading message about the framebuffer rather than
	// about the invalid size. A minimum-size clamp belongs at the top of this
	// function -- the base class documents the same gap.
	void OpenGLFramebuffer::Resize(uint32_t width, uint32_t height)
	{
		// Order of the two writes is irrelevant; both must precede Invalidate().
		m_Specification.Width = width;
		m_Specification.Height = height;
		Invalidate();
	}

	// Synchronous single-texel readback. ReadPixel necessarily stalls the
	// pipeline until the GPU has finished producing that texel, which is why
	// it is only used for picking and not per frame.
	int OpenGLFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		WL_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Attachment index out of range!");

		// The read buffer must be selected before glReadPixels: without it the
		// default is GL_COLOR_ATTACHMENT0 regardless of `attachmentIndex`, so
		// reading attachment 1 would silently return attachment 0's texel.
		// This call also resets on Unbind()/the next Bind(), which is why it is
		// repeated here rather than cached at creation time.
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		// Left uninitialised on purpose -- glReadPixels writes through the pointer.
		// DEFECT: if the read fails (out-of-bounds coordinates, or a GL error from
		// the hardcoded format below), glReadPixels may leave it untouched and the
		// function then returns an indeterminate value.
		int pixelData;
		// DEFECT: GL_RED_INTEGER / GL_INT are hardcoded, so this only reads
		// correctly for a RED_INTEGER attachment. RGBA8 is also a legal attachment
		// format and the helper above creates a GL_RGBA8 texture for it, for which
		// a GL_RED_INTEGER read is a GL error -- the read silently fails and the
		// uninitialised local above is returned. The format should come from
		// m_ColorAttachmentSpecifications[attachmentIndex].TextureFormat, which is
		// exactly what the dead GLDataType helper above was written for.
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
		return pixelData;
	}

	// glClearTexImage fills the texture object itself, so unlike glClearBufferfi
	// this needs no framebuffer binding -- which is why no Bind() call appears
	// here and the function is safe to call mid-frame.
	// DEFECT: the value is passed by address, so the caller must keep the int
	// alive across the call; the glClearTexImage signature takes a const void*
	// and does not copy synchronously in a way that outlives the argument.
	void OpenGLFramebuffer::ClearAttachment(uint32_t attachmentIndex, int value)
	{
		WL_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Attachment index out of range!");

		auto &spec = m_ColorAttachmentSpecifications[attachmentIndex];
		// Format comes from the specification, not from a hardcoded constant -- the
		// one place in this file that consults the per-attachment format for a
		// texel operation. Level 0 only, matching the mip level the texture was
		// allocated at.
		// DEFECT: GL_INT is a 4-byte type but glClearTexImage derives the source
		// byte count from the internal format. For GL_R32I that is 4 bytes and it
		// matches; for GL_RGBA8 it implies four components, so GL reads 16 bytes
		// from a 4-byte int -- an out-of-bounds read of adjacent stack.
		glClearTexImage(m_ColorAttachments[attachmentIndex], 0,
						Utils::WastelandFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
	}

}