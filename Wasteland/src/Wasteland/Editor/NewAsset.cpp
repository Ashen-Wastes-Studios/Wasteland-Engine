#include "wlpch.h"

#include "Wasteland/Editor/NewAsset.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "Wasteland/Animation/AnimationSerializer.h"
#include "Wasteland/Avb/Graph/Graph.h"
#include "Wasteland/Avb/Graph/GraphSerializer.h"
#include "Wasteland/Mesh/MeshSerializer.h"
#include "Wasteland/Scene/MaterialSerializer.h"
#include "Wasteland/VisualBlocks/Graph.h"
#include "Wasteland/VisualBlocks/GraphSerializer.h"

namespace Wasteland::Editor
{
	namespace
	{
		std::string ExtensionOf(const std::filesystem::path &path)
		{
			std::string extension = path.extension().string();
			if (!extension.empty() && extension[0] == '.')
				extension.erase(extension.begin());
			std::transform(extension.begin(), extension.end(), extension.begin(),
						   [](unsigned char c)
						   {
							   return static_cast<char>(std::tolower(c));
						   });
			return extension;
		}

		std::string StemOf(const std::filesystem::path &path)
		{
			return path.stem().string();
		}

		// A default-constructed Visual Blocks graph. `Kind` is the only field
		// that decides whether the file is a flow graph (.wlblocks) or a
		// behaviour tree (.wlbtree) -- the same serializer writes both, and the
		// reader dispatches on the same key.
		VisualBlocks::Graph MakeGraph(const std::string &kind, const std::string &name)
		{
			VisualBlocks::Graph graph;
			graph.Kind = kind;
			graph.Name = name;
			return graph;
		}

		// The audio graph (.wlavb) is a different type with a different
		// serializer, but the same contract: an empty graph with a name.
		Avb::AvbGraph MakeAvbGraph(const std::string &name)
		{
			Avb::AvbGraph graph;
			graph.Name = name;
			return graph;
		}
	}

	bool CreateEmptyAsset(const std::string &absolutePath,
						  const std::string &assetName, std::string &error)
	{
		const std::filesystem::path path(absolutePath);
		const std::string extension = ExtensionOf(path);
		const std::string name = assetName.empty() ? StemOf(path) : assetName;

		std::error_code code;
		if (const std::filesystem::path parent = path.parent_path();
			!parent.empty())
		{
			std::filesystem::create_directories(parent, code);
			if (code)
			{
				error = "cannot create " + parent.string() + ": " + code.message();
				return false;
			}
		}

		if (extension == "wlblocks")
		{
			VisualBlocks::GraphError graphError;
			if (!VisualBlocks::GraphSerializer::Save(
					absolutePath, MakeGraph("flow", name), graphError))
			{
				error = graphError.IsError ? graphError.Message
										  : std::string("graph save failed");
				return false;
			}
			return true;
		}

		if (extension == "wlbtree")
		{
			VisualBlocks::GraphError graphError;
			if (!VisualBlocks::GraphSerializer::Save(
					absolutePath, MakeGraph("tree", name), graphError))
			{
				error = graphError.IsError ? graphError.Message
										  : std::string("tree save failed");
				return false;
			}
			return true;
		}

		if (extension == "wlavb")
		{
			Avb::AvbGraphError graphError;
			if (!Avb::AvbGraphSerializer::Save(absolutePath, MakeAvbGraph(name),
											  graphError))
			{
				// AvbGraphError carries a Message but no IsError flag: its
				// failure is expressed by the bool return alone, unlike the
				// Visual Blocks GraphError which also sets a flag.
				error = graphError.Message.empty()
							? std::string("audio graph save failed")
							: graphError.Message;
				return false;
			}
			return true;
		}

		if (extension == "wlanim")
		{
			Animation::AnimationAsset asset;
			asset.Name = name;
			Animation::AnimationError animationError;
			if (!Animation::AnimationSerializer::Save(absolutePath, asset,
													  animationError))
			{
				error = animationError.IsError ? animationError.Message
											  : std::string("animation save failed");
				return false;
			}
			return true;
		}

		if (extension == "wlmesh")
		{
			// MeshAsset has no Name field -- the mesh format carries geometry
			// and material slots, not a display name. The name lives in the
			// file name, which is what the browser shows anyway.
			MeshAsset asset;
			std::string meshError;
			if (!MeshSerializer::Save(absolutePath, asset, false, &meshError))
			{
				error = meshError.empty() ? std::string("mesh save failed") : meshError;
				return false;
			}
			return true;
		}

		if (extension == "wlmaterial")
		{
			MaterialComponent material;
			std::string materialError;
			if (!MaterialSerializer::Save(absolutePath, material, &materialError))
			{
				error = materialError.empty() ? std::string("material save failed")
											 : materialError;
				return false;
			}
			return true;
		}

		error = "unsupported asset type: ." + extension;
		return false;
	}
}
