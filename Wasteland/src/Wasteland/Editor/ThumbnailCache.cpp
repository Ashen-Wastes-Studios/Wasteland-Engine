#include "wlpch.h"

#include "Wasteland/Editor/ThumbnailCache.h"
#include "Wasteland/Editor/ThumbnailGraph.h"

#include "Wasteland/VisualBlocks/Graph.h"
#include "Wasteland/VisualBlocks/GraphSerializer.h"
#include "Wasteland/VisualBlocks/NodeRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

namespace Wasteland::Editor
{
	// ---------------------------------------------------------------------
	// Kind classification
	// ---------------------------------------------------------------------

	const char *ThumbnailKindName(ThumbnailKind kind)
	{
		switch (kind)
		{
		case ThumbnailKind::Texture:      return "Texture";
		case ThumbnailKind::Mesh:         return "Mesh";
		case ThumbnailKind::Material:     return "Material";
		case ThumbnailKind::Animation:    return "Animation";
		case ThumbnailKind::Scene:        return "Scene";
		case ThumbnailKind::FlowGraph:    return "FlowGraph";
		case ThumbnailKind::BehaviorTree: return "BehaviorTree";
		case ThumbnailKind::AudioGraph:   return "AudioGraph";
		case ThumbnailKind::AudioClip:    return "AudioClip";
		case ThumbnailKind::Terrain:      return "Terrain";
		case ThumbnailKind::Script:       return "Script";
		case ThumbnailKind::Shader:       return "Shader";
		case ThumbnailKind::Document:     return "Document";
		case ThumbnailKind::Compiled:     return "Compiled";
		case ThumbnailKind::Directory:    return "Directory";
		case ThumbnailKind::Unknown:      break;
		}
		return "Unknown";
	}

	namespace
	{
		// Lowercase the extension IN PLACE and return it without the dot.
		//
		// Done by hand rather than with std::tolower on the whole path: the path
		// itself is case-significant on some filesystems, and only the extension
		// is a type marker. std::tolower on a char is also locale-dependent for
		// values above 127, so the cast goes through unsigned char.
		std::string LowercaseExtension(const std::filesystem::path &path)
		{
			std::string extension = path.extension().string();
			if (!extension.empty() && extension.front() == '.')
				extension.erase(extension.begin());
			for (char &c : extension)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return extension;
		}
	}

	ThumbnailKind Classify(const std::filesystem::path &path)
	{
		// A directory is classified before the extension test. An extension-less
		// directory would otherwise be Unknown, and the browser shows folders
		// through this same classifier.
		std::error_code error;
		if (std::filesystem::is_directory(path, error))
			return ThumbnailKind::Directory;

		const std::string extension = LowercaseExtension(path);

		// No extension is Unknown, NOT Script. See the header.
		if (extension.empty())
			return ThumbnailKind::Unknown;

		if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
			extension == "bmp" || extension == "tga" || extension == "gif" ||
			extension == "webp")
			return ThumbnailKind::Texture;

		if (extension == "obj" || extension == "gltf" || extension == "glb" ||
			extension == "fbx" || extension == "dae" || extension == "stl" ||
			extension == "ply" || extension == "wlmesh")
			return ThumbnailKind::Mesh;

		if (extension == "wlmaterial")
			return ThumbnailKind::Material;

		if (extension == "wlanim")
			return ThumbnailKind::Animation;

		if (extension == "wastescene" || extension == "wlbin")
			return ThumbnailKind::Scene;

		if (extension == "wlblocks")
			return ThumbnailKind::FlowGraph;

		if (extension == "wlbtree")
			return ThumbnailKind::BehaviorTree;

		if (extension == "wlavb")
			return ThumbnailKind::AudioGraph;

		if (extension == "wav" || extension == "ogg" || extension == "mp3")
			return ThumbnailKind::AudioClip;

		if (extension == "wlterrain")
			return ThumbnailKind::Terrain;

		if (extension == "py")
			return ThumbnailKind::Script;

		if (extension == "glsl" || extension == "hlsl" || extension == "slang" ||
			extension == "spv" || extension == "cso")
			return ThumbnailKind::Shader;

		if (extension == "md" || extension == "txt")
			return ThumbnailKind::Document;

		if (extension == "pyc" || extension == "r8")
			return ThumbnailKind::Compiled;

