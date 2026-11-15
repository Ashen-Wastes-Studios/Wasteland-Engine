#include "wlpch.h"

#include "Wasteland/Editor/ThumbnailPng.h"

#include <fstream>

namespace Wasteland::Editor
{
	namespace
	{
		void PushU32(std::vector<std::uint8_t> &out, std::uint32_t value)
		{
			out.push_back(static_cast<std::uint8_t>(value >> 24));
			out.push_back(static_cast<std::uint8_t>(value >> 16));
			out.push_back(static_cast<std::uint8_t>(value >> 8));
			out.push_back(static_cast<std::uint8_t>(value));
		}

		std::uint32_t Crc32(const std::uint8_t *data, size_t length)
		{
			static std::uint32_t table[256];
			static bool initialised = false;
			if (!initialised)
			{
				for (std::uint32_t i = 0; i < 256; ++i)
				{
					std::uint32_t c = i;
					for (int k = 0; k < 8; ++k)
						c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
					table[i] = c;
				}
				initialised = true;
			}

			std::uint32_t crc = 0xFFFFFFFFu;
			for (size_t i = 0; i < length; ++i)
				crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
			return crc ^ 0xFFFFFFFFu;
		}

		void PushChunk(std::vector<std::uint8_t> &out, const char *type,
					   const std::vector<std::uint8_t> &payload)
		{
			PushU32(out, static_cast<std::uint32_t>(payload.size()));
			const size_t start = out.size();
			out.insert(out.end(), type, type + 4);
			out.insert(out.end(), payload.begin(), payload.end());
			PushU32(out, Crc32(out.data() + start, out.size() - start));
		}

		// The zlib stream: a 0x78 0x01 header (no compression, fastest), one or
		// more stored deflate blocks, and an Adler-32 of the UNCOMPRESSED data.
		std::vector<std::uint8_t> ZlibStore(const std::vector<std::uint8_t> &raw)
		{
			std::vector<std::uint8_t> z;
			z.reserve(raw.size() + raw.size() / 65535 * 5 + 8);
			z.push_back(0x78);
			z.push_back(0x01);

			size_t position = 0;
			while (position < raw.size())
			{
				size_t chunk = raw.size() - position;
				if (chunk > 65535)
					chunk = 65535;
				const bool final = (position + chunk >= raw.size());
				z.push_back(final ? 1 : 0);
				z.push_back(static_cast<std::uint8_t>(chunk & 0xFF));
				z.push_back(static_cast<std::uint8_t>(chunk >> 8));
				z.push_back(static_cast<std::uint8_t>((~chunk) & 0xFF));
				z.push_back(static_cast<std::uint8_t>((~chunk >> 8) & 0xFF));
				z.insert(z.end(), raw.begin() + position, raw.begin() + position + chunk);
				position += chunk;
			}

			std::uint32_t a = 1, b = 0;
			for (std::uint8_t byte : raw)
			{
				a = (a + byte) % 65521;
				b = (b + a) % 65521;
			}
			PushU32(z, (b << 16) | a);
			return z;
		}

		// Filter type 0 (None) on every scanline. A real filter would shrink the
		// file; stored blocks already do not, so a filter would only add CPU to a
		// cache write for no benefit.
		std::vector<std::uint8_t> BuildRaw(const std::uint8_t *pixels,
										   uint32_t width, uint32_t height,
										   uint32_t channels)
		{
			std::vector<std::uint8_t> raw;
			raw.reserve(static_cast<size_t>(height) * (width * channels + 1));
			for (uint32_t y = 0; y < height; ++y)
			{
				raw.push_back(0);
				const std::uint8_t *row = pixels +
					static_cast<size_t>(y) * width * channels;
				raw.insert(raw.end(), row, row + static_cast<size_t>(width) * channels);
			}
			return raw;
		}

		bool EncodePng(const std::uint8_t *pixels, uint32_t width, uint32_t height,
					   uint32_t channels, std::vector<std::uint8_t> &outPng,
					   std::string &error)
		{
			if (!pixels || width == 0 || height == 0)
			{
				error = "no pixels to encode";
				return false;
			}
			if (channels != 3 && channels != 4)
			{
				error = "only RGB and RGBA are supported";
				return false;
			}

			const std::vector<std::uint8_t> raw = BuildRaw(pixels, width, height, channels);
			const std::vector<std::uint8_t> z = ZlibStore(raw);

			std::vector<std::uint8_t> png = { 0x89, 'P', 'N', 'G',
											  0x0D, 0x0A, 0x1A, 0x0A };

			std::vector<std::uint8_t> ihdr;
			PushU32(ihdr, width);
			PushU32(ihdr, height);
			ihdr.push_back(8);                      // bit depth
			ihdr.push_back(channels == 4 ? 6 : 2);  // 6 = truecolour+alpha, 2 = RGB
			ihdr.push_back(0);                      // deflate
			ihdr.push_back(0);                      // no interlace
			ihdr.push_back(0);

			PushChunk(png, "IHDR", ihdr);
			PushChunk(png, "IDAT", z);
			PushChunk(png, "IEND", std::vector<std::uint8_t>());

			outPng = std::move(png);
			return true;
		}
	}

	bool EncodePngRGBA(const std::uint8_t *rgba, uint32_t width, uint32_t height,
					   std::vector<std::uint8_t> &outPng, std::string &error)
	{
		return EncodePng(rgba, width, height, 4, outPng, error);
	}

	bool EncodePngRGB(const std::uint8_t *rgb, uint32_t width, uint32_t height,
					  std::vector<std::uint8_t> &outPng, std::string &error)
	{
		return EncodePng(rgb, width, height, 3, outPng, error);
	}

	bool WritePngFile(const std::string &path, const std::vector<std::uint8_t> &png,
					  std::string &error)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			error = "cannot open " + path;
			return false;
		}
		file.write(reinterpret_cast<const char *>(png.data()),
				   static_cast<std::streamsize>(png.size()));
		if (!file.good())
		{
			error = "short write to " + path;
			return false;
		}
		return true;
	}
}
