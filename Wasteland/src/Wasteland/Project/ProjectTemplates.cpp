#include "wlpch.h"

#include "Wasteland/Project/ProjectTemplates.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Wasteland/Mesh/Json.h"

namespace Wasteland
{
	namespace
	{
		std::string ReadFileText(const std::string &path, bool &ok)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open())
			{
				ok = false;
				return {};
			}
			std::string text((std::istreambuf_iterator<char>(file)),
							 std::istreambuf_iterator<char>());
			ok = true;
			return text;
		}

		bool ReadString(const JsonValue &root, const char *key, std::string &out)
		{
			const JsonValue *value = root.Find(key);
			if (!value)
				return false;
			return value->AsString(out);
		}

		// One template directory: the manifest is in the directory root, and the
		// seed content is the directory itself. A template with no manifest is not
		// a template -- it is a folder, and treating every folder as a template
		// would put the user's own asset directories in the dialog.
		bool ScanTemplateDirectory(const std::filesystem::path &directory,
								   bool builtIn, const std::string &engineVersion,
								   ProjectTemplate &out, std::string &error)
		{
			std::error_code code;
			if (!std::filesystem::is_directory(directory, code) || code)
				return false;

			const std::filesystem::path manifest = directory / ProjectTemplateRegistry::kManifestName;
			if (!std::filesystem::exists(manifest, code) || code)
				return false;

			bool readOk = false;
			const std::string text = ReadFileText(manifest.string(), readOk);
			if (!readOk)
			{
				error = "cannot read " + manifest.string();
				return false;
			}

			return ProjectTemplateRegistry::ParseManifest(text, directory.string(),
														  builtIn, out, error);
		}
	}

	const std::vector<ProjectTemplate> &ProjectTemplateRegistry::DeclaredBuiltIns()
	{
		// The plan's I.2 table. SeedPath is left EMPTY here: the seed CONTENT is
		// editor-side and does not exist yet, and a declared template with no seed
		// is visibly incomplete rather than silently pointing at a directory that
		// may not be the one the user expected.
		static const std::vector<ProjectTemplate> builtIns = {
			{ "empty", "Empty", "A bare project with shaders only.",
			  "Empty", "", "", "Scene", "0.1.0", true },
			{ "third_person", "Third Person",
			  "Ground plane, capsule character, follow camera and a Player script.",
			  "Games", "", "", "Scene", "0.1.0", true },
			{ "first_person", "First Person",
			  "Ground plane, a camera at eye height and a Player script with mouse-look.",
			  "Games", "", "", "Scene", "0.1.0", true },
			{ "top_down", "Top-Down",
			  "Orthographic camera, tileable ground and an 8-way Player script.",
			  "Games", "", "", "Scene", "0.1.0", true },
			{ "2d_platformer", "2D Platformer",
			  "Orthographic camera, sprite renderer, a tilemap stub and a Player script with jump and gravity.",
			  "Games", "", "", "Scene", "0.1.0", true },
			{ "animation_showcase", "Animation Showcase",
			  "A rigged character, a walk cycle on the timeline and a state machine.",
			  "Film", "", "", "Animation", "0.1.0", true },
			{ "modeling_starter", "Modeling Starter",
			  "A sculpt-friendly base mesh, a material and a three-point light rig.",
			  "Film", "", "", "Modeling", "0.1.0", true },
			{ "cinematic", "Cinematic",
			  "A camera track, a lit set and a post-process volume.",
			  "Film", "", "", "Review", "0.1.0", true },
		};
		return builtIns;
	}

	bool ProjectTemplateRegistry::ParseManifest(const std::string &text,
												const std::string &seedPath,
												bool builtIn, ProjectTemplate &out,
												std::string &error)
	{
		JsonValue root;
		if (!JsonParse(text, root, error))
			return false;
		if (!root.IsObject())
		{
			error = "the manifest is not a JSON object";
			return false;
		}

		ProjectTemplate parsed;
		parsed.BuiltIn = builtIn;
		parsed.SeedPath = seedPath;

		// Id and Name are required. A template with no id cannot be referenced by
		// a saved project or a default layout, and one with no name would show a
		// blank row in the grid.
		if (!ReadString(root, "id", parsed.Id) || parsed.Id.empty())
		{
			error = "the manifest has no id";
			return false;
		}
		if (!ReadString(root, "name", parsed.Name) || parsed.Name.empty())
		{
			error = "the manifest has no name";
			return false;
		}

		// The rest have defaults, so a template authored before a field existed
		// still loads -- the same rule MaterialSerializer::Load applies.
		parsed.Description = "No description.";
		ReadString(root, "description", parsed.Description);

		parsed.Category = "Games";
		ReadString(root, "category", parsed.Category);

		parsed.DefaultLayout = "Scene";
		ReadString(root, "defaultLayout", parsed.DefaultLayout);

		parsed.EngineVersion = "0.1.0";
		ReadString(root, "engineVersion", parsed.EngineVersion);

		std::string thumbnail;
		if (ReadString(root, "thumbnail", thumbnail) && !thumbnail.empty())
			parsed.ThumbnailPath =
				(std::filesystem::path(seedPath) / thumbnail).string();

		out = std::move(parsed);
		return true;
	}

	int ProjectTemplateRegistry::CompareVersions(const std::string &left,
												  const std::string &right)
	{
		auto component = [](const std::string &version, int index) -> int
		{
			int current = 0;
			int seen = 0;
			for (char c : version)
			{
				if (c == '.')
				{
					if (seen == index)
						return current;
					++seen;
					current = 0;
					continue;
				}
				if (c < '0' || c > '9')
					continue;   // a non-numeric component sorts as 0
				current = current * 10 + (c - '0');
			}
			return seen == index ? current : 0;
		};

		for (int index = 0; index < 3; ++index)
		{
			const int a = component(left, index);
			const int b = component(right, index);
			if (a != b)
				return a < b ? -1 : 1;
		}
		return 0;
	}

	std::vector<ProjectTemplate> ProjectTemplateRegistry::Discover() const
	{
		std::vector<ProjectTemplate> found;

		auto scanRoot = [&](const std::string &root, bool builtIn)
		{
			std::error_code code;
			const std::filesystem::path rootPath(root);
			if (!std::filesystem::is_directory(rootPath, code) || code)
				return;

			for (const auto &entry : std::filesystem::directory_iterator(rootPath, code))
			{
				if (code)
					break;
				if (!entry.is_directory(code) || code)
					continue;

				ProjectTemplate parsed;
				std::string error;
				if (!ScanTemplateDirectory(entry.path(), builtIn, m_EngineVersion,
										   parsed, error))
					continue;   // not a template, or a broken one: skip quietly

				// Version gate. The template is still returned, marked
				// incompatible, so the dialog can grey it out WITH the reason.
				if (!m_EngineVersion.empty() &&
					CompareVersions(parsed.EngineVersion, m_EngineVersion) > 0)
				{
					parsed.Compatible = false;
					parsed.IncompatibleReason =
						"Requires engine " + parsed.EngineVersion + " (running " +
						m_EngineVersion + ")";
				}

				found.push_back(std::move(parsed));
			}
		};

		for (const std::string &root : m_BuiltInRoots)
			scanRoot(root, true);
		if (!m_UserRoot.empty())
			scanRoot(m_UserRoot, false);

		// Stable order: category, then name. A directory listing order is
		// filesystem-dependent and would reshuffle the grid on every launch.
		std::sort(found.begin(), found.end(),
				  [](const ProjectTemplate &a, const ProjectTemplate &b)
				  {
					  if (a.Category != b.Category)
						  return a.Category < b.Category;
					  return a.Name < b.Name;
				  });

		return found;
	}

	namespace
	{
		// I.4 id generation: lowercase alphanumerics, runs of punctuation collapse
		// to a single hyphen.
		std::string Slugify(const std::string &name)
		{
			std::string slug;
			bool pendingHyphen = false;
			for (char c : name)
			{
				const bool kept = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
				if (kept || (c >= 'A' && c <= 'Z'))
				{
					if (pendingHyphen && !slug.empty())
						slug += '-';
					pendingHyphen = false;
					slug += kept ? c : static_cast<char>(c - 'A' + 'a');
				}
				else
				{
					pendingHyphen = true;
				}
			}
			if (slug.empty())
				slug = "template";
			return slug;
		}

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

		// Copy a tree, skipping the per-user thumbnail cache directory (Phase F.8)
		// and any compiled Python. Returns false on a copy error.
		bool CopyTreeExcluding(const std::filesystem::path &from,
		                       const std::filesystem::path &to)
		{
			std::error_code ec;
			std::filesystem::create_directories(to, ec);
			if (ec)
				return false;

			for (std::filesystem::recursive_directory_iterator it(from, ec), end;
			     it != end; it.increment(ec))
			{
				if (ec)
					return false;
				if (it->path().filename() == ".thumbnails")
				{
					it.disable_recursion_pending();
					continue;
				}
				if (it->is_directory())
					continue;
				if (it->path().extension() == ".pyc")
					continue;

				const std::filesystem::path dest =
				    to / it->path().lexically_relative(from);
				std::filesystem::create_directories(dest.parent_path(), ec);
				std::filesystem::copy_file(it->path(), dest,
				                           std::filesystem::copy_options::overwrite_existing,
				                           ec);
				if (ec)
					return false;
			}
			return true;
		}
	}

	bool SaveAsTemplate(const std::string &targetDirStr,
	                    const std::string &assetsDirStr,
	                    const std::string &name,
	                    const std::string &description,
	                    std::string &outManifestPath, std::string &error)
	{
		const std::filesystem::path targetDir(targetDirStr);

		std::error_code ec;
		std::filesystem::create_directories(targetDir, ec);
		if (ec)
		{
			error = "could not create the template directory " + targetDirStr + ": " +
			        ec.message();
			return false;
		}

		// Copy the assets. An EMPTY or missing assets dir is legal -- an empty
		// template is a real ("Empty") template. The manifest is written AFTER the
		// copy, so a copy that dies halfway leaves a directory with no manifest,
		// which the registry simply does not discover as a template.
		const std::filesystem::path assetsDir(assetsDirStr);
		if (!assetsDirStr.empty() && std::filesystem::exists(assetsDir, ec))
		{
			if (!CopyTreeExcluding(assetsDir, targetDir / "assets"))
			{
				error = "could not copy the project assets into the template";
				return false;
			}
		}

		// The manifest. id is derived from the name so a user never has to type an
		// id; everything else is a parse default (category "User" so it sorts apart
		// from the built-ins).
		std::string id;
		id = Slugify(name);
		const std::string manifest =
		    "{\"id\":\"" + JsonEscape(id) + "\",\"name\":\"" + JsonEscape(name) +
		    "\",\"description\":\"" + JsonEscape(description) +
		    "\",\"category\":\"User\"}";

		const std::filesystem::path manifestPath =
		    targetDir / ProjectTemplateRegistry::kManifestName;
		{
			std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "could not write " + manifestPath.string();
				return false;
			}
			out << manifest;
			if (!out.good())
			{
				error = "could not flush " + manifestPath.string();
				return false;
			}
		}

		outManifestPath = manifestPath.string();
		return true;
	}
}
