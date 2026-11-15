#pragma once

// The thumbnail disk cache (Task F.8).
//
// Location: <root>/thumbnails/<hash>.png. The root is
// Project::GetUserConfigDirectory(), i.e. OUTSIDE the project tree. A `.thumb/`
// directory inside assets/ would appear in the browser as an asset, be
// drag-dropped into scenes, and be serialized into .wastescene files.
//
// The cache is content-addressed by the project-relative PATH, not by the
// path+mtime+size key the memory cache uses. Hashing only the path means an
// mtime bump OVERWRITES the same file instead of leaving an orphan that can
// never be reached again -- the browser lists a path, not a version of a path.
//
// No index file. The freshness rule is "the cached file's mtime is not older
// than the source's mtime", which needs no sidecar that can disagree with the
// files it describes.

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland::Editor
{
	class ThumbnailDiskCache
	{
	public:
		static constexpr const char *kSubdirectory = "thumbnails";

		void SetRoot(const std::string &root) { m_Root = root; }
		const std::string &GetRoot() const { return m_Root; }
		bool IsEnabled() const { return !m_Root.empty(); }

		std::string CachePath(const std::string &relativePath) const;

		// True when a cached image exists for `relativePath` and is not older
		// than the source at `absolutePath`. Both halves matter: a missing cache
		// is not fresh, and a cache older than the source is not fresh either.
		bool IsFresh(const std::string &relativePath,
					 const std::string &absolutePath) const;

		bool Write(const std::string &relativePath, const std::uint8_t *rgba,
				   uint32_t width, uint32_t height, std::string &error);

		bool Read(const std::string &relativePath, std::vector<std::uint8_t> &rgba,
				  uint32_t &width, uint32_t &height, std::string &error);

		// Delete every cached file. The Refresh Thumbnails button and a project
		// switch both call it.
		bool Clear(std::string &error);

		// Number of files currently cached. Observable because "the disk cache
		// survives a restart" is only checkable by counting before and after.
		int Count() const;

	private:
		bool EnsureDirectory(std::string &error) const;
		std::string m_Root;
	};
}
