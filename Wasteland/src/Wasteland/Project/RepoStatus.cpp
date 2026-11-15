#include "wlpch.h"

#include "Wasteland/Project/RepoStatus.h"

#include <cstdio>
#include <sstream>

namespace Wasteland
{
	namespace
	{
#ifdef _WIN32
		constexpr const char *kPopen = "_popen";
		constexpr const char *kPclose = "_pclose";
#else
		constexpr const char *kPopen = "popen";
		constexpr const char *kPclose = "pclose";
#endif

		// Runs `git -C <root> <args...>` and returns stdout. A root containing a
		// double quote is rejected (returns empty): the path is double-quoted into
		// the command, so a quote would break out. git-bash quoting of a drive
		// path like C:/dev/X is fine with `"C:/dev/X"`.
		std::string GitCapture(const std::string &root, const std::string &args)
		{
			if (root.find('"') != std::string::npos)
				return "";
			const std::string command =
			    std::string("git -C \"") + root + "\" " + args;
#ifdef _WIN32
			FILE *pipe = _popen(command.c_str(), "r");
#else
			FILE *pipe = popen(command.c_str(), "r");
#endif
			if (!pipe)
				return "";
			std::string out;
			char buffer[4096];
			std::size_t read;
			while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
				out.append(buffer, read);
#ifdef _WIN32
			_pclose(pipe);
#else
			pclose(pipe);
#endif
			return out;
		}
	}

	RepoStatus QueryRepoStatus(const std::filesystem::path &repoRoot,
	                           std::uint64_t lfsThresholdBytes)
	{
		RepoStatus status;
		const std::string root = repoRoot.string();

		// status --porcelain yields one line per changed file, and is the form
		// designed to be machine-read (no colour, no localization, stable columns).
		const std::string porcelain = GitCapture(root, "status --porcelain");
		if (porcelain.empty())
		{
			// Empty could mean "clean repo" or "not a repo". The branch probe
			// disambiguates: a clean real repo still has a branch.
			const std::string branchOut =
			    GitCapture(root, "rev-parse --abbrev-ref HEAD");
			if (branchOut.empty())
			{
				status.Error = "not a git repository (or git is unavailable): " + root;
				return status;
			}
			std::istringstream branchIn(branchOut);
			std::getline(branchIn, status.Branch);
			status.IsRepository = true;
			return status;
		}

		// Parse the porcelain lines. `?? ` is the untracked marker; anything else
		// is a file changed in the working tree or index. A porcelain path is the
		// text after the two status columns and a space; git quotes paths that
		// need it, which the simple-path cases this feeds do not.
		std::istringstream stream(porcelain);
		std::string line;
		while (std::getline(stream, line))
		{
			std::string path;
			if (line.rfind("?? ", 0) == 0)
			{
				status.UntrackedFiles += 1;
				path = line.substr(3);
			}
			else if (!line.empty())
			{
				status.DirtyFiles += 1;
				if (line.size() >= 4)
					path = line.substr(3);
			}

			// J.5 LFS hint: a dirty/untracked file over the threshold is how a big
			// asset silently enters a repo without LFS. Stat the file, not the
			// porcelain claim, for its size.
			if (lfsThresholdBytes > 0 && !path.empty())
			{
				std::error_code sizeError;
				const std::uintmax_t size =
				    std::filesystem::file_size(root + "/" + path, sizeError);
				if (!sizeError && size > lfsThresholdBytes)
					status.LargeFileHints.emplace_back(
					    root + "/" + path, static_cast<std::uint64_t>(size));
			}
		}

		const std::string branchOut = GitCapture(root, "rev-parse --abbrev-ref HEAD");
		if (branchOut.empty())
		{
			// Has dirty/untracked files but no resolvable branch: detached HEAD or
			// bare. Still a repository.
			status.IsRepository = true;
			return status;
		}
		std::istringstream branchIn(branchOut);
		std::getline(branchIn, status.Branch);
		status.IsRepository = true;
		return status;
	}
}
