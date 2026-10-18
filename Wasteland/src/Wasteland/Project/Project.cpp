// PCH first: it is what makes WL_CORE_ERROR/WL_CORE_INFO and the
// std::filesystem/CoTaskMem prerequisites visible to this TU, and it
// is what defines WL_PLATFORM_WINDOWS for the guard at line 12.
#include "wlpch.h"
// The Project singleton's own declaration, included second so the
// compiler sees the class -- and its nested Settings type -- before
// the free helpers below that refer to it.
#include "Project.h"

// YAML is the on-disk format of project.wasteland: parsed in Open()
// and emitted in WriteDescriptor().
#include <yaml-cpp/yaml.h>

// std::replace and std::transform back ToForwardSlashes and the
// reserved-name uppercasing in SanitizeName.
#include <algorithm>
// std::isspace / std::toupper for SanitizeName's whitespace
// collapsing and its case-insensitive reserved-name comparison.
#include <cctype>
// std::getenv reads HOME / USERPROFILE / APPDATA / XDG_CONFIG_HOME
// in the well-known-directory helpers near the bottom.
#include <cstdlib>
#include <fstream>
// std::function is the type of WriteDescriptor's customize hook.
#include <functional>

// SHGetKnownFolderPath and CoTaskMemFree live here. Guarded because
// every other directory helper in this file is portable and only this
// one branch is Windows-specific.
#ifdef WL_PLATFORM_WINDOWS
#include <shlobj.h>
#endif

// Namespace Wasteland, so engine and editor code name Project
// without qualification.
namespace Wasteland
{

	// Internal linkage for everything that follows: these names must not
	// collide with anything else in the engine's link, and none of them
	// are part of Project's API.
	namespace
	{
		// The descriptor's filename, fixed by convention. Open() accepts
		// either the project directory or this file and infers the other from
		// whichever one it was handed, so this constant is what makes both
		// forms work.
		constexpr const char *kProjectFileName = "project.wasteland";
		// 'assets' is the single subdirectory every path helper keys off.
		// GetAssetPath() hangs off it and AttachAssetRoot() refuses a root
		// without it, because a root lacking it makes every asset resolve to
		// nothing.
		constexpr const char *kAssetDirName = "assets";
		// Default location for user projects: Documents/Wasteland Projects.
		// Under Documents rather than the profile folder because Documents is
		// where a user looks for saved work, and on most installs it is the
		// OneDrive-redirected one.
		constexpr const char *kProjectsFolderName = "Wasteland Projects";
		// The recent-projects list lives in the CONFIG directory, not the
		// projects directory: it describes editor state, not user content.
		constexpr const char *kRecentFileName = "recent_projects.txt";
		// Cap on the recent list. GetRecentProjects() stops reading at this
		// many and AddRecentProject() trims to it, so the list cannot grow
		// without bound across launches.
		constexpr size_t kMaxRecentProjects = 10;

		// Asset subdirectories every new project gets. Keeping these
		// pre-made means the content browser and the file dialogs have
		// somewhere obvious to send the user, and importers never have to
		// create a tree on the fly.
		// Every new project gets these. Pre-created so the content browser and
		// the file dialogs have somewhere obvious to send the user, and so
		// importers never have to build a tree on the fly at first use.
		const char *kAssetSubdirs[] = {"scenes", "scripts", "shaders", "textures", "models", "ai"};

		// Normalizes separators to '/'. Takes its argument BY VALUE on
		// purpose: the copy is the point, and it lets callers write
		// ToForwardSlashes(expr) inline without a named temporary. Storing
		// project paths with forward slashes is what makes them comparable
		// and portable across platforms.
		std::string ToForwardSlashes(std::string s)
		{
			// In-place over the by-value copy; the return lets it stay an
			// expression.
			std::replace(s.begin(), s.end(), '\\', '/');
			return s;
		}

		// Reports a failure to the caller and logs it. Returns false so
		// callers can "return Fail(...)" without repeating the plumbing.
		// Single funnel for every failure in this file: log once, copy the
		// message to the caller's outError when it wants one, and return
		// false -- so callers write "return Fail(...)" without repeating
		// the logging and the null check.
		bool Fail(std::string *outError, const std::string &message)
		{
			// Logged here rather than at each call site, so a failure has exactly
			// one report, in the engine's usual {0} placeholder style.
			WL_CORE_ERROR("Project: {0}", message);
			// outError is optional. Bootstrap's internal tryOpen passes one, but
			// SyncBuiltInShaders deliberately passes nullptr because it wants the
			// log and not a surfaced error.
			if (outError)
				*outError = message;
			return false;
		}

		// Destructive recursive copy. Used only for the caller's explicit
		// seed tree, where overwriting is the intent. CopyTreeMissingOnly
		// below is the non-destructive variant for engine-provided files.
		bool CopyTree(const std::filesystem::path &from, const std::filesystem::path &to, std::string *outError)
		{
			// Every call takes a std::error_code instead of using the throwing
			// overloads: a missing or unreadable directory here is an expected,
			// reportable condition, not an exception.
			std::error_code ec;
			// Missing source is SUCCESS, not failure. This is what lets Create()
			// treat an empty seedAssets as "no seed" and carry on, and makes
			// SyncBuiltInShaders a no-op on an install with no engine assets.
			//
			// Worth knowing: a directory that exists but cannot be READ (a
			// permissions problem) sets ec here and is then treated identically
			// to "absent", so an unreadable seed looks exactly like an empty
			// one.
			if (!std::filesystem::is_directory(from, ec))
				return true; // Nothing to seed from; not an error.

			// create_directories is a no-op when the tree already exists, so this
			// is safe to re-run over an existing directory.
			std::filesystem::create_directories(to, ec);
			if (ec)
				return Fail(outError, "could not create '" + to.string() + "': " + ec.message());

			// recursive_directory_iterator descends into subdirectories. The
			// end-iterator comparison is the loop condition rather than ec, which
			// is why the error check below is a separate statement inside the
			// body instead of a loop guard.
			for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
				 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				// Checked at the TOP of each iteration, so an increment() that failed
				// is caught on the following pass rather than silently ending the
				// walk. Returning early leaves a half-copied tree, which is exactly
				// why the caller that needs atomicity uses staging + rename.
				if (ec)
					return Fail(outError, "failed while walking '" + from.string() + "': " + ec.message());

				// Regular files only. Directories are descended into by the iterator
				// itself, and skipping everything else keeps the seed copy to actual
				// file content.
				if (!it->is_regular_file(ec))
					continue;

				// This file's path RELATIVE to the seed root -- exactly what has to
				// be re-appended to the destination to preserve the tree's shape.
				const std::filesystem::path rel = std::filesystem::relative(it->path(), from, ec);
				if (ec)
					// A path that cannot be made relative is skipped rather than failing
					// the whole copy: it cannot be placed correctly, and one unmappable
					// entry should not abort seeding an entire project.
					continue;

				// `to / rel` re-derives the destination including subdirectories, so a
				// nested seed tree lands nested.
				const std::filesystem::path dst = to / rel;
				// The parent may not exist yet for a nested source. Ignoring ec here
				// is deliberate: copy_file below is the real test -- if the directory
				// could not be made, the copy fails and IS reported.
				std::filesystem::create_directories(dst.parent_path(), ec);
				// overwrite_existing: this is the destructive variant. Seed assets
				// land on top of the built-in shaders, which is how a caller
				// deliberately overrides an engine default of the same name.
				std::filesystem::copy_file(it->path(), dst, std::filesystem::copy_options::overwrite_existing, ec);
				if (ec)
					return Fail(outError, "could not copy '" + it->path().string() + "': " + ec.message());
			}
			// true means "copied everything it could", not "the source existed"
			// -- the absent-source case already returned true above.
			return true;
		}