		return ThumbnailKind::Unknown;
	}

	ThumbnailKind Classify(const std::string &path)
	{
		return Classify(std::filesystem::path(path));
	}

	// ---------------------------------------------------------------------
	// Cache key
	// ---------------------------------------------------------------------

	bool ThumbnailCacheKey::operator==(const ThumbnailCacheKey &other) const
	{
		return ModifiedTime == other.ModifiedTime &&
			   Size == other.Size &&
			   RelativePath == other.RelativePath;
	}

	std::string ThumbnailCacheKey::ToString() const
	{
		// The exact format is a contract: it is what the disk cache names its
		// files after. Changing it invalidates every cached thumbnail on disk.
		return RelativePath + "|" + std::to_string(ModifiedTime) + "|" +
			   std::to_string(Size);
	}

	ThumbnailCacheKey MakeThumbnailCacheKey(const std::string &relativePath,
											const std::string &absolutePath)
	{
		ThumbnailCacheKey key;
		key.RelativePath = relativePath;

		// The path is normalised to forward slashes so the key is the same on
		// Windows and POSIX: a cache key that differs by platform would miss
		// every entry when a project moves between them.
		std::replace(key.RelativePath.begin(), key.RelativePath.end(), '\\', '/');

		const std::filesystem::path absolute(absolutePath);
		std::error_code error;
		const auto modified = std::filesystem::last_write_time(absolute, error);
		if (!error)
		{
			// The epoch differs between libstdc++ and MSVC, so the raw count is
			// only a CHANGE DETECTOR, never a portable timestamp. That is all a
			// cache key needs: it must differ when the file changes and match
			// when it does not.
			key.ModifiedTime = static_cast<std::uint64_t>(modified.time_since_epoch().count());
		}

		const auto size = std::filesystem::file_size(absolute, error);
		if (!error)
			key.Size = static_cast<std::uint64_t>(size);

		return key;
	}

	// ---------------------------------------------------------------------
	// ThumbnailCache
	// ---------------------------------------------------------------------

	ThumbnailCache::ThumbnailCache() = default;
	ThumbnailCache::~ThumbnailCache() = default;

	void ThumbnailCache::SetGenerator(
		std::function<ThumbnailEntry(const ThumbnailCacheKey &)> generator)
	{
		m_Generator = std::move(generator);
	}

	ThumbnailEntry *ThumbnailCache::Request(
		const std::string &relativePath,
		const std::string &absolutePath,
		const std::unordered_map<std::string, bool> &visible)
	{
		const ThumbnailCacheKey key =
			MakeThumbnailCacheKey(relativePath, absolutePath);
		const std::string name = key.RelativePath;

		if (auto it = m_Entries.find(name); it != m_Entries.end())
		{
			it->second.LastUsed = ++m_Now;
			// The key changes when the file changes, so a stale entry must not be
			// handed back just because the path matches.
			if (it->second.Thumbnail.Key == key)
				return &it->second.Thumbnail;
			// Same path, different content: drop the old entry and regenerate.
			m_Entries.erase(it);
		}

		// A memory miss consults the disk cache before anything is queued. The
		// freshness rule compares the cached file's mtime against the source's,
		// so a stale cache is simply a miss -- there is no index file that could
		// disagree with the PNGs it describes.
		if (m_Disk.IsEnabled() && m_Disk.IsFresh(relativePath, absolutePath))
		{
			std::vector<std::uint8_t> rgba;
			std::uint32_t width = 0, height = 0;
			std::string error;
			if (m_Disk.Read(relativePath, rgba, width, height, error))
			{
				Entry &entry = m_Entries[name];
				entry.AbsolutePath = absolutePath;
				entry.Thumbnail.Kind = std::filesystem::is_directory(absolutePath)
					? ThumbnailKind::Directory
					: Classify(relativePath);
				entry.Thumbnail.Key = key;
				entry.Thumbnail.Width = width;
				entry.Thumbnail.Height = height;
				entry.Thumbnail.Pixels = std::move(rgba);
				entry.Thumbnail.Ready = true;
				entry.LastUsed = ++m_Now;
				return &entry.Thumbnail;
			}
		}

		// A not-ready placeholder. The caller draws it and calls Tick(); returning
		// null here would force every call site to null-check.
		Entry &entry = m_Entries[name];
		entry.Thumbnail.Kind = std::filesystem::is_directory(absolutePath)
			? ThumbnailKind::Directory
			: Classify(relativePath);
		entry.Thumbnail.Key = key;
		entry.Thumbnail.Width = kThumbnailSize;
		entry.Thumbnail.Height = kThumbnailSize;
		entry.Thumbnail.Ready = false;
		entry.LastUsed = ++m_Now;
		entry.AbsolutePath = absolutePath;

		// Only queue what is on screen. A scrolled-past entry is the easiest way
		// to spend a frame budget for nothing.
		const bool isVisible = visible.find(name) != visible.end();
		if (isVisible)
			m_Pending.push_back(key);

		EvictIfNeeded();
		return &entry.Thumbnail;
	}

	ThumbnailEntry *ThumbnailCache::Find(const std::string &relativePath)
	{
		const auto it = m_Entries.find(relativePath);
		return it == m_Entries.end() ? nullptr : &it->second.Thumbnail;
	}

	int ThumbnailCache::Tick(float dt)
	{
		(void)dt;

		if (!m_Generator)
			return 0;

		int generated = 0;

		// Process at most kMaxPerFrame entries. The loop scans for a key that is
		// still absent AND still pending -- an entry may have been generated by an
		// earlier iteration of this same loop, and re-generating it would burn
		// the whole budget on one asset.
		while (generated < kMaxPerFrame && !m_Pending.empty())
		{
			const ThumbnailCacheKey key = m_Pending.front();
			m_Pending.erase(m_Pending.begin());

			const auto it = m_Entries.find(key.RelativePath);
			if (it == m_Entries.end())
				continue;
			if (it->second.Thumbnail.Ready)
				continue;

			ThumbnailEntry rendered = m_Generator(key);
			rendered.Key = key;
			rendered.Ready = true;
			// The generator sets Failed when the source could not be read. A
			// failed entry is cached like any other so it is not retried.
			it->second.Thumbnail = std::move(rendered);

			// Write-through. The disk cache is written HERE, on the main thread,
			// because Tick() runs on the main thread and the generator is not
			// thread-safe (it may touch a GL context). A background write would
			// need the pixels copied off first for no benefit: the file is
			// ~256 KB and the encode is microseconds.
			//
			// A failed entry is NOT written. A corrupt asset would otherwise get
			// a "failed" PNG cached against it, and the next run would load that
			// instead of showing the icon.
			if (m_Disk.IsEnabled() && !it->second.Thumbnail.Failed)
			{
				std::string error;
				m_Disk.Write(key.RelativePath,
							 it->second.Thumbnail.Pixels.data(),
							 it->second.Thumbnail.Width,
							 it->second.Thumbnail.Height,
							 error);
			}

			++generated;
		}

		return generated;
	}

	void ThumbnailCache::InvalidateAll()
	{
		m_Entries.clear();
		m_Pending.clear();
		m_Evictions = 0;
		m_Now = 0;
		// The throttle table is keyed by path and cleared with the entries, so
		// a refresh does not leave a stale "just checked" timestamp that would
		// suppress the first post-refresh check.
		m_LastChangeCheck.clear();
	}

	int ThumbnailCache::CheckForChanges(
		const std::unordered_map<std::string, bool> &visible,
		float nowSeconds, float throttleSeconds)
	{
		int invalidated = 0;

		for (const auto &pair : visible)
		{
			const std::string &relativePath = pair.first;

			// Throttle. The listing is rebuilt every frame, so an unthrottled
			// stat per entry per frame is a stat storm on a large folder.
			if (auto checked = m_LastChangeCheck.find(relativePath);
				checked != m_LastChangeCheck.end())
			{
				if (nowSeconds - checked->second < throttleSeconds)
					continue;
			}
			m_LastChangeCheck[relativePath] = nowSeconds;

			// Only a READY entry can be stale: a not-ready placeholder has
			// nothing to invalidate, and queueing it twice would burn the frame
			// budget on one asset.
			auto entry = m_Entries.find(relativePath);
			if (entry == m_Entries.end() || !entry->second.Thumbnail.Ready)
				continue;

			// Re-key against the source. The key is path + mtime + size, so a
			// changed file produces a different key; the comparison is the
			// whole check and needs no separate stat call.
			const ThumbnailCacheKey current =
				MakeThumbnailCacheKey(relativePath, entry->second.AbsolutePath);
			if (current == entry->second.Thumbnail.Key)
				continue;

			m_Entries.erase(entry);
			// A stale path may still be queued; drop it so the queue does not
			// hold a key for an entry that no longer exists.
			m_Pending.erase(std::remove_if(m_Pending.begin(), m_Pending.end(),
										   [&relativePath](const ThumbnailCacheKey &key)
										   {
											   return key.RelativePath == relativePath;
										   }),
							m_Pending.end());
			++invalidated;
		}

		return invalidated;
	}

	void ThumbnailCache::EvictIfNeeded()
	{
		// A partial selection rather than a full sort: the cap is 256 and the
		// eviction is amortised over inserts, so an O(n log n) sort here would
		// dominate the insert it follows.
		while (m_Entries.size() > kMaxEntries)
		{
			auto oldest = m_Entries.begin();
			for (auto it = m_Entries.begin(); it != m_Entries.end(); ++it)
				if (it->second.LastUsed < oldest->second.LastUsed)
					oldest = it;

			// Also drop it from the pending queue, or a stale key would
			// re-insert an entry the cache just decided it could not hold.
			const std::string &name = oldest->first;
			m_Pending.erase(std::remove_if(m_Pending.begin(), m_Pending.end(),
										   [&name](const ThumbnailCacheKey &key)
										   {
											   return key.RelativePath == name;
										   }),
							m_Pending.end());

			m_Entries.erase(oldest);
			++m_Evictions;
		}
	}

	// ---------------------------------------------------------------------
	// Procedural type icons (Task F.2)
	// ---------------------------------------------------------------------

	namespace
	{
		struct Color
		{
			std::uint8_t R, G, B, A;
		};

		constexpr Color kBackground { 32, 34, 40, 255 };
		constexpr Color kInk        { 226, 230, 238, 255 };
		constexpr Color kAccent     { 220, 20, 60, 255 };   // the studio's blood red
		constexpr Color kCool       { 80, 140, 200, 255 };
		constexpr Color kWarm       { 230, 170, 70, 255 };
		constexpr Color kGreen      { 90, 190, 110, 255 };
		constexpr Color kDim        { 70, 74, 84, 255 };

		// A tiny RGBA rasterizer. Deliberately not ImGui and deliberately not an
		// image file: an icon PNG would be a new asset the repo must ship, version
		// and keep in sync with the code, whereas this cannot drift. The same
		// reasoning as the hand-written WritePNG in EditorLayer.
		class Raster
		{
		public:
			Raster(std::uint32_t width, std::uint32_t height)
				: m_Width(width), m_Height(height), m_Pixels((size_t)width * height * 4, 0)
			{
			}

			void Clear(Color color)
			{
				for (std::uint32_t y = 0; y < m_Height; ++y)
					for (std::uint32_t x = 0; x < m_Width; ++x)
						SetPixel(x, y, color);
			}

			void SetPixel(int x, int y, Color color)
			{
				if (x < 0 || y < 0 || x >= static_cast<int>(m_Width) ||
					y >= static_cast<int>(m_Height))
					return;
				const size_t index = (static_cast<size_t>(y) * m_Width + x) * 4;
				m_Pixels[index + 0] = color.R;
				m_Pixels[index + 1] = color.G;
				m_Pixels[index + 2] = color.B;
				m_Pixels[index + 3] = color.A;
			}

			// Signed-area coverage for a rounded rectangle. Sampling at pixel
			// centres keeps the edges aliased but correct; a supersampled edge
			// would cost 4x the fill for an icon nobody inspects at 1:1.
			void FillRoundedRect(int x0, int y0, int x1, int y1, int radius, Color color)
			{
				if (x1 < x0) std::swap(x0, x1);
				if (y1 < y0) std::swap(y0, y1);
				radius = std::max(0, std::min(radius, std::min(x1 - x0, y1 - y0) / 2));

				for (int y = y0; y <= y1; ++y)
				{
					for (int x = x0; x <= x1; ++x)
					{
						// Distance into the nearest corner circle's centre.
						int dx = 0, dy = 0;
						if (x < x0 + radius && y < y0 + radius)
						{
							dx = x0 + radius - x;
							dy = y0 + radius - y;
						}
						else if (x > x1 - radius && y < y0 + radius)
						{
							dx = x - (x1 - radius);
							dy = y0 + radius - y;
						}
						else if (x < x0 + radius && y > y1 - radius)
						{
							dx = x0 + radius - x;
							dy = y - (y1 - radius);
						}
						else if (x > x1 - radius && y > y1 - radius)
						{
							dx = x - (x1 - radius);
							dy = y - (y1 - radius);
						}
						if (dx * dx + dy * dy <= radius * radius)
							SetPixel(x, y, color);
					}
				}
			}

			void FillCircle(int cx, int cy, int radius, Color color)
			{
				for (int y = cy - radius; y <= cy + radius; ++y)
					for (int x = cx - radius; x <= cx + radius; ++x)
						if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius)
							SetPixel(x, y, color);
			}

			// Bresenham, with a square brush of the given thickness.
			void DrawLine(int x0, int y0, int x1, int y1, int thickness, Color color)
			{
				const int half = thickness / 2;
				int x = x0, y = y0;
				const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
				const int sx = x0 < x1 ? 1 : -1;
				const int sy = y0 < y1 ? 1 : -1;
				int err = dx - dy;

				while (true)
				{
					FillCircle(x, y, half, color);
					if (x == x1 && y == y1)
						break;
					const int twice = 2 * err;
					if (twice > -dy) { err -= dy; x += sx; }
					if (twice < dx)  { err += dx; y += sy; }
				}
			}

			std::uint32_t GetWidth() const { return m_Width; }
			std::uint32_t GetHeight() const { return m_Height; }
			const std::uint8_t *GetPixels() const { return m_Pixels.data(); }

		private:
			std::uint32_t m_Width, m_Height;
			std::vector<std::uint8_t> m_Pixels;
		};

		// Draw one icon. Each silhouette uses at least four distinct colours --
		// the verification requires it, because a blank or single-colour icon
		// would pass a "did not crash" test.
		void DrawIcon(Raster &raster, ThumbnailKind kind)
		{
			const int s = static_cast<int>(raster.GetWidth());

			switch (kind)
			{
			case ThumbnailKind::Texture:
				// A framed picture: frame, sky, a sun and a mountain.
				raster.FillRoundedRect(36, 36, s - 37, s - 37, 12, kInk);
				raster.FillRoundedRect(48, 48, s - 49, s - 49, 8, kCool);
				raster.FillCircle(s * 3 / 4, s / 3, 18, kWarm);
				raster.DrawLine(48, s - 60, s / 2, s * 2 / 5, 6, kGreen);
				raster.DrawLine(s / 2, s * 2 / 5, s - 48, s - 60, 6, kGreen);
				break;

			case ThumbnailKind::Mesh:
				// A wireframe cube: front face, back face, four joining edges.
				raster.DrawLine(64, 96, 192, 96, 3, kInk);
				raster.DrawLine(64, 192, 192, 192, 3, kInk);
				raster.DrawLine(64, 96, 64, 192, 3, kInk);
				raster.DrawLine(192, 96, 192, 192, 3, kInk);
				raster.DrawLine(88, 72, 216, 72, 3, kDim);
				raster.DrawLine(88, 168, 216, 168, 3, kDim);
				raster.DrawLine(88, 72, 88, 168, 3, kDim);
				raster.DrawLine(216, 72, 216, 168, 3, kDim);
				raster.DrawLine(64, 96, 88, 72, 3, kCool);
				raster.DrawLine(192, 96, 216, 72, 3, kCool);
				raster.DrawLine(64, 192, 88, 168, 3, kCool);
				raster.DrawLine(192, 192, 216, 168, 3, kCool);
				break;

			case ThumbnailKind::Material:
				// A sphere with a highlight and a shadow. ORDER MATTERS: the base
				// colour is filled FIRST and the highlights drawn over it, or the
				// base circle erases the details drawn before it -- which is how
				// this icon shipped with three visible colours instead of four.
				raster.FillCircle(s / 2, s / 2, 72, kAccent);
				raster.FillCircle(s / 2 - 24, s / 2 - 28, 22, kInk);
				raster.FillCircle(s / 2 + 30, s / 2 + 34, 14, kDim);
				raster.DrawLine(s / 2 - 60, s / 2 + 40, s / 2 + 60, s / 2 + 40, 3, kCool);
				break;

			case ThumbnailKind::Animation:
				// A bone: two circles joined by a bar, plus a joint dot.
				raster.FillCircle(74, 74, 26, kWarm);
				raster.FillCircle(182, 182, 26, kWarm);
				raster.DrawLine(74, 74, 182, 182, 26, kInk);
				raster.FillCircle(s / 2, s / 2, 16, kAccent);
				raster.FillCircle(74, 182, 20, kCool);
				break;

			case ThumbnailKind::Scene:
				// A stage with a backdrop.
				raster.FillRoundedRect(56, 56, s - 57, s - 57, 10, kDim);
				raster.FillRoundedRect(72, 72, s - 73, s - 97, 6, kCool);
				raster.DrawLine(72, s - 80, s - 73, s - 80, 4, kInk);
				raster.DrawLine(72, s - 96, 72, s - 64, 3, kWarm);
				raster.FillCircle(s / 2, s - 64, 14, kAccent);
				break;

			case ThumbnailKind::FlowGraph:
				// Three boxes joined by a directed wire.
				raster.FillRoundedRect(40, 44, 104, 108, 6, kInk);
				raster.FillRoundedRect(152, 44, 216, 108, 6, kCool);
				raster.FillRoundedRect(96, 148, 160, 212, 6, kAccent);
				raster.DrawLine(104, 76, 152, 76, 3, kGreen);
				raster.DrawLine(184, 108, 128, 148, 3, kGreen);
				break;

			case ThumbnailKind::BehaviorTree:
				// A root box with two children.
				raster.FillRoundedRect(96, 40, 160, 104, 6, kAccent);
				raster.FillRoundedRect(40, 152, 104, 216, 6, kCool);
				raster.FillRoundedRect(152, 152, 216, 216, 6, kCool);
				raster.DrawLine(112, 104, 72, 152, 3, kInk);
				raster.DrawLine(144, 104, 184, 152, 3, kInk);
				break;

			case ThumbnailKind::AudioGraph:
				// Three boxes joined by undirected wires.
				raster.FillRoundedRect(40, 44, 104, 108, 6, kInk);
				raster.FillRoundedRect(152, 44, 216, 108, 6, kCool);
				raster.FillRoundedRect(96, 148, 160, 212, 6, kWarm);
				raster.DrawLine(104, 76, 152, 76, 3, kDim);
				raster.DrawLine(72, 108, 128, 148, 3, kDim);
				raster.DrawLine(184, 108, 128, 148, 3, kDim);
				break;

			case ThumbnailKind::AudioClip:
				// A note. Head, stem, flag and a dot are four distinct colours:
				// with the stem the same colour as the head this icon measured
				// three visible colours, because the head and stem merge into
				// one region.
				raster.FillCircle(96, 176, 26, kAccent);
				raster.DrawLine(122, 176, 122, 88, 5, kCool);
				raster.DrawLine(122, 88, 176, 100, 5, kWarm);
				raster.FillCircle(176, 100, 10, kWarm);
				raster.FillCircle(96, 176, 10, kInk);
				break;

			case ThumbnailKind::Terrain:
				// A heightmap grid with a peak.
				for (int i = 0; i <= 4; ++i)
				{
					raster.DrawLine(56 + i * 34, 64, 56 + i * 34, 192, 2, kDim);
					raster.DrawLine(56, 64 + i * 32, 192, 64 + i * 32, 2, kDim);
				}
				raster.DrawLine(56, 192, 124, 96, 5, kGreen);
				raster.DrawLine(124, 96, 192, 192, 5, kGreen);
				raster.FillCircle(124, 96, 10, kAccent);
				break;

			case ThumbnailKind::Script:
				// Angle brackets. The two brackets are DIFFERENT colours: with
				// both kCool this icon measured three visible colours, because
				// the left and right brackets read as one region.
				raster.DrawLine(84, 72, 56, 128, 6, kCool);
				raster.DrawLine(56, 128, 84, 184, 6, kCool);
				raster.DrawLine(172, 72, 200, 128, 6, kWarm);
				raster.DrawLine(200, 128, 172, 184, 6, kWarm);
				raster.DrawLine(140, 64, 116, 192, 5, kAccent);
				raster.FillCircle(140, 64, 10, kInk);
				break;

			case ThumbnailKind::Shader:
				// A pixel grid.
				for (int gy = 0; gy < 3; ++gy)
					for (int gx = 0; gx < 3; ++gx)
					{
						const Color shades[3][3] = {
							{ kCool, kInk,   kDim   },
							{ kInk,  kAccent, kCool  },
							{ kDim,   kCool,  kInk   },
						};
						raster.FillRoundedRect(64 + gx * 34, 64 + gy * 34,
											   88 + gx * 34, 88 + gy * 34, 4,
											   shades[gy][gx]);
					}
				break;

			case ThumbnailKind::Document:
				// A page with lines.
				raster.FillRoundedRect(72, 48, 184, 208, 8, kInk);
				raster.FillRoundedRect(84, 60, 172, 196, 6, kDim);
				for (int i = 0; i < 5; ++i)
					raster.DrawLine(96, 84 + i * 22, 160, 84 + i * 22, 4, kCool);
				raster.DrawLine(96, 196, 132, 196, 4, kAccent);
				break;

			case ThumbnailKind::Compiled:
				// A binary block.
				raster.FillRoundedRect(64, 64, 192, 192, 10, kDim);
				for (int i = 0; i < 4; ++i)
				{
					raster.DrawLine(84, 88 + i * 26, 120, 88 + i * 26, 6, kCool);
					raster.DrawLine(140, 88 + i * 26, 172, 88 + i * 26, 6,
								   (i % 2 == 0) ? kAccent : kGreen);
				}
				break;

			case ThumbnailKind::Directory:
				// A folder: body, tab, inner panel and a label line, four
				// distinct colours. The tab is kDim rather than the body colour
				// -- with the tab the same colour as the body the icon measured
				// three visible colours, because the tab merged into it.
				raster.FillRoundedRect(48, 84, 208, 188, 10, kWarm);
				raster.FillRoundedRect(48, 64, 128, 100, 8, kDim);
				raster.FillRoundedRect(60, 104, 196, 176, 6, kBackground);
				raster.DrawLine(76, 140, 180, 140, 4, kInk);
				raster.DrawLine(76, 158, 140, 158, 4, kCool);
				break;

			case ThumbnailKind::Unknown:
				// A question mark: a hook, a stem and a dot. The hook and the
				// stem are DIFFERENT colours, and the stem has a cap dot, giving
				// four distinct colours -- with the whole mark one colour this
				// icon measured three.
				raster.DrawLine(156, 64, 156, 96, 8, kWarm);
				raster.DrawLine(156, 64, 96, 64, 8, kWarm);
				raster.DrawLine(96, 64, 96, 112, 8, kWarm);
				raster.DrawLine(96, 112, 128, 140, 8, kWarm);
				raster.DrawLine(128, 140, 128, 160, 8, kCool);
				raster.FillCircle(128, 196, 12, kAccent);
				raster.FillCircle(128, 64, 10, kInk);
				break;
			}
		}
	}

	// Generate the fallback icon for a kind. Public because the cache's default
	// generator uses it and a test wants to compare a real thumbnail against the
	// icon it should have fallen back to.
	ThumbnailEntry GenerateIcon(ThumbnailKind kind)
	{
		ThumbnailEntry entry;
		entry.Kind = kind;
		entry.Width = ThumbnailCache::kThumbnailSize;
		entry.Height = ThumbnailCache::kThumbnailSize;
		entry.Ready = true;

		Raster raster(entry.Width, entry.Height);
		raster.Clear(kBackground);
		DrawIcon(raster, kind);

		entry.Pixels.assign(raster.GetPixels(),
							raster.GetPixels() +
								static_cast<size_t>(entry.Width) * entry.Height * 4);
		return entry;
	}

	// The default generator: icons only. The editor replaces it with one that
	// renders real assets; a test that does not install a generator still gets a
	// complete, non-empty thumbnail for every kind.
	ThumbnailEntry GenerateDefaultThumbnail(const ThumbnailCacheKey &key)
	{
		(void)key;
		return GenerateIcon(ThumbnailKind::Unknown);
	}
}

