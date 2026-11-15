#include "wlpch.h"

#include "Wasteland/Mesh/MeshSerializer.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <utility>

namespace Wasteland
{
	namespace
	{
		// --- small readers --------------------------------------------------

		// A node for an ABSENT key is a ZOMBIE in yaml-cpp, and IsSequence() on a
		// zombie THROWS InvalidNode rather than returning false -- the throw
		// escapes Parse and surfaces from Python as a bare RuntimeError with no
		// indication of which field was missing. Every presence test must go
		// through IsDefined() first. (`if (node["x"])` is safe, because
		// operator bool reads IsDefined() without throwing; it is the typed
		// predicates that do not.)
		bool IsPresentSequence(const YAML::Node &node)
		{
			return node.IsDefined() && node.IsSequence();
		}

		// True when the node is a scalar that YAML would read as an INTEGER.
		//
		// yaml-cpp's own convert<int> streams the scalar into an int and does not
		// require the stream to be fully consumed, so `1.5` decodes as `1` with
		// no error. For a triangle index that is a silent truncation of the
		// geometry, so the text is checked directly instead.
		bool IsIntegerScalar(const YAML::Node &node)
		{
			if (!node.IsScalar())
				return false;
			const std::string text = node.Scalar();
			size_t index = 0;
			if (index < text.size() && (text[index] == '-' || text[index] == '+'))
				++index;
			if (index >= text.size())
				return false;
			for (; index < text.size(); ++index)
			{
				if (text[index] < '0' || text[index] > '9')
					return false;
			}
			return true;
		}

		bool ReadFloat(const YAML::Node &node, const std::string &what, float &out,
					   std::string &error)
		{
			if (!node.IsScalar())
			{
				error = what + " is not a number";
				return false;
			}
			try
			{
				out = node.as<float>();
			}
			catch (const YAML::Exception &)
			{
				error = what + " is not a number";
				return false;
			}
			return true;
		}

		bool ReadVec3(const YAML::Node &node, const std::string &what, float out[3],
					  std::string &error)
		{
			if (!node.IsSequence() || node.size() != 3)
			{
				error = what + " must be a 3-element [x, y, z] sequence";
				return false;
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!ReadFloat(node[axis], what + "[" + std::to_string(axis) + "]",
							   out[axis], error))
					return false;
			}
			return true;
		}

		bool ReadVec2(const YAML::Node &node, const std::string &what, float out[2],
					  std::string &error)
		{
			if (!node.IsSequence() || node.size() != 2)
			{
				error = what + " must be a 2-element [u, v] sequence";
				return false;
			}
			for (int axis = 0; axis < 2; ++axis)
			{
				if (!ReadFloat(node[axis], what + "[" + std::to_string(axis) + "]",
							   out[axis], error))
					return false;
			}
			return true;
		}

		// --- emitters -------------------------------------------------------

		// The four geometry keys every mesh carries, at the current map level.
		// Shared by the top-level asset and by each sculpt_history entry so the
		// two cannot drift into different spellings of the same section.
		//
		// Returns false when a face's loop is broken and cannot be written as a
		// triangle. Emitting a short index list instead would produce a file
		// that LOADS as a different mesh, which is the one outcome a serializer
		// must never produce.
		bool EmitGeometry(YAML::Emitter &out, const EditableMesh &mesh)
		{
			out << YAML::Key << "vertices" << YAML::Value << YAML::Block
				<< YAML::BeginSeq;
			for (const EditableMesh::Vertex &vertex : mesh.Vertices)
			{
				out << YAML::Flow << YAML::BeginSeq << vertex.Position[0]
					<< vertex.Position[1] << vertex.Position[2] << YAML::EndSeq;
			}
			out << YAML::EndSeq;

			// Normals and UVs are written only when they are non-trivial. A mesh
			// whose normals are all the +Y default and whose UVs are all zero is
			// a mesh that has not been shaded or unwrapped, and writing 20,000
			// identical triplets to say so is noise. They are recomputed on load
			// (normals) or left at zero (UVs), which is what an absent section
			// already means.
			bool anyNormal = false;
			for (const EditableMesh::Vertex &vertex : mesh.Vertices)
			{
				if (vertex.Normal[0] != 0.0f || vertex.Normal[1] != 1.0f
					|| vertex.Normal[2] != 0.0f)
				{
					anyNormal = true;
					break;
				}
			}
			if (anyNormal)
			{
				out << YAML::Key << "normals" << YAML::Value << YAML::Block
					<< YAML::BeginSeq;
				for (const EditableMesh::Vertex &vertex : mesh.Vertices)
				{
					out << YAML::Flow << YAML::BeginSeq << vertex.Normal[0]
						<< vertex.Normal[1] << vertex.Normal[2] << YAML::EndSeq;
				}
				out << YAML::EndSeq;
			}

			bool anyUV = false;
			for (const EditableMesh::Vertex &vertex : mesh.Vertices)
			{
				if (vertex.TexCoord[0] != 0.0f || vertex.TexCoord[1] != 0.0f)
				{
					anyUV = true;
					break;
				}
			}
			if (anyUV)
			{
				out << YAML::Key << "uvs" << YAML::Value << YAML::Block
					<< YAML::BeginSeq;
				for (const EditableMesh::Vertex &vertex : mesh.Vertices)
				{
					out << YAML::Flow << YAML::BeginSeq << vertex.TexCoord[0]
						<< vertex.TexCoord[1] << YAML::EndSeq;
				}
				out << YAML::EndSeq;
			}

			out << YAML::Key << "triangles" << YAML::Value << YAML::Block
				<< YAML::BeginSeq;
			int32_t corners[3] = {0, 0, 0};
			for (int32_t face = 0; face < (int32_t)mesh.Faces.size(); ++face)
			{
				if (!mesh.GetFaceCorners(face, corners))
					return false;
				out << corners[0] << corners[1] << corners[2];
			}
			out << YAML::EndSeq;
			return true;
		}

