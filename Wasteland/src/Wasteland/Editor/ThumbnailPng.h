#pragma once

// A minimal PNG writer, shared by the editor's screenshot path and the
// thumbnail disk cache.
//
// Hand-written rather than pulled from stb_image_write because that header is
// NOT on this project's include path -- only stb_image.h is, under
// vendor/stb_image. A screenshot is a verification aid and a cached thumbnail is
// a regenerable file, so adding a vendored file plus a build-path change to save
// ~40 lines would be the larger change. The reasoning is the one recorded at
// EditorLayer.cpp:57; it is repeated here so the copy that moved survives with
// its justification.
//
// Stored (uncompressed) deflate blocks are valid zlib, so the output opens
// anywhere. A 256x256 RGBA thumbnail is ~256 KB, which is fine for a cache whose
// whole purpose is to avoid re-decoding.

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland::Editor
{
	// Encode RGBA8 pixels (top row first, 4 bytes per pixel) as a PNG.
	// Returns false and fills `error` on an empty buffer or a zero dimension.
	bool EncodePngRGBA(const std::uint8_t *rgba, uint32_t width, uint32_t height,
					   std::vector<std::uint8_t> &outPng, std::string &error);

	// Encode RGB8 pixels (3 bytes per pixel).
	bool EncodePngRGB(const std::uint8_t *rgb, uint32_t width, uint32_t height,
					  std::vector<std::uint8_t> &outPng, std::string &error);

	// Write the encoded PNG to `path`, creating no parent directories -- the
	// caller must ensure the cache directory exists.
	bool WritePngFile(const std::string &path, const std::vector<std::uint8_t> &png,
					  std::string &error);
}
