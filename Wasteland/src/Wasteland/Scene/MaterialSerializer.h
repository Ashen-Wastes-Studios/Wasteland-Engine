// MaterialSerializer.h -- the authored .wlmaterial asset.
//
// WHY A SEPARATE FILE
// -------------------
// A .wlmaterial is one MaterialComponent, whereas SceneSerializer's
// MaterialComponent block is one component AMONG THIRTY inside an entity. The
// two share field names on purpose -- a material authored in the editor and a
// material imported from glTF should read back identically -- but they are
// different documents with different failure modes, and folding one into the
// other would mean a scene save and a material save competing for the same
// code.
//
// WHAT IT DOES NOT STORE
// ----------------------
// MaterialComponent::Texture is a live GPU object (Ref<Texture2D>) and cannot
// be serialized. Only TexturePath is written; Load recreates the texture if the
// file exists, exactly as SceneSerializer does. A material whose texture is
// missing on disk loads with the path intact and a null texture, so it renders
// with its flat Albedo rather than failing.
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Wasteland/Scene/Components.h"

namespace Wasteland
{

	// Reads and writes .wlmaterial YAML. All methods are static and stateless:
	// a material file is a pure function of a MaterialComponent, so there is
	// nothing for an instance to hold.
	class MaterialSerializer
	{
	public:
		// Writes the material to `path`. Creates parent directories.
		// Returns false and fills `error` rather than throwing: a material save
		// is a user-facing action and a full disk is user data, not a bug.
		static bool Save(const std::filesystem::path &path,
						 const MaterialComponent &material,
						 std::string *error = nullptr);

		// Reads a material. Missing fields keep the component's existing value
		// rather than being reset: a file authored before a field existed must
		// load with that field's default, not zero.
		static bool Load(const std::filesystem::path &path,
						 MaterialComponent &material,
						 std::string *error = nullptr);

		// The YAML text for a material, without the filesystem. Exists so the
		// format can be unit-tested headlessly and so a tool can preview a save.
		static std::string Emit(const MaterialComponent &material);

		// The inverse of Emit, for the same reason.
		static bool Parse(const std::string &text,
						  MaterialComponent &material,
						  std::string &error);
	};

	// Loads and caches .wlmaterial assets, for MeshComponent::MaterialPath.
	//
	// A material override is read every frame for every mesh entity; parsing
	// YAML per frame would be an unbounded cost. Mirrors MeshAssetCache,
	// including caching FAILURES with their reason -- a missing file re-opened
	// every frame is the same bug in a smaller file.
	class MaterialAssetCache
	{
	public:
		// Returns the material for a path, loading it on first use. nullptr when
		// the file is missing or unparseable; `outError` then carries the reason
		// from the first attempt, not a fresh parse.
		const MaterialComponent *Get(const std::string &path,
									 std::string *outError = nullptr);

		void Clear();

		size_t Count() const { return m_Materials.size(); }

	private:
		std::unordered_map<std::string, size_t> m_Index;
		std::vector<MaterialComponent> m_Materials;
		std::vector<std::string> m_Errors;
	};

} // namespace Wasteland
