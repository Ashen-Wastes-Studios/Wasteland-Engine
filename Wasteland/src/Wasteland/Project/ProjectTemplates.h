#pragma once

// Project template registry (Task I.1).
//
// Lives in the ENGINE rather than in DemonCore-Editor/src/ as the plan specifies,
// for the same reason Phase F's thumbnail cache does: discovery, manifest
// parsing and version gating are pure file I/O plus JSON, and the plan's
// verification is a headless test. The template GRID is editor UI and stays
// there.
//
// A template is a SEED DIRECTORY plus a manifest. No new engine primitive is
// needed -- Project::Create(name, seedAssets, outError) already does the copy.
// The work here is the registry, the manifest format and the gating.

#include <string>
#include <vector>

namespace Wasteland
{
	struct ProjectTemplate
	{
		std::string Id;            // "third_person"
		std::string Name;          // "Third Person"
		std::string Description;
		std::string Category;      // "Games", "Empty", "Film"
		std::string SeedPath;      // absolute path to the seed directory
		std::string ThumbnailPath; // absolute path to a PNG
		std::string DefaultLayout; // "Scene"
		std::string EngineVersion; // minimum engine version
		bool BuiltIn = false;

		// Computed by the registry, not read from the manifest: a template whose
		// EngineVersion is newer than the running editor is shown greyed out with
		// the reason, NOT hidden. Hiding it would make a missing template
		// indistinguishable from a bug.
		bool Compatible = true;
		std::string IncompatibleReason;
	};

	class ProjectTemplateRegistry
	{
	public:
		static constexpr const char *kManifestName = "template.wasteland.json";

		void AddBuiltInRoot(const std::string &root) { m_BuiltInRoots.push_back(root); }
		void SetUserRoot(const std::string &root) { m_UserRoot = root; }
		void SetEngineVersion(const std::string &version) { m_EngineVersion = version; }
		const std::string &GetEngineVersion() const { return m_EngineVersion; }

		// Scan every root for a directory containing kManifestName. Returns the
		// templates sorted by category then name, so the grid's order is stable
		// across runs and platforms -- an unordered directory listing would
		// reshuffle the dialog on every launch.
		std::vector<ProjectTemplate> Discover() const;

		// The built-in template set as DECLARED, independent of what is on disk.
		// The dialog shows these before any seed assets ship, and a test can
		// assert the set without authoring eight directories.
		static const std::vector<ProjectTemplate> &DeclaredBuiltIns();

		// Parse a manifest from TEXT -- the seam a test drives, and the same code
		// path a loaded manifest takes. `seedPath` is the directory the manifest
		// was found in; it is not a field of the manifest because the manifest
		// lives in the directory it describes.
		static bool ParseManifest(const std::string &text, const std::string &seedPath,
								  bool builtIn, ProjectTemplate &out,
								  std::string &error);

		// Numeric major.minor.patch comparison. Returns <0, 0, >0. A malformed
		// component sorts as 0 rather than failing: "0.1" and "0.1.0" are the same
		// version to a user, and rejecting the file would be the worse outcome.
		static int CompareVersions(const std::string &left, const std::string &right);

	private:
		std::vector<std::string> m_BuiltInRoots;
		std::string m_UserRoot;
		std::string m_EngineVersion = "0.1.0";
	};

	// Task I.4: save a project's assets as a new USER template.
	//
	// Copies `assetsDir` into a fresh `targetDir` and writes a
	// template.wasteland.json manifest describing it, so the copied directory is
	// immediately discoverable by ProjectTemplateRegistry::Discover once its
	// parent is SetUserRoot. The per-user thumbnail cache (Phase F.8) and any
	// *.pyc are excluded -- the former is per-user, the latter is code; neither is
	// seed content.
	//
	// `outManifestPath` receives the absolute manifest path on success; `error` a
	// message on failure. Returns false + error on any failure. Safety: the
	// manifest is written LAST, so a copy that dies halfway leaves a directory
	// with NO manifest -- which the registry does not discover as a template. A
	// failed save must be judged by its return value, never by inspecting the
	// target.
	bool SaveAsTemplate(const std::string &targetDir,
	                    const std::string &assetsDir,
	                    const std::string &name,
	                    const std::string &description,
	                    std::string &outManifestPath, std::string &error);
}
