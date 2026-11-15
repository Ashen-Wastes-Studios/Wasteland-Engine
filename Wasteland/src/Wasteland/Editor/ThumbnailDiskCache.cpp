#include "wlpch.h"

#include "Wasteland/Editor/ThumbnailDiskCache.h"

#include <filesystem>
#include <fstream>

#include "Wasteland/Editor/ThumbnailImage.h"
#include "Wasteland/Editor/ThumbnailPng.h"

namespace Wasteland::Editor
{
	namespace
	{
		std::string ToLower(const std::string &text)
		{
			std::string lowered = text;
			for (char &c : lowered)
				c = static_cast<char>(std::tolower(
					static_cast<unsigned char>(c)));
			return lowered;
		}
	}

	std::string ThumbnailDiskCache::CachePath(const std::string &relativePath) const
	{
		if (!IsEnabled())
			return {};

		// std::hash is stable within a single standard library build, which is
		// all a cache needs: it is never compared across machines, and a stale
		// entry is merely a miss. The extension is kept lowercase so a listing of
		// the directory is uniform on a case-insensitive filesystem.
		const std::size_t hash = std::hash<std::string>{}(ToLower(relativePath));

		std::string hex;
		char buffer[16];
		for (int i = 0; i < 16; ++i)
		{
			const std::uint8_t nibble = static_cast<std::uint8_t>((hash >> (i * 4)) & 0xF);
			std::snprintf(buffer, sizeof(buffer), "%x", nibble);
			hex += buffer[0];
		}

		return (std::filesystem::path(m_Root) / kSubdirectory / (hex + ".png")).string();
	}

	bool ThumbnailDiskCache::EnsureDirectory(std::string &error) const
	{
		std::error_code code;
		const std::filesystem::path directory =
			std::filesystem::path(m_Root) / kSubdirectory;
		std::filesystem::create_directories(directory, code);
		if (code)
		{
			error = "cannot create " + directory.string() + ": " + code.message();
			return false;
		}
		return true;
	}

	bool ThumbnailDiskCache::IsFresh(const std::string &relativePath,
									 const std::string &absolutePath) const
	{
		if (!IsEnabled())
			return false;

		std::error_code code;
		const std::string cached = CachePath(relativePath);
		if (cached.empty())
			return false;

		// Both stat calls must succeed; a cache with a missing source is not a
		// fresh entry, it is a dangling reference the browser should not show.
		if (!std::filesystem::exists(cached, code) || code)
			return false;
		if (!std::filesystem::exists(absolutePath, code) || code)
			return false;

		const auto cachedTime = std::filesystem::last_write_time(cached, code);
		if (code)
			return false;
		const auto sourceTime = std::filesystem::last_write_time(absolutePath, code);
		if (code)
			return false;

		return cachedTime >= sourceTime;
	}

	bool ThumbnailDiskCache::Write(const std::string &relativePath,
								   const std::uint8_t *rgba, uint32_t width,
								   uint32_t height, std::string &error)
	{
		if (!IsEnabled())
		{
			error = "no cache root";
			return false;
		}
		if (!EnsureDirectory(error))
			return false;

		std::vector<std::uint8_t> png;
		if (!EncodePngRGBA(rgba, width, height, png, error))
			return false;

		return WritePngFile(CachePath(relativePath), png, error);
	}

	bool ThumbnailDiskCache::Read(const std::string &relativePath,
								  std::vector<std::uint8_t> &rgba, uint32_t &width,
								  uint32_t &height, std::string &error)
	{
		rgba.clear();
		width = 0;
		height = 0;

		if (!IsEnabled())
		{
			error = "no cache root";
			return false;
		}

		const std::string path = CachePath(relativePath);
		if (path.empty())
		{
			error = "empty relative path";
			return false;
		}

		const DecodedImage decoded = LoadImage(path);
		if (!decoded.Ok())
		{
			error = decoded.Error;
			return false;
		}

		rgba = decoded.Pixels;
		width = decoded.Width;
		height = decoded.Height;
		return true;
	}

	bool ThumbnailDiskCache::Clear(std::string &error)
	{
		error.clear();
		if (!IsEnabled())
			return true;

		std::error_code code;
		const std::filesystem::path directory =
			std::filesystem::path(m_Root) / kSubdirectory;

		if (std::filesystem::exists(directory, code))
		{
			std::filesystem::remove_all(directory, code);
			if (code)
			{
				error = "cannot clear " + directory.string() + ": " + code.message();
				return false;
			}
		}

		// Recreate it, so the next Write does not depend on the caller having
		// done it. A cache that silently stops working after a refresh is worse
		// than one that recreates its directory.
		return EnsureDirectory(error);
	}

	int ThumbnailDiskCache::Count() const
	{
		if (!IsEnabled())
			return 0;

		std::error_code code;
		const std::filesystem::path directory =
			std::filesystem::path(m_Root) / kSubdirectory;
		if (!std::filesystem::exists(directory, code) || code)
			return 0;

		int total = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory, code))
		{
			if (entry.is_regular_file(code) && !code)
			{
				if (entry.path().extension() == ".png")
					++total;
			}
		}
		return total;
	}
}
