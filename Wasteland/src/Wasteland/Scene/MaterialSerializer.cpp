#include "wlpch.h"

#include "Wasteland/Scene/MaterialSerializer.h"

#include <yaml-cpp/yaml.h>
// The shared glm -> Emitter overloads, without which `out << material.Albedo`
// does not compile here -- they used to live in SceneSerializer.cpp.
#include "Wasteland/Scene/YamlGlm.h"

// Texture2D::Create: Load recreates the material's texture from TexturePath,
// exactly as SceneSerializer's MaterialComponent block does.
#include "Wasteland/Renderer/Texture.h"

#include <fstream>
#include <sstream>

namespace Wasteland
{
	namespace
	{
		// The field names are IDENTICAL to SceneSerializer's MaterialComponent
		// block, deliberately: a material authored in the editor and a material
		// embedded in a scene should read back with the same rules, and two
		// spellings of the same field would mean a scene that saves a material
		// one way and reloads it another.
		//
		// TextureWidth/Height are NOT written. SceneSerializer emits them for
		// historical reasons and no reader consumes them -- a documented defect
		// there. Repeating a defect in a new format would be the worst outcome.
		void WriteMaterial(YAML::Emitter &out, const MaterialComponent &material)
		{
			out << YAML::Key << "Albedo" << YAML::Value << material.Albedo;
			out << YAML::Key << "Metallic" << YAML::Value << material.Metallic;
			out << YAML::Key << "Roughness" << YAML::Value << material.Roughness;
			out << YAML::Key << "NormalStrength" << YAML::Value << material.NormalStrength;
			out << YAML::Key << "EmissionColor" << YAML::Value << material.EmissionColor;
			out << YAML::Key << "EmissionIntensity" << YAML::Value << material.EmissionIntensity;
			out << YAML::Key << "DisplacementScale" << YAML::Value << material.DisplacementScale;
			out << YAML::Key << "TexturePath" << YAML::Value << material.TexturePath;
		}

		// Every field presence-checked. A .wlmaterial authored before a field
		// existed must load with that field's DEFAULT, not zero -- a material
		// that silently resets its roughness to 0 is shiny in a way the author
		// never chose.
		void ReadMaterial(const YAML::Node &node, MaterialComponent &material)
		{
			if (node["Albedo"])
				material.Albedo = node["Albedo"].as<glm::vec4>();
			if (node["Metallic"])
				material.Metallic = node["Metallic"].as<float>();
			if (node["Roughness"])
				material.Roughness = node["Roughness"].as<float>();
			if (node["NormalStrength"])
				material.NormalStrength = node["NormalStrength"].as<float>();
			if (node["EmissionColor"])
				material.EmissionColor = node["EmissionColor"].as<glm::vec4>();
			if (node["EmissionIntensity"])
				material.EmissionIntensity = node["EmissionIntensity"].as<float>();
			if (node["DisplacementScale"])
				material.DisplacementScale = node["DisplacementScale"].as<float>();
			if (node["TexturePath"])
				material.TexturePath = node["TexturePath"].as<std::string>();
		}
	}

	std::string MaterialSerializer::Emit(const MaterialComponent &material)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		WriteMaterial(out, material);
		out << YAML::EndMap;
		return out.c_str() ? std::string(out.c_str()) : std::string();
	}

	bool MaterialSerializer::Parse(const std::string &text,
								  MaterialComponent &material,
								  std::string &error)
	{
		YAML::Node root;
		try
		{
			root = YAML::Load(text);
		}
		catch (const YAML::Exception &exception)
		{
			error = "material YAML: " + std::string(exception.what());
			return false;
		}

		if (!root || !root.IsMap())
		{
			error = "material file is empty or not a mapping";
			return false;
		}

		try
		{
			ReadMaterial(root, material);
		}
		catch (const YAML::Exception &exception)
		{
			// A field with the WRONG TYPE throws here. Reported rather than
			// skipped: a material that loaded with a silently defaulted colour
			// is worse than one that failed to load.
			error = "material field: " + std::string(exception.what());
			return false;
		}

		return true;
	}

	bool MaterialSerializer::Save(const std::filesystem::path &path,
								  const MaterialComponent &material,
								  std::string *error)
	{
		std::error_code directoryError;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), directoryError);
		// A failed create_directories is not fatal: the parent may already
		// exist, and the ofstream below is the real test.

		std::ofstream file(path, std::ios::trunc);
		if (!file.is_open())
		{
			if (error)
				*error = "material file could not be opened for writing: " + path.string();
			return false;
		}

		file << Emit(material);
		file.flush();
		if (!file.good())
		{
			if (error)
				*error = "material file write failed: " + path.string();
			return false;
		}
		return true;
	}

	bool MaterialSerializer::Load(const std::filesystem::path &path,
								  MaterialComponent &material,
								  std::string *error)
	{
		std::ifstream file(path);
		if (!file.is_open())
		{
			if (error)
				*error = "material file could not be opened: " + path.string();
			return false;
		}

		std::ostringstream buffer;
		buffer << file.rdbuf();

		std::string parseError;
		if (!Parse(buffer.str(), material, parseError))
		{
			if (error)
				*error = parseError;
			return false;
		}

		// The texture is recreated ONLY if the file exists, matching
		// SceneSerializer's MaterialComponent block. A material with a missing
		// texture renders with its flat Albedo rather than failing the load --
		// a moved asset should not make a whole level unloadable.
		if (!material.TexturePath.empty() &&
			std::filesystem::is_regular_file(material.TexturePath))
		{
			material.Texture = Texture2D::Create(material.TexturePath);
			material.HasGeneratedMaps = true;
		}
		else
		{
			material.Texture = nullptr;
			material.HasGeneratedMaps = false;
		}

		return true;
	}

	const MaterialComponent *MaterialAssetCache::Get(const std::string &path,
													  std::string *outError)
	{
		// An EMPTY path means "no override" -- use the entity's own material.
		// It is the common case (MeshComponent is added before the asset is
		// chosen) and must not be a load of whatever "" resolves to.
		if (path.empty())
		{
			if (outError)
				outError->clear();
			return nullptr;
		}

		const auto found = m_Index.find(path);
		if (found != m_Index.end())
		{
			const size_t index = found->second;
			if (!m_Errors[index].empty())
			{
				if (outError)
					*outError = m_Errors[index];
				return nullptr;
			}
			if (outError)
				outError->clear();
			return &m_Materials[index];
		}

		MaterialComponent material;
		std::string error;
		const bool loaded = MaterialSerializer::Load(path, material, &error);

		const size_t index = m_Materials.size();
		m_Index.emplace(path, index);
		m_Materials.push_back(std::move(material));
		m_Errors.push_back(loaded ? std::string() : error);

		if (!loaded)
		{
			if (outError)
				*outError = error;
			return nullptr;
		}
		if (outError)
			outError->clear();
		return &m_Materials[index];
	}

	void MaterialAssetCache::Clear()
	{
		m_Index.clear();
		m_Materials.clear();
		m_Errors.clear();
	}

} // namespace Wasteland