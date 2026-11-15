// MeshWriter.cpp -- OBJ export. See MeshWriter.h for why OBJ and not glTF.
//
// The emitter is the tested part; SaveOBJ is a thin file write around it. A
// test that can call EmitOBJ and inspect the text can check the format without
// touching the disk, and the file half is then too small to hide a bug.

#include "wlpch.h"

#include "Wasteland/Mesh/MeshWriter.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace Wasteland
{
namespace
{
	// Nine significant digits: a float has about 7, so this round-trips every
	// value exactly and stays readable. `std::defaultfloat` rather than fixed --
	// fixed notation turns 1e-7 into 0.000000100 and a 10000-unit mesh into a
	// 200-character line.
	void AppendFloat(std::ostringstream &out, float value)
	{
		out << std::setprecision(9) << value;
	}

	void AppendVec3(std::ostringstream &out, const float *value)
	{
		AppendFloat(out, value[0]);
		out << ' ';
		AppendFloat(out, value[1]);
		out << ' ';
		AppendFloat(out, value[2]);
	}

	// The three corner vertices of a face, walked through the half-edge links.
	// Returns false for a degenerate or open face rather than emitting a
	// malformed triangle: OBJ has no way to say "this face is broken", so the
	// only honest options are to skip it or to refuse the write, and a writer
	// that silently drops geometry is worse than one that reports it.
	bool FaceCorners(const EditableMesh &mesh, int32_t face, int32_t out[3])
	{
		const EditableMesh::Face &record = mesh.Faces[(size_t)face];
		if (record.FirstEdge < 0)
			return false;

		int32_t first = record.FirstEdge;
		int32_t second = mesh.Edges[(size_t)first].Next;
		if (second < 0)
			return false;
		int32_t third = mesh.Edges[(size_t)second].Next;
		if (third < 0)
			return false;

		// The loop must close back on itself. An open loop is a structural
		// defect Validate() reports, and emitting it would produce a triangle
		// the mesh does not contain.
		if (mesh.Edges[(size_t)third].Next != first)
			return false;

		out[0] = mesh.Edges[(size_t)first].Vertex;
		out[1] = mesh.Edges[(size_t)second].Vertex;
		out[2] = mesh.Edges[(size_t)third].Vertex;

		for (int corner = 0; corner < 3; ++corner)
		{
			if (out[corner] < 0 ||
				out[corner] >= (int32_t)mesh.Vertices.size())
				return false;
		}

		// Three corners that are not three DISTINCT vertices make a zero-area
		// triangle. Emitting one would put degenerate geometry into the file and
		// the parser would read it back as a real (invisible) triangle, so the
		// defect would propagate silently instead of stopping here.
		if (out[0] == out[1] || out[1] == out[2] || out[0] == out[2])
			return false;

		return true;
	}
} // namespace

std::string MeshWriter::EmitOBJ(const EditableMesh &mesh)
{
	std::ostringstream out;

	out << "# Wasteland OBJ export\n";
	out << "# vertices " << mesh.Vertices.size() << ", faces "
		<< mesh.Faces.size() << "\n";

	for (const EditableMesh::Vertex &vertex : mesh.Vertices)
	{
		out << "v ";
		AppendVec3(out, vertex.Position);
		out << '\n';
	}

	for (const EditableMesh::Vertex &vertex : mesh.Vertices)
	{
		out << "vt ";
		AppendFloat(out, vertex.TexCoord[0]);
		out << ' ';
		AppendFloat(out, vertex.TexCoord[1]);
		out << '\n';
	}

	for (const EditableMesh::Vertex &vertex : mesh.Vertices)
	{
		out << "vn ";
		AppendVec3(out, vertex.Normal);
		out << '\n';
	}

	int32_t skipped = 0;
	for (int32_t face = 0; face < (int32_t)mesh.Faces.size(); ++face)
	{
		int32_t corners[3];
		if (!FaceCorners(mesh, face, corners))
		{
			++skipped;
			continue;
		}
		out << "f";
		for (int corner = 0; corner < 3; ++corner)
		{
			// OBJ indices are 1-based and this mesh shares one index space
			// across position, texcoord and normal, so all three fields carry
			// the same number. Writing three copies of one index is what keeps
			// a hard edge hard: two coincident vertices with different normals
			// stay two vertices.
			const int32_t oneBased = corners[corner] + 1;
			out << ' ' << oneBased << '/' << oneBased << '/' << oneBased;
		}
		out << '\n';
	}

	if (skipped > 0)
	{
		out << "# " << skipped << " face(s) omitted: degenerate or open loops\n";
	}
	return out.str();
}

std::string MeshWriter::EmitOBJFromMeshData(const MeshData &data)
{
	std::ostringstream out;

	out << "# Wasteland OBJ export (de-indexed source)\n";
	out << "# corners " << data.Vertices.size() << ", triangles "
		<< data.GetTriangleCount() << "\n";

	for (const MeshVertex &vertex : data.Vertices)
	{
		out << "v ";
		AppendVec3(out, vertex.Position);
		out << '\n';
	}

	for (const MeshVertex &vertex : data.Vertices)
	{
		out << "vt ";
		AppendFloat(out, vertex.TexCoord[0]);
		out << ' ';
		AppendFloat(out, vertex.TexCoord[1]);
		out << '\n';
	}

	for (const MeshVertex &vertex : data.Vertices)
	{
		out << "vn ";
		AppendVec3(out, vertex.Normal);
		out << '\n';
	}

	// De-indexed: every corner is its own vertex, so the face records are a
	// plain 1,2,3 / 4,5,6 sequence. A MeshData whose Indices are not the
	// identity is still written correctly by following them, but a partial or
	// empty index buffer means there are no complete triangles to emit.
	const size_t triangleCount = data.Indices.size() / 3;
	for (size_t triangle = 0; triangle < triangleCount; ++triangle)
	{
		out << "f";
		for (int corner = 0; corner < 3; ++corner)
		{
			const size_t index = data.Indices[triangle * 3 + (size_t)corner];
			if (index >= data.Vertices.size())
			{
				// A malformed index cannot be written as a face. Emitting the
				// triangle with a clamped index would silently weld two
				// distant corners together.
				out << " 0/0/0";
				continue;
			}
			const size_t oneBased = index + 1;
			out << ' ' << oneBased << '/' << oneBased << '/' << oneBased;
		}
		out << '\n';
	}
	return out.str();
}

bool MeshWriter::SaveOBJ(const std::filesystem::path &path,
						 const EditableMesh &mesh,
						 std::string *error)
{
	if (mesh.Vertices.empty())
	{
		if (error)
			*error = "the mesh has no vertices";
		return false;
	}

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file.is_open())
	{
		if (error)
			*error = "could not open the file for writing: " + path.string();
		return false;
	}

	file << EmitOBJ(mesh);
	file.flush();
	if (!file.good())
	{
		if (error)
			*error = "the write failed: " + path.string();
		return false;
	}
	return true;
}

bool MeshWriter::SaveOBJFromMeshData(const std::filesystem::path &path,
									 const MeshData &data,
									 std::string *error)
{
	if (data.Vertices.empty())
	{
		if (error)
			*error = "the mesh has no vertices";
		return false;
	}

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file.is_open())
	{
		if (error)
			*error = "could not open the file for writing: " + path.string();
		return false;
	}

	file << EmitOBJFromMeshData(data);
	file.flush();
	if (!file.good())
	{
		if (error)
			*error = "the write failed: " + path.string();
		return false;
	}
	return true;
}

} // namespace Wasteland
