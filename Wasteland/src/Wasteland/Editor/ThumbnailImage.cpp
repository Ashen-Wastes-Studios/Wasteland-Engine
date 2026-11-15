#include "wlpch.h"

#include "Wasteland/Editor/ThumbnailImage.h"

#include "stb_image.h"

#include <algorithm>
#include <cstring>

namespace Wasteland::Editor
{
	DecodedImage LoadImage(const std::string &path)
	{
		DecodedImage image;

		if (path.empty())
		{
			image.Error = "empty path";
			return image;
		}

		// Force 4 channels. stb_image returns the file's native channel count
		// otherwise, so a greyscale PNG would come back 1-channel and every
		// consumer that assumes RGBA would read past the buffer.
		int width = 0, height = 0, channels = 0;
		// stbi_load takes a const char*; it does not open the file itself until
		// the read, so a missing file returns null rather than throwing.
		stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
		if (!pixels)
		{
			image.Error = stbi_failure_reason()
				? std::string(stbi_failure_reason())
				: "unknown decode failure";
			return image;
		}

		image.Width = width;
		image.Height = height;
		const size_t bytes = static_cast<size_t>(width) * height * 4;
		image.Pixels.assign(pixels, pixels + bytes);
		stbi_image_free(pixels);

		return image;
	}

