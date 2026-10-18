#include "wlpch.h"
#include "OpenGLTexture.h"

// Third-party image decoder, header-only. Angled because it is an
// external dependency, not a project header.
// Implemented as a .cpp in some builds, so this also brings in its
// single translation unit.
#include "stb_image.h"

namespace Wasteland
{

	// Blank-texture constructor: allocates GPU storage and uploads nothing.
	// m_Path is deliberately NOT in this initialiser list -- see the header
	// note on the const member. An empty path is the implicit state.
	OpenGLTexture2D::OpenGLTexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		WL_PROFILE_FUNCTION();

		// Storage format: 8 bits per channel, RGBA. Immutable once the texture
		// is created, which is why glTextureStorage2D below can allocate the
		// whole chain in one call and no further format changes are possible.
		// Hardcoded to RGBA regardless of what the caller intends -- this
		// constructor cannot produce a single-channel or float texture.
		m_InternalFormat = GL_RGBA8;
		// Source format for uploads, which is what SetData on line 115 passes
		// to glTextureSubImage2D. Matches the internal format here because
		// every upload is assumed to be RGBA bytes.
		m_DataFormat = GL_RGBA;

		// The DSA (direct state access) form: creates and initialises in one
		// call and writes the name straight into m_RendererID.
		// Requires a core-profile context (GL 4.5+), which is consistent with
		// the shaders in this repo declaring #version 450 core.
		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		// Exactly ONE mip level. With GL_LINEAR minification set on line 20
		// that is self-consistent -- no mip chain means no mip-based minification
		// -- but it means this texture aliases badly when minified, which is
		// the opposite of what the load-from-file constructor sets up (see the
		// mipLevels loop and GL_LINEAR_MIPMAP_LINEAR there).
		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, m_Width, m_Height);

		// Plain bilinear, no mipmaps. Matches the single-level allocation
		// above; requesting GL_LINEAR_MIPMAP_LINEAR here would make the texture
		// incomplete and render black.
		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		// Magnification also bilinear. GL_NEAREST would give the crisp pixel
		// art many 2D games want, so this is a deliberate softening.
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// Horizontal wrap. DEFECT (inconsistent): the load-from-file
		// constructor uses GL_MIRRORED_REPEAT on both axes (lines 82-83) and
		// Resize uses GL_CLAMP_TO_EDGE (lines 139-140). Three different wrap
		// modes in one class, so how a texture behaves at its edges depends on
		// which constructor built it.
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

		// Existing comment. The comment is duplicated verbatim on line 85 in
		// the other constructor -- the same three lines of logic are written
		// out twice rather than shared in a private helper.
		// Anisotropic filtering for sharper textures at oblique angles
		// Zero-initialised so the glGetFloatv on the next line has a defined
		// destination even on a context where the query fails.
		float maxAniso = 0.0f;
		// Queries the driver's hardware limit. This is a synchronous driver
		// round-trip in the constructor.
		// DEFECT: the return value is not checked. On a context without the
		// extension enabled, GL_INVALID_ENUM is raised and maxAniso keeps 0.0f,
		// so line 29 silently sets anisotropy to 0 rather than skipping it.
		// GL will clamp 0 to the minimum of 1, so this degrades rather than
		// breaks.
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
		// std::min caps the request at 16x so the constructor does not depend on
		// an optional extension for correctness on hardware that reports more.
		// std::min comes from the algorithm header, which is not included
		// here -- it is reaching <algorithm> transitively through wlpch.h.
		glTextureParameterf(m_RendererID, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 16.0f));
	}

	// Load-from-file constructor. Only m_Path is in the init list:
	// m_Width and m_Height are assigned in the body from the decoder, which
	// is why the header notes they stay indeterminate on a load failure.
	OpenGLTexture2D::OpenGLTexture2D(const std::string &path)
		: m_Path(path)
	{
		WL_PROFILE_FUNCTION();

		// stb_image out-parameters. Declared uninitialised here and written by
		// the stbi_load call below, so if that call fails they are never set.
		int width, height, channels;
		// stb_image decodes top-down; OpenGL samples bottom-up, so without this
		// every loaded image appears vertically mirrored.
		// DEFECT (global state): this is a GLOBAL stb setting, not per-call.
		// It affects every stbi_load anywhere in the process for the rest of
		// the session, including other libraries' image loads.
		stbi_set_flip_vertically_on_load(1);
		// stb_image returns unsigned char* pointing at its own heap buffer.
		// Null on failure. Freed by stbi_image_free on line 98.
		stbi_uc *data = nullptr;
		{
			// Named profiling scope so image decode time can be attributed
			// separately -- it dominates texture construction and is worth
			// seeing in a profile.
			WL_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			// The trailing 0 means "do not force a channel count": the decoder
			// reports what the file actually has via `channels`, which is what the
			// format branch below keys off.
			// No explicit desired_channels, so a greyscale JPEG stays 1-channel and
			// will fail the format assert on line 63 -- see that note.
			data = stbi_load(path.c_str(), &width, &height, &channels, 0);
		}
		// DEFECT (order of operations): this fires BEFORE the format decision,
		// but in a release build where WL_CORE_ASSERT is compiled out, a null
		// `data` falls straight through to the width/height assignments and
		// eventually to glTextureSubImage2D on line 95 with a null pointer. The
		// assert is the only thing preventing that.
		// A returned-error-code path would be safer than an assert for a
		// genuinely recoverable user error (a bad asset path).
		WL_CORE_ASSERT(data, "Failed to load image!");
		m_Width = width;
		m_Height = height;

		// Locals, zero-initialised so the assert on line 63 has something to
		// test if neither branch below is taken.
		GLenum internalFormat = 0, dataFormat = 0;
		// Branch on the decoder-reported channel count rather than hardcoding
		// RGBA, so a 4-channel image uses the 4-channel format pair.
		if (channels == 4)
		{
			internalFormat = GL_RGBA8;
			dataFormat = GL_RGBA;
		}
		// 3-channel (no alpha) images get RGB8/RGB.
		// DEFECT: there is NO branch for channels == 1 (greyscale) or 2 (grey+
		// alpha). Both fall through with internalFormat and dataFormat still 0,
		// so the assert on line 63 is the only outcome. A greyscale PNG would
		// therefore fail to load rather than being promoted to RGB.
		else if (channels == 3)
		{
			internalFormat = GL_RGB8;
			dataFormat = GL_RGB;
		}

		// The locals are copied into the members BEFORE the assert on line 63,
		// so on an unsupported format the object is left holding 0s rather
		// than garbage -- which is why a caller that ignores the assert gets a
		// texture whose later uploads use an invalid format.
		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		WL_CORE_ASSERT(internalFormat && dataFormat, "Format not supported!");

		// Existing comment explaining WHY the whole chain is allocated rather
		// than guessing at a conventional count.
		// Full mip chain: the ray tracer minifies aggressively (distant floors at
		// grazing angles, reduced-res tracing) and samples explicit LODs. A single
		// level + LINEAR minification causes moire lines and shimmer.
		// Starts at 1 so a 1x1 texture gets one level and the loop below adds
		// nothing.
		int mipLevels = 1;
		// Iterates over the LARGER dimension, halving each time, which is the
		// standard full-chain formula: floor(log2(max(w,h))) + 1 levels.
		// DEFECT: a width or height of 0 (which stb_image can report for a
		// malformed file) makes max(w,h) 0, so the loop body never runs and
		// mipLevels stays 1 while glTextureStorage2D below is asked for a 0x0
		// texture -- a GL error.
		// Style: the ternary inside the for-init is dense; a named local would
		// read better.
		for (int s = width > height ? width : height; s > 1; s >>= 1)
			mipLevels++;

		// Same DSA call as the blank constructor. Duplicated rather than shared.
		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		// Allocates the WHOLE chain now. Immutable storage means the format and
		// the level count can never change afterwards -- resizing would require
		// recreating the texture, which is exactly what Resize does.
		// Uses the LOCAL internalFormat, not the member, though the member was
		// just set to the same value on line 60.
		glTextureStorage2D(m_RendererID, mipLevels, internalFormat, m_Width, m_Height);

		// Trilinear: bilinear within a mip level AND blended between two levels.
		// Correct here because a full chain was allocated. This is the key
		// difference from the blank constructor, which sets plain GL_LINEAR.
		// Note the texture stays INCOMPLETE until glGenerateMipmap runs on
		// line 96 -- so between line 75 and line 96 this texture would sample
		// as black if anything drew it.
		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// Mirrored repeat: photographic textures (wood, metal floors) are tiled
		// ~1/m by the ray tracer but are NOT tileable (edge MAD ~30/255), so
		// GL_REPEAT shows hard seams every tile and spikes the POM height
		// gradient at borders. Mirroring is C0-continuous for any photo.
		// Existing comment above explains the ray tracer's tiling need.
		// DEFECT: GL_MIRRORED_REPEAT is NOT supported on a non-power-of-two
		// texture in core profile, and stb_image will happily return an NPOT
		// image (any modern PNG). The GL spec requires such a texture to
		// become incomplete, which renders black, and no GL error is checked.
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
		// Same on the vertical axis. The wrap mode assumes the texture IS
		// tileable in content; the comment above acknowledges real photos are
		// not, which is why mirroring (C0-continuous) rather than repeating is
		// used.
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

		// Anisotropic filtering for sharper textures at oblique angles
		float maxAniso = 0.0f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
		glTextureParameterf(m_RendererID, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 16.0f));

		// Existing comment. This is a subtle, correct fix and it is worth
		// knowing that GL_UNPACK_ALIGNMENT is global state that persists
		// beyond this constructor.
		// Row pitch safety: 3-channel widths like 225 (675 bytes/row) are not a
		// multiple of GL's default unpack alignment of 4, which shears every
		// row and shows up as diagonal RGB rainbow stripes. Alignment 1 is
		// always safe for tightly-packed stb data.
		// Byte alignment for the upload. Set to 1 so rows whose byte length is
		// not a multiple of 4 are still copied correctly.
		// DEFECT (global state): glPixelStorei is NOT texture state -- it is
		// global context state that persists for every subsequent upload from any
		// texture, for the rest of the context's life. Nothing ever restores it
		// to 4. It is harmless while every texture in this class also sets it,
		// but any third-party or future upload path inherits alignment 1.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		// Uploads ONLY mip level 0 (the third argument is level, then x, y).
		// The remaining levels are filled by glGenerateMipmap on the next line.
		// GL_UNSIGNED_BYTE matches stb_image's stbi_uc output.
		// The local dataFormat is used rather than the member; they are equal.
		// DEFECT: the return value is unchecked, so a failed upload here leaves
		// an incomplete texture that only shows up as a black or corrupted
		// sample at draw time.
		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, dataFormat, GL_UNSIGNED_BYTE, data);
		// Populates levels 1..N from level 0. Required before this texture can
		// be sampled, because GL_LINEAR_MIPMAP_LINEAR was set on line 75.
		// This is a per-texture call rather than a framebuffer-level
		// glGenerateMipmap, so the cost is paid once per load.
		glGenerateMipmap(m_RendererID);

		// Frees stb's buffer. Must use stbi_image_free, not delete[]: the
		// allocation came from stb_image's own allocator.
		stbi_image_free(data);
	}

	// Not defaulted: it has to release the GL name.
	OpenGLTexture2D::~OpenGLTexture2D()
	{
		WL_PROFILE_FUNCTION();

		// Releases the texture name back to the driver.
		// DEFECT: no glIsTexture check first, and no GL context check. If this
		// runs after the context has been destroyed -- which is a normal
		// shutdown-ordering hazard for a Ref<> held past device teardown -- the
		// call targets a dead context.
		// Also note it does not null m_RendererID afterwards, so a use after
		// the destructor would pass a stale name to GL.
		glDeleteTextures(1, &m_RendererID);
	}

	// The single upload entry point, reached through the Texture2D
	// interface. void* because the base class is API-agnostic.
	void OpenGLTexture2D::SetData(void *data, uint32_t size)
	{
		WL_PROFILE_FUNCTION();

		// Bytes per pixel derived from the member. DEFECT: this is the only
		// place the format is decoded, and it hardcodes the assumption that the
		// format is either RGBA or RGB. A texture built with any other
		// m_DataFormat would silently compute the wrong bpp -- and the assert
		// on the next line would then reject a correctly-sized upload.
		// The ternary also treats anything that is not exactly GL_RGBA as 3
		// bytes, including the 0 left behind by an unsupported-format load.
		uint32_t bpp = m_DataFormat == GL_RGBA ? 4 : 3;
		// Requires the caller to hand over the WHOLE texture, not a sub-rect.
		// So partial updates are impossible through this API.
		// DEFECT: m_Width * m_Height is computed in uint32_t, so it overflows
		// for a texture over roughly 65535x65535 (or much smaller in total
		// bytes) and the comparison can pass with a wrong size.
		// And in a release build the assert vanishes, leaving glTextureSubImage2D
		// to read past the end of a short buffer.
		WL_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture!");
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		// Level 0 again, as in the constructor. A texture built by Resize has
		// GL_RGBA32F storage but this uploads GL_UNSIGNED_BYTE bytes against a
		// float internal format, which is a format/transfer-type mismatch.
		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	// const because binding changes GL state, not this object.
	void OpenGLTexture2D::Bind(uint32_t slot) const
	{
		WL_PROFILE_FUNCTION();

		// Binds to a specific texture UNIT rather than to a target (GL_TEXTURE_
		// 2D). glBindTextureUnit is the DSA equivalent and separates the
		// "which unit" decision from the "what kind of texture" binding, so no
		// glActiveTexture call is needed.
		// This is why Bind takes a slot parameter at all.
		glBindTextureUnit(slot, m_RendererID);
	}

	// Recreates the texture at a new size. Not const because the GL name,
	// and the object's identity in the renderer's dedup, both change.
	void OpenGLTexture2D::Resize(uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();

		m_Width = width;
		m_Height = height;

		// The old storage is discarded before a new one is made, because
		// glTextureStorage2D cannot resize an immutable texture.
		// Any shader still holding a sampler bound to the old unit would now
		// sample nothing.
		glDeleteTextures(1, &m_RendererID);

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		// DEFECT (format inconsistency): GL_RGBA32F is hardcoded here, but
		// Resize does NOT update m_InternalFormat or m_DataFormat. A texture
		// that was loaded from a 3-channel PNG keeps m_DataFormat == GL_RGB and
		// m_InternalFormat == GL_RGB8 while its storage is now RGBA32F, so every
		// later SetData uploads bytes against a float format -- undefined
		// results.
		// also allocates a single mip level while setting GL_LINEAR (which is
		// self-consistent), but with float data the caller almost certainly
		// wants at least NEAREST or a float-aware filter.
		glTextureStorage2D(m_RendererID, 1, GL_RGBA32F, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// Clamping here, versus GL_REPEAT in the blank constructor and
		// GL_MIRRORED_REPEAT in the file constructor -- the third of the three
		// different wrap choices noted on line 23.
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

}