namespace Wasteland::Editor
{

// ---------------------------------------------------------------------
// Graph thumbnails (Task F.4)
// ---------------------------------------------------------------------

namespace
{
	struct NodeLayout
	{
		float X = 0.0f, Y = 0.0f;       // box top-left, graph canvas space
		float Width = 0.0f, Height = 0.0f;
		int InputCount = 0;
		int OutputCount = 0;
		bool IsEvent = false;
	};

	float PinY(const NodeLayout &node, int index)
	{
		return node.Y + 28.0f + 13.0f * static_cast<float>(index);
	}
}

GraphThumbnailResult MakeGraphThumbnail(const std::string &path, uint32_t size)
{
	GraphThumbnailResult result;

	using namespace Wasteland::VisualBlocks;

	Graph graph;
	GraphError error;
	if (!GraphSerializer::Load(path, graph, error))
	{
		// The serializer message names the file and the offending key. A
		// thumbnail must never throw out of a directory listing, so this is a
		// returned error, not an exception.
		result.Error = error.IsError ? error.Message
									 : std::string("graph load failed");
		return result;
	}

	result.NodeCount = static_cast<int>(graph.Nodes.size());
	result.WireCount = static_cast<int>(graph.Links.size());

	const NodeRegistry &registry = NodeRegistry::Get();

	// ---- node boxes ---------------------------------------------------
	// Pin counts come from the registry when the type is known, falling back
	// to the instance own inputs. An unregistered type still gets a box -- a
	// graph saved by a newer build than the running one must thumbnail, not
	// blank.
	std::vector<NodeLayout> layouts;
	layouts.reserve(graph.Nodes.size());

	for (const GraphNode &node : graph.Nodes)
	{
		NodeLayout layout;
		layout.X = node.X;
		layout.Y = node.Y;

		std::vector<PinDef> pins;
		if (const NodeDef *def = registry.Find(node.TypeId))
			pins = NodeRegistry::EffectivePins(*def, graph, node);

		if (!pins.empty())
		{
			for (const PinDef &pin : pins)
			{
				if (pin.Dir == PinDir::In)
					++layout.InputCount;
				else
					++layout.OutputCount;
			}
		}
		else
		{
			// GraphNode has no Inputs member -- the resolved per-instance inputs
			// are the links, and the literal-bearing pins are `Literals`. So the
			// fallback counts come from the wire list: distinct ToPin values
			// targeting this node, distinct FromPin values leaving it. That is
			// the honest answer from the data a graph actually carries, and it
			// keeps a graph saved by a newer build thumbnail-able rather than
			// blank.
			for (const GraphLink &link : graph.Links)
			{
				if (link.ToNode == node.Id)
					++layout.InputCount;
				if (link.FromNode == node.Id)
					++layout.OutputCount;
			}
			// A head node with no outgoing wire still needs an exec out stub, or
			// its pin row would collapse to the input count.
			if (layout.OutputCount == 0)
				layout.OutputCount = 1;
		}

		const int rows = std::max(1, std::max(layout.InputCount, layout.OutputCount));
		layout.Width = 120.0f;
		layout.Height = 36.0f + 13.0f * static_cast<float>(rows);
		layout.IsEvent = !node.EventName.empty() || node.IsTypeNamedEvent();

		layouts.push_back(layout);
	}

	// ---- bounding box, with a fallback for an un-authored layout -------
	// A graph whose nodes all sit at the origin (the common result of
	// authoring from Python rather than on the canvas) has an empty bounding
	// box. Stacking them in a column is the honest rendering of that: the
	// wires still show the topology, which is the point of a thumbnail.
	float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
	bool haveBox = false;
	for (const NodeLayout &layout : layouts)
	{
		if (!haveBox)
		{
			minX = layout.X;
			minY = layout.Y;
			maxX = layout.X + layout.Width;
			maxY = layout.Y + layout.Height;
			haveBox = true;
			continue;
		}
		minX = std::min(minX, layout.X);
		minY = std::min(minY, layout.Y);
		maxX = std::max(maxX, layout.X + layout.Width);
		maxY = std::max(maxY, layout.Y + layout.Height);
	}

	const float bboxWidth = haveBox ? (maxX - minX) : 0.0f;
	const float bboxHeight = haveBox ? (maxY - minY) : 0.0f;

	if (bboxWidth < 1e-3f || bboxHeight < 1e-3f)
	{
		for (size_t i = 0; i < layouts.size(); ++i)
		{
			layouts[i].X = 40.0f;
			layouts[i].Y = 40.0f + static_cast<float>(i) * 140.0f;
		}
		minX = 40.0f;
		minY = 40.0f;
		maxX = 160.0f;
		maxY = 40.0f + static_cast<float>(std::max<size_t>(1, layouts.size()) - 1) * 140.0f
			   + (layouts.empty() ? 0.0f : layouts.front().Height);
	}

	// ---- fit into the square ------------------------------------------
	const float margin = 16.0f;
	const float available = static_cast<float>(size) - margin * 2.0f;
	float scale = std::min(available / (maxX - minX), available / (maxY - minY));
	// Clamp the upscale: a three-node graph blown up 40x is a wall of flat
	// colour carrying no more information than at 4x.
	scale = std::min(scale, 8.0f);

	const float drawnWidth = (maxX - minX) * scale;
	const float drawnHeight = (maxY - minY) * scale;
	const float offsetX = margin + (available - drawnWidth) * 0.5f - minX * scale;
	const float offsetY = margin + (available - drawnHeight) * 0.5f - minY * scale;

	auto toScreenX = [scale, offsetX](float x) { return x * scale + offsetX; };
	auto toScreenY = [scale, offsetY](float y) { return y * scale + offsetY; };

	// ---- raster -------------------------------------------------------
	ThumbnailEntry entry;
	entry.Kind = graph.Kind == "tree" ? ThumbnailKind::BehaviorTree
									 : ThumbnailKind::FlowGraph;
	entry.Width = size;
	entry.Height = size;
	entry.Ready = true;

	Raster raster(size, size);
	raster.Clear(kBackground);

	// Pin index lookup: the index of pinId among the node effective pins of
	// direction dir. Returns 0 when the pin is unknown -- the wire still draws,
	// anchored at the first pin rather than silently vanishing.
	auto pinIndex = [&registry, &graph](const GraphNode &node, PinDir dir,
									   const std::string &pinId) -> int
	{
		const NodeDef *def = registry.Find(node.TypeId);
		if (!def)
			return 0;
		const std::vector<PinDef> pins = NodeRegistry::EffectivePins(*def, graph, node);
		int index = 0;
		for (const PinDef &pin : pins)
		{
			if (pin.Dir != dir)
				continue;
			if (pin.Id == pinId)
				return index;
			++index;
		}
		return 0;
	};

	// The pin type of a link end, for the category colour.
	auto pinType = [&registry, &graph](const GraphNode &node,
									   const std::string &pinId) -> PinType
	{
		const NodeDef *def = registry.Find(node.TypeId);
		if (!def)
			return PinType::Float;
		const std::vector<PinDef> pins = NodeRegistry::EffectivePins(*def, graph, node);
		for (const PinDef &pin : pins)
			if (pin.Id == pinId)
				return pin.Type;
		return PinType::Float;
	};

	// Locate the layout for a node id. Linear, matching Graph::FindNode.
	auto layoutIndex = [&graph](const std::string &id) -> size_t
	{
		for (size_t i = 0; i < graph.Nodes.size(); ++i)
			if (graph.Nodes[i].Id == id)
				return i;
		return static_cast<size_t>(-1);
	};

	// 1. Wires first, under the nodes.
	for (const GraphLink &link : graph.Links)
	{
		const size_t fromIndex = layoutIndex(link.FromNode);
		const size_t toIndex = layoutIndex(link.ToNode);
		if (fromIndex >= layouts.size() || toIndex >= layouts.size())
			continue;

		const GraphNode &from = graph.Nodes[fromIndex];
		const GraphNode &to = graph.Nodes[toIndex];
		const NodeLayout &fromLayout = layouts[fromIndex];
		const NodeLayout &toLayout = layouts[toIndex];

		const int outIndex = pinIndex(from, PinDir::Out, link.FromPin);
		const int inIndex = pinIndex(to, PinDir::In, link.ToPin);

		const float x0 = toScreenX(fromLayout.X + fromLayout.Width);
		const float y0 = toScreenY(PinY(fromLayout, outIndex));
		const float x1 = toScreenX(toLayout.X);
		const float y1 = toScreenY(PinY(toLayout, inIndex));

		// Exec wires carry control flow and get the accent colour; data wires
		// get the cool one. That distinction is the most useful thing a graph
		// thumbnail can show at a glance.
		const bool isExec = pinType(from, link.FromPin) == PinType::Exec ||
							pinType(to, link.ToPin) == PinType::Exec;
		raster.DrawLine(static_cast<int>(x0), static_cast<int>(y0),
						static_cast<int>(x1), static_cast<int>(y1),
						2, isExec ? kAccent : kCool);
	}

	// 2. Nodes second, over the wires.
	for (size_t i = 0; i < layouts.size(); ++i)
	{
		const NodeLayout &layout = layouts[i];

		const int x0 = static_cast<int>(toScreenX(layout.X));
		const int y0 = static_cast<int>(toScreenY(layout.Y));
		const int x1 = static_cast<int>(toScreenX(layout.X + layout.Width));
		const int y1 = static_cast<int>(toScreenY(layout.Y + layout.Height));

		// A 1px darker border: a fill alone makes adjacent nodes of the same
		// colour read as one block when the boxes touch.
		raster.FillRoundedRect(x0, y0, x1, y1, 5, kDim);
		raster.FillRoundedRect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 4,
							   layout.IsEvent ? kAccent : kInk);

		// Pin stubs: a node with no visible pins looks like a bare box, and
		// its connectivity is invisible even with the wires drawn.
		const int pinRows = std::max(layout.InputCount, layout.OutputCount);
		for (int row = 0; row < pinRows; ++row)
		{
			const int y = static_cast<int>(toScreenY(PinY(layout, row)));
			raster.FillCircle(x0, y, 2, kCool);
			raster.FillCircle(x1, y, 2, kCool);
		}
	}

	entry.Pixels.assign(raster.GetPixels(),
						raster.GetPixels() +
							static_cast<size_t>(size) * size * 4);

	result.Entry = std::move(entry);
	return result;
}

}  // namespace Wasteland::Editor
