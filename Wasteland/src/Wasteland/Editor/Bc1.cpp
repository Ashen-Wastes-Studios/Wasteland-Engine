#include "wlpch.h"

#include "Wasteland/Editor/Bc1.h"

#include <algorithm>

namespace Wasteland
{
	namespace
	{
		constexpr std::uint16_t Pack565(int r, int g, int b)
		{
			const int rr = std::max(0, std::min(31, r >> 3));
			const int gg = std::max(0, std::min(63, g >> 2));
			const int bb = std::max(0, std::min(31, b >> 3));
			return static_cast<std::uint16_t>((rr << 11) | (gg << 5) | bb);
		}

		void Unpack565(std::uint16_t packed, int &r, int &g, int &b)
		{
			const int r5 = (packed >> 11) & 0x1F;
			const int g6 = (packed >> 5) & 0x3F;
			const int b5 = packed & 0x1F;
			r = (r5 << 3) | (r5 >> 2);
			g = (g6 << 2) | (g6 >> 4);
			b = (b5 << 3) | (b5 >> 2);
		}

		// Euclidean distance (squared) in RGB.
		int DistSq(int r, int g, int b, int rr, int gg, int bb)
		{
			const int dr = r - rr, dg = g - gg, db = b - bb;
			return dr * dr + dg * dg + db * db;
		}
	}

	Bc1Block EncodeBc1Block(const std::uint8_t (&pixels)[16][3])
	{
		// Endpoints: channel min/max -- a valid, deterministic, opaque-mode block.
		int minR = 255, minG = 255, minB = 255;
		int maxR = 0, maxG = 0, maxB = 0;
		for (int i = 0; i < 16; ++i)
		{
			minR = std::min(minR, (int)pixels[i][0]);
			minG = std::min(minG, (int)pixels[i][1]);
			minB = std::min(minB, (int)pixels[i][2]);
			maxR = std::max(maxR, (int)pixels[i][0]);
			maxG = std::max(maxG, (int)pixels[i][1]);
			maxB = std::max(maxB, (int)pixels[i][2]);
		}

		// 4-colour (opaque) mode requires c0 > c1 as 565 integers. Pack both; the
		// 565 truncation or a monochrome block can produce c0 == c1 (or inverted),
		// which would silently switch to the transparent 3-colour mode and make the
		// closest colour pick the transparent index. Nudge deterministically by one
		// 565 step -- a 1/31 offset is invisible -- to keep the invariant.
		std::uint16_t c0 = Pack565(minR, minG, minB);
		std::uint16_t c1 = Pack565(maxR, maxG, maxB);
		if (c0 <= c1)
		{
			if (c0 < 0xFFFF)
				c0 += 1;
			else
				c1 -= 1;
		}

		int e0r, e0g, e0b, e1r, e1g, e1b;
		Unpack565(c0, e0r, e0g, e0b);
		Unpack565(c1, e1r, e1g, e1b);
		const int p23r = (2 * e0r + e1r) / 3;
		const int p23g = (2 * e0g + e1g) / 3;
		const int p23b = (2 * e0b + e1b) / 3;
		const int p13r = (e0r + 2 * e1r) / 3;
		const int p13g = (e0g + 2 * e1g) / 3;
		const int p13b = (e0b + 2 * e1b) / 3;

		Bc1Block block = {};  // zero-first: the index bytes are OR'd, not assigned
		block.Bytes[0] = static_cast<std::uint8_t>(c0 & 0xFF);
		block.Bytes[1] = static_cast<std::uint8_t>(c0 >> 8);
		block.Bytes[2] = static_cast<std::uint8_t>(c1 & 0xFF);
		block.Bytes[3] = static_cast<std::uint8_t>(c1 >> 8);

		// Each pixel gets the closest of {c0, c1, c2/3, c1/3} (indices 0..3),
		// breaking ties toward the lower index for determinism.
		for (int i = 0; i < 16; ++i)
		{
			const int pr = pixels[i][0], pg = pixels[i][1], pb = pixels[i][2];
			int best = 0;
			int bestDist = DistSq(pr, pg, pb, e0r, e0g, e0b);
			const int d1 = DistSq(pr, pg, pb, e1r, e1g, e1b);
			if (d1 < bestDist) { best = 1; bestDist = d1; }
			const int d2 = DistSq(pr, pg, pb, p23r, p23g, p23b);
			if (d2 < bestDist) { best = 2; bestDist = d2; }
			const int d3 = DistSq(pr, pg, pb, p13r, p13g, p13b);
			if (d3 < bestDist) { best = 3; bestDist = d3; }

			const int byte = 4 + (i / 4);
			const int shift = 2 * (i % 4);
			block.Bytes[byte] |= static_cast<std::uint8_t>((best & 0x3) << shift);
		}
		return block;
	}

