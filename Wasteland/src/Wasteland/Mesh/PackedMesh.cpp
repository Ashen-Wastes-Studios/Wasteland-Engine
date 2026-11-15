#include "wlpch.h"

#include "Wasteland/Mesh/PackedMesh.h"

#include "Wasteland/Mesh/EditableMesh.h"
#include "Wasteland/Renderer/MeshLoader.h"

#include <fstream>
#include <vector>

namespace Wasteland
{
	namespace
	{
		// The on-disk magic, as raw bytes. "WLMESH01".
		constexpr char kMagic[8] = { 'W', 'L', 'M', 'E', 'S', 'H', '0', '1' };
		constexpr uint32_t kVersion = 1;
	}

	bool WritePackedMesh(const std::filesystem::path &output,
	                     const EditableMesh &mesh, int materialIndex,
	                     std::string *error)
	{
		const MeshData deindexed = mesh.Deindex();
		const uint32_t triangleCount = static_cast<uint32_t>(deindexed.Vertices.size() / 3);

		std::ofstream stream(output, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			if (error) *error = "could not open " + output.string() + " for writing";
			return false;
		}

		const auto write8 = [&stream](const void *data, std::size_t size)
		{
			stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
		};

		// 20-byte header, then corners. Written in one pass; the header is written
		// first purely so a human hex-dumping the file sees the magic up front.
		write8(kMagic, sizeof(kMagic));
		write8(&kVersion, sizeof(kVersion));
		write8(&materialIndex, sizeof(materialIndex));
		write8(&triangleCount, sizeof(triangleCount));
		if (!stream.good())
		{
			if (error) *error = "failed while writing the header of " + output.string();
			return false;
		}

		// Corners are the MeshVertex bytes verbatim: Position, Normal, TexCoord, in
		// the struct's field order. memcpy each so there is no endianness or packing
		// surprise -- the struct has no padding, but the copy states that explicitly.
		for (const MeshVertex &vertex : deindexed.Vertices)
		{
			write8(vertex.Position, sizeof(vertex.Position));
			write8(vertex.Normal, sizeof(vertex.Normal));
			write8(vertex.TexCoord, sizeof(vertex.TexCoord));
		}
		if (!stream.good())
		{
			if (error) *error = "failed while writing the corners of " + output.string();
			return false;
		}

		stream.flush();
		if (!stream.good())
		{
			if (error) *error = "failed while flushing " + output.string();
			return false;
		}
		return true;
	}

	bool ReadPackedMesh(const std::filesystem::path &path, MeshData &out,
	                    int *outMaterialIndex, std::string *error)
	{
		const auto fail = [error](const std::string &message)
		{
			if (error) *error = message;
			return false;
		};

		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return fail("could not open " + path.string() + " for reading");

		const auto read8 = [&stream](void *data, std::size_t size) -> bool
		{
			stream.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
			return stream.good();
		};

		char magic[8];
		uint32_t version = 0;
		int32_t materialIndex = 0;
		uint32_t triangleCount = 0;
		if (!read8(magic, sizeof(magic)) ||
		    !read8(&version, sizeof(version)) ||
		    !read8(&materialIndex, sizeof(materialIndex)) ||
		    !read8(&triangleCount, sizeof(triangleCount)))
			return fail("truncated header in " + path.string());

		for (int i = 0; i < 8; ++i)
			if (magic[i] != kMagic[i])
				return fail("not a packed mesh (bad magic) in " + path.string());
		if (version != kVersion)
			return fail("unsupported packed mesh version " + std::to_string(version) +
			            " in " + path.string());

		// A triangleCount that would overflow is rejected up front, so a corrupt
		// header cannot make the allocation below explode.
		const std::size_t cornerCount =
		    static_cast<std::size_t>(triangleCount) * 3ull;
		if (cornerCount > (static_cast<std::size_t>(1) << 31))
			return fail("implausible triangle count " + std::to_string(triangleCount) +
			            " in " + path.string());

		// Truncated payload is an error, not a silent short mesh.
		stream.seekg(0, std::ios::end);
		const std::streamoff fileSize = stream.tellg();
		const std::streamoff expected =
		    static_cast<std::streamoff>(20) +
		    static_cast<std::streamoff>(cornerCount * kPackedFloatsPerCorner *
		                                sizeof(float));
		if (fileSize < expected)
			return fail("truncated payload in " + path.string() +
			            " (expected " + std::to_string(expected) + " bytes, got " +
			            std::to_string(fileSize) + ")");
		if (fileSize > expected)
			return fail("trailing bytes after the mesh in " + path.string());
		stream.seekg(20, std::ios::beg);

		out.Vertices.resize(cornerCount);
		out.Indices.resize(cornerCount);
		for (std::size_t i = 0; i < cornerCount; ++i)
		{
			MeshVertex &vertex = out.Vertices[i];
			if (!read8(vertex.Position, sizeof(vertex.Position)) ||
			    !read8(vertex.Normal, sizeof(vertex.Normal)) ||
			    !read8(vertex.TexCoord, sizeof(vertex.TexCoord)))
				return fail("truncated corner in " + path.string());
			out.Indices[i] = static_cast<uint32_t>(i);
		}

		if (outMaterialIndex) *outMaterialIndex = materialIndex;
		return true;
	}
}