		// The engine's own assets directory, remembered from Bootstrap so
		// Create() can seed new projects with the built-in shaders. Starts
		// empty, meaning "not located yet" -- which is not an error, it just
		// means a project gets an empty assets/shaders.
		// Process-wide singleton holding the engine's own assets root. Set
		// once by Bootstrap() and read by Create() and SyncBuiltInShaders().
		// Function-local static rather than a namespace-scope one, so it is
		// immune to static initialization order across translation units.
		// Starts EMPTY, which is the documented "not located yet" state and
		// is explicitly not an error.
		std::filesystem::path &EngineAssetsRootStorage()
		{
			// The storage itself.
			static std::filesystem::path root;
			return root;
		}

		// Copies files from one tree to another, leaving anything that
		// already exists at the destination untouched.
		//
		// This is deliberately non-destructive: refreshing a project's
		// shaders must never clobber a file the user has edited. Returns
		// true when nothing was copied because the source is absent, so
		// callers can treat "engine has no built-ins here" as normal.
		// Only genuine I/O failures are reported as errors.
		// Non-destructive copy: anything already at the destination is left
		// alone. This is the shader-refresh path and it MUST NOT clobber a
		// file the user has edited -- refreshing built-ins is a background
		// convenience, not what the user asked for.
		bool CopyTreeMissingOnly(const std::filesystem::path &from,
								 const std::filesystem::path &to,
								 std::string *outError,
								 int *outCopied = nullptr)
		{
			// Same absent-source-is-success contract as CopyTree, for the same
			// reason: an install with no engine assets simply has no built-ins to
			// seed, and the caller still succeeds.
			std::error_code ec;
			if (!std::filesystem::is_directory(from, ec))
				return true; // Nothing to seed from; not an error.

			// Counted so the caller can log "refreshed N shader(s)" instead of
			// claiming a refresh that copied nothing.
			int copied = 0;
			// Same iterator shape as CopyTree, with the same reasoning for the
			// separate top-of-body error check.
			for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
				 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				if (ec)
					return Fail(outError, "failed while walking '" + from.string() + "': " + ec.message());

				if (!it->is_regular_file(ec))
					continue;

				const std::filesystem::path rel = std::filesystem::relative(it->path(), from, ec);
				if (ec)
					continue;

				const std::filesystem::path dst = to / rel;

				// Never overwrite: a file already here is either the user's
				// own work or was seeded on a previous run.
				// The overwrite guard. A file already here is either the user's own
				// work or was seeded on a previous run -- both are load-bearing, and
				// this is the check that protects them.
				if (std::filesystem::exists(dst, ec))
					continue;

				std::filesystem::create_directories(dst.parent_path(), ec);
				if (ec)
					return Fail(outError, "could not create '" + dst.parent_path().string() + "': " + ec.message());

				// Second line of defence. copy_options::none refuses to overwrite, so
				// if the file appeared between the exists() check above and here, the
				// copy FAILS instead of silently clobbering. That failure is reported
				// by the ec check below.
				// copy_options::none (the default) is the second line of
				// defence against clobbering, in case the file appeared
				// between the check above and here.
				std::filesystem::copy_file(it->path(), dst, std::filesystem::copy_options::none, ec);
				if (ec)
					return Fail(outError, "could not copy '" + it->path().string() + "': " + ec.message());

				// Counted only on a real copy, so the log reflects work actually done.
				++copied;
			}

