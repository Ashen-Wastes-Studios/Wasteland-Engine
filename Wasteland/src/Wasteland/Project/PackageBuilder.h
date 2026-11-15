// PackageBuilder.h -- the deterministic package layout (Phase J.3).
//
// The plan's J.3 verification -- "run the package under a clean user profile,
// assert it boots and plays" -- needs a working runtime, so the EDITOR half is
// blocked. What is testable headlessly, and what this type provides, is the
// LAYOUT: a directory that a package is by construction.
//
//   <package>/
//     <exe>          # the runtime executable, byte-identical to the source
//     project.wasteland
//     version.txt    # the shipped version, one line
//     manifest.json  # deterministic: every file's path, byte count, content hash
//     assets/...     # the cooked assets (relative tree preserved)
//
// Nothing here depends on the editor or a GPU, so the layout and its manifest are
// the part of packaging that CAN be verified on a CI box. "It boots and plays" is
// a separate, later, on-device check -- this guarantees the package is COMPLETE,
// DETERMINISTIC and NON-OVERLAPPING, which are the properties a package has to
// have before "does it run" is even sensible.
//
// Determinism is the same loaded requirement as the cooker: a manifest with a
// build timestamp, an unordered file list, or a hash-order makes every build a
// different artifact. The manifest is sorted by relative path and carries only
// content-derived fields, so two builds of the same inputs produce identical
// bytes.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Wasteland
{
	struct PackageEntry
	{
		// Forward-slash path relative to the package root.
		std::string RelativePath;
		std::uint64_t Bytes = 0;
		std::uint64_t ContentHash = 0;
	};

	struct PackageResult
	{
		bool Ok = true;
		std::string Error;

		// The version written to version.txt.
		std::string Version;
		// Every packaged file, sorted by RelativePath. A file is in this list if
		// and only if it is on disk under the staging directory.
		std::vector<PackageEntry> Files;
	};

	// Lays out a package into `stagingDir` (created if absent). `cookedAssetsDir`
	// may be empty (a package with no cooked assets yet); `.cook` sidecars are
	// deliberately NOT packaged -- they are the cooker's dev bookkeeping, not
	// runtime data, and they would be one more thing a package carries that
	// nothing in it reads.
	PackageResult BuildPackage(const std::filesystem::path &stagingDir,
	                           const std::filesystem::path &runtimeExe,
	                           const std::filesystem::path &projectFile,
	                           const std::filesystem::path &cookedAssetsDir,
	                           const std::string &version);

	// The manifest filename, exposed so a test can assert it without a constant
	// that silently drifts from this file's spelling.
	inline const char *PackageManifestName() { return "manifest.json"; }
}
