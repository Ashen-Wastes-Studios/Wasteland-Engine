#pragma once

// Texture thumbnails: the decode-and-resample half of Task F.3.
//
// Split out of ThumbnailCache for the same reason the cache lives in the engine:
// `Texture2D::Create` needs a GL or NVRHI device and cannot run headlessly, but
// `stb_image` is pure CPU and already compiled into Wasteland.lib. So the part
// that turns a PNG on disk into a 256x256 RGBA buffer is here and testable; the
// panel uploads that buffer and draws it.

#include <cstdint>
#include <string>
#include <vector>

#include "Wasteland/Editor/ThumbnailCache.h"

namespace Wasteland::Editor
{
	// A decoded image, always RGBA8, top row first.
	struct DecodedImage
	{
		int32_t Width = 0;
		int32_t Height = 0;
		std::vector<std::uint8_t> Pixels;
		std::string Error;

		bool Ok() const
		{
			return Error.empty() && Width > 0 && Height > 0 && !Pixels.empty();
		}
	};

	// Decode from a file. The path is a plain std::string because pybind11 ships
	// no caster for std::filesystem::path -- see ThumbnailCache.h.
	DecodedImage LoadImage(const std::string &path);

	// Decode from memory. Used by the disk cache (a cached thumbnail is read back
	// from bytes, not re-decoded from the source asset) and by the tests, which
	// author PNGs in memory.
	DecodedImage LoadImageFromMemory(const std::uint8_t *data, size_t size);

	// Scale `source` to fit inside a square of `size` x `size`, preserving aspect
	// ratio, and letterbox the remainder with an opaque background.
	//
	// The letterbox is part of the contract, not a nicety: a texture cell in the
	// Content Browser is a square, and stretching a 16x9 texture into it is
	// immediately obvious in a grid of them. The background is the browser's
	// panel colour rather than black or white, so an aspect-mismatched thumbnail
	// does not read as a missing image.
	//
	// Returns a ThumbnailEntry with Kind = Texture, Ready = true. On failure the
	// caller gets the texture's fallback icon via GenerateIcon(ThumbnailKind::Texture).
	ThumbnailEntry MakeTextureThumbnail(const DecodedImage &source,
										uint32_t size,
										std::uint8_t backgroundR,
										std::uint8_t backgroundG,
										std::uint8_t backgroundB);

	// The default letterbox colour: the browser's panel background. Exposed so a
	// test can assert the letterbox pixels rather than hardcode a colour.
	struct ThumbnailBackground
	{
		std::uint8_t R = 32, G = 34, B = 40;
	};
}