			// Optional: a caller that does not care how many files were refreshed
			// passes nullptr and skips the write.
			if (outCopied)
				*outCopied = copied;
			return true;
		}

	// End of the internal helpers. Everything below is Project's real API.
	} // namespace

	// Read side of the engine-assets singleton. Returns a const reference
	// to avoid copying a path on a call made per asset.
	const std::filesystem::path &Project::GetEngineAssetsRoot()
	{
		return EngineAssetsRootStorage();
	}

	// Write side, separate from the getter so a caller that only reads
	// never has to take a mutable reference to process-wide state.
	void Project::SetEngineAssetsRoot(const std::filesystem::path &root)
	{
		EngineAssetsRootStorage() = root;
	}

	// The Project singleton. Function-local static means it is constructed
	// on first use and cannot suffer static initialization order problems
	// across translation units -- which matters because engine systems
	// reach for Project::Get() during their own startup.
	Project &Project::Get()
	{
		// Function-local static: constructed on first use, so it is immune
		// to static initialization order across translation units.
		// Constructed on first call, so nothing depends on link order.
		static Project instance;
		return instance;
	}

	// "Is there a project?" answered without exposing the instance. False
	// after Close(), and also after AttachDetached(), which installs a
	// usable asset root but deliberately leaves the project closed.
	bool Project::Exists()
	{
		return Get().IsOpen();
	}

	// -------------------------------
	// Lifecycle
	// -------------------------------
	// ------------------------------------------------------------------
	// Lifecycle
	// ------------------------------------------------------------------

	// Creates root/assets plus every subdirectory in kAssetSubdirs.
	// Idempotent: create_directories succeeds when the tree is already
	// there, so this doubles as "repair the layout".
	bool Project::EnsureDirectoryLayout(const std::filesystem::path &root, std::string *outError)
	{
		std::error_code ec;
		// The asset root itself, created before its subdirectories so the
		// children have a parent to hang off.
		std::filesystem::create_directories(root / kAssetDirName, ec);
		if (ec)
			return Fail(outError, "could not create asset root: " + ec.message());

		// Range-for over the fixed subdir list. Written to tolerate being
		// called on an existing tree rather than assuming a fresh project.
		for (const char *sub : kAssetSubdirs)
		{
			std::filesystem::create_directories(root / kAssetDirName / sub, ec);
			if (ec)
				return Fail(outError, std::string("could not create asset subdirectory '") + sub + "': " + ec.message());
		}
		return true;
	}

	// Turns a user-typed project name into something safe to use as a
	// single directory component, on any platform.
	//
	// Deliberately platform-independent: a project created on Linux must
	// still open on Windows, so the Windows-illegal character set is
	// stripped unconditionally rather than only under a Windows build.
	std::string Project::SanitizeName(const std::string &name)
	{
		// reserve up front so the per-character pushes below do not repeatedly
		// reallocate for a long name.
		std::string out;
		out.reserve(name.size());

		// Tracks whether the last emitted character was a space. That single
		// flag is what collapses runs of whitespace and trims the ends.
		bool lastWasSpace = false;
		for (char c : name)
		{
			// Strip anything that is illegal in a Windows path component,
			// plus separators and control characters. Done unconditionally
			// on every platform so a project made on Linux still opens on
			// Windows.
			// Everything illegal in a Windows path component, plus BOTH separators
			// and C0 control characters. NUL is named separately because a
			// std::string can legitimately contain one and the filesystem call
			// downstream would silently truncate the path at it.
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|' || c == '\0' ||
				// Cast to unsigned char before the range check: the isspace-style
				// predicate is undefined for negative values and char is signed on
				// most targets, so a high-bit byte would be undefined behaviour
				// rather than "not a control character".
				static_cast<unsigned char>(c) < 0x20)
				continue;

			// Collapse runs of whitespace and trim the ends, so "  My Game  "
			// becomes "My Game" instead of a directory with invisible padding
			// that is miserable to work with from a shell.
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				// Collapse runs of whitespace and trim the ends, so
				// "  My Game  " becomes "My Game" rather than a directory
				// with invisible padding that is painful to work with on a
				// shell.
				// Drop a space that would be leading (out is still empty) or that
				// repeats the previous one -- one check doing both jobs.
				if (lastWasSpace || out.empty())
					continue;
				lastWasSpace = true;
				out.push_back(' ');
				continue;
			}

			// Reset on every real character, which is what makes the next run of
			// spaces collapse correctly.
			lastWasSpace = false;
			out.push_back(c);
		}

		// Trailing space can survive if the input ended on one.
		// Belt and braces: a trailing space can still survive when the input
		// ended on one that no following character reset.
		while (!out.empty() && out.back() == ' ')
			out.pop_back();

		// Windows refuses these as device names regardless of extension.
		// Windows reserves these device names regardless of extension, so
		// "CON" cannot name a directory. Uppercasing happens in one
		// immediately-invoked lambda so the loop below is a plain string
		// compare.
		const std::string upper = [&] {
			std::string u = out;
			// The lambda takes unsigned char so std::toupper is never handed a
			// negative value -- the same signedness trap as isspace above.
			std::transform(u.begin(), u.end(), u.begin(),
						   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return u;
		}();

		// The classic DOS device names, matched against the UPPERCASED name
		// so "con" and "Con" are caught too. An exact match, not a prefix:
		// "CONSOLE" is a perfectly legal directory and is left alone.
		static const char *reserved[] = {"CON", "PRN", "AUX", "NUL",
										  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
										  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
		for (const char *r : reserved)
		{
			// Append an underscore rather than rejecting. The user asked for
			// "CON"; a directory called CON_ is a usable project, whereas an
			// error message here would just be confusing.
			if (upper == r)
				return out + "_";
		}

		// Final fallback so the caller never has to handle an empty name. The
		// resulting directory is "Untitled", which is also the Settings
		// default, so the two agree.
		return out.empty() ? std::string("Untitled") : out;
	}

	// Creates <projectsDir>/<safeName>/, seeds it, writes the descriptor,
	// renames it into place, then opens it. Ending in Open() is what lets
	// a caller of Create() get a fully activated project without a
	// second call.
	bool Project::Create(const std::string &name, const std::filesystem::path &seedAssets, std::string *outError)
	{
		// Sanitized first, so every path built below is derived from a name
		// already known to be a legal directory component.
		const std::string safeName = SanitizeName(name);
		// SanitizeName falls back to "Untitled" when nothing usable
		// survives, so that string alone is ambiguous: it is both the
		// fallback AND a perfectly legitimate name the user may have typed.
		// Only treat it as a failure when the input did not actually ask
		// for it. (The substring test is case-sensitive, so a user-typed
		// "untitled" in lower case is correctly treated as a failure.)
		if (safeName == "Untitled" && name.find("Untitled") == std::string::npos)
			return Fail(outError, "project name contains no usable characters");

		// New projects always land under the user's projects directory, never
		// relative to the CWD -- a project must open identically regardless
		// of where the editor was launched from.
		const std::filesystem::path root = GetUserProjectsDirectory() / safeName;
		const std::filesystem::path projectFile = root / kProjectFileName;

		std::error_code ec;
		// Refuse to overwrite an existing project outright. Create() is not a
		// "save as": the descriptor's presence IS the existence test.
		if (std::filesystem::exists(projectFile, ec))
			return Fail(outError, "a project already exists at '" + root.string() + "'");

		// Build into a temporary sibling first, then rename into place, so
		// a failure part-way through (a full disk, a copy error) cannot
		// leave a half-populated project directory that later looks like a
		// valid project.
		// Build into a TEMPORARY SIBLING, then rename into place. A failure
		// part-way through -- a full disk, a locked file, a copy error -- then
		// leaves no half-populated directory that later looks like a valid
		// project. The suffix goes on the full path so staging shares a
		// parent with the target, which is what makes the final rename a
		// same-volume move rather than a cross-device copy.
		const std::filesystem::path staging = root.string() + ".creating";
		// Clear any staging directory abandoned by an earlier crashed run, so
		// a retry starts clean instead of merging with stale half-written
		// files.
		std::filesystem::remove_all(staging, ec);

		// The layout is built in staging, never directly in root -- root does
		// not exist yet, and creating it early would defeat the point.
		if (!EnsureDirectoryLayout(staging, outError))
		{
			// Clean up staging on this failure path. Without it a failed create
			// leaves a '.creating' directory sitting in the user's projects
			// folder.
			std::filesystem::remove_all(staging, ec);
			return false;
		}

		// Seed the built-in shaders first, so a new project can actually
		// render. The engine assets root is often empty (an unusual install
		// layout, or a unit test that has not set one), which is not an
		// error -- the project just starts with an empty shaders directory.
		{
			// The engine assets root, remembered from Bootstrap. Read through the
			// storage helper rather than a member because it is process-wide
			// state, not per-project.
			const std::filesystem::path &engineAssets = EngineAssetsRootStorage();
			// Empty means "Bootstrap never located the engine assets": an
			// unusual install layout, or a unit test that never set one. Not an
			// error -- the project just starts with an empty shaders directory.
			// Built-ins are seeded FIRST so a new project can actually render.
			if (!engineAssets.empty() &&
				!CopyTreeMissingOnly(engineAssets / "shaders", staging / kAssetDirName / "shaders", outError))
			{
				std::filesystem::remove_all(staging, ec);
				return false;
			}
		}

		// A seed is optional: an empty path means "just the built-ins",
		// which is the normal case for a brand new project. The seed is
		// copied on top and may overwrite a built-in of the same name,
		// which is how a caller deliberately overrides one.
		// Seed assets are optional: an empty path means "just the built-ins",
		// which is the normal case for a brand new project. Copied with the
		// DESTRUCTIVE CopyTree, on top of the built-ins, which is how a
		// caller deliberately overrides an engine default of the same name.
		if (!seedAssets.empty() && !CopyTree(seedAssets, staging / kAssetDirName, outError))
		{
			std::filesystem::remove_all(staging, ec);
			return false;
		}

		// Write the descriptor into staging, then move the whole tree.
		// Fresh Settings rather than the loaded one: a brand new project has
		// nothing to inherit, and the defaults declared in Project.h (version
		// 1, Medium quality, RendererAPI -1 meaning "engine default") are
		// the intended baseline.
		Settings fresh;
		fresh.Name = safeName;
		// Written into staging and not into root, so the descriptor becomes
		// visible only as part of the atomic rename below.
		if (!WriteDescriptor(staging / kProjectFileName, fresh))
		{
			std::filesystem::remove_all(staging, ec);
			return Fail(outError, "could not write project descriptor");
		}

		// The parent is the projects directory itself, which Create() has not
		// otherwise guaranteed exists.
		std::filesystem::create_directories(root.parent_path(), ec);
		// The atomic step. A same-volume rename, so the project directory
		// appears complete or not at all -- never populated-but-broken.
		std::filesystem::rename(staging, root, ec);
		if (ec)
		{
			// rename() failed, so staging is still sitting there. Remove it before
			// returning, or the next attempt at this same name inherits the
			// debris.
			std::filesystem::remove_all(staging, ec);
			return Fail(outError, "could not create project directory '" + root.string() + "': " + ec.message());
		}

		// Logged after the move has succeeded, so the message describes a
		// project that actually exists.
		WL_CORE_INFO("Project: created '{0}' at {1}", safeName, root.string());
		// Hand off to Open(), so the descriptor just written is parsed by
		// exactly the code that will parse it on the next launch. Creating
		// and opening in one step also means Create() cannot return success
		// while leaving the singleton closed.
		return Get().Open(root, outError);
	}

	// Accepts either the project directory or the project.wasteland file
	// itself, inferring one from the other. That is what lets a stored
	// recent-projects entry (a file path) be passed straight in.
	bool Project::Open(const std::filesystem::path &path, std::string *outError)
	{
		std::error_code ec;

		// Defaults assume the caller passed the descriptor FILE; both are
		// corrected below once the filesystem has told us which it was.
		std::filesystem::path projectFile = path;
		std::filesystem::path root = path;

		// A directory means the caller handed us the project root, so the
		// descriptor is the conventional filename inside it.
		if (std::filesystem::is_directory(path, ec))
		{
			projectFile = path / kProjectFileName;
		}
		else
		{
			// Not a directory: the caller passed the descriptor itself, so the
			// root is its parent. This is the shape a recent-projects entry
			// takes.
			root = path.parent_path();
		}

		// The descriptor is what makes a directory a PROJECT. Its absence is
		// the one thing Open() refuses -- everything else here is
		// repairable.
		if (!std::filesystem::exists(projectFile, ec))
			return Fail(outError, "no " + std::string(kProjectFileName) + " at '" + path.string() + "'");

		// A missing assets directory is NOT fatal: a project may
		// legitimately be empty of content, and the user can add assets from
		// the content browser.
		//
		// DEFECT: the create_directories() call below discards its
		// std::error_code. If the directory cannot be created, the warning
		// has already been logged, Open() still returns true, and the
		// project is activated with an asset root that does not exist -- so
		// every later GetAssetPath() resolves into nothing and the symptom
		// points nowhere near this line.
		if (!std::filesystem::is_directory(root / kAssetDirName, ec))
		{
			// Not fatal: a project may legitimately be empty of content
			// yet, and the user can add assets from the content browser.
			// Warned but not failed. The message names the root so the user can
			// see which project is incomplete.
			WL_CORE_WARN("Project: '{0}' has no assets directory yet", root.string());
			std::filesystem::create_directories(root / kAssetDirName, ec);
		}

		// The singleton is filled in place rather than returned, because
		// Project::Get() is the only handle the rest of the engine has.
		Project &self = Get();

		// Parsed through yaml-cpp. The Node stays local because the settings
		// below are copied out of it key by key.
		YAML::Node root_node;
		try
		{
			// Inside try/catch because LoadFile throws on malformed YAML, which
			// turns a corrupt descriptor into an ordinary reported error instead
			// of an exception escaping into the editor loop.
			//
			// DEFECT: the catch covers ONLY LoadFile. The .as<int>() and
			// .as<std::string>() conversions further down sit OUTSIDE it, and
			// yaml-cpp throws YAML::TypedBadConversion for a key that is present
			// but has the wrong type (for example Version: "two"). That
			// exception escapes Open() uncaught.
			root_node = YAML::LoadFile(projectFile.string());
		}
		catch (const std::exception &e)
		{
			return Fail(outError, std::string("could not parse project file: ") + e.what());
		}

		// Defaults come from Settings' own initializers, so a descriptor
		// missing any key still yields a usable Settings.
		Settings settings;
		// A null root node (an empty file) leaves every default in place
		// rather than failing: an empty descriptor is read as "all
		// defaults".
		if (root_node)
		{
			// Each key is tested for presence before conversion. These .as<>
			// calls are exactly the ones that can throw on a type mismatch, and
			// as noted above they are not inside the try block.
			if (root_node["Version"])
				settings.Version = root_node["Version"].as<int>();
			if (root_node["Name"])
				settings.Name = root_node["Name"].as<std::string>();
			if (root_node["EngineVersion"])
				settings.EngineVersion = root_node["EngineVersion"].as<std::string>();
			if (root_node["StartScene"])
				settings.StartScene = root_node["StartScene"].as<std::string>();
			if (root_node["QualityPreset"])
				settings.QualityPreset = root_node["QualityPreset"].as<std::string>();
			if (root_node["RendererAPI"])
				settings.RendererAPI = root_node["RendererAPI"].as<int>();

			// Render settings live under a nested "Render" map so adding more of
			// them never collides with a top-level key. Optional on read: a project
			// written before this existed simply keeps the struct's defaults, which
			// is why every access is guarded by a presence check.
			if (const YAML::Node render_node = root_node["Render"])
			{
				Project::Settings::RenderSettings &r = settings.Render;
				if (render_node["RenderScale"])            r.RenderScale = render_node["RenderScale"].as<float>();
				if (render_node["RayTracingEnabled"]){    r.RayTracingEnabled = render_node["RayTracingEnabled"].as<bool>(); r.HasRayTracingValue = true; }
				if (render_node["BloomEnabled"])           r.BloomEnabled = render_node["BloomEnabled"].as<bool>();
				if (render_node["BloomThreshold"])        r.BloomThreshold = render_node["BloomThreshold"].as<float>();
				if (render_node["BloomIntensity"])        r.BloomIntensity = render_node["BloomIntensity"].as<float>();
				if (render_node["VignetteEnabled"])        r.VignetteEnabled = render_node["VignetteEnabled"].as<bool>();
				if (render_node["VignetteIntensity"])     r.VignetteIntensity = render_node["VignetteIntensity"].as<float>();
				if (render_node["VignetteRadius"])         r.VignetteRadius = render_node["VignetteRadius"].as<float>();
				if (render_node["VignetteSoftness"])      r.VignetteSoftness = render_node["VignetteSoftness"].as<float>();
				if (render_node["ChromaticAberrationEnabled"])  r.ChromaticAberrationEnabled = render_node["ChromaticAberrationEnabled"].as<bool>();
				if (render_node["ChromaticAberrationIntensity"]) r.ChromaticAberrationIntensity = render_node["ChromaticAberrationIntensity"].as<float>();
				if (render_node["ColorGradingEnabled"])   r.ColorGradingEnabled = render_node["ColorGradingEnabled"].as<bool>();
				if (render_node["ColorTemperature"])      r.ColorTemperature = render_node["ColorTemperature"].as<float>();
				if (render_node["ColorTint"])             r.ColorTint = render_node["ColorTint"].as<float>();
				if (render_node["ColorGamma"])            r.ColorGamma = render_node["ColorGamma"].as<float>();
				if (render_node["FilmGrainEnabled"])      r.FilmGrainEnabled = render_node["FilmGrainEnabled"].as<bool>();
				if (render_node["FilmGrainIntensity"])   r.FilmGrainIntensity = render_node["FilmGrainIntensity"].as<float>();
			}
		}
		// Re-sanitized on load: this Name came from a file the user can edit,
		// and it feeds directory paths. Two consequences worth knowing: a
		// descriptor with an EMPTY Name becomes "Untitled" and no longer
		// matches its folder name, and a project deliberately named
		// "CON" is silently rewritten to "CON_" in memory while its
		// directory keeps the original spelling.
		settings.Name = SanitizeName(settings.Name);

		// Assigned in full, then Activate() fills in the paths and sets
		// m_Open -- those two steps together are what makes Exists() true.
		self.m_Settings = settings;
		// Activate() also normalizes the root to an absolute path, so every
		// later resolution is independent of the process CWD.
		self.Activate(root, projectFile);

		// Deliberately NOT chdir-ing into the project root. That was a crutch
		// for the old design, where relative "assets/..." strings were
		// resolved against the CWD; every engine system now goes through
		// GetAssetPath() instead.
		//
		// It also has a concrete cost on Windows: a process cannot delete or
		// rename a directory that is its current working directory, so
		// chdir-ing made the open project impossible to remove from
		// Explorer or from the editor's own project tools.
		// Deliberately NOT setting the process working directory to the
		// project root. That was a crutch for the old design, where
		// relative "assets/..." paths were resolved against the CWD; every
		// engine system now goes through GetAssetPath() instead.
		//
		// It also has a real cost on Windows: a process cannot delete or
		// rename a directory that is its current working directory, so
		// chdir-ing into the project made the open project impossible to
		// remove from Explorer or from the editor's own project tools.

		// Recorded only after a successful activation, so a project that
		// failed to open never enters the recent list.
		AddRecentProject(self.m_ProjectFile);
		WL_CORE_INFO("Project: opened '{0}' at {1}", self.m_Settings.Name, self.m_Root.string());
		return true;
	}

	// Tears down the active project. Idempotent: a second call returns
	// immediately instead of re-logging the close.
	void Project::Close()
	{
		Project &self = Get();
		// Guard, so Close() with nothing open is a no-op.
		if (!self.m_Open)
			return;

		// Logged before the state is cleared, while the name is still
		// readable.
		WL_CORE_INFO("Project: closed '{0}'", self.m_Settings.Name);
		self.m_Open = false;
		// Every path is CLEARED, not left pointing at the old project. A
		// half-cleared Project would let GetAssetPath() keep resolving assets
		// out of a project the caller believes is closed.
		self.m_Root.clear();
		self.m_AssetRoot.clear();
		self.m_ProjectFile.clear();
		// Settings reset to defaults, so a subsequent read sees a clean
		// project rather than the previous one's name and quality preset.
		self.m_Settings = Settings();
	}

	// Persists m_Settings to m_ProjectFile. Requires an open project: a
	// detached asset root has no descriptor to write.
	bool Project::Save()
	{
		// Both conditions matter. AttachDetached() sets the paths but leaves
		// m_Open false, so Save() correctly refuses for a convenience root.
		if (!m_Open || m_ProjectFile.empty())
			return false;

		// Written BEFORE the shader refresh, so the settings the user asked
		// to persist are already durable if the refresh below fails.
		if (!WriteDescriptor(m_ProjectFile, m_Settings))
			return false;

		// Refresh the built-in shaders so a project created before a shader
		// was added to the engine picks it up, and a project whose shaders
		// were deleted gets them back.
		//
		// This runs after the descriptor is already safely on disk, and a
		// failure here is reported but does not fail the save: the settings
		// are what the user asked to persist, and refusing to save them
		// because a shader copy hit a locked file would be worse than
		// leaving the shader refresh for the next save.
		// Shader refresh runs after the descriptor is safely on disk, and a
		// failure is reported but does NOT fail the save. The settings are
		// what the user asked to persist; refusing to save them because a
		// shader copy hit a locked file would be worse than leaving the
		// refresh for the next save.
		SyncBuiltInShaders();

		return true;
	}

	// Restores engine built-in shaders missing from the project: one
	// created before a shader was added picks it up, and one whose
	// shaders were deleted gets them back. Non-destructive, so it cannot
	// clobber an edited shader.
	void Project::SyncBuiltInShaders()
	{
		// Same guard as Save(): a detached root has no project to refresh.
		if (!m_Open)
			return;

		const std::filesystem::path &engineAssets = EngineAssetsRootStorage();
		// Nothing to copy from on an install whose engine assets were never
		// located. Silent, because that is a normal state and not a failure.
		if (engineAssets.empty())
			return;

		int copied = 0;
		// outError is nullptr on purpose -- this runs from Save(), which must
		// not fail because a shader copy did. CopyTreeMissingOnly still logs
		// through Fail().
		if (!CopyTreeMissingOnly(engineAssets / "shaders", m_AssetRoot / "shaders", nullptr, &copied))
			return; // Already logged by CopyTreeMissingOnly.

		// Logs only when something was actually copied, so a steady-state save
		// is quiet instead of claiming a refresh every single time.
		if (copied > 0)
			WL_CORE_INFO("Project: refreshed {0} built-in shader(s) for '{1}'",
						 copied, m_Settings.Name);
	}

	// Emits project.wasteland. The customize hook lets a caller adjust
	// Settings just for serialization without disturbing live project
	// state.
	bool Project::WriteDescriptor(const std::filesystem::path &file,
								  const Settings &base,
								  const std::function<void(Settings &)> &customize)
	{
		// Work on a copy, so a failure part-way through cannot leave the
		// in-memory project and the file on disk disagreeing.
		// Works on a COPY, and customize() runs against that copy, so a
		// failure part-way through cannot leave the in-memory project and the
		// file on disk disagreeing.
		Settings settings = base;
		// Optional hook: null means "write base unchanged", which is what
		// every current caller does.
		if (customize)
			customize(settings);

		// The yaml-cpp Emitter builds the whole document in memory first, so
		// a bad value cannot leave a truncated file: the ofstream is opened
		// only once the text exists.
		YAML::Emitter out;
		// Two-space indent, matching the hand-authored project files.
		out.SetIndent(2);
		// Explicit BeginMap/EndMap rather than relying on an implicit
		// top-level map, so the key order below is exactly the file layout and
		// is stable across yaml-cpp versions.
		out << YAML::BeginMap;
		// Every key is always written, including ones left at their default.
		// A descriptor that spells out its defaults is self-documenting and
		// diffs cleanly.
		out << YAML::Key << "Version" << YAML::Value << settings.Version;
		out << YAML::Key << "Name" << YAML::Value << settings.Name;
		out << YAML::Key << "EngineVersion" << YAML::Value << settings.EngineVersion;
		out << YAML::Key << "StartScene" << YAML::Value << settings.StartScene;
		out << YAML::Key << "QualityPreset" << YAML::Value << settings.QualityPreset;
		out << YAML::Key << "RendererAPI" << YAML::Value << settings.RendererAPI;

		// Nested so the top level stays a fixed, known set of keys.
		const Project::Settings::RenderSettings &r = settings.Render;
		out << YAML::Key << "Render" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "RenderScale" << YAML::Value << r.RenderScale;
		// Only written once the user has touched the toggle, so opening a project
		// that predates the setting does not force ray tracing on.
		if (r.HasRayTracingValue)
			out << YAML::Key << "RayTracingEnabled" << YAML::Value << r.RayTracingEnabled;
		out << YAML::Key << "BloomEnabled" << YAML::Value << r.BloomEnabled;
		out << YAML::Key << "BloomThreshold" << YAML::Value << r.BloomThreshold;
		out << YAML::Key << "BloomIntensity" << YAML::Value << r.BloomIntensity;
		out << YAML::Key << "VignetteEnabled" << YAML::Value << r.VignetteEnabled;
		out << YAML::Key << "VignetteIntensity" << YAML::Value << r.VignetteIntensity;
		out << YAML::Key << "VignetteRadius" << YAML::Value << r.VignetteRadius;
		out << YAML::Key << "VignetteSoftness" << YAML::Value << r.VignetteSoftness;
		out << YAML::Key << "ChromaticAberrationEnabled" << YAML::Value << r.ChromaticAberrationEnabled;
		out << YAML::Key << "ChromaticAberrationIntensity" << YAML::Value << r.ChromaticAberrationIntensity;
		out << YAML::Key << "ColorGradingEnabled" << YAML::Value << r.ColorGradingEnabled;
		out << YAML::Key << "ColorTemperature" << YAML::Value << r.ColorTemperature;
		out << YAML::Key << "ColorTint" << YAML::Value << r.ColorTint;
		out << YAML::Key << "ColorGamma" << YAML::Value << r.ColorGamma;
		out << YAML::Key << "FilmGrainEnabled" << YAML::Value << r.FilmGrainEnabled;
		out << YAML::Key << "FilmGrainIntensity" << YAML::Value << r.FilmGrainIntensity;
		out << YAML::EndMap;
		out << YAML::EndMap;

		// The descriptor's directory is created if missing, so
		// WriteDescriptor() works for a fresh project without a separate
		// layout call.
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);

		// Opened in truncate mode (the ofstream default), so rewriting an
		// EXISTING descriptor replaces it rather than leaving trailing bytes
		// from a longer previous version.
		std::ofstream fout(file);
		if (!fout.is_open())
		{
			WL_CORE_ERROR("Project: could not write '{0}'", file.string());
			return false;
		}

		// DEFECT: fail() covers a failed write but NOT a failure that only
		// surfaces during the flush in the destructor. A descriptor accepted
		// here can still have failed on disk -- and Create() renames staging
		// into place on the strength of this return value.
		fout << out.c_str();
		return !fout.fail();
	}

	// Records the paths that make this an OPEN project. Deliberately does
	// not read the descriptor: Open() has already parsed it into
	// m_Settings, so this is purely bookkeeping.
	void Project::Activate(const std::filesystem::path &root, const std::filesystem::path &projectFile)
	{
		// absolute() so resolution never depends on the process CWD -- the
		// same reason Open() does not chdir.
		m_Root = std::filesystem::absolute(root);
		m_AssetRoot = m_Root / kAssetDirName;
		// An empty projectFile means "caller did not name one", so fall back
		// to the conventional filename inside the root.
		m_ProjectFile = projectFile.empty() ? m_Root / kProjectFileName : projectFile;
		// m_Open is set here and ONLY here. That is what lets Exists() be a
		// single bool read with no chance of the flag and the paths
		// disagreeing.
		m_Open = true;
	}

	// Adopts an arbitrary directory that merely HAS an assets
	// subdirectory, with no project.wasteland. For opening a content
	// folder that is not a project.
	bool Project::AttachAssetRoot(const std::filesystem::path &rootWithAssets,
								  std::string *outError)
	{
		std::error_code ec;
		// Non-throwing absolute(); on failure the caller's own (possibly
		// relative) path is used as-is, so the attach still succeeds.
		std::filesystem::path root = std::filesystem::absolute(rootWithAssets, ec);
		if (ec)
			root = rootWithAssets;

		// Refuse a directory with no assets subdirectory. Attaching anyway
		// would make every GetAssetPath() resolve into an empty directory, and
		// the symptom -- "my asset is missing" -- points nowhere near the
		// actual mistake.
		// Refuse a directory with no assets subdirectory. Attaching anyway
		// would make every GetAssetPath() call resolve into an empty
		// directory, and the symptom -- "my asset is missing" -- points
		// nowhere near the actual mistake.
		if (!std::filesystem::is_directory(root / kAssetDirName, ec))
		{
			if (outError)
				*outError = "no 'assets' directory under " + root.string();
			return false;
		}

		// Detached, not opened: the caller gets a usable asset root but
		// Exists() stays false and Save() refuses, because there is no
		// descriptor to write.
		AttachDetached(root);
		return true;
	}

	// The convenience-root path. Everything except m_Open is filled in so
	// GetAssetPath() works immediately; only project-level state is
	// absent.
	void Project::AttachDetached(const std::filesystem::path &rootWithAssets)
	{
		std::error_code ec;
		// Same non-throwing absolute() with the same fallback as above.
		m_Root = std::filesystem::absolute(rootWithAssets, ec);
		if (ec)
			m_Root = rootWithAssets;
		m_AssetRoot = m_Root / kAssetDirName;
		m_ProjectFile = m_Root / kProjectFileName;
		// Settings reset, because nothing was parsed from a descriptor -- any
		// carried-over values would be from a previously open project.
		m_Settings = Settings();
		// The directory's own name is the best available label for a root
		// with no descriptor.
		m_Settings.Name = m_Root.filename().string();
		// m_Open stays FALSE. This is the whole point of "detached": a
		// convenience root resolves assets but is not a project, so Exists()
		// reports false and Save() refuses.
		m_Open = false; // No descriptor: a convenience root, not a project.

		// Logged as INFO, not an error: running with a plain asset root is a
		// supported mode (source checkouts, asset-less tools).
		WL_CORE_INFO("Project: no project file; using '{0}' as a plain asset root", m_Root.string());
	}

	// -------------------------------
	// Paths
	// -------------------------------
	// ------------------------------------------------------------------
	// Paths
	// ------------------------------------------------------------------

	// Resolves an asset reference to an absolute path under the project.
	// Handles empty, absolute, "assets/..."-prefixed and bare-relative
	// forms, because all four turn up in saved content.
	std::filesystem::path Project::GetAssetPath(const std::string &relative) const
	{
		// An empty reference means the asset root itself -- the directory a
		// content browser would list.
		if (relative.empty())
			return m_AssetRoot;

		// Work from the original string for the absolute test: a path built
		// from forward slashes is still absolute on Windows, but returning it
		// verbatim would hand callers a mixed-separator path that will not
		// compare equal to one built natively.
		// The ORIGINAL string is kept for the absolute test: a path built
		// from forward slashes is still absolute on Windows, but returning it
		// verbatim would hand callers a mixed-separator path that will not
		// compare equal to one built natively.
		const std::filesystem::path original(relative);
		const std::filesystem::path rel(ToForwardSlashes(relative));

		// Components are rejoined one at a time because std::filesystem
		// preserves whatever separator a path was constructed from, and a
		// Windows path carrying "/" would not compare equal to the same
		// path built with "\". lexically_normal() also restores the "\" after
		// a drive root, which appending components alone would not.
		// Components are rejoined one at a time because std::filesystem
		// preserves whatever separator a path was constructed from, and a
		// Windows path carrying "/" would not compare equal to the same path
		// built with "\\". lexically_normal() also restores the separator
		// after a drive root, which appending components alone would not.
		const auto joinNative = [](const std::filesystem::path &source,
								   std::filesystem::path::iterator begin)
		{
			std::filesystem::path native;
			for (auto it = begin; it != source.end(); ++it)
				native /= (*it).string();
			// lexically_normal() is purely textual -- no filesystem access, no
			// symlink resolution -- which is what makes this safe to call on
			// paths that do not exist yet.
			return native.lexically_normal();
		};

		// An absolute reference is honoured as-is (normalized to native
		// separators). Needed so a scene can reference a file outside the
		// project, such as a source-tree asset during development.
		if (original.is_absolute())
			return joinNative(rel, rel.begin());

		// Accept both "textures/x.png" and "assets/textures/x.png" so
		// scene files written by older builds -- which stored the full
		// "assets/..." string -- keep resolving without a migration pass.
		// The leading "assets" is stripped so the two forms cannot produce
		// "assets/assets/...".
		// Accept both "textures/x.png" and "assets/textures/x.png" so
		// scene files written by older builds -- which stored the full
		// "assets/..." string -- keep resolving without a migration pass.
		if (rel.begin() != rel.end() && (*rel.begin()).string() == kAssetDirName)
			return (m_Root / kAssetDirName / joinNative(rel, std::next(rel.begin())));

		// The common case: already project-relative, so it hangs off the
		// asset root rather than the project root.
		return (m_AssetRoot / joinNative(rel, rel.begin()));
	}

	// Same normalization rules as GetAssetPath, but rooted at the project
	// directory instead of the asset directory -- for things that are not
	// assets, such as a scene file or a project-relative config path.
	std::filesystem::path Project::GetProjectPath(const std::string &relative) const
	{
		if (relative.empty())
			return m_Root;

		// No leading-"assets" special case here, unlike GetAssetPath: a
		// project path may genuinely start with "assets/", so stripping it
		// would be wrong.
		std::filesystem::path rel(ToForwardSlashes(relative));
		if (rel.is_absolute())
			return rel;

		// Rejoin with the native separator for the same reason as
		// GetAssetPath above.
		std::filesystem::path native;
		for (const auto &part : rel)
			native /= part.string();

		return (m_Root / native.lexically_normal());
	}

	// Inverse of GetAssetPath: turns an absolute path back into a
	// project-relative, forward-slashed reference for writing into a
	// scene file. Returns empty when there is no representable relative
	// path.
	std::string Project::MakeProjectRelative(const std::filesystem::path &absolute) const
	{
		// No open project means no frame of reference, so there is nothing
		// for a path to be relative to.
		if (m_Root.empty())
			return "";

		std::error_code ec;
		// Normalized to absolute first: relative() below needs two absolute
		// paths to mean anything.
		std::filesystem::path abs = std::filesystem::absolute(absolute, ec);
		if (ec)
			return "";

		// std::filesystem::relative produces a "../.." chain for anything
		// outside the root -- see the check below.
		std::filesystem::path rel = std::filesystem::relative(abs, m_Root, ec);
		if (ec)
			return "";

		// Anything that climbs out of the project is not representable as a
		// project-relative reference. Returning empty (rather than the
		// "a/../../b" string) lets callers detect the foreign path instead of
		// writing a reference that breaks the moment the project moves.
		// Anything that climbs out of the project is not representable as
		// a project-relative reference. Returning empty (rather than the
		// "a/../../b" string) lets callers detect the foreign path instead
		// of writing a reference that breaks the moment the project moves.
		if (rel.empty())
			return "";
		// Only the FIRST component needs checking: relative() emits ".."
		// only as a leading run, never in the middle.
		if (*rel.begin() == "..")
			return "";

		// Forward slashes, so the stored reference matches every other
		// project-relative string in the file format.
		return ToForwardSlashes(rel.generic_string());
	}

	// -------------------------------
	// Well-known directories
	// -------------------------------
	// ------------------------------------------------------------------
	// Well-known directories
	// ------------------------------------------------------------------

	// Where the user's documents live -- asked of the shell rather than
	// assumed from %USERPROFILE%.
	std::filesystem::path Project::GetUserDocumentsDirectory()
	{
#ifdef WL_PLATFORM_WINDOWS
		// Ask the shell rather than assuming %USERPROFILE%\Documents:
		// on most installs Documents is redirected to OneDrive, and
		// writing to the profile folder would silently put the user's
		// projects somewhere they will not look for them.
		PWSTR wpath = nullptr;
		// FOLDERID_Documents, not FOLDERID_Profile, precisely because of the
		// OneDrive redirection noted above. The shell allocates wpath; it is
		// released with CoTaskMemFree below.
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &wpath)) && wpath)
		{
			std::filesystem::path result = std::filesystem::path(wpath);
			// Frees the shell's allocation. NOTE: wpath is NOT set to nullptr
			// afterwards, and the check at line 637 tests it again -- see the
			// defect noted there.
			CoTaskMemFree(wpath);
			if (!result.empty())
				return result;
		}
		// DEFECT: this frees wpath a SECOND time. The success path above
		// already called CoTaskMemFree(wpath) at line 633 without clearing
		// the pointer, so the only way to reach this line is result having
		// come back EMPTY -- and then this is a double free. In practice
		// SHGetKnownFolderPath rarely returns an empty string, which is why
		// this has survived. The intent was presumably to catch a call that
		// FAILED but still set wpath, which needs wpath = nullptr after
		// line 633.
		if (wpath)
			CoTaskMemFree(wpath);
