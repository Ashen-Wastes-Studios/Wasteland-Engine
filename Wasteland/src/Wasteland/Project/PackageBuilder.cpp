#include "wlpch.h"

#include "Wasteland/Project/PackageBuilder.h"

#include "Wasteland/Project/AssetCooker.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Wasteland
{
	namespace
	{
		// Copy a file byte-for-byte.
		bool CopyFileBinary(const std::filesystem::path &from,
		                    const std::filesystem::path &to, std::string *error)
		{
			std::ifstream in(from, std::ios::binary);
			if (!in)
			{
				if (error) *error = "could not open " + from.string() + " for reading";
				return false;
			}
			std::ofstream out(to, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				if (error) *error = "could not open " + to.string() + " for writing";
				return false;
			}
			out << in.rdbuf();
			if (!out.good())
			{
				if (error) *error = "failed while copying " + from.string();
				return false;
			}
			return true;
		}

		// All files under `dir` as relative paths, sorted. Directories are walked
		// depth-first; the sort at the end makes the order platform-independent.
		void CollectFiles(const std::filesystem::path &dir,
		                  const std::filesystem::path &base,
		                  std::vector<std::filesystem::path> &out)
		{
			std::error_code ec;
			for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			{
				if (ec) continue;
				if (entry.is_directory())
					CollectFiles(entry.path(), base, out);
				else if (entry.is_regular_file())
					out.push_back(entry.path().lexically_relative(base));
			}
		}

		std::string ForwardSlash(const std::filesystem::path &path)
		{
			std::string text = path.generic_string();
			return text;
		}

		// Escape a string for a JSON string literal (backslash and quote).
		std::string JsonEscape(const std::string &text)
		{
			std::string escaped;
			escaped.reserve(text.size());
			for (char c : text)
			{
				if (c == '\\' || c == '"')
				{
					escaped.push_back('\\');
					escaped.push_back(c);
				}
				else
				{
					escaped.push_back(c);
				}
			}
			return escaped;
		}

		std::string HashText(std::uint64_t hash)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%016llx",
			              static_cast<unsigned long long>(hash));
			return std::string(buffer);
		}
	}

	PackageResult BuildPackage(const std::filesystem::path &stagingDir,
	                           const std::filesystem::path &runtimeExe,
	                           const std::filesystem::path &projectFile,
	                           const std::filesystem::path &cookedAssetsDir,
	                           const std::string &version)
	{
		PackageResult result;
		result.Version = version;

		const auto fail = [&result](const std::string &message)
		{
			result.Ok = false;
			result.Error = message;
			return result;
		};

		std::error_code createError;
		const std::filesystem::path assetsTarget = stagingDir / "assets";
		std::filesystem::create_directories(assetsTarget, createError);
		if (createError)
			return fail("could not create " + assetsTarget.string() + ": " +
			            createError.message());

		// A package that RAN only on the dev machine is the failure this layout
		// exists to prevent a step away from. The exe is copied byte-identical; a
		// copy that silently picked up a different file would not be "the" runtime.
		if (!CopyFileBinary(runtimeExe, stagingDir / runtimeExe.filename(),
		                    &result.Error))
		{
			result.Ok = false;
			return result;
		}

		// The project file names the start scene; without it a package has nothing
		// to open.
		if (!CopyFileBinary(projectFile, stagingDir / projectFile.filename(),
		                    &result.Error))
		{
			result.Ok = false;
			return result;
		}

		// Cooked assets, preserving the relative tree. `.cook` sidecars are the
		// cooker's bookkeeping -- the incremental core -- and are not runtime data,
		// so they never ship.
		std::vector<std::filesystem::path> assetFiles;
		std::error_code walkError;
		if (std::filesystem::exists(cookedAssetsDir, walkError))
			CollectFiles(cookedAssetsDir, cookedAssetsDir, assetFiles);
		std::sort(assetFiles.begin(), assetFiles.end());

		std::vector<PackageEntry> entries;
		for (const auto &relative : assetFiles)
		{
			const std::string rel = ForwardSlash(relative);
			if (rel.rfind(".cook", rel.size()) != std::string::npos)
				continue;

			const std::filesystem::path source = cookedAssetsDir / relative;
			const std::filesystem::path target = assetsTarget / relative;
			std::filesystem::create_directories(target.parent_path(), createError);
			if (createError)
				return fail("could not create " + target.parent_path().string());

			if (!CopyFileBinary(source, target, &result.Error))
			{
				result.Ok = false;
				return result;
			}
			std::string readError;
			const std::uint64_t hash = HashBytesOfFile(target, &readError);
			PackageEntry entry;
			entry.RelativePath = "assets/" + rel;
			entry.Bytes = static_cast<std::uint64_t>(
			    std::filesystem::file_size(target));
			entry.ContentHash = hash;
			entries.push_back(entry);
		}

		// version.txt, one line.
		{
			std::ofstream versionOut(stagingDir / "version.txt",
			                         std::ios::binary | std::ios::trunc);
			if (!versionOut)
				return fail("could not write version.txt in " + stagingDir.string());
			versionOut << version << "\n";
			if (!versionOut.good())
				return fail("could not flush version.txt in " + stagingDir.string());
		}

		// The exe and project file are packaged too; fold them in so the manifest
		// is a complete inventory, sorted.
		{
			PackageEntry exeEntry;
			exeEntry.RelativePath = runtimeExe.filename().string();
			exeEntry.Bytes = static_cast<std::uint64_t>(
			    std::filesystem::file_size(stagingDir / runtimeExe.filename()));
			std::string readError;
			exeEntry.ContentHash = HashBytesOfFile(
			    stagingDir / runtimeExe.filename(), &readError);
			entries.push_back(exeEntry);

			PackageEntry projectEntry;
			projectEntry.RelativePath = projectFile.filename().string();
			projectEntry.Bytes = static_cast<std::uint64_t>(
			    std::filesystem::file_size(stagingDir / projectFile.filename()));
			projectEntry.ContentHash = HashBytesOfFile(
			    stagingDir / projectFile.filename(), &readError);
			entries.push_back(projectEntry);

			// A version of version.txt too: the manifest is the complete inventory.
			PackageEntry versionEntry;
			versionEntry.RelativePath = "version.txt";
			versionEntry.Bytes = static_cast<std::uint64_t>(
			    std::filesystem::file_size(stagingDir / "version.txt"));
			versionEntry.ContentHash = HashBytesOfFile(
			    stagingDir / "version.txt", &readError);
			entries.push_back(versionEntry);
		}

		std::sort(entries.begin(), entries.end(),
		          [](const PackageEntry &a, const PackageEntry &b)
		          { return a.RelativePath < b.RelativePath; });

		// Deterministic manifest: sorted, content-derived fields only. No
		// timestamp, no build id -- two builds of the same inputs are byte-identical.
		{
			std::ostringstream manifest;
			manifest << "{\"version\":";
			manifest << '"' << JsonEscape(version) << '"';
			manifest << ",\"files\":[";
			for (std::size_t i = 0; i < entries.size(); ++i)
			{
				if (i) manifest << ',';
				manifest << "{\"path\":";
				manifest << '"' << JsonEscape(entries[i].RelativePath) << '"';
				manifest << ",\"bytes\":" << entries[i].Bytes;
				manifest << ",\"hash\":";
				manifest << '"' << HashText(entries[i].ContentHash) << '"';
				manifest << '}';
			}
			manifest << "]}";
			std::ofstream manifestOut(stagingDir / PackageManifestName(),
			                          std::ios::binary | std::ios::trunc);
			if (!manifestOut)
				return fail("could not write " + stagingDir.string() + "/manifest.json");
			manifestOut << manifest.str();
			if (!manifestOut.good())
				return fail("could not flush the manifest in " + stagingDir.string());
		}

		result.Ok = true;
		result.Files = entries;
		return result;
	}
}
