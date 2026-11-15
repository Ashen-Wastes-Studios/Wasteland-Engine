// AssetCooker.h -- the deterministic, incremental cook core (Phase J.2).
//
// WHY THIS EXISTS
// The plan's J.2 says a cook "must be deterministic and incremental -- a cook that
// reruns from scratch is not usable on a large project." This type is that core,
// independent of WHAT is being cooked:
//
//   * DETERMINISTIC: an asset is re-cooked if and only if its CONTENT changed.
//     A cook that cycles on a touch-with-no-edit -- or, worse, on a modified
//     mtime -- makes every save an unbounded rebuild.
//   * INCREMENTAL: cook the whole project, and unchanged assets are a no-op.
//     `CookAll` is the normal invocation; there is no "cook this one file I
//     touched" step to remember.
//
// HOW STALENESS WORKS
// A cook of `source` to `output` records the source's content hash in a sidecar
// `<output>.cook`. The next run hashes the source again; if it matches, `output`
// is current and the cook operation never runs. Change the CONTENT and it
// re-cooks; touch the file and it does not.
//
// The sidecar lives beside the OUTPUT, not the source: the output is what a
// packaged game ships, so the bookkeeping must travel with it and be trivially
// deleted -- it is a dev artifact, never a runtime requirement. A deterministic
// output needs no timestamp; the hash is the record of how it was made.
//
// WHY CONTENT HASH, NOT MTIME
// An mtime is non-deterministic in the bad direction: two cooks of the same byte
// content can be triggered by an unrelated `touch`, and -- worse for a build tool
// -- a tool that reproduces a file byte-for-byte still invalidates everything
// whose mtime was touched. The hash is a property of the CONTENT, so it is stable
// across hosts, checkouts and archive extractions.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Wasteland
{
	// FNV-1a 64-bit over a byte range. A cook stamp is not a security boundary --
	// it answers "did this file's bytes change?" -- and FNV-1a is deterministic,
	// dependency-free and fast enough to hash a whole source tree in a tick.
	uint64_t Fnv1a64(const uint8_t *data, std::size_t size);

	// Hash an entire file's bytes. read_error is set when the file cannot be read
	// (and the hash is 0); whether a missing file is an error is the caller's call.
	uint64_t HashBytesOfFile(const std::filesystem::path &path, std::string *read_error);

	struct CookResult
	{
		bool Ok = true;
		std::string Error;
		// True when this call actually re-cooked; false when the output was already
		// current. The difference is what makes "second run is a no-op" shareable
		// with a progress bar.
		bool Cooked = false;
	};

	class AssetCooker
	{
	public:
		// Cooks `source` to `output` through the given operation, but ONLY when the
		// source's content hash differs from the last recorded one.
		//
		// `op` receives the output path and must produce it. It may read `source` to
		// do so. If `op` fails, the previous output and stamp are left untouched
		// (the old artifact is not clobbered by a cook that dies halfway), and the
		// error is returned.
		CookResult CookIfStale(
		    const std::filesystem::path &source,
		    const std::filesystem::path &output,
		    const std::function<bool(const std::filesystem::path &output)> &op);

	private:
		// The sidecar path for an output. `<output>.<ext>` -> `<output>.cook`.
		static std::filesystem::path StampPath(const std::filesystem::path &output);
	};
}