	std::size_t Bc1BlockCount(int width, int height)
	{
		const std::size_t cols = static_cast<std::size_t>(width + 3) / 4;
		const std::size_t rows = static_cast<std::size_t>(height + 3) / 4;
		return cols * rows;
	}

	void EncodeBc1Rgba(const std::uint8_t *rgba, int width, int height,
	                   std::vector<std::uint8_t> &out)
	{
		const std::size_t blocks = Bc1BlockCount(width, height);
		out.clear();
		out.reserve(blocks * 8);
		const int blockCols = (width + 3) / 4;
		const int blockRows = (height + 3) / 4;

		for (int by = 0; by < blockRows; ++by)
		{
			for (int bx = 0; bx < blockCols; ++bx)
			{
				std::uint8_t pixels[16][3];
				for (int py = 0; py < 4; ++py)
				{
					for (int px = 0; px < 4; ++px)
					{
						const int x = bx * 4 + px;
						const int y = by * 4 + py;
						const int cx = std::min(x, width - 1); // clamp edges
						const int cy = std::min(y, height - 1);
						const std::size_t idx =
						    (static_cast<std::size_t>(cy) * width + cx) * 4;
						pixels[py * 4 + px][0] = rgba[idx + 0];
						pixels[py * 4 + px][1] = rgba[idx + 1];
						pixels[py * 4 + px][2] = rgba[idx + 2];
					}
				}
				const Bc1Block block = EncodeBc1Block(pixels);
				out.insert(out.end(), block.Bytes, block.Bytes + 8);
			}
		}
	}

	void DownsampleBox(const std::uint8_t *src, int width, int height,
	                   std::vector<std::uint8_t> &out, int &outWidth,
	                   int &outHeight)
	{
		outWidth = std::max(1, width / 2);
		outHeight = std::max(1, height / 2);
		out.assign(static_cast<std::size_t>(outWidth) * outHeight * 4, 0);

		for (int y = 0; y < outHeight; ++y)
			for (int x = 0; x < outWidth; ++x)
			{
				int sum[4] = { 0, 0, 0, 0 };
				int count = 0;
				for (int dy = 0; dy < 2; ++dy)
					for (int dx = 0; dx < 2; ++dx)
					{
						const int sx = x * 2 + dx;
						const int sy = y * 2 + dy;
						if (sx >= width || sy >= height)
							continue;
						const std::size_t idx = (static_cast<std::size_t>(sy) * width + sx) * 4;
						for (int c = 0; c < 4; ++c)
							sum[c] += src[idx + c];
						count += 1;
					}
				if (count == 0)
					count = 1;
				const std::size_t o = (static_cast<std::size_t>(y) * outWidth + x) * 4;
				for (int c = 0; c < 4; ++c)
					out[o + c] = static_cast<std::uint8_t>(sum[c] / count);
			}
	}

	void MipChainSizes(int width, int height,
	                   std::vector<std::pair<int, int>> &out)
	{
		out.clear();
		int w = std::max(1, width), h = std::max(1, height);
		while (true)
		{
			out.emplace_back(w, h);
			if (w == 1 && h == 1)
				break;
			w = std::max(1, w / 2);
			h = std::max(1, h / 2);
		}
	}

	void CookTextureBc1(const std::uint8_t *rgba, int width, int height,
	                    std::vector<std::uint8_t> &combined,
	                    std::vector<std::pair<int, int>> &mipSizes)
	{
		combined.clear();
		mipSizes.clear();
		MipChainSizes(width, height, mipSizes);

		const std::uint8_t *current = rgba;
		int w = std::max(1, width), h = std::max(1, height);
		std::vector<std::uint8_t> holder;  // outlives the loop, so `current` stays valid
		for (size_t m = 0; m < mipSizes.size(); ++m)
		{
			const int mw = mipSizes[m].first, mh = mipSizes[m].second;
			std::vector<std::uint8_t> encoded;
			EncodeBc1Rgba(current, mw, mh, encoded);
			combined.insert(combined.end(), encoded.begin(), encoded.end());

			// Downsample the SOURCE for the next loop iteration (not the encoded
			// bytes -- those are BC1, not RGBA source for the next mip).
			if (m + 1 < mipSizes.size())
			{
				std::vector<std::uint8_t> down;
				int dw = 0, dh = 0;
				DownsampleBox(current, w, h, down, dw, dh);
				holder.swap(down);
				current = holder.data();
				w = dw;
				h = dh;
			}
		}
	}
}
