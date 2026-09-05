#include "wlpch.h"
#include "OpenGLTexture.h"

#include "stb_image.h"

namespace Wasteland
{

	OpenGLTexture2D::OpenGLTexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		WL_PROFILE_FUNCTION();

		m_InternalFormat = GL_RGBA8;
		m_DataFormat = GL_RGBA;

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

		// Anisotropic filtering for sharper textures at oblique angles
		float maxAniso = 0.0f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
		glTextureParameterf(m_RendererID, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 16.0f));
	}

	OpenGLTexture2D::OpenGLTexture2D(const std::string &path)
		: m_Path(path)
	{
		WL_PROFILE_FUNCTION();

		int width, height, channels;
		stbi_set_flip_vertically_on_load(1);
		stbi_uc *data = nullptr;
		{
			WL_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			data = stbi_load(path.c_str(), &width, &height, &channels, 0);
		}
		WL_CORE_ASSERT(data, "Failed to load image!");
		m_Width = width;
		m_Height = height;

		GLenum internalFormat = 0, dataFormat = 0;
		if (channels == 4)
		{
			internalFormat = GL_RGBA8;
			dataFormat = GL_RGBA;
		}
		else if (channels == 3)
		{
			internalFormat = GL_RGB8;
			dataFormat = GL_RGB;
		}

		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		WL_CORE_ASSERT(internalFormat && dataFormat, "Format not supported!");

		// Full mip chain: the ray tracer minifies aggressively (distant floors at
		// grazing angles, reduced-res tracing) and samples explicit LODs. A single
		// level + LINEAR minification causes moire lines and shimmer.
		int mipLevels = 1;
		for (int s = width > height ? width : height; s > 1; s >>= 1)
			mipLevels++;

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, mipLevels, internalFormat, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// Mirrored repeat: photographic textures (wood, metal floors) are tiled
		// ~1/m by the ray tracer but are NOT tileable (edge MAD ~30/255), so
		// GL_REPEAT shows hard seams every tile and spikes the POM height
		// gradient at borders. Mirroring is C0-continuous for any photo.
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

		// Anisotropic filtering for sharper textures at oblique angles
		float maxAniso = 0.0f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
		glTextureParameterf(m_RendererID, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 16.0f));

		// Row pitch safety: 3-channel widths like 225 (675 bytes/row) are not a
		// multiple of GL's default unpack alignment of 4, which shears every
		// row and shows up as diagonal RGB rainbow stripes. Alignment 1 is
		// always safe for tightly-packed stb data.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, dataFormat, GL_UNSIGNED_BYTE, data);
		glGenerateMipmap(m_RendererID);

		stbi_image_free(data);
	}

	OpenGLTexture2D::~OpenGLTexture2D()
	{
		WL_PROFILE_FUNCTION();

		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTexture2D::SetData(void *data, uint32_t size)
	{
		WL_PROFILE_FUNCTION();

		uint32_t bpp = m_DataFormat == GL_RGBA ? 4 : 3;
		WL_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture!");
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	void OpenGLTexture2D::Bind(uint32_t slot) const
	{
		WL_PROFILE_FUNCTION();

		glBindTextureUnit(slot, m_RendererID);
	}

	void OpenGLTexture2D::Resize(uint32_t width, uint32_t height)
	{
		WL_PROFILE_FUNCTION();

		m_Width = width;
		m_Height = height;

		glDeleteTextures(1, &m_RendererID);

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, 1, GL_RGBA32F, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

}
