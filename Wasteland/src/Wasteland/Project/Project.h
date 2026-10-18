#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Wasteland
{

	// A project is a directory on disk containing a project.wasteland
	// descriptor and an "assets" tree. Everything the engine and the editor
	// load is resolved through the active project rather than against the
	// process working directory, so a game can be moved or opened from
	// anywhere without breaking its asset references.
	//
	// Projects live under the user's Documents directory by default
	// (Documents/Wasteland Projects/<Name>). See GetUserProjectsDirectory.
	// Mixed model: a few statics for process-wide locations and the singleton,
	// plus non-static members for the currently-open project state. The
	// non-static half is what a second Project instance would hold -- but the
	// private default ctor means Get() is the only way to obtain one, so in
	// practice there is exactly one.
	class Project
	{
	public:
		// Contents of project.wasteland. Persisted as YAML.
		// A POD-ish aggregate with in-class defaults, so a default-constructed
		// Settings is already a valid settings block. That matters for
		// WriteDescriptor, which is called by Create() for a project that has
		// never been opened and therefore has no saved settings to start from.
		struct Settings
		{
			// Bumped when the on-disk layout changes; the loader migrates
			// older files forward rather than rejecting them.
			// Bumped on layout change; the loader migrates forward. Note the default
			// is 1 rather than 0, so a missing Version key in a hand-edited YAML
			// reads as "current" instead of "ancient".
			int Version = 1;

			std::string Name = "Untitled";
			// The engine that wrote the file, not the one that last touched it. There
			// is no migration policy documented in this header beyond the Version bump
			// above, so this is informational -- appears to be display-only.
			std::string EngineVersion = "0.1.0";

			// Project-relative path (forward slashes) of the scene to open
			// when the project loads, e.g. "assets/scenes/Main.wastescene".
			std::string StartScene;

			// A STRING, not the renderer's QualityPreset enum. The enum lives in
			// Renderer3D.h, which this header deliberately does not include, so the
			// project file format stays independent of the renderer. The cost is that
			// an invalid value can be written here and only fail later at parse time.
			std::string QualityPreset = "Medium";

			// -1 means "use the engine default" rather than pinning a
			// backend in the project file.
			// -1 is the "unset" sentinel rather than a real API id, which means a
			// file that genuinely wants backend -1 is indistinguishable from one that
			// never chose -- harmless while real ids are non-negative.
			int RendererAPI = -1;

			// Render settings, persisted so a reopened project looks the same.
			//
			// These were previously global statics in Renderer3DData with no
			// descriptor key anywhere, so every one of them was silently lost on
			// restart: render scale and the whole post-processing chain (bloom
			// threshold/intensity, vignette, chromatic aberration, colour grading,
			// film grain) plus the ray-tracing toggle. The UI called setters that
			// affected the current frame and nothing more.
			//
			// Deliberately a PLAIN struct of scalars rather than the renderer's own
			// types, for the same reason QualityPreset is a string above: this header
			// does not include Renderer3D.h, and the project file format stays
			// independent of the renderer. EditorLayer applies these through the
			// normal setters on open and reads them back on save.
			struct RenderSettings
			{
				// 0 = unset sentinel, so "never chosen" stays distinguishable from
				// a real value. 0.5..1.0 in practice; the renderer clamps to its range.
				float RenderScale = 0.0f;
				bool RayTracingEnabled = false;
				bool HasRayTracingValue = false; // false until the user touches the toggle

				bool BloomEnabled = false;
				float BloomThreshold = 1.0f;
				float BloomIntensity = 0.0f;

				bool VignetteEnabled = false;
				float VignetteIntensity = 0.0f;
				float VignetteRadius = 0.0f;
				float VignetteSoftness = 0.0f;

				bool ChromaticAberrationEnabled = false;
				float ChromaticAberrationIntensity = 0.0f;

				bool ColorGradingEnabled = false;
				float ColorTemperature = 0.0f;
				float ColorTint = 0.0f;
				// Floored at 0.1 by the setter and intentionally not capped above.
				float ColorGamma = 1.0f;

				bool FilmGrainEnabled = false;
				float FilmGrainIntensity = 0.0f;
			};

			RenderSettings Render;
		};

		// The active project. Never null, but IsOpen() is false until a
		// project is opened or a project is created, in which case asset
		// resolution falls back to the process working directory so
		// asset-less tools (tests, the Sandbox app) keep working.
		// Reference return, never null: a function-local static would be the usual
		// implementation, and it is deliberately NOT reset between runs, so a test
		// that opens a project leaks that state into the next test unless it calls
		// Close() itself.
		static Project &Get();

		// Distinct from IsOpen(): this asks whether a project is DISCOVERABLE,
		// which is what Bootstrap needs before it has chosen one.
		static bool Exists();

		// --- Lifecycle -------------------------------------------------

		// Creates <projectsDir>/<sanitized name>/ with a fresh descriptor
		// and the standard asset subdirectory layout.
		//
		// Built-in shaders are copied in from the engine's own assets so a
		// new project can render immediately instead of starting from an
		// empty assets/shaders. seedAssets, when non-empty, is copied on
		// top of <root>/assets to supply starter content, and takes
		// precedence over the built-ins for any file both provide.
		//
		// Becomes the active project on success. Returns false and fills
		// outError on failure; the previously active project is left
		// untouched and no partial directory is left behind.
		// outError as a defaulted out-param rather than an exception or a Result
		// type: consistent with the rest of the engine's file API, and it keeps
		// the create path usable from C-facing boundaries.
		//
		// Documented as all-or-nothing: no partial directory is left behind, and
		// the previously active project is untouched on failure. That is a real
		// constraint on the implementation -- it means cleanup on the error path,
		// not just a bail-out.
		static bool Create(const std::string &name, const std::filesystem::path &seedAssets,
						   std::string *outError = nullptr);

		// Loads a project.wasteland (or a directory containing one) and
		// makes it active. Asset resolution is relative to the project
		// root, not the process working directory, so the project can be
		// moved, deleted, or opened from anywhere.
		// Non-static, unlike Create(): opening is an action on the singleton, so
		// making it a member signals "this is the active project changing"
		// without needing the caller to have fetched it via Get() first.
		bool Open(const std::filesystem::path &path, std::string *outError = nullptr);

		// Deactivates the project. Asset paths fall back to the working
		// directory. Does not delete anything on disk.
		// Non-destructive by design: this is "stop resolving assets against this
		// project", not "delete the project". The OnDiskLifetime distinction
		// matters because Close() is on the path of merely switching projects.
		void Close();

		// Writes project.wasteland, then refreshes the project's built-in
		// shaders from the engine. No-op when no project is open.
		//
		// The refresh only adds shaders the project is missing, so shader
		// files the user has edited or deleted are never overwritten --
		// saving a project can therefore never destroy work.
		// Write-then-refresh-shaders. The refresh is additive only, which is what
		// makes Save() safe to call from an auto-save path: it can never clobber a
		// shader the user edited or deleted.
		bool Save();

		// Copies any built-in shader the project is missing from the
		// engine, leaving existing shader files untouched. Called
		// automatically by Save() and by Create(); exposed so a project can
		// be refreshed after the engine is updated in place.
		// Public mostly for the "engine updated in place" case -- a dev build that
		// gained new built-in shaders. Exposed for tests and tools too, per the
		// comment above, so a caller can force a refresh without a full Save().
		void SyncBuiltInShaders();

		// Inline in the header because it is a single flag read on the asset
		// resolution path. Note this is deliberately FALSE for a detached asset
		// root -- see AttachAssetRoot below.
		bool IsOpen() const { return m_Open; }

		// Point asset resolution at `rootWithAssets` (a directory CONTAINING
		// an "assets" subdirectory) without requiring a project.wasteland.
		//
		// This is what Bootstrap's last-resort branch does internally, made
		// public because the need is real beyond bootstrap: a tool, a test,
		// or a checkout that has assets but no descriptor still needs to
		// resolve "trees/EnemyChase.wlbtree" somewhere.
		//
		// `IsOpen()` stays FALSE -- this is a convenience asset root, not a
		// project -- so anything that gates on "a project is loaded" still
		// refuses, which is correct: there is no descriptor to save settings
		// into. Asset resolution is the only thing this changes.
		//
		// Rejects a directory with no assets subdirectory, so a typo cannot
		// silently resolve every asset path to somewhere empty.
		// The escape hatch that lets the editor run from a source checkout. It
		// relaxes the "must have a descriptor" rule in one direction only: asset
		// resolution changes, IsOpen() does not.
		//
		// DEFECT (design tension, documented not fixed): the header says a
		// directory with no assets subdirectory is rejected so a typo cannot
		// resolve everything into the void -- but Create() seeds a fresh project
		// from a caller-supplied seedAssets path, so a typo in THAT path still
		// produces a project with no shaders. The check exists on one entry point
		// and not the other.
		bool AttachAssetRoot(const std::filesystem::path &rootWithAssets,
							 std::string *outError = nullptr);

		// --- Identity and paths ---------------------------------------

		// Const reference, not by value: avoids a filesystem::path copy per call,
		// which matters on a per-asset resolution path. Safe because the reference
		// points into the singleton, which outlives any caller.
		const std::filesystem::path &GetRoot() const { return m_Root; }
		const std::filesystem::path &GetAssetRoot() const { return m_AssetRoot; }
		const std::filesystem::path &GetProjectFile() const { return m_ProjectFile; }
		const std::string &GetName() const { return m_Settings.Name; }

		// Overload pair rather than a defaulted or templated accessor: the non-const
		// one lets Bootstrap/editor code write settings directly and then call
		// Save(), instead of a Set/Apply round-trip per field.
		//
		// DEFECT (encapsulation leak, documented not fixed): returning a mutable
		// reference means nothing enforces the Version bump or the QualityPreset
		// vocabulary. A caller can set QualityPreset to a nonsense string and
		// Save() will happily write it.
		Settings &GetSettings() { return m_Settings; }
		const Settings &GetSettings() const { return m_Settings; }

		// Absolute path for a project-relative string. The argument may be
		// either project-relative ("textures/x.png") or asset-relative
		// ("assets/textures/x.png") — both resolve identically.
		// By value: the result is a temporary path the caller usually appends to,
		// and returning a reference to a temporary member computed inside would
		// dangle. The double-acceptance of project-relative and asset-relative
		// input is what saves every caller from getting the prefix wrong.
		std::filesystem::path GetAssetPath(const std::string &relative) const;

		// Absolute path for a project-relative string, without the implied
		// "assets/" prefix.
		std::filesystem::path GetProjectPath(const std::string &relative) const;

		// Absolute -> project-relative, with forward slashes. Returns an
		// empty string when the path lies outside the project, so callers
		// can detect a foreign path instead of silently writing a broken
		// reference.
		// The inverse, and the one with a defined failure: an empty string for a
		// path outside the project. Forward slashes are forced regardless of
		// platform so a saved reference is portable between a Windows editor and
		// a Linux build server.
		std::string MakeProjectRelative(const std::filesystem::path &absolute) const;

		// --- Well-known directories -----------------------------------

		// The user's Documents folder. Windows: the real known folder, so
		// OneDrive-redirected Documents is respected. Linux/macOS:
		// $HOME/Documents.
		// The real Windows known folder rather than a hardcoded path, so a
		// OneDrive-redirected Documents is respected. That matters because
		// GetUserProjectsDirectory() is built on top of this.
		static std::filesystem::path GetUserDocumentsDirectory();

		// Documents/Wasteland Projects — where Create() puts new projects.
		static std::filesystem::path GetUserProjectsDirectory();

		// %APPDATA%/Wasteland (or ~/.config/Wasteland) — engine-level
		// state that belongs to the user, not to any one project: the
		// recent-projects list and API keys.
		// Deliberately NOT under the project: recent-projects and API keys belong
		// to the user, not to any one project, so switching projects must not lose
		// them and deleting a project must not take them with it.
		static std::filesystem::path GetUserConfigDirectory();

		// The engine's own "assets" directory — the built-in shaders that
		// ship with the editor. Located relative to the running executable
		// and remembered by Bootstrap, so new projects can be seeded with
		// a working set of shaders instead of an empty directory.
		//
		// Empty when the engine assets could not be found, which is not
		// fatal: projects still create, they just start without built-in
		// shaders.
		// Returned by const reference to a remembered static, so repeated calls in
		// a seeding loop do not copy a path each time.
		//
		// DEFECT (lifetime hazard, documented not fixed): this hands out a
		// reference into the singleton while SetEngineAssetsRoot() below can
		// reassign it. A caller holding the reference across a Set gets a
		// dangling path. The const-ness of the accessor makes it look safe.
		static const std::filesystem::path &GetEngineAssetsRoot();

		// Overrides the remembered engine assets root. For tests and for
		// tools that need to seed from a known location.
		// Test seam. Because GetEngineAssetsRoot returns a reference into the same
		// storage, this is a global mutation with no scoping -- a test must restore
		// the previous value or later tests inherit the override.
		static void SetEngineAssetsRoot(const std::filesystem::path &root);

		// --- Recent projects -------------------------------------------

		// Most-recent-first list of absolute project.wasteland paths.
		// By value, unlike the engine-assets-root getter: this list is read from
		// config on every call and mutating it in place would let a caller corrupt
		// the persisted state without going through the setters.
		static std::vector<std::string> GetRecentProjects();
		// Takes a project.wasteland path, not a root directory -- the descriptor
		// is what identifies a project, and a root alone could point at a
		// directory that was never opened successfully.
		static void AddRecentProject(const std::filesystem::path &projectFile);
		static void ClearRecentProjects();

		// --- Bootstrap --------------------------------------------------

		// Selects and opens the project to use for this run, in order:
		//   1. explicitPath, if non-empty (--project on the command line)
		//   2. the WASTELAND_PROJECT environment variable
		//   3. the most recent entry that still exists on disk
		//   4. a directory containing "assets" near the executable
		// Steps 1-3 open a real project. Step 4 attaches a detached root
		// with no descriptor, which is how the editor keeps working from a
		// source checkout.
		// Returns true when a real project was opened.
		// The four-step ladder in the comment above is an ordering decision, and
		// the return value only says whether step 1-3 happened. A caller cannot
		// distinguish "attached a detached root" from "opened the real project"
		// from this bool alone -- it has to call IsOpen() to find out.
		//
		// Steps 3 and 4 are existence-checked (the comment says "that still
		// exists on disk" and "a directory containing assets"), so the two
		// fallbacks cannot silently attach to nothing.
		static bool Bootstrap(const std::string &explicitPath, const std::filesystem::path &exePath);

		// Strips characters that are illegal in a directory name and
		// collapses whitespace runs. Returns "Untitled" if nothing usable
		// is left.
		// Static and public because it is needed BEFORE a Project exists, to
		// turn a typed name into a directory name. Returning "Untitled" rather
		// than an empty string keeps the Create() path from producing a folder
		// literally named "".
		static std::string SanitizeName(const std::string &name);

		// Creates the standard asset subdirectory layout under root.
		// Public because it is idempotent and useful on its own: opening an
		// existing project that predates a new subdirectory can repair it without
		// a full create.
		static bool EnsureDirectoryLayout(const std::filesystem::path &root, std::string *outError = nullptr);

	private:
		// Defaulted, not deleted: the singleton in Get() needs to be constructible,
		// and defaulted leaves the class with the aggregate-ish value semantics the
		// in-class member defaults already provide.
		Project() = default;

		// Writes a project.wasteland. customize, when provided, is applied
		// to `base` just before the settings are emitted -- used by Create()
		// to name a project before it has ever been opened. Static because
		// Create() is static and must write a descriptor for a project that
		// does not exist yet.
		// customize is a callback rather than a Settings value because Create()
		// wants to name the project before anything has been opened -- there is no
		// instance to call a non-static method on at that point, which is why this
		// is static.
		static bool WriteDescriptor(const std::filesystem::path &file,
									const Settings &base,
									const std::function<void(Settings &)> &customize = nullptr);

		// Points the project at an existing directory without a descriptor.
		// Used by the last-resort branch of Bootstrap.
		// The private twin of the public AttachAssetRoot. The split looks like
		// duplication; it exists because the public one validates and reports an
		// error while this one is the internal no-failure step Bootstrap takes
		// once it has already established the directory is good.
		void AttachDetached(const std::filesystem::path &rootWithAssets);

		// The one place that sets m_Open, m_Root, m_AssetRoot and m_ProjectFile
		// together. Having a single activation path is what keeps those four
		// fields from ever disagreeing with each other.
		void Activate(const std::filesystem::path &root, const std::filesystem::path &projectFile);

	private:
		// All members default-initialised so a default-constructed Project is in a
		// defined state. m_Open is the authority: it is what IsOpen() reports and
		// what distinguishes a real project from a detached asset root, even
		// though both populate the same path fields.
		bool m_Open = false;
		// Root is the project directory; m_AssetRoot is root/assets. Storing both
		// rather than deriving one keeps GetAssetPath a cheap concatenation -- the
		// alternative recomputes "assets" on every asset lookup.
		std::filesystem::path m_Root;
		std::filesystem::path m_AssetRoot;
		// Empty for a detached root: there is no descriptor, which is the other
		// half of why IsOpen() is false in that case.
		std::filesystem::path m_ProjectFile;
		// The in-memory settings, which exist even with no project open -- they
		// then hold the defaults. Save() is documented as a no-op in that state,
		// so the defaults can never be written to disk by accident.
		Settings m_Settings;
	};

} // namespace Wasteland
