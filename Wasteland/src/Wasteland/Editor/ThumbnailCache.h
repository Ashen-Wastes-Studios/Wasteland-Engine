#pragma once

// Thumbnail cache: the pure-logic half of the Content Browser's thumbnail
// system.
//
// This lives in the ENGINE rather than in DemonCore-Editor/src/Panels/ even
// though the plan placed it there, for one reason: the headless test must be
// able to execute the classifier, the cache key and the eviction policy without
// a GL context. An ImTextureID cannot exist here; an RGBA buffer can. The
// editor's panel owns uploading the buffer and drawing it, and nothing about
// the cache knows ImGui exists.

#include "Wasteland/Editor/ThumbnailDiskCache.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wasteland::Editor
{
	// One kind per asset type, matching the plan's contract table. A kind with
	// no generator must still produce an icon, so every kind has a fallback and
	// the enum is exhaustive rather than "known types plus other".
	enum class ThumbnailKind
	{
		Texture,
		Mesh,
		Material,
		Animation,
		Scene,
		FlowGraph,
		BehaviorTree,
		AudioGraph,
		AudioClip,
		Terrain,
		Script,
		Shader,
		Document,
		Compiled,
		Directory,
		Unknown
	};

	constexpr int kThumbnailKindCount = 16;

	// Stable, human-readable name. The editor shows it in the status bar and the
	// test asserts on it, so it is a contract rather than a debug aid.
	const char *ThumbnailKindName(ThumbnailKind kind);

	// Classify by EXTENSION, case-insensitively.
	//
	// ContentBrowserPanel::IsOpenable documents its own case-sensitive
	// comparison on a case-insensitive filesystem ("Script.PY" fails to open
	// while "script.py" works). The classifier must not repeat that defect: the
	// extension is lowercased before it is compared.
	//
	// A path with no extension is Unknown, NOT Script. Treating a bare name as a
	// script would put an angle-bracket icon on every extension-less file,
	// including directories and executables.
	ThumbnailKind Classify(const std::filesystem::path &path);
	ThumbnailKind Classify(const std::string &path);

	// A cache key: project-relative path + source mtime + source size.
	//
	// Both file attributes come from the non-throwing error_code overloads. A
	// file that disappears between the directory listing and the thumbnailing
	// must produce a zeroed key, not an exception out of a UI pass.
	struct ThumbnailCacheKey
	{
		std::string RelativePath;
		std::uint64_t ModifiedTime = 0;
		std::uint64_t Size = 0;

		bool operator==(const ThumbnailCacheKey &other) const;
		std::string ToString() const;
	};

	// Both paths are std::string rather than std::filesystem::path because
	// pybind11 ships no caster for std::filesystem::path: a Python string does
	// not convert automatically, and every call from the editor and the test
	// arrives as a string. The conversion happens here, once.
	ThumbnailCacheKey MakeThumbnailCacheKey(const std::string &relativePath,
											const std::string &absolutePath);

	// One cached thumbnail.
	//
	// RGBA8, row-major, TOP ROW FIRST. Top-first because that is what
	// Texture2D::SetData expects and what the test asserts against; a
	// bottom-first buffer would need a flip at the upload site, which is exactly
	// the kind of one-line bug that survives until someone looks at a mesh
	// thumbnail upside down.
	struct ThumbnailEntry
	{
		ThumbnailKind Kind = ThumbnailKind::Unknown;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::vector<std::uint8_t> Pixels;
		ThumbnailCacheKey Key;

		// A generation attempt RAN and produced this entry (an icon, a failed
		// placeholder, or a real render). Distinct from "queued".
		bool Ready = false;

		// The source could not be read or decoded. A failed entry is cached like
		// any other so a corrupt asset does not get retried every frame -- the
		// failure is a fact about the file, not a transient condition.
		bool Failed = false;

		bool IsEmpty() const { return Width == 0 || Height == 0 || Pixels.empty(); }

		// The ONLY field a Python-side generator may set. Width/Height/Pixels are
		// read-only because a script that could write into a cached buffer would
		// be able to corrupt a thumbnail the cache still considers valid; Failed
		// is a status flag the generator must be able to report, and exposing it
		// as a setter is the narrowest way to allow that.
		void SetFailed(bool failed) { Failed = failed; }
	};

	// Generate the fallback icon for a kind.
	//
	// A FREE function rather than a ThumbnailCache member: it needs no cache
	// state, and a test must be able to reconstruct the fallback to compare a
	// real thumbnail against it. Without that, "the corrupt asset produced the
	// fallback" is untestable -- the fallback cannot be rebuilt from outside.
	ThumbnailEntry GenerateIcon(ThumbnailKind kind);

	// The cache.
	//
	// Rule 1 of the plan: nothing expensive is generated synchronously on the
	// UI thread. Request() therefore never generates; it returns the cached
	// entry, or a NOT-READY placeholder and queues the key. Tick() drains the
	// queue under a per-frame budget.
	class ThumbnailCache
	{
	public:
		// 256x256 is the cell size the plan's generators target. A cache entry
		// may be smaller (icons are generated at the same size for uniformity).
		static constexpr std::uint32_t kThumbnailSize = 256;

		// The LRU cap. 256 entries x 256 x 256 x 4 bytes = 64 MB at the cap,
		// which is the most a browser should hold before it starts evicting.
		static constexpr size_t kMaxEntries = 256;

		// The per-frame generation budget. The plan's estimate for a 256x256
		// mesh render is ~1 ms and explicitly says it is UNMEASURED -- the budget
		// is cheap insurance either way, and 2 keeps a frame's worst case at
		// ~2 ms even if the estimate is off by 10x.
		static constexpr int kMaxPerFrame = 2;

		ThumbnailCache();
		~ThumbnailCache();

		ThumbnailCache(const ThumbnailCache &) = delete;
		ThumbnailCache &operator=(const ThumbnailCache &) = delete;

		// Install the generator. The editor passes a lambda that renders or
		// loads the asset and returns a ThumbnailEntry; the test passes the
		// built-in icon generator. Taking a std::function rather than a pure
		// virtual keeps the cache constructible without a renderer.
		void SetGenerator(std::function<ThumbnailEntry(const ThumbnailCacheKey &)> generator);

		// Never null. Returns the cached entry, or a not-ready placeholder for a
		// key that has not been generated yet. The caller draws the placeholder
		// and calls Tick() to make progress.
		//
		// `visible` is the set of keys currently on screen. The budget only
		// spends on visible entries: generating a thumbnail the user has scrolled
		// past is the single easiest way to burn a frame budget for nothing.
		ThumbnailEntry *Request(const std::string &relativePath,
								const std::string &absolutePath,
								const std::unordered_map<std::string, bool> &visible);

		ThumbnailEntry *Find(const std::string &relativePath);

		// Drain the queue, at most kMaxPerFrame entries. Returns how many were
		// generated, so a test can assert the budget rather than infer it.
		int Tick(float dt);

		void InvalidateAll();

		// ---- F.8: disk persistence ---------------------------------------
		// The disk cache is a separate object rather than members of the memory
		// cache, so it can be driven directly by the panel (Refresh Thumbnails)
		// and by a test (write, restart, read) without going through a
		// ThumbnailCache at all.
		void SetDiskCacheRoot(const std::string &root) { m_Disk.SetRoot(root); }
		const std::string &GetDiskCacheRoot() const { return m_Disk.GetRoot(); }
		ThumbnailDiskCache &GetDiskCache() { return m_Disk; }
		const ThumbnailDiskCache &GetDiskCache() const { return m_Disk; }

		// Throttled source-change check. The panel rebuilds its listing every
		// frame, so an unthrottled stat per entry per frame is a real cost --
		// this visits each visible entry at most once per `throttleSeconds` and
		// returns how many entries it invalidated, so "the mtime bump
		// invalidates" is checkable rather than inferred.
		//
		// `nowSeconds` is the caller's clock, not a wall clock read here: a
		// function that reads the clock internally cannot be tested, because the
		// test cannot advance it.
		int CheckForChanges(const std::unordered_map<std::string, bool> &visible,
							float nowSeconds, float throttleSeconds);

		size_t GetEntryCount() const { return m_Entries.size(); }
		size_t GetPendingCount() const { return m_Pending.size(); }
		int GetMaxPerFrame() const { return kMaxPerFrame; }
		size_t GetMaxEntries() const { return kMaxEntries; }

		// Eviction is observable on purpose: a test that cannot see the cap take
		// effect cannot tell an LRU from an unbounded map that will grow until
		// the process dies.
		size_t GetEvictionCount() const { return m_Evictions; }

	private:
		// Drop the least-recently-used entries until the map is back at the cap.
		// Called after an insert, never on a read: an eviction must not be able
		// to cost time on the frame that merely LOOKED at a thumbnail.
		void EvictIfNeeded();

		struct Entry
		{
			ThumbnailEntry Thumbnail;
			// LRU bookkeeping: a monotonically increasing tick, touched on every
			// hit. A list splice would be O(1) too, but a counter needs no
			// iterator stability and survives rehash.
			std::uint64_t LastUsed = 0;

			// The source path this entry was built from. Kept here rather than
			// in ThumbnailCacheKey because the key is the cache's public
			// contract (path + mtime + size) and adding a field to it would
			// change what the F.2 test asserts. CheckForChanges needs it to
			// re-key without the caller re-supplying every absolute path.
			std::string AbsolutePath;
		};

		std::unordered_map<std::string, Entry> m_Entries;
		std::vector<ThumbnailCacheKey> m_Pending;
		std::function<ThumbnailEntry(const ThumbnailCacheKey &)> m_Generator;
		std::uint64_t m_Now = 0;
		size_t m_Evictions = 0;

		ThumbnailDiskCache m_Disk;
		// Per-path last-check time for CheckForChanges. Kept across calls rather
		// than recomputed, because the throttle is a function of the caller's
		// clock and must not reset when a frame is skipped.
		std::unordered_map<std::string, float> m_LastChangeCheck;
	};
}
