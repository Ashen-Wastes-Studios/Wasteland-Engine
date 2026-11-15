#include "wlpch.h"

#include "Wasteland/Editor/BoneThumbnail.h"

#include <algorithm>
#include <cmath>

namespace Wasteland
{
	namespace
	{
		constexpr int kMargin = 4;

		void SetPixel(std::uint8_t *buf, int size, int x, int y,
		              std::uint8_t r, std::uint8_t g, std::uint8_t b)
		{
			if (x < 0 || y < 0 || x >= size || y >= size)
				return;
			const std::size_t base = 4u * (static_cast<std::size_t>(y) * size + x);
			buf[base + 0] = r;
			buf[base + 1] = g;
			buf[base + 2] = b;
			buf[base + 3] = 255;
		}

		// Bresenham line, written to `buf`. Deterministic by construction.
		void DrawLine(std::uint8_t *buf, int size,
		              float x0, float y0, float x1, float y1,
		              std::uint8_t r, std::uint8_t g, std::uint8_t b)
		{
			int ix0 = (int)std::round(x0), iy0 = (int)std::round(y0);
			int ix1 = (int)std::round(x1), iy1 = (int)std::round(y1);
			const int dx = std::abs(ix1 - ix0);
			const int dy = -std::abs(iy1 - iy0);
			const int sx = ix0 < ix1 ? 1 : -1;
			const int sy = iy0 < iy1 ? 1 : -1;
			int err = dx + dy;
			for (;;)
			{
				// 2 px wide, so a bone reads as a silhouette even at 256px.
				SetPixel(buf, size, ix0, iy0, r, g, b);
				SetPixel(buf, size, ix0 + 1, iy0, r, g, b);
				if (ix0 == ix1 && iy0 == iy1)
					break;
				const int e2 = 2 * err;
				if (e2 >= dy)
				{
					err += dy;
					ix0 += sx;
				}
				if (e2 <= dx)
				{
					err += dx;
					iy0 += sy;
				}
			}
		}
	}

	BoneThumbnailResult MakeBoneHierarchyThumbnail(
	    const std::vector<float> &worldPositions,
	    const std::vector<std::int32_t> &parents,
	    int size)
	{
		BoneThumbnailResult result;
		result.Size = size;
		if (size <= 0)
			return result;

		const std::size_t pixels = static_cast<std::size_t>(size) * size * 4u;
		std::vector<std::uint8_t> buffer(pixels, 0);
		// Background: the editor's dark tone, so a silhouetted bone reads.
		for (std::size_t i = 0; i < pixels; i += 4)
		{
			buffer[i + 0] = 20;
			buffer[i + 1] = 20;
			buffer[i + 2] = 24;
			buffer[i + 3] = 255;
		}

		const std::size_t boneCount = worldPositions.size() / 3u;
		if (boneCount == 0 || parents.size() < boneCount)
		{
			result.Pixels = std::move(buffer);
			return result;
		}

		// Fit: the largest extent of all bones maps to (size - 2*margin).
		float minX = worldPositions[0], maxX = worldPositions[0];
		float minY = worldPositions[1], maxY = worldPositions[1];
		for (std::size_t i = 1; i < boneCount; ++i)
		{
			const float x = worldPositions[i * 3 + 0];
			const float y = worldPositions[i * 3 + 1];
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}
		const float extent = std::max(maxX - minX, maxY - minY);
		const float range = (extent > 1e-6f) ? extent : 1.0f;
		const float scale = (float)(size - 2 * kMargin) / range;
		// Centre via the midpoint of the whole box, not the origin.
		const float midX = (minX + maxX) * 0.5f;
		const float midY = (minY + maxY) * 0.5f;

		const auto project = [&](std::size_t bone, float &px, float &py)
		{
			const float x = (worldPositions[bone * 3 + 0] - midX) * scale + (float)size * 0.5f;
			const float y = -(worldPositions[bone * 3 + 1] - midY) * scale + (float)size * 0.5f;
			px = x;
			py = y;
		};

		// Bone colour (a warm tone on the dark ground), joints a brighter cap.
		const std::uint8_t kR = 224, kG = 172, kB = 128;
		const std::uint8_t kJR = 255, kJG = 255, kJB = 255;

		std::vector<std::uint8_t> h;
		for (std::size_t bone = 1; bone < boneCount; ++bone)
		{
			const std::int32_t parent = parents[bone];
			if (parent < 0 || static_cast<std::size_t>(parent) >= boneCount)
				continue;
			float x0, y0, x1, y1;
			project(bone, x0, y0);
			project(static_cast<std::size_t>(parent), x1, y1);
			// Child->parent line, so a non-root bone is always a segment to its kin.
			DrawLine(buffer.data(), size, x0, y0, x1, y1, kR, kG, kB);
			// A joint dot at each end, so even a near-zero-length level reads.
			SetPixel(buffer.data(), size, (int)x0, (int)y0, kJR, kJG, kJB);
			SetPixel(buffer.data(), size, (int)x1, (int)y1, kJR, kJG, kJB);
		}
		// A lone root (a 1-bone hierarchy) still gets a visible joint.
		{
			float px, py;
			project(0, px, py);
			SetPixel(buffer.data(), size, (int)px, (int)py, kJR, kJG, kJB);
		}

		result.Pixels = std::move(buffer);
		result.BoneCount = (int)boneCount;
		return result;
	}
}
