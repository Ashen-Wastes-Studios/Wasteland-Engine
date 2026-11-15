// Bc1.h -- J.2 texture compression: BC1 (DXT1) encode + a mip chain.
//
// The plan's J.2 textures bullet: "source -> a compressed GPU format (BC/ASTC)
// with mips". BC1 is the one that is a precise, spec-visible algorithm (no
// hardware or SDK needed to COMPRESS; decoding is the GPU's job), so it is
// implementable and verifiable HEADLESSLY -- unlike the F.5/F.6 rendered
// thumbnails which need a device.
//
// Scope is deliberately BC1-OPAQUE (the 4-colour mode, no 1-bit alpha) -- the
// crash-condition transparent mode is a different story and out of this cook's
// first pass. Compression is valid-but-not-optimal: endpoints are the channel
// min/max of the block's diagonal bounding box, which is what a cook wants
// (correctness over squeezing the last bit out of a PNG every frame).
//
// Determinism is the same loaded requirement as the cooker: block order (row
// major, 4x4), endpoint order and index assignment must be a pure function of
// the pixels, so two cooks of the same source are byte-identical.
//
// The mip chain is a plain box downsample (2x2 average), matching the resample
// the thumbnail path already uses -- exponentially smaller power-of-two or
// floor-halved levels down to 1x1.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Wasteland
{
	// A single 4x4 BC1 block: 2 RGB565 endpoints + 4 2-bit indices = 8 bytes.
	// Exposed so a block encoder can be tested in isolation and a tool can be
	// built on it without re-deriving the layout.
	struct Bc1Block
	{
		std::uint8_t Bytes[8];
	};

	// Encode one 4x4 block. `pixels` is 16 RGB triples in row-major block order
	// (top-left of the block first). Deterministic.
	Bc1Block EncodeBc1Block(const std::uint8_t (&pixels)[16][3]);

	// Block count for a w x h RGBA image: ceil(w/4) * ceil(h/4), the standard
	// required storage (a partial block pads to a full 4x4).
	std::size_t Bc1BlockCount(int width, int height);

	// Encode a whole w x h RGBA image into BC1-opaque bytes (blockCount*8).
	// `rgba` is tightly packed RGBA, top row first. A partial right/bottom edge
	// is CLAMPED (the last pixel repeats) -- the standard DXT handling.
	void EncodeBc1Rgba(const std::uint8_t *rgba, int width, int height,
	                   std::vector<std::uint8_t> &out);

	// A 2x2 box downsample of `src` (RGBA, w x h) into `out`, with the output
	// size floor(w/2) x floor(h/2). A size-1 dimension yields size 1 (no shrink).
	// Deterministic; the average is per channel, rounded, of the up-to-4 pixels.
	void DownsampleBox(const std::uint8_t *src, int width, int height,
	                   std::vector<std::uint8_t> &out, int &outWidth,
	                   int &outHeight);

	// The mip sizes starting at (width, height), down to 1x1.
	void MipChainSizes(int width, int height,
	                   std::vector<std::pair<int, int>> &out);

	// Cook a full BC1 mip chain: for every mip in MipChainSizes, box-downsample
	// then BC1-encode, and lay the mips end to end. This is the J.2 textures cook
	// output -- a single deterministic blob a texture import path can store as
	// one asset. `mipSizes` receives (width, height) per mip (== MipChainSizes);
	// `combined` is the concatenation of each mip's Bc1BlockCount*8 bytes, in
	// mip order. Deterministic: identical source, identical blob, so the cooker's
	// content-hash sidecar works on the compressed output exactly as on any asset.
	void CookTextureBc1(const std::uint8_t *rgba, int width, int height,
	                    std::vector<std::uint8_t> &combined,
	                    std::vector<std::pair<int, int>> &mipSizes);
}
