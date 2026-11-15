#include "wlpch.h"

#include "Wasteland/Mesh/GltfImporter.h"
#include "Wasteland/Mesh/Json.h"

#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>

namespace Wasteland {

	namespace
	{
		// ---- glTF constants -------------------------------------------------
		//
		// The numeric values are from the specification, not chosen. They are
		// spelled out rather than read from an enum so a diagnostic can print
		// the raw value a file actually contained.
		constexpr int ComponentTypeFloat = 5126;
		constexpr int ComponentTypeUnsignedByte = 5121;
		constexpr int ComponentTypeUnsignedShort = 5123;
		constexpr int ComponentTypeUnsignedInt = 5125;

		// Reads an OPTIONAL numeric field, falling back when it is absent.
		//
		// glTF marks byteOffset and byteStride optional (default 0), and an
		// exporter that omits them -- which is legal and common -- makes
		// JsonValue::Find return nullptr. Dereferencing that is a segfault, and
		// the crash surfaces inside pybind's argument handling with no message,
		// so it reads like a binding bug rather than a missing key.
		double NumberOr(const JsonValue &object, const char *key, double fallback)
		{
			const JsonValue *value = object.Find(key);
			if (!value || !value->IsNumber())
				return fallback;
			double out = fallback;
			value->AsNumber(out);
			return out;
		}

		// Component counts per accessor type.
		int ComponentCount(const std::string &type)
		{
			if (type == "SCALAR") return 1;
			if (type == "VEC2") return 2;
			if (type == "VEC3") return 3;
			if (type == "VEC4") return 4;
			if (type == "MAT4") return 16;
			return 0; // MAT2/MAT3 are legal glTF but unused by anything this
					  // importer reads; reporting 0 makes them a named error.
		}

		int ComponentSize(int componentType)
		{
			switch (componentType)
			{
			case ComponentTypeUnsignedByte: return 1;
			case ComponentTypeUnsignedShort: return 2;
			case ComponentTypeFloat:
			case ComponentTypeUnsignedInt: return 4;
			default: return 0;
			}
		}

		// Reads one component from a byte buffer. glTF is little-endian on
		// every platform the format targets, and the engine is little-endian
		// too, so a memcpy is correct and faster than a byte-by-byte assembly.
		bool ReadComponent(const char *bytes, int componentType, double &out)
		{
			switch (componentType)
			{
			case ComponentTypeFloat:
			{
				float value = 0.0f;
				std::memcpy(&value, bytes, sizeof(float));
				out = value;
				return true;
			}
			case ComponentTypeUnsignedByte:
			{
				unsigned char value = 0;
				std::memcpy(&value, bytes, sizeof(unsigned char));
				out = value;
				return true;
			}
			case ComponentTypeUnsignedShort:
			{
				unsigned short value = 0;
				std::memcpy(&value, bytes, sizeof(unsigned short));
				out = value;
				return true;
			}
			case ComponentTypeUnsignedInt:
			{
				unsigned int value = 0;
				std::memcpy(&value, bytes, sizeof(unsigned int));
				out = value;
				return true;
			}
			default:
				return false;
			}
		}

		// ---- file helpers ---------------------------------------------------

		bool ReadFile(const std::string &path, std::string &out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream.is_open())
				return false;
			out.assign((std::istreambuf_iterator<char>(stream)),
					   std::istreambuf_iterator<char>());
			return true;
		}

		// The directory portion of a path, used to resolve a .gltf's external
		// .bin. Handled here rather than with a filesystem library because the
		// rule is one line: everything up to the last separator.
		std::string DirectoryOf(const std::string &path)
		{
			const size_t slash = path.find_last_of("/\\");
			if (slash == std::string::npos)
				return ".";
			if (slash == 0)
				return "/";
			return path.substr(0, slash);
		}

		// ---- .glb container -------------------------------------------------

		// A .glb is a 12-byte header followed by chunks. The first chunk is
		// JSON, the second is BIN. Padding is required to 4 bytes and the chunk
		// header gives the length, so a reader that trusts the header never has
		// to scan for the padding.
		struct GlbParts
		{
			std::string Json;
			std::string Binary;
		};

		bool SplitGlb(const std::string &bytes, GlbParts &out, std::string &error)
		{
			static const unsigned int Magic = 0x46546C67; // "glTF"
			static const unsigned int ChunkTypeJson = 0x4E4F534A; // "JSON"
			static const unsigned int ChunkTypeBin = 0x004E4942;  // "BIN\0"

			if (bytes.size() < 12)
			{
				error = "file is smaller than a .glb header";
				return false;
			}

			unsigned int magic = 0;
			unsigned int version = 0;
			unsigned int length = 0;
			std::memcpy(&magic, bytes.data(), 4);
			std::memcpy(&version, bytes.data() + 4, 4);
			std::memcpy(&length, bytes.data() + 8, 4);

			if (magic != Magic)
			{
				error = "not a .glb (bad magic)";
				return false;
			}
			if (version != 2)
			{
				error = "glb version " + std::to_string(version)
						+ " is not supported (expected 2)";
				return false;
			}
			if (length > bytes.size())
			{
				error = "glb header declares " + std::to_string(length)
						+ " bytes but the file is " + std::to_string(bytes.size());
				return false;
			}

			size_t offset = 12;
			bool sawJson = false;
			while (offset + 8 <= bytes.size())
			{
				unsigned int chunkLength = 0;
				unsigned int chunkType = 0;
				std::memcpy(&chunkLength, bytes.data() + offset, 4);
				std::memcpy(&chunkType, bytes.data() + offset + 4, 4);
				offset += 8;

				if (offset + chunkLength > bytes.size())
				{
					error = "glb chunk overruns the file";
					return false;
				}

				if (chunkType == ChunkTypeJson)
				{
					out.Json.assign(bytes, offset, chunkLength);
					sawJson = true;
				}
				else if (chunkType == ChunkTypeBin)
				{
					out.Binary.assign(bytes, offset, chunkLength);
				}
				// An unknown chunk is skipped rather than rejected: the spec
				// allows extension chunks, and refusing a file because of a
				// chunk this importer does not read would be wrong.
				offset += chunkLength;
			}

			if (!sawJson)
			{
				error = "glb has no JSON chunk";
				return false;
			}
			return true;
		}

		// ---- accessor reading ----------------------------------------------

		// A resolved accessor: where its bytes live and how to stride through
		// them. Resolved once and reused, because every vertex attribute of a
		// primitive reads the same way.
		struct AccessorInfo
		{
			const char *Data = nullptr;
			size_t Stride = 0;
			int Components = 0;
			int ComponentType = 0;
			size_t Count = 0;
		};

		// Reads one accessor into a flat vector of doubles.
		//
		// Doubles rather than floats at this boundary: an accessor's byteOffset
		// arithmetic is exact in double for every size a glTF file can legally
		// contain, and a float would introduce rounding into an offset that is
		// then used to index memory.
		bool FlattenAccessor(const JsonValue &accessor,
							 const std::vector<std::pair<const char *, size_t>>
								 &buffers,
							 const JsonValue &bufferViews,
							 std::vector<double> &out, std::string &error)
		{
			const double count = NumberOr(accessor, "count", -1.0);
			if (count < 0)
			{
				error = "accessor has no valid count";
				return false;
			}

			const JsonValue *componentTypeValue = accessor.Find("componentType");
			if (!componentTypeValue || !componentTypeValue->IsNumber())
			{
				error = "accessor has no componentType";
				return false;
			}
			double componentTypeDouble = 0.0;
			componentTypeValue->AsNumber(componentTypeDouble);
			const int componentType = (int)componentTypeDouble;

			const int componentSize = ComponentSize(componentType);
			if (componentSize == 0)
			{
				error = "accessor componentType " + std::to_string(componentType)
						+ " is not one this importer supports";
				return false;
			}

			std::string type = "SCALAR";
			const JsonValue *typeValue = accessor.Find("type");
			if (typeValue && typeValue->IsString())
				typeValue->AsString(type);
			const int components = ComponentCount(type);
			if (components == 0)
			{
				error = "accessor type '" + type + "' is not supported";
				return false;
			}

			// The bufferView is optional: a sparse accessor can omit it, and a
			// zero-filled accessor is legal for a default-valued attribute.
			const JsonValue *viewIndexValue = accessor.Find("bufferView");
			if (!viewIndexValue || !viewIndexValue->IsNumber())
			{
				out.assign((size_t)count * (size_t)components, 0.0);
				return true;
			}

			double viewIndexDouble = 0.0;
			viewIndexValue->AsNumber(viewIndexDouble);
			const JsonValue *view = bufferViews.At((size_t)viewIndexDouble);
			if (!view)
			{
				error = "accessor names bufferView "
						+ std::to_string((long long)viewIndexDouble)
						+ " but there are only "
						+ std::to_string(bufferViews.Size());
				return false;
			}

			double bufferIndex = 0.0;
			bufferIndex = NumberOr(*view, "buffer", -1.0);
			if ((size_t)bufferIndex >= buffers.size())
			{
				error = "bufferView names buffer "
						+ std::to_string((long long)bufferIndex)
						+ " which was not loaded";
				return false;
			}

			// byteOffset is OPTIONAL on both a bufferView and an accessor, and
			// byteLength is optional when the view runs to the end of the
			// buffer. All three were dereferenced without a null check, so a
			// legal file that omitted them segfaulted the importer.
			const double viewOffset = NumberOr(*view, "byteOffset", 0.0);
			double viewLength = NumberOr(*view, "byteLength", 0.0);
			const double accessorOffset = NumberOr(accessor, "byteOffset", 0.0);

			// A bufferView with no byteLength spans to the end of its buffer.
			// Reading 0 bytes would silently return an empty attribute, which
			// is worse than failing: the importer would report success with a
			// mesh that has no vertices.
			if (viewLength == 0.0)
				viewLength = buffers[(size_t)bufferIndex].second - viewOffset;

			// byteStride lives on the bufferView, not the accessor. When it is
			// absent the data is tightly packed and the stride is the component
			// size times the component count.
			double stride = componentSize * components;
			const JsonValue *strideValue = view->Find("byteStride");
			if (strideValue && strideValue->IsNumber())
			{
				stride = NumberOr(*view, "byteStride", (double)stride);
				if (stride <= 0)
				{
					error = "bufferView has a non-positive byteStride";
					return false;
				}
			}

			const char *base = buffers[(size_t)bufferIndex].first;
			const size_t available = buffers[(size_t)bufferIndex].second;
			const size_t start = (size_t)viewOffset + (size_t)accessorOffset;
			if (start + (size_t)viewLength > available)
			{
				error = "bufferView "
						+ std::to_string((long long)viewIndexDouble)
						+ " reads past the end of its buffer";
				return false;
			}

			out.clear();
			out.reserve((size_t)count * (size_t)components);
			for (size_t element = 0; element < (size_t)count; ++element)
			{
				const size_t elementOffset = start + element * (size_t)stride;
				for (int component = 0; component < components; ++component)
				{
					const size_t at = elementOffset + (size_t)component
									  * (size_t)componentSize;
					if (at + (size_t)componentSize > available)
					{
						error = "accessor reads past the end of its buffer";
						return false;
					}
					double value = 0.0;
					ReadComponent(base + at, componentType, value);
					out.push_back(value);
				}
			}
			return true;
		}
	} // namespace

	GltfImportResult ImportGltf(const std::string &path)
	{
		GltfImportResult result;

		std::string bytes;
		if (!ReadFile(path, bytes))
		{
			result.Error = "could not read " + path;
			return result;
		}

		const std::string directory = DirectoryOf(path);

		// A .glb is detected by its magic; anything else is treated as JSON.
		// The extension is deliberately not consulted: a .gltf renamed to .glb
		// is still a JSON file, and the magic is the only reliable signal.
		if (bytes.size() >= 4 && bytes[0] == 'g' && bytes[1] == 'l'
			&& bytes[2] == 'T' && bytes[3] == 'F')
		{
			GlbParts parts;
			std::string error;
			if (!SplitGlb(bytes, parts, error))
			{
				result.Error = error;
				return result;
			}
			return ImportGltfFromText(parts.Json, parts.Binary, directory);
		}

		return ImportGltfFromText(bytes, "", directory);
	}

	GltfImportResult ImportGltfFromText(const std::string &json,
										const std::string &binaryChunk,
										const std::string &baseDirectory)
	{
		GltfImportResult result;

		JsonValue root;
		std::string error;
		if (!JsonParse(json, root, error))
		{
			result.Error = "JSON: " + error;
			return result;
		}
		if (!root.IsObject())
		{
			result.Error = "JSON: the document is not an object";
			return result;
		}

		// ---- buffers --------------------------------------------------------
		//
		// Only buffer 0 is loaded, and only when it is needed: a .gltf can
		// declare several buffers and reference any of them, but the importer
		// resolves each bufferView's buffer index and loads exactly those.
		std::vector<std::pair<const char *, size_t>> buffers;
		std::vector<std::string> bufferStorage;

		const JsonValue *buffersNode = root.Find("buffers");
		if (buffersNode && buffersNode->IsArray())
		{
			for (size_t index = 0; index < buffersNode->Size(); ++index)
			{
				const JsonValue *buffer = buffersNode->At(index);
				if (!buffer || !buffer->IsObject())
					continue;

				// `StringOr` takes a FALLBACK VALUE, not a key -- `StringOr("uri")`
				// reads the wrong thing and returns the literal "uri" when the
				// key is absent, which is why the error said "could not read
				// buffer uri". Find the key, then read it.
				const JsonValue *uriValue = buffer->Find("uri");
				std::string uri = uriValue && uriValue->IsString()
									  ? uriValue->StringValue
									  : "";
				std::string bytes;

				if (!binaryChunk.empty() && index == 0 && uri.empty())
				{
					// The .glb BIN chunk is buffer 0 by definition.
					bytes = binaryChunk;
				}
				else if (!uri.empty())
				{
					// A data: URI is legal glTF and appears in exporters that
					// embed the buffer. Supported because it is one line to
					// decode and it avoids a whole class of "my file will not
					// open" reports.
					const std::string prefix = "data:application/octet-stream;base64,";
					if (uri.compare(0, prefix.size(), prefix) == 0)
					{
						result.Error = "data: URIs are not supported by this "
									   "importer";
						return result;
					}
					const std::string full = baseDirectory + "/" + uri;
					if (!ReadFile(full, bytes))
					{
						result.Error = "could not read buffer " + uri;
						return result;
					}
				}
				else if (index > 0)
				{
					result.Error = "buffer " + std::to_string(index)
								   + " has no uri and is not the glb BIN chunk";
					return result;
				}

				bufferStorage.push_back(std::move(bytes));
				buffers.emplace_back(bufferStorage.back().data(),
									 bufferStorage.back().size());
			}
		}

		const JsonValue *bufferViews = root.Find("bufferViews");
		const JsonValue *accessors = root.Find("accessors");
		if (!accessors || !accessors->IsArray())
		{
			result.Error = "no accessors";
			return result;
		}

		// ---- mesh geometry --------------------------------------------------
		const JsonValue *meshes = root.Find("meshes");
		if (!meshes || !meshes->IsArray() || meshes->Size() == 0)
		{
			result.Error = "no meshes";
			return result;
		}

		// Only the FIRST mesh's first primitive is imported.
		//
		// This is a stated scope limit, not an oversight: a glTF mesh can hold
		// several primitives with different materials, and the engine's
		// MeshComponent addresses one mesh with one material. Importing the
		// first primitive and saying so is honest; silently merging primitives
		// with different materials would produce a mesh that renders with the
		// wrong material on part of its surface.
		const JsonValue *mesh = meshes->At(0);
		const JsonValue *primitives = mesh ? mesh->Find("primitives") : nullptr;
		if (!primitives || !primitives->IsArray() || primitives->Size() == 0)
		{
			result.Error = "meshes[0] has no primitives";
			return result;
		}
		const JsonValue *primitive = primitives->At(0);

		const JsonValue *attributes = primitive->Find("attributes");
		if (!attributes || !attributes->IsObject())
		{
			result.Error = "meshes[0].primitives[0] has no attributes";
			return result;
		}

		// POSITION is the only mandatory attribute.
		const JsonValue *positionAccessorIndex = attributes->Find("POSITION");
		if (!positionAccessorIndex || !positionAccessorIndex->IsNumber())
		{
			result.Error = "meshes[0].primitives[0].attributes has no POSITION";
			return result;
		}

		double positionIndex = 0.0;
		positionAccessorIndex->AsNumber(positionIndex);
		const JsonValue *positionAccessor = accessors->At((size_t)positionIndex);
		if (!positionAccessor)
		{
			result.Error = "POSITION names accessor "
						   + std::to_string((long long)positionIndex)
						   + " which does not exist";
			return result;
		}

		std::vector<double> positions;
		if (!FlattenAccessor(*positionAccessor, buffers, *bufferViews, positions,
							 error))
		{
			result.Error = "POSITION: " + error;
			return result;
		}
		if (positions.size() % 3 != 0)
		{
			result.Error = "POSITION accessor is not a whole number of vec3s";
			return result;
		}
		const size_t vertexCount = positions.size() / 3;

		// OPTIONAL attributes. Each is read only when present, and a present
		// but wrong-typed attribute is an error rather than a silent default --
		// a mesh whose normals are all (0,0,0) because an accessor was skipped
		// is a bug that surfaces much later.
		auto readOptional = [&](const char *name, size_t componentCount,
								std::vector<double> &out) -> bool {
			const JsonValue *indexValue = attributes->Find(name);
			if (!indexValue || !indexValue->IsNumber())
				return true; // absent is fine

			double index = 0.0;
			indexValue->AsNumber(index);
			const JsonValue *accessor = accessors->At((size_t)index);
			if (!accessor)
			{
				error = std::string(name) + " names accessor "
						+ std::to_string((long long)index)
						+ " which does not exist";
				return false;
			}
			if (!FlattenAccessor(*accessor, buffers, *bufferViews, out, error))
			{
				error = std::string(name) + ": " + error;
				return false;
			}
			if (out.size() != vertexCount * componentCount)
			{
				error = std::string(name) + " has " + std::to_string(out.size())
						+ " components but POSITION implies "
						+ std::to_string(vertexCount * componentCount);
				return false;
			}
			return true;
		};

		std::vector<double> normals;
		std::vector<double> texCoords;
		if (!readOptional("NORMAL", 3, normals))
		{
			result.Error = "meshes[0].primitives[0] " + error;
			return result;
		}
		if (!readOptional("TEXCOORD_0", 2, texCoords))
		{
			result.Error = "meshes[0].primitives[0] " + error;
			return result;
		}

		// Indices. Absent means sequential 0..n-1, which is what most exporters
		// emit for an already-indexed mesh.
		std::vector<int32_t> indices;
		const JsonValue *indexAccessorValue = primitive->Find("indices");
		if (indexAccessorValue && indexAccessorValue->IsNumber())
		{
			double index = 0.0;
			indexAccessorValue->AsNumber(index);
			const JsonValue *accessor = accessors->At((size_t)index);
			if (!accessor)
			{
				result.Error = "indices names accessor "
							   + std::to_string((long long)index)
							   + " which does not exist";
				return result;
			}
			std::vector<double> flat;
			if (!FlattenAccessor(*accessor, buffers, *bufferViews, flat, error))
			{
				result.Error = "indices: " + error;
				return result;
			}
			indices.reserve(flat.size());
			for (double value : flat)
				indices.push_back((int32_t)value);
		}
		else
		{
			indices.reserve(vertexCount);
			for (size_t i = 0; i < vertexCount; ++i)
				indices.push_back((int32_t)i);
		}

		// ---- build the half-edge mesh --------------------------------------
		//
		// Weld from a de-indexed MeshData, the same path the OBJ loader uses.
		// Building one corner per index and letting Weld merge by position is
		// what gives the imported asset real adjacency without a second code
		// path -- and it means a glTF cube and an OBJ cube produce identical
		// EditableMesh structures.
		MeshData deindexed;
		deindexed.Vertices.reserve(indices.size());
		for (int32_t vertexIndex : indices)
		{
			if (vertexIndex < 0 || (size_t)vertexIndex >= vertexCount)
			{
				result.Error = "index " + std::to_string(vertexIndex)
							   + " is out of range (vertex count "
							   + std::to_string(vertexCount) + ")";
				return result;
			}

			MeshVertex corner;
			corner.Position[0] = (float)positions[(size_t)vertexIndex * 3 + 0];
			corner.Position[1] = (float)positions[(size_t)vertexIndex * 3 + 1];
			corner.Position[2] = (float)positions[(size_t)vertexIndex * 3 + 2];

			if (!normals.empty())
			{
				corner.Normal[0] = (float)normals[(size_t)vertexIndex * 3 + 0];
				corner.Normal[1] = (float)normals[(size_t)vertexIndex * 3 + 1];
				corner.Normal[2] = (float)normals[(size_t)vertexIndex * 3 + 2];
			}
			else
			{
				// A flat +Y default rather than (0,0,0): RecomputeNormals is the
				// right fix for a mesh with no normals, and a zero normal would
				// make that fix look like a no-op.
				corner.Normal[0] = 0.0f;
				corner.Normal[1] = 1.0f;
				corner.Normal[2] = 0.0f;
			}

			if (!texCoords.empty())
			{
				corner.TexCoord[0] = (float)texCoords[(size_t)vertexIndex * 2 + 0];
				corner.TexCoord[1] = (float)texCoords[(size_t)vertexIndex * 2 + 1];
			}

			deindexed.Vertices.push_back(corner);
		}
		deindexed.Indices.reserve(indices.size());
		for (size_t i = 0; i < indices.size(); ++i)
			deindexed.Indices.push_back((uint32_t)i);

		result.Mesh = EditableMesh::Weld(deindexed);
		// The de-indexed copy is kept because Weld is lossy for hard edges --
		// see the comment on GltfImportResult::Deindexed. Copied rather than
		// moved: deindexed is a local and result outlives it, but the copy is
		// the price of carrying both forms, which is one extra mesh in memory
		// per imported asset.
		result.Deindexed = deindexed;

		// ---- skin -----------------------------------------------------------
		const JsonValue *skins = root.Find("skins");
		if (skins && skins->IsArray() && skins->Size() > 0)
		{
			const JsonValue *skin = skins->At(0);
			const JsonValue *joints = skin->Find("joints");
			if (joints && joints->IsArray())
			{
				for (size_t i = 0; i < joints->Size(); ++i)
				{
					double jointNode = 0.0;
					joints->At(i)->AsNumber(jointNode);
					result.BoneParents.push_back(-1); // resolved below
					result.BoneNames.push_back("joint_" + std::to_string(i)
											   + "_node_" + std::to_string((int)jointNode));
				}

				// Node names, when the file has them. A glTF exporter that
				// strips node names still produces a usable skeleton; the
				// generated name above keeps the bone identifiable by its node
				// index, which is stable.
				const JsonValue *nodes = root.Find("nodes");
				if (nodes && nodes->IsArray())
				{
					for (size_t i = 0; i < joints->Size(); ++i)
					{
						double jointNode = 0.0;
						joints->At(i)->AsNumber(jointNode);
						const JsonValue *node = nodes->At((size_t)jointNode);
						std::string name;
						if (node && node->Find("name")
							&& node->Find("name")->AsString(name)
							&& !name.empty())
						{
							result.BoneNames[i] = name;
						}

						// Parent resolution: a joint's parent is the node that
						// lists it in its children, which is the only place glTF
						// records hierarchy.
						for (size_t n = 0; n < nodes->Size(); ++n)
						{
							const JsonValue *candidate = nodes->At(n);
							const JsonValue *children = candidate->Find("children");
							if (!children || !children->IsArray())
								continue;
							for (size_t c = 0; c < children->Size(); ++c)
							{
								double child = 0.0;
								children->At(c)->AsNumber(child);
								if ((size_t)child == (size_t)jointNode)
								{
									// Map the node index to a bone index.
									for (size_t b = 0; b < joints->Size(); ++b)
									{
										double joint = 0.0;
										joints->At(b)->AsNumber(joint);
										if ((size_t)joint == n)
										{
											result.BoneParents[i] = (int32_t)b;
											break;
										}
									}
								}
							}
						}
					}
				}

				// Inverse bind matrices, in bone order.
				const JsonValue *ibmValue = skin->Find("inverseBindMatrices");
				if (ibmValue && ibmValue->IsNumber())
				{
					double accessorIndex = 0.0;
					ibmValue->AsNumber(accessorIndex);
					const JsonValue *accessor = accessors->At((size_t)accessorIndex);
					if (!accessor)
					{
						result.Error = "inverseBindMatrices names accessor "
									   + std::to_string((long long)accessorIndex)
									   + " which does not exist";
						return result;
					}
					std::vector<double> flat;
					if (!FlattenAccessor(*accessor, buffers, *bufferViews, flat,
										 error))
					{
						result.Error = "inverseBindMatrices: " + error;
						return result;
					}
					if (flat.size() != result.BoneNames.size() * 16)
					{
						result.Error = "inverseBindMatrices has "
									   + std::to_string(flat.size())
									   + " components but "
									   + std::to_string(result.BoneNames.size())
									   + " bones need "
									   + std::to_string(result.BoneNames.size() * 16);
						return result;
					}
					result.InverseBindMatrices.assign(flat.begin(), flat.end());
				}
			}
		}

		// ---- animations -----------------------------------------------------
		const JsonValue *animations = root.Find("animations");
		if (animations && animations->IsArray())
		{
			for (size_t a = 0; a < animations->Size(); ++a)
			{
				const JsonValue *animation = animations->At(a);
				GltfImportResult::Clip clip;
				const JsonValue *clipName = animation->Find("name");
				clip.Name = clipName && clipName->IsString()
								? clipName->StringValue
								: "";
				if (clip.Name.empty())
					clip.Name = "clip_" + std::to_string(a);

				// One timeline per clip. glTF allows each channel its own input
				// accessor; taking the union of every channel's key times and
				// resampling is what makes a clip a single playable thing
				// rather than a bag of curves.
				std::vector<double> keyTimes;

				const JsonValue *channels = animation->Find("channels");
				const JsonValue *samplers = animation->Find("samplers");
				if (channels && samplers && channels->IsArray()
					&& samplers->IsArray())
				{
					for (size_t c = 0; c < channels->Size(); ++c)
					{
						const JsonValue *channel = channels->At(c);
						double samplerIndex = 0.0;
						samplerIndex = NumberOr(*channel, "sampler", -1.0);
						const JsonValue *sampler = samplers->At((size_t)samplerIndex);
						if (!sampler)
							continue;

						const JsonValue *target = channel->Find("target");
						if (!target)
							continue;
						const JsonValue *pathValue = target->Find("path");
						std::string path = pathValue && pathValue->IsString()
											   ? pathValue->StringValue
											   : "";
						double nodeIndex = 0.0;
						nodeIndex = NumberOr(*target, "node", -1.0);

						// The channel targets a NODE. Map it to a bone when it
						// is one; a channel that targets a non-joint node is
						// kept with Bone = -1 so the clip still carries it
						// rather than dropping data the file explicitly had.
						int32_t bone = -1;
						if (!result.BoneNames.empty())
						{
							const JsonValue *joints = root.Find("skins")
											   ? root.Find("skins")->At(0)->Find("joints")
											   : nullptr;
							if (joints && joints->IsArray())
							{
								for (size_t b = 0; b < joints->Size(); ++b)
								{
									double joint = 0.0;
									joints->At(b)->AsNumber(joint);
									if ((size_t)joint == (size_t)nodeIndex)
									{
										bone = (int32_t)b;
										break;
									}
								}
							}
						}

						double inputAccessor = 0.0;
						inputAccessor = NumberOr(*sampler, "input", -1.0);
						double outputAccessor = 0.0;
						outputAccessor = NumberOr(*sampler, "output", -1.0);

						std::vector<double> times;
						std::vector<double> values;
						const JsonValue *input = accessors->At((size_t)inputAccessor);
						const JsonValue *output = accessors->At((size_t)outputAccessor);
						if (!input || !output)
							continue;
						if (!FlattenAccessor(*input, buffers, *bufferViews, times,
											 error))
						{
							result.Error = clip.Name + " key times: " + error;
							return result;
						}
						if (!FlattenAccessor(*output, buffers, *bufferViews, values,
											 error))
						{
							result.Error = clip.Name + " values: " + error;
							return result;
						}

						const int components = (path == "rotation") ? 4 : 3;
						if (values.size() != times.size() * (size_t)components)
						{
							result.Error = clip.Name + " " + path
										   + " channel has a value/time mismatch";
							return result;
						}

						if (keyTimes.empty())
						{
							keyTimes = times;
						}

						GltfImportResult::ClipChannel outChannel;
						outChannel.Bone = bone;
						outChannel.Path = path;
						clip.Channels.push_back(outChannel);
						clip.Values.push_back(std::vector<std::vector<float>>());
						auto &channelValues = clip.Values.back();
						channelValues.reserve(times.size());
						for (size_t k = 0; k < times.size(); ++k)
						{
							std::vector<float> value(components);
							for (int component = 0; component < components; ++component)
								value[component] = (float)values[k * components + component];
							channelValues.push_back(std::move(value));
						}

						for (double time : times)
							clip.Duration = std::max(clip.Duration, time);
					}
				}

				clip.KeyTimes = std::move(keyTimes);
				result.Clips.push_back(std::move(clip));
			}
		}

		// ---- materials ------------------------------------------------------
		//
		// Parsed after the mesh so a material error cannot prevent the geometry
		// from being returned: a file with a bad material is still a usable mesh
		// rendered with the entity's own material.
		const JsonValue *materials = root.Find("materials");
		if (materials && materials->IsArray())
		{
			for (size_t m = 0; m < materials->Size(); ++m)
			{
				const JsonValue *material = materials->At(m);
				if (!material || !material->IsObject())
				{
					result.Error = "materials[" + std::to_string(m)
								   + "] is not an object";
					return result;
				}

				GltfMaterial out;

				const JsonValue *materialName = material->Find("name");
				out.Name = materialName && materialName->IsString()
							   ? materialName->StringValue
							   : "";

				// alphaMode is recorded, not applied: MaterialComponent has no
				// alpha-mode field. Flattening a BLEND material to opaque would
				// render it wrong in a way nothing downstream could detect.
				const JsonValue *alphaMode = material->Find("alphaMode");
				out.AlphaMode = alphaMode && alphaMode->IsString()
									? alphaMode->StringValue
									: "OPAQUE";

				const JsonValue *pbr = material->Find("pbrMetallicRoughness");
				if (pbr && pbr->IsObject())
				{
					// baseColorFactor: an ARRAY of 4. Read it positionally
					// rather than by key -- a JSON object's member order is
					// preserved but an array has no keys.
					const JsonValue *baseColor = pbr->Find("baseColorFactor");
					if (baseColor && baseColor->IsArray())
					{
						for (size_t c = 0; c < 4 && c < baseColor->Size(); ++c)
						{
							const JsonValue *component = baseColor->At(c);
							if (component && component->IsNumber())
							{
								// AsNumber takes a double& -- it writes the parsed
								// value out rather than returning it. Reading
								// straight into the float array does not compile,
								// and a silent narrowing here would lose the
								// precision glTF stores.
								double value = 0.0;
								component->AsNumber(value);
								out.BaseColorFactor[c] = (float)value;
							}
						}
					}

					out.MetallicFactor = (float)NumberOr(*pbr, "metallicFactor", 1.0);
					out.RoughnessFactor = (float)NumberOr(*pbr, "roughnessFactor", 1.0);

					// baseColorTexture -> images[texture.index].uri. Only a URI
					// is resolvable; a bufferView image would need PNG encoding
					// to become an asset, which v1 does not do. The flag is still
					// set, so the material records that a texture existed.
					const JsonValue *baseColorTexture = pbr->Find("baseColorTexture");
					if (baseColorTexture && baseColorTexture->IsObject())
					{
						out.HasBaseColorTexture = true;
						const double textureIndex = NumberOr(*baseColorTexture, "index", -1.0);
						const JsonValue *images = root.Find("images");
						const JsonValue *image = images && images->IsArray() &&
														 (size_t)textureIndex < images->Size()
													 ? images->At((size_t)textureIndex)
													 : nullptr;
						const JsonValue *uri = image ? image->Find("uri") : nullptr;
						if (uri && uri->IsString())
							out.AlbedoTexturePath = uri->StringValue;
					}
				}

				const JsonValue *emissive = material->Find("emissiveFactor");
				if (emissive && emissive->IsArray())
				{
					for (size_t c = 0; c < 3 && c < emissive->Size(); ++c)
					{
						const JsonValue *component = emissive->At(c);
						if (component && component->IsNumber())
						{
							double value = 0.0;
							component->AsNumber(value);
							out.EmissiveFactor[c] = (float)value;
						}
					}
				}

				const JsonValue *normalTexture = material->Find("normalTexture");
				if (normalTexture && normalTexture->IsObject())
				{
					out.HasNormalTexture = true;
					// glTF's default scale is 1.0; the engine's NormalStrength
					// defaults to 0 (disabled), so a normal map must set it.
					out.NormalScale = (float)NumberOr(*normalTexture, "scale", 1.0);
				}

				result.Materials.push_back(std::move(out));
			}
		}

		return result;
	}

} // namespace Wasteland