		// --- readers ------------------------------------------------------------

		// Builds an EditableMesh from the four geometry keys.
		//
		// The file is INDEXED and EditableMesh is position-welded, so the path is
		// de-index -> Weld, exactly as OBJ and glTF import do it. That makes the
		// format's loader the same code path as every other mesh that enters the
		// engine, and it means a .wlmesh written by this build round-trips
		// exactly: an EditableMesh has unique positions, so nothing merges.
		bool ReadGeometry(const YAML::Node &node, EditableMesh &mesh,
						  std::string &error)
		{
			if (!node || !node.IsMap())
			{
				error = "mesh section is not a mapping";
				return false;
			}

			const YAML::Node &vertices = node["vertices"];
			if (!IsPresentSequence(vertices))
			{
				error = "vertices is missing or is not a sequence";
				return false;
			}

			std::vector<std::array<float, 3>> positionsStorage;
			positionsStorage.reserve(vertices.size());
			for (size_t index = 0; index < vertices.size(); ++index)
			{
				std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
				if (!ReadVec3(vertices[index],
							  "vertices[" + std::to_string(index) + "]",
							  position.data(), error))
					return false;
				positionsStorage.push_back(position);
			}

			// A position-welded mesh cannot represent two vertices at the same
			// position. Weld would merge them and keep the first-seen normal, so
			// a hard edge would silently become a smooth one. Refused with the
			// count rather than merged: the alternative is a file that loads and
			// does not contain the mesh the author saved.
			{
				std::vector<std::array<float, 3>> sorted = positionsStorage;
				std::sort(sorted.begin(), sorted.end());
				const size_t duplicates =
					static_cast<size_t>(std::unique(sorted.begin(), sorted.end())
										- sorted.begin());
				if (duplicates != sorted.size())
				{
					error = "vertices contains "
							+ std::to_string(sorted.size() - duplicates)
							+ " duplicate position(s); an EditableMesh is "
							  "position-welded and cannot represent them";
					return false;
				}
			}

			std::vector<std::array<float, 3>> normals;
			if (const YAML::Node &normalNode = node["normals"])
			{
				if (!normalNode.IsSequence())
				{
					error = "normals is not a sequence";
					return false;
				}
				if (normalNode.size() != vertices.size())
				{
					error = "normals has " + std::to_string(normalNode.size())
							+ " entries for " + std::to_string(vertices.size())
							+ " vertices";
					return false;
				}
				normals.reserve(normalNode.size());
				for (size_t index = 0; index < normalNode.size(); ++index)
				{
					std::array<float, 3> normal = {0.0f, 1.0f, 0.0f};
					if (!ReadVec3(normalNode[index],
								  "normals[" + std::to_string(index) + "]",
								  normal.data(), error))
						return false;
					normals.push_back(normal);
				}
			}

			std::vector<std::array<float, 2>> uvs;
			if (const YAML::Node &uvNode = node["uvs"])
			{
				if (!uvNode.IsSequence())
				{
					error = "uvs is not a sequence";
					return false;
				}
				if (uvNode.size() != vertices.size())
				{
					error = "uvs has " + std::to_string(uvNode.size())
							+ " entries for " + std::to_string(vertices.size())
							+ " vertices";
					return false;
				}
				uvs.reserve(uvNode.size());
				for (size_t index = 0; index < uvNode.size(); ++index)
				{
					std::array<float, 2> uv = {0.0f, 0.0f};
					if (!ReadVec2(uvNode[index], "uvs[" + std::to_string(index) + "]",
								  uv.data(), error))
						return false;
					uvs.push_back(uv);
				}
			}

			const YAML::Node &triangles = node["triangles"];
			if (!IsPresentSequence(triangles))
			{
				error = "triangles is missing or is not a sequence";
				return false;
			}
			if (triangles.size() % 3 != 0)
			{
				error = "triangles has " + std::to_string(triangles.size())
						+ " entries, which is not a multiple of 3";
				return false;
			}

			std::vector<int32_t> indices;
			indices.reserve(triangles.size());
			for (size_t index = 0; index < triangles.size(); ++index)
			{
				const YAML::Node &entry = triangles[index];
				if (!IsIntegerScalar(entry))
				{
					error = "triangles[" + std::to_string(index)
							+ "] is not an integer (got '" + entry.Scalar() + "')";
					return false;
				}
				const int32_t vertex = entry.as<int32_t>();
				if (vertex < 0 || vertex >= (int32_t)vertices.size())
				{
					error = "triangles[" + std::to_string(index) + "] indexes vertex "
							+ std::to_string(vertex) + " but there are only "
							+ std::to_string(vertices.size());
					return false;
				}
				indices.push_back(vertex);
			}

			// De-index into the renderer's shape and weld it back. Weld is what
			// builds the half-edge structure, so this is the one place the
			// format's topology is decided.
			//
			// `Indices` MUST be populated even though the corner mapping Weld
			// performs is `triangle*3 + k`: the TRIANGLE COUNT comes from
			// MeshData::GetTriangleCount(), which is Indices.size()/3. A
			// MeshData with vertices and an empty index list welds to a mesh
			// with every vertex and ZERO faces, and it does so without an error
			// -- which reads exactly like a format that silently dropped the
			// geometry.
			MeshData data;
			data.Vertices.resize(indices.size());
			data.Indices.resize(indices.size());
			for (size_t corner = 0; corner < indices.size(); ++corner)
			{
				const size_t vertex = (size_t)indices[corner];
				MeshVertex &target = data.Vertices[corner];
				data.Indices[corner] = (uint32_t)corner;
				target.Position[0] = positionsStorage[vertex][0];
				target.Position[1] = positionsStorage[vertex][1];
				target.Position[2] = positionsStorage[vertex][2];
				if (!normals.empty())
				{
					target.Normal[0] = normals[vertex][0];
					target.Normal[1] = normals[vertex][1];
					target.Normal[2] = normals[vertex][2];
				}
				if (!uvs.empty())
				{
					target.TexCoord[0] = uvs[vertex][0];
					target.TexCoord[1] = uvs[vertex][1];
				}
			}

			mesh = EditableMesh::Weld(data);
			return true;
		}
	}

	std::string MeshSerializer::Emit(const MeshAsset &asset, bool includeHistory)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;

		out << YAML::Key << "version" << YAML::Value << asset.Version;

		if (!EmitGeometry(out, asset.Mesh))
			return std::string();

		if (!asset.Materials.empty())
		{
			out << YAML::Key << "materials" << YAML::Value << YAML::Block
				<< YAML::BeginSeq;
			for (const std::string &material : asset.Materials)
				out << material;
			out << YAML::EndSeq;
		}

		if (!asset.FaceMaterial.empty())
		{
			out << YAML::Key << "face_material" << YAML::Value << YAML::Block
				<< YAML::BeginSeq;
			for (const int32_t slot : asset.FaceMaterial)
				out << slot;
			out << YAML::EndSeq;
		}

		if (includeHistory && !asset.SculptHistory.empty())
		{
			out << YAML::Key << "sculpt_history" << YAML::Value << YAML::Block
				<< YAML::BeginSeq;
			for (const EditableMesh &snapshot : asset.SculptHistory)
			{
				out << YAML::BeginMap;
				if (!EmitGeometry(out, snapshot))
					return std::string();
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		out << YAML::EndMap;
		if (!out.good())
			return std::string();
		return out.c_str() ? std::string(out.c_str()) : std::string();
	}

	bool MeshSerializer::Parse(const std::string &text, MeshAsset &asset,
							   std::string &error)
	{
		YAML::Node root;
		try
		{
			root = YAML::Load(text);
		}
		catch (const YAML::Exception &exception)
		{
			error = "mesh YAML: " + std::string(exception.what());
			return false;
		}

		if (!root || !root.IsMap())
		{
			error = "mesh file is empty or not a mapping";
			return false;
		}

		MeshAsset parsed;

		const YAML::Node &version = root["version"];
		if (!version.IsDefined())
		{
			error = "the file has no version field";
			return false;
		}
		if (!IsIntegerScalar(version))
		{
			error = "version is not an integer";
			return false;
		}
		parsed.Version = version.as<int32_t>();
		if (parsed.Version != kSupportedVersion)
		{
			error = "unsupported .wlmesh version "
					+ std::to_string(parsed.Version) + " (this build reads version "
					+ std::to_string(kSupportedVersion) + ")";
			return false;
		}

		if (!ReadGeometry(root, parsed.Mesh, error))
			return false;

		if (const YAML::Node &materials = root["materials"])
		{
			if (!materials.IsSequence())
			{
				error = "materials is not a sequence";
				return false;
			}
			parsed.Materials.reserve(materials.size());
			for (size_t index = 0; index < materials.size(); ++index)
			{
				if (!materials[index].IsScalar())
				{
					error = "materials[" + std::to_string(index)
							+ "] is not a path";
					return false;
				}
				parsed.Materials.push_back(materials[index].as<std::string>());
			}
		}

		if (const YAML::Node &faceMaterial = root["face_material"])
		{
			if (!faceMaterial.IsSequence())
			{
				error = "face_material is not a sequence";
				return false;
			}
			const size_t triangleCount = parsed.Mesh.Faces.size();
			if (faceMaterial.size() != triangleCount)
			{
				error = "face_material has "
						+ std::to_string(faceMaterial.size())
						+ " entries for "
						+ std::to_string(triangleCount) + " triangles";
				return false;
			}
			parsed.FaceMaterial.reserve(faceMaterial.size());
			for (size_t index = 0; index < faceMaterial.size(); ++index)
			{
				if (!IsIntegerScalar(faceMaterial[index]))
				{
					error = "face_material[" + std::to_string(index)
							+ "] is not an integer";
					return false;
				}
				const int32_t slot = faceMaterial[index].as<int32_t>();
				if (slot < 0 || slot >= (int32_t)parsed.Materials.size())
				{
					error = "face_material[" + std::to_string(index)
							+ "] names slot " + std::to_string(slot)
							+ " but there are only "
							+ std::to_string(parsed.Materials.size())
							+ " material(s)";
					return false;
				}
				parsed.FaceMaterial.push_back(slot);
			}
		}

		if (const YAML::Node &history = root["sculpt_history"])
		{
			if (!history.IsSequence())
			{
				error = "sculpt_history is not a sequence";
				return false;
			}
			for (size_t index = 0; index < history.size(); ++index)
			{
				EditableMesh snapshot;
				if (!ReadGeometry(history[index], snapshot, error))
				{
					error = "sculpt_history[" + std::to_string(index) + "]: "
							+ error;
					return false;
				}
				parsed.SculptHistory.push_back(std::move(snapshot));
			}
		}

		asset = std::move(parsed);
		return true;
	}

	bool MeshSerializer::Save(const std::filesystem::path &path,
							  const MeshAsset &asset,
							  bool includeHistory,
							  std::string *error)
	{
		const std::string text = Emit(asset, includeHistory);
		if (text.empty())
		{
			if (error)
				*error = "the mesh could not be emitted (a face loop is broken)";
			return false;
		}

		std::error_code directoryError;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), directoryError);
		// A failed create_directories is not fatal: the parent may already
		// exist, and the ofstream below reports a genuine failure.

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			if (error)
				*error = "could not open '" + path.string() + "' for writing";
			return false;
		}
		file << text;
		file.close();
		if (!file)
		{
			if (error)
				*error = "could not write '" + path.string() + "'";
			return false;
		}
		if (error)
			error->clear();
		return true;
	}

	bool MeshSerializer::Load(const std::filesystem::path &path,
							  MeshAsset &asset,
							  std::string *error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			if (error)
				*error = "could not open '" + path.string() + "' for reading";
			return false;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (file.bad())
		{
			if (error)
				*error = "could not read '" + path.string() + "'";
			return false;
		}

		std::string parseError;
		if (!Parse(buffer.str(), asset, parseError))
		{
			if (error)
				*error = parseError;
			return false;
		}
		if (error)
			error->clear();
		return true;
	}

} // namespace Wasteland