	DecodedImage LoadImageFromMemory(const std::uint8_t *data, size_t size)
	{
		DecodedImage image;

		if (!data || size == 0)
		{
			image.Error = "no data";
			return image;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc *pixels = stbi_load_from_memory(
			data, static_cast<int>(size), &width, &height, &channels, 4);
		if (!pixels)
		{
			image.Error = stbi_failure_reason()
				? std::string(stbi_failure_reason())
				: "unknown decode failure";
			return image;
		}

		image.Width = width;
		image.Height = height;
		const size_t bytes = static_cast<size_t>(width) * height * 4;
		image.Pixels.assign(pixels, pixels + bytes);
		stbi_image_free(pixels);

		return image;
	}

	ThumbnailEntry MakeTextureThumbnail(const DecodedImage &source,
										uint32_t size,
										std::uint8_t backgroundR,
										std::uint8_t backgroundG,
										std::uint8_t backgroundB)
	{
		ThumbnailEntry entry;
		entry.Kind = ThumbnailKind::Texture;
		entry.Width = size;
		entry.Height = size;
		entry.Ready = true;
		entry.Pixels.assign(static_cast<size_t>(size) * size * 4, 0);

		if (!source.Ok())
		{
			entry.SetFailed(true);
			return entry;
		}

		// ---- letterbox geometry ------------------------------------------
		// The image is scaled so its LONGER axis exactly fills the square and
		// the shorter axis is centred. Integer rounding is deliberate: a
		// non-integer scaled size would need a resample to fractional pixel
		// boundaries, and the half-pixel error at 256px is invisible in a grid.
		const int32_t sourceWidth = source.Width;
		const int32_t sourceHeight = source.Height;
		const float scale = std::min(static_cast<float>(size) / sourceWidth,
									 static_cast<float>(size) / sourceHeight);
		const int32_t scaledWidth = std::max(1, static_cast<int32_t>(sourceWidth * scale + 0.5f));
		const int32_t scaledHeight = std::max(1, static_cast<int32_t>(sourceHeight * scale + 0.5f));
		const int32_t offsetX = (static_cast<int32_t>(size) - scaledWidth) / 2;
		const int32_t offsetY = (static_cast<int32_t>(size) - scaledHeight) / 2;

		// ---- background --------------------------------------------------
		for (uint32_t y = 0; y < size; ++y)
		{
			std::uint8_t *row = entry.Pixels.data() +
								static_cast<size_t>(y) * size * 4;
			for (uint32_t x = 0; x < size; ++x)
			{
				row[x * 4 + 0] = backgroundR;
				row[x * 4 + 1] = backgroundG;
				row[x * 4 + 2] = backgroundB;
				row[x * 4 + 3] = 255;
			}
		}

		// ---- area-average resample ---------------------------------------
		// Each destination pixel maps to a rectangle in the source. Averaging
		// every source pixel that rectangle covers (with fractional edge
		// weights) is an exact box filter: no aliasing on a downscale, which is
		// what a mip-less 4K texture shown at 256px would otherwise produce, and
		// nearest-neighbour behaviour on an upscale, which is the least-wrong
		// simple answer for an enlargement.
		//
		// Bilinear would be worse here: it ignores the sample density, so a
		// 512x512 texture shrunk to 64x64 would keep 4 of every 64 source pixels
		// and shimmer.
		const float sourceToDestX = static_cast<float>(sourceWidth) / scaledWidth;
		const float sourceToDestY = static_cast<float>(sourceHeight) / scaledHeight;

		for (int32_t destY = 0; destY < scaledHeight; ++destY)
		{
			// Source rows [sy0, sy1) covered by this destination row.
			const float sy0f = destY * sourceToDestY;
			const float sy1f = (destY + 1) * sourceToDestY;
			const int32_t sy0 = static_cast<int32_t>(sy0f);
			const int32_t sy1 = std::min(sourceHeight,
										 static_cast<int32_t>(std::ceilf(sy1f)));

			for (int32_t destX = 0; destX < scaledWidth; ++destX)
			{
				const float sx0f = destX * sourceToDestX;
				const float sx1f = (destX + 1) * sourceToDestX;
				const int32_t sx0 = static_cast<int32_t>(sx0f);
				const int32_t sx1 = std::min(sourceWidth,
											 static_cast<int32_t>(std::ceilf(sx1f)));

				float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f, weightSum = 0.0f;

				for (int32_t sy = sy0; sy < sy1; ++sy)
				{
					// Fraction of this source row the destination pixel covers.
					const float rowTop = std::max(sy0f, static_cast<float>(sy));
					const float rowBottom = std::min(sy1f, static_cast<float>(sy + 1));
					const float rowWeight = rowBottom - rowTop;
					if (rowWeight <= 0.0f)
						continue;

					const std::uint8_t *row = source.Pixels.data() +
											  static_cast<size_t>(sy) * sourceWidth * 4;

					for (int32_t sx = sx0; sx < sx1; ++sx)
					{
						const float colLeft = std::max(sx0f, static_cast<float>(sx));
						const float colRight = std::min(sx1f, static_cast<float>(sx + 1));
						const float colWeight = colRight - colLeft;
						if (colWeight <= 0.0f)
							continue;

						const float weight = rowWeight * colWeight;
						const std::uint8_t *pixel = row + static_cast<size_t>(sx) * 4;

						// Premultiply by alpha while averaging. A straight
						// average of a partly transparent edge bleeds the
						// colour of whatever is behind the image; weighting by
						// alpha keeps the thumbnail's own colour.
						//
						// `a` accumulates the NORMALISED alpha (0..1), NOT the
						// 0..255 byte. Dividing a 0..255 colour sum by a 0..255
						// alpha sum yields a fraction, which is exactly how this
						// shipped: a solid red (255,0,0,255) came back as
						// (1,0,0,255). The un-premultiply below divides r by a.
						const float alpha = pixel[3] / 255.0f;
						r += pixel[0] * alpha * weight;
						g += pixel[1] * alpha * weight;
						b += pixel[2] * alpha * weight;
						a += alpha * weight;
						weightSum += weight;
					}
				}

				if (weightSum <= 0.0f)
					continue;

				std::uint8_t *out = entry.Pixels.data() +
									static_cast<size_t>(offsetY + destY) * size * 4 +
									static_cast<size_t>(offsetX + destX) * 4;

				// Un-premultiply. A fully transparent region has a sum of zero,
				// so it takes the background rather than dividing by zero.
				const float alphaSum = a;
				if (alphaSum <= 0.0f)
				{
					out[0] = backgroundR;
					out[1] = backgroundG;
					out[2] = backgroundB;
					out[3] = 255;
					continue;
				}

				out[0] = static_cast<std::uint8_t>(
					std::clamp(r / alphaSum, 0.0f, 255.0f) + 0.5f);
				out[1] = static_cast<std::uint8_t>(
					std::clamp(g / alphaSum, 0.0f, 255.0f) + 0.5f);
				out[2] = static_cast<std::uint8_t>(
					std::clamp(b / alphaSum, 0.0f, 255.0f) + 0.5f);
				// `a` is normalised, so the coverage fraction is a/weightSum
				// and the byte is that fraction of 255.
				out[3] = static_cast<std::uint8_t>(
					std::clamp(alphaSum / weightSum * 255.0f, 0.0f, 255.0f) + 0.5f);
			}
		}

		return entry;
	}
}
