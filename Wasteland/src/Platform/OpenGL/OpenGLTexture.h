#pragma once

// The abstract Texture2D base. glad is included here rather than in
// the .cpp so that m_InternalFormat/m_DataFormat can be typed as
// GLenum -- the only place in this class the GL types leak into the
// header.
#include "Wasteland/Renderer/Texture.h"

#include <glad/glad.h>

namespace Wasteland {

	// A GL-specific subclass of an engine-side abstraction: the same
	// pattern Buffer.cpp uses for buffers. Texture2D::Create in the
	// factory picks this or the NVRHI one based on Renderer::GetAPI().
	class OpenGLTexture2D : public Texture2D
	{
	public:
		// Blank-texture constructor: allocates storage, uploads nothing.
		// The one Renderer2D::Init uses for the 1x1 white texture.
		OpenGLTexture2D(uint32_t width, uint32_t height);
		// Load-from-file. The 1x1 white texture in Renderer2D is created
		// with the blank form, so this path is for authored assets.
		OpenGLTexture2D(const std::string& path);
		virtual ~OpenGLTexture2D();

		// Trivial getters defined inline to keep the .cpp free of accessors.
		// m_Width is written only at construction and by Resize.
		virtual uint32_t GetWidth() const override { return m_Width; }
		virtual uint32_t GetHeight() const override { return m_Height; }
		// The real GL texture name, allocated by glGenTextures. This is a
		// genuine driver-level integer here, unlike NVRHITexture2D's
		// GetRendererID which returns a truncated pointer hash.
		virtual uint32_t GetRendererID() const override { return m_RendererID; }

		// The only upload path, and it is declared rather than defined here:
		// it has to know the current pixel format to choose the right
		// glTexImage2D overload.
		virtual void SetData(void* data, uint32_t size) override;

		// Slot defaults to 0, matching Texture2D's signature. A default
		// argument in a derived override is legal but easy to miss -- it
		// means a call through a Texture2D* uses the base's default, and one
		// through an OpenGLTexture2D* uses this one. They should agree.
		virtual void Bind(uint32_t slot = 0) const override;

		virtual void Resize(uint32_t width, uint32_t height) override;

		// Value semantics by RENDERER ID, not by pointer or by path. Two
		// OpenGLTexture2D objects that uploaded the same file have different
		// glGenTextures IDs and so compare unequal -- which is what lets the
		// renderer's per-batch texture dedup work.
		virtual bool operator==(const Texture& other) const override 
		{ 
			// DEFECT (unsafe downcast): a C-style cast to OpenGLTexture2D& with
			// NO type check. Comparing an OpenGLTexture2D against any other
			// Texture subclass -- a NVRHITexture2D, say, in a mixed-backend frame
			// or a test -- reinterprets that object's memory as an OpenGLTexture2D
			// and reads a bogus m_RendererID. dynamic_cast plus a null test would
			// fail cleanly; the same pattern is worth checking in the other
			// backends' operator==.
			return m_RendererID == ((OpenGLTexture2D&)other).m_RendererID;
		}
	private:
		// const AND not initialised in the member list: the path ctor
		// initialises it, the blank ctor must not try (a std::string cannot be
		// assigned). That is what makes the object non-assignable, which
		// suits a handle-like resource.
		const std::string m_Path;
		// No in-class initialisers. The blank ctor's init list covers
		// Width/Height from its arguments, but a load FAILURE path that
		// returns early without setting them leaves these indeterminate --
		// the same hazard as MacWindow's WindowData.
		uint32_t m_Width, m_Height;
		uint32_t m_RendererID;
		// TWO GL formats, not one, because glTexImage2D takes them
		// separately: the internal (storage) format and the data (source
		// pixel) format. Set once in the .cpp and used by every upload.
		GLenum m_InternalFormat, m_DataFormat;
	};

}