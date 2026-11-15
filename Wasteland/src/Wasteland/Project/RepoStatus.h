// RepoStatus.h -- the status-bar data core (Phase J.5).
//
// The J.5 status BAR is editor UI; what a headless test (and the bar itself) can
// both rely on is the DATA: which branch the repo is on, how many files are
// dirty, how many are untracked. This queries git for it.
//
// git is invoked as a subprocess. That is a deliberately narrow dependency --
// the alternative is reimplementing .git parsing (refs, index, HEAD), which is a
// second source of truth that would drift from git's own view, exactly the thing
// the status bar exists to show. Shelling out to git is what every editor does;
// it is the git binary's unsolicited opinion, not a reimplementation.
//
// The path is taken as a string and double-quoted into the command, never
// interpolated unquoted, and a root containing a double quote is rejected rather
// than risk command injection (the editor passes its own project root, so this is
// defence in depth, not a trust boundary).
//
// Determinism: callers read what git answers at call time. There is no caching
// here -- a stale status is a different lie than a fresh one, and the LFS-hint
// user (J.5) wants the current truth.
#pragma once

#include <filesystem>
#include <string>

namespace Wasteland
{
	struct RepoStatus
	{
		// The current branch ("" when not on a branch or not a repo).
		std::string Branch;
		// Files git reports as changed in any way (staged, modified, deleted,
		// renamed) -- anything that is not simply untracked.
		int DirtyFiles = 0;
		// Files git reports as untracked (`?? `).
		int UntrackedFiles = 0;
		// False when the directory is not under git or git is unavailable. Branch
		// and counts are then all zero/empty, and Error names the cause.
		bool IsRepository = false;
		std::string Error;

		// (full path, size bytes) of every dirty/untracked file over the LFS
		// threshold -- the J.5 "warn when a file over a threshold is added without
		// LFS" signal. Populated only when a positive threshold was passed.
		std::vector<std::pair<std::string, std::uint64_t>> LargeFileHints;
	};

	// Queries the repo state of `repoRoot`. Not a repository (or git missing) is
	// reported as `IsRepository == false`, never a throw or a guess.
	//
	// `lfsThresholdBytes` > 0 additionally populates LargeFileHints with every
	// dirty/untracked file whose on-disk size exceeds it -- tracked-but-modified
	// files AND new files are the two ways a big asset reaches a repo.
	RepoStatus QueryRepoStatus(const std::filesystem::path &repoRoot,
	                           std::uint64_t lfsThresholdBytes = 0);
}