#endif
		// Non-Windows fallbacks in order of preference. Reaching any of these
		// means the Windows branch above did not return.
		if (const char *home = std::getenv("HOME"))
			return std::filesystem::path(home) / "Documents";

		if (const char *profile = std::getenv("USERPROFILE"))
			return std::filesystem::path(profile) / "Documents";

		// Last resort: a Documents folder under the CWD. Wrong in almost
		// every case, but a defined path beats an empty one.
		return std::filesystem::current_path() / "Documents";
	}

	// Projects live under Documents, so this is the parent every new
	// project is created in.
	std::filesystem::path Project::GetUserProjectsDirectory()
	{
		return GetUserDocumentsDirectory() / kProjectsFolderName;
	}

	// Where editor state (currently just the recent-projects list) is
	// written. Platform branches rather than one portable lookup because
	// the conventions genuinely differ: Windows uses %APPDATA%, Linux
	// honours XDG_CONFIG_HOME.
	std::filesystem::path Project::GetUserConfigDirectory()
	{
#ifdef WL_PLATFORM_WINDOWS
		// Windows: the roaming AppData location. Guarded because on a
		// non-Windows build APPDATA is usually absent and the XDG branch
		// below is the correct one.
		if (const char *appData = std::getenv("APPDATA"))
			return std::filesystem::path(appData) / "Wasteland";
#endif
		// Linux/BSD convention, honouring the XDG override when set.
		if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
			return std::filesystem::path(xdg) / "Wasteland";
		// $HOME/.config/Wasteland is the default when XDG_CONFIG_HOME is
		// unset.
		if (const char *home = std::getenv("HOME"))
			return std::filesystem::path(home) / ".config" / "Wasteland";

		// Last resort: a .config/Wasteland directory under the CWD.
		return std::filesystem::current_path() / ".config" / "Wasteland";
	}

	// -------------------------------
	// Recent projects
	// -------------------------------
	// ------------------------------------------------------------------
	// Recent projects
	// ------------------------------------------------------------------

	// Reads the recent list, most recent first. A missing or unreadable
	// file yields an empty list rather than an error: no recents is a
	// normal first-run state.
	std::vector<std::string> Project::GetRecentProjects()
	{
		std::vector<std::string> out;
		// Opened by path; ifstream takes a std::filesystem::path directly.
		std::ifstream fin(GetUserConfigDirectory() / kRecentFileName);
		// A missing file is not an error -- it just means nothing has been
		// opened yet.
		if (!fin.is_open())
			return out;

		std::string line;
		// One line per entry. Stored forward-slashed (see AddRecentProject),
		// so nothing has to be translated on the way out.
		while (std::getline(fin, line))
		{
			line = ToForwardSlashes(line);
			// Skip blank lines rather than storing them, so a trailing newline
			// does not become an empty entry.
			if (!line.empty())
				out.push_back(line);

			// Enforce the cap while reading, so a hand-edited or corrupted file
			// with thousands of entries cannot flood the UI.
			if (out.size() >= kMaxRecentProjects)
				break;
		}
		return out;
	}

	// Records a project as most-recently-used, de-duplicating and trimming
	// to the cap.
	void Project::AddRecentProject(const std::filesystem::path &projectFile)
	{
		if (projectFile.empty())
			return;

		std::error_code ec;
		// Made absolute and forward-slashed so the stored key is stable no
		// matter how the caller expressed it -- otherwise the same project
		// opened via two different relative paths would appear twice.
		std::string key = ToForwardSlashes(std::filesystem::absolute(projectFile, ec).generic_string());
		if (ec)
			key = ToForwardSlashes(projectFile.generic_string());

		// Re-read from disk rather than trusting an in-memory list, so the
		// file stays the single source of truth.
		std::vector<std::string> recents = GetRecentProjects();

		// Most-recent-first, no duplicates.
		// Most-recent-first with no duplicates: erase every existing copy of
		// the key, then push it to the front.
		recents.erase(std::remove(recents.begin(), recents.end(), key), recents.end());
		recents.insert(recents.begin(), key);
		// Trim from the END, because the newest entry was just placed at the
		// front.
		if (recents.size() > kMaxRecentProjects)
			recents.resize(kMaxRecentProjects);

		const std::filesystem::path dir = GetUserConfigDirectory();
		std::filesystem::create_directories(dir, ec);

		// The whole list is rewritten on every open. Fine at this size (ten
		// short lines) and it avoids the read-modify-write race a
		// seek-and-append approach would invite.
		std::ofstream fout(dir / kRecentFileName);
		// A config directory we cannot write to is not worth failing over --
		// the recent list is a convenience, not state the engine needs.
		if (!fout.is_open())
			return;

		// One line per entry, newline-terminated, matching what
		// GetRecentProjects() parses back.
		for (const std::string &r : recents)
			fout << r << "\n";
	}

	// Forgets every recent project. Removes the file rather than
	// truncating it, so a missing file is simply a no-op.
	void Project::ClearRecentProjects()
	{
		std::error_code ec;
		std::filesystem::remove(GetUserConfigDirectory() / kRecentFileName, ec);
	}

	// -------------------------------
	// Bootstrap
	// -------------------------------
	// ------------------------------------------------------------------
	// Bootstrap
	// ------------------------------------------------------------------

	// Startup resolution, in a fixed priority order: explicit argument,
	// environment variable, recent list, then an assets directory near
	// the executable. Returns true only when a real PROJECT was opened --
	// a convenience root attaches and still returns false.
	bool Project::Bootstrap(const std::string &explicitPath, const std::filesystem::path &exePath)
	{
		// The singleton every step below writes into.
		Project &self = Get();

		// Locate the engine's own assets FIRST, before anything that can
		// return early. Otherwise the early returns below would skip it and
		// leave new projects with an empty shaders directory on a perfectly
		// good install. Step 4 below reuses this same candidate list.
		// Locate the engine's own assets before anything else, because the
		// early returns below would otherwise skip it and leave new projects
		// with an empty shaders directory on a perfectly good install.
		// Step 4 below reuses this same candidate list.
		{
			std::error_code ec;
			// Candidates are the executable's directory and its two ancestors,
			// plus the CWD: this covers bin/, build/bin/, and running from a
			// source checkout.
			const std::filesystem::path exeDir = exePath.parent_path();
			const std::vector<std::filesystem::path> candidates = {
				exeDir,
				exeDir.parent_path(),
				exeDir.parent_path().parent_path(),
				std::filesystem::current_path(),
			};

			// First match wins, so the nearest candidate to the executable is
			// preferred.
			for (const std::filesystem::path &root : candidates)
			{
				// Guard against an empty candidate from a degenerate path.
				if (root.empty())
					continue;
				// Require a shaders subdirectory: a bare "assets" folder
				// somewhere up the tree is not the engine install, and
				// picking it would seed new projects from the wrong place.
				// Require an assets/SHADERS subdirectory: a bare "assets" folder
				// somewhere up the tree is not the engine install, and picking it
				// would seed new projects from the wrong place.
				if (std::filesystem::is_directory(root / kAssetDirName / "shaders", ec))
				{
					// Stored as root/assets (not root), which is what step 4's
					// parent_path() call below relies on.
					EngineAssetsRootStorage() = root / kAssetDirName;
					WL_CORE_INFO("Project: engine assets at '{0}'",
								 EngineAssetsRootStorage().string());
					break;
				}
			}
		}

		// One helper for all four sources, so the logging and the
		// "could not be opened" message are identical no matter where a
		// candidate came from.
		auto tryOpen = [&self](const std::string &candidate, const char *source) -> bool {
			// An empty candidate is skipped rather than handed to Open(), which
			// would report a confusing "no project.wasteland at ''".
			if (candidate.empty())
				return false;
			std::string err;
			if (self.Open(candidate, &err))
			{
				WL_CORE_INFO("Project: loaded from {0}: {1}", source, candidate);
				return true;
			}
			// A failed candidate is a WARN, not an error: with four sources in
			// priority order, three of them are expected to fail on any given
			// launch.
			WL_CORE_WARN("Project: {0} '{1}' could not be opened: {2}", source, candidate, err);
			return false;
		};

		// 1. Explicit command-line request.
		// 1. Explicit command-line request.
		if (tryOpen(explicitPath, "--project"))
			return true;

		// 2. Environment variable, for launchers and desktop shortcuts that
		// cannot pass arguments.
		// 2. Environment variable, for launchers and desktop shortcuts.
		if (const char *env = std::getenv("WASTELAND_PROJECT"))
		{
			if (tryOpen(env, "WASTELAND_PROJECT"))
				return true;
		}

		// 3. Most recent project that still exists. Iterated rather than
		// taking only the newest, because that entry may have been deleted
		// or moved since last run.
		// 3. Most recent project that still exists. Iterate because the
		// newest entry may have been deleted or moved since last run.
		for (const std::string &recent : GetRecentProjects())
		{
			std::error_code ec;
			// Existence checked here so a stale entry is skipped quietly; Open()
			// would report it as a hard error otherwise.
			if (!std::filesystem::exists(recent, ec))
				continue;
			if (tryOpen(recent, "recent projects list"))
				return true;
		}

		// 4. Fall back to an "assets" directory near the executable. This
		// is how the editor runs from a source checkout or a bin/ folder
		// with no project -- a convenience root, not a real project. Reuses
		// the assets root located at the top of this function.
		// 4. Fall back to an "assets" directory near the executable. This
		// is how the editor runs from a source checkout or a bin/ folder
		// with no project — a convenience root, not a real project. Reuses
		// the assets root located at the top of this function.
		if (!EngineAssetsRootStorage().empty())
		{
			// AttachDetached takes the directory CONTAINING assets, which is
			// exactly parent_path() of the stored root/assets.
			self.AttachDetached(EngineAssetsRootStorage().parent_path());
			// Returns FALSE even though a usable asset root is now attached:
			// the contract is "did a project open", and a convenience root is
			// not a project.
			return false;
		}

		// Nothing at all worked. WARN rather than ERROR, because launching
		// with no project is legitimate -- the user can then create one.
		WL_CORE_WARN("Project: no project found and no assets directory near '{0}'", exePath.string());
		return false;
	}

} // namespace Wasteland
