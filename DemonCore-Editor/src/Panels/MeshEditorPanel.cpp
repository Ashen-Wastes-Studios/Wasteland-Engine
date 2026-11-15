// MeshEditorPanel.cpp -- the ImGui surface. See MeshEditorPanel.h for the
// architecture and the verification status.

#include "wlpch.h"

#include "MeshEditorPanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Wasteland/Core/Log.h"
#include "Wasteland/Mesh/GltfImporter.h"
#include "Wasteland/Mesh/MeshPicking.h"
#include "Wasteland/Mesh/MeshWriter.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Renderer/MeshLoader.h"
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Scene/Scene.h"

namespace Wasteland
{
namespace
{
	// An entity's display name. TagComponent when it has one, otherwise a short
	// UUID fragment -- stable across frames, unlike the entity's index, so a row
	// does not change label when another entity is destroyed.
	std::string EntityLabel(Entity entity)
	{
		if (!entity)
			return "(invalid)";

		if (entity.HasComponent<TagComponent>())
		{
			const std::string &tag = entity.GetComponent<TagComponent>().Tag;
			if (!tag.empty())
				return tag;
		}

		char buffer[40];
		std::snprintf(buffer, sizeof(buffer), "entity %llu",
			static_cast<unsigned long long>(static_cast<uint64_t>(entity.GetUUID())));
		return std::string(buffer);
	}

	bool HasExtension(const std::string &path, const char *suffix)
	{
		if (path.size() < std::strlen(suffix))
			return false;
		const std::string tail = path.substr(path.size() - std::strlen(suffix));
		return std::equal(tail.begin(), tail.end(), suffix, suffix + std::strlen(suffix),
						  [](char a, char b) {
							  return std::tolower(static_cast<unsigned char>(a)) ==
									 std::tolower(static_cast<unsigned char>(b));
						  });
	}
} // namespace

const char *MeshEditModeName(MeshEditMode mode)
{
	switch (mode)
	{
	case MeshEditMode::Object: return "Object";
	case MeshEditMode::Vertex: return "Vertex";
	case MeshEditMode::Edge: return "Edge";
	case MeshEditMode::Face: return "Face";
	}
	return "Object";
}

size_t MeshEditorPanel::GetSelectedCount() const
{
	return m_SelectedFaces.size();
}

void MeshEditorPanel::SelectFace(int32_t face, const float hitPoint[3],
										 bool additive)
	{
		if (face < 0)
			return;
		if (hitPoint != nullptr)
		{
			m_LastHit[0] = hitPoint[0];
			m_LastHit[1] = hitPoint[1];
			m_LastHit[2] = hitPoint[2];
			m_HasPick = true;
		}

		if (!additive)
			m_SelectedFaces.clear();

		// A toggle on an additive click: clicking a face that is already
		// selected removes it. Without this, shift-click can only ever add,
		// and the only way to reduce a selection is to clear it.
		const auto existing = m_SelectedFaces.find(face);
		if (existing != m_SelectedFaces.end())
			m_SelectedFaces.erase(existing);
		else
			m_SelectedFaces.insert(face);
	}

void MeshEditorPanel::ClearSelection()
{
	m_SelectedFaces.clear();
}

bool MeshEditorPanel::LoadTarget(Scene &scene)
{
	if (!m_TargetEntity)
	{
		m_LoadError = "no entity selected";
		return false;
	}
	if (!m_TargetEntity.HasComponent<MeshComponent>())
	{
		m_LoadError = EntityLabel(m_TargetEntity) + " has no MeshComponent";
		return false;
	}

	const std::string &path = m_TargetEntity.GetComponent<MeshComponent>().MeshPath;
	if (path.empty())
	{
		m_LoadError = EntityLabel(m_TargetEntity) + " has no mesh asset assigned";
		return false;
	}

	m_TargetPath = path;
	const std::filesystem::path absolute = Project::Get().GetAssetPath(path);

	std::error_code error;
	if (!std::filesystem::exists(absolute, error))
	{
		m_LoadError = "file not found: " + absolute.string();
		return false;
	}

	if (HasExtension(path, ".obj"))
	{
		MeshData data;
		std::string loadError;
		if (!LoadOBJ(absolute.string(), data, loadError))
		{
			m_LoadError = loadError;
			return false;
		}
		m_Mesh = EditableMesh::Weld(data);
	}
	else
	{
		// glTF is the format every DCC export path produces (C.5/C.6), so an
		// unrecognised extension is assumed to be glTF rather than rejected --
		// but a failure is reported with the importer's own reason, which names
		// the actual defect.
		const GltfImportResult result = ImportGltf(absolute.string());
		if (!result.Ok())
		{
			m_LoadError = result.Error.empty()
							  ? "the glTF import failed"
							  : result.Error;
			return false;
		}
		m_Mesh = result.Mesh;
	}

	m_LoadError.clear();
	m_SelectedFaces.clear();
	m_Status = "loaded " + path;
	return true;
}

void MeshEditorPanel::OnImGuiRender(Scene &scene)
{
	if (!m_Visible)
		return;

	ImGuiWindowFlags flags = ImGuiWindowFlags_None;
	if (!m_SizedOnce)
	{
	 ImGui::SetNextWindowSize(ImVec2{360.0f, 520.0f}, ImGuiCond_FirstUseEver);
		m_SizedOnce = true;
	}

	if (!ImGui::Begin("Mesh Editor", &m_Visible, flags))
	{
	 ImGui::End();
		return;
	}

	DrawTargetPicker(scene);

	if (m_HasTarget)
	{
		DrawToolbar();
		DrawSelection();
		DrawOperations();
		DrawHistory();
		DrawSculpt();
		SaveTarget();
	}
	else if (!m_LoadError.empty())
	{
	 ImGui::TextWrapped("Load failed: %s", m_LoadError.c_str());
	}

	if (!m_Status.empty())
	{
	 ImGui::Separator();
		// Dimmed, not red: a status line carries both successes and failures,
		// and colour-coding it would need a second piece of state to remember
		// which kind the last message was.
	ImGui::TextDisabled("%s", m_Status.c_str());
	}

 ImGui::End();
}

void MeshEditorPanel::DrawTargetPicker(Scene &scene)
{
	// Every entity carrying a MeshComponent, whether or not it is the current
	// target. .each() rather than the bare view: it yields (entity, component&),
	// which is what makes the structured binding work.
	std::vector<Entity> candidates;
	std::vector<std::string> labels;
	for (auto [entityHandle, mesh] : scene.GetAllEntitiesWith<MeshComponent>().each())
	{
		Entity entity{entityHandle, &scene};
		candidates.push_back(entity);
		labels.push_back(EntityLabel(entity) + "  [" + mesh.MeshPath + "]");
	}

	if (candidates.empty())
	{
	 ImGui::TextWrapped("No entity in this scene has a MeshComponent.");
		return;
	}

	int current = -1;
	for (size_t index = 0; index < candidates.size(); ++index)
	{
		if (candidates[index] == m_TargetEntity)
		{
			current = static_cast<int>(index);
			break;
		}
	}

	// ImGui::Combo takes an int* for the current index, not a char buffer --
	// the buffer overload is a newer ImGui API and fails to compile here.
	if (ImGui::Combo("Target", &current, [](void *data, int index,
										   const char **outText) -> bool {
			const auto *items = static_cast<std::vector<std::string> *>(data);
			if (index < 0 || index >= static_cast<int>(items->size()))
				return false;
			*outText = (*items)[(size_t)index].c_str();
			return true;
		}, &labels, static_cast<int>(labels.size())) && current >= 0)
	{
		m_TargetEntity = candidates[(size_t)current];
		m_HasTarget = LoadTarget(scene);
		if (!m_HasTarget)
			WL_CORE_WARN("MeshEditorPanel: {0}", m_LoadError);
	}

	if (m_HasTarget)
	{
	 ImGui::SameLine();
		// The asset path, not the entity label: two entities can share an
		// asset, and the artist needs to know WHICH asset an edit will write.
	 ImGui::TextDisabled("%s", m_TargetPath.c_str());
	}
}

void MeshEditorPanel::DrawToolbar()
{
	const char *modes[] = {"Object", "Vertex", "Edge", "Face"};
	int mode = static_cast<int>(m_Mode);
	if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
	{
		m_Mode = static_cast<MeshEditMode>(mode);
		// Sub-object selection is mode-specific in meaning, so switching modes
		// clears it. Keeping a face set while switching to Vertex mode would
		// show a count that no longer describes what the operations act on.
		ClearSelection();
	}

 ImGui::SameLine();
	if (ImGui::Checkbox("Wireframe", &m_ShowWireframe))
	{
		// Applied by the viewport through GetMesh()/the flags below; the panel
		// only owns the state so it survives a layout switch.
	}
	if (ImGui::Checkbox("Normals", &m_ShowNormals))
	{
	}

	if (m_HasTarget)
	{
		// The counts, because the plan says "measure, then optimize" and a
		// brute-force per-face raycast's cost is proportional to this number.
		// An artist on a 200k-triangle prop should be able to see why clicking
		// is slow before blaming the panel.
	 ImGui::TextDisabled("%d vertices, %d edges, %d faces",
						m_Mesh.GetVertexCount(), m_Mesh.GetEdgeCount(),
						m_Mesh.GetFaceCount());
	}
}

void MeshEditorPanel::DrawSelection()
{
ImGui::Separator();
ImGui::Text("Selection: %zu face(s)", m_SelectedFaces.size());
ImGui::SameLine();
	if (ImGui::Button("Clear"))
		ClearSelection();

	if (m_SelectedFaces.empty())
	{
	 ImGui::TextDisabled("Click a face in the viewport to select it.");
		return;
	}

	// A bounded preview rather than the full set: a box-select can pick five
	// thousand faces, and rendering five thousand rows would stall the panel
	// for no benefit -- the count above is the number that matters.
	const size_t preview = std::min(m_SelectedFaces.size(), size_t(8));
	size_t shown = 0;
	std::string list;
	for (const int32_t face : m_SelectedFaces)
	{
		if (shown++ >= preview)
			break;
		if (!list.empty())
			list += ", ";
		list += std::to_string(face);
	}
	if (m_SelectedFaces.size() > preview)
		list += ", ...";
 ImGui::TextDisabled("faces: %s", list.c_str());
}

void MeshEditorPanel::DrawOperations()
{
ImGui::Separator();
ImGui::Text("Operations");

	if (ImGui::Button("Delete Faces"))
	{
		m_History.Push(m_Mesh);
		std::vector<int32_t> faces(m_SelectedFaces.begin(), m_SelectedFaces.end());
		if (!m_Mesh.DeleteFaces(faces.data(), static_cast<int32_t>(faces.size())))
		{
			m_Status = "delete failed: an index was out of range";
		}
		else
		{
			m_Status = "deleted " + std::to_string(faces.size()) + " face(s)";
			ClearSelection();
		}
	}

	if (ImGui::Button("Flip Normals"))
	{
		m_History.Push(m_Mesh);
		m_Mesh.FlipNormals();
		m_Status = "normals flipped";
	}

	if (ImGui::Button("Recalculate Normals"))
	{
		m_History.Push(m_Mesh);
		m_Mesh.RecomputeNormals();
		m_Status = "normals recalculated";
	}

	// The epsilon slider is logarithmic: the useful range spans a 2m prop
	// (0.001) and a 2cm miniature (0.00001), and a linear slider that fits one
	// makes the other unreachable.
	float logDistance = std::log10(std::max(m_MergeDistance, 1e-7f));
	if (ImGui::SliderFloat("Merge by Distance", &logDistance, -7.0f, -1.0f,
						   "%.1e", ImGuiSliderFlags_Logarithmic))
	{
		m_MergeDistance = std::pow(10.0f, logDistance);
	}
	if (ImGui::Button("Merge by Distance"))
	{
		m_History.Push(m_Mesh);
		const int32_t removed = m_Mesh.MergeByDistance(m_MergeDistance);
		if (removed < 0)
		{
			m_Status = "merge refused: the distance must be positive";
		}
		else
		{
			m_Status = "merged " + std::to_string(removed) + " vertex(ves)";
			ClearSelection();
		}
	}

	ImGui::Spacing();
	ImGui::Text("Extrude / Inset");
	ImGui::InputFloat3("Delta", m_ExtrudeDelta);
	if (ImGui::Button("Extrude Faces"))
	{
		m_History.Push(m_Mesh);
		std::vector<int32_t> faces(m_SelectedFaces.begin(), m_SelectedFaces.end());
		if (!m_Mesh.ExtrudeFaces(faces.data(), static_cast<int32_t>(faces.size()), m_ExtrudeDelta))
		{
			m_Status = "extrude failed: the selection is empty or an index is out of range";
			// The snapshot stays: the mesh did not change, but a later
			// operation must still be undoable to this state.
		}
		else
		{
			m_Status = "extruded";
			ClearSelection();
		}
	}
	ImGui::InputFloat("Thickness", &m_InsetThickness, 0.01f, 0.1f, "%.3f");
	if (ImGui::Button("Inset Faces"))
	{
		m_History.Push(m_Mesh);
		std::vector<int32_t> faces(m_SelectedFaces.begin(), m_SelectedFaces.end());
		if (!m_Mesh.InsetFaces(faces.data(), static_cast<int32_t>(faces.size()), m_InsetThickness))
		{
			m_Status = "inset failed: the thickness is zero or an index is out of range";
		}
		else
		{
			m_Status = "inset";
			ClearSelection();
		}
	}

	const bool valid = m_Mesh.IsValid();
	if (!valid)
	{
		// Red because this is the one state that means the mesh is broken and
		// the artist should stop and look. Validate() returns every defect, not
		// the first, so the message is complete.
	 ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
	 ImGui::TextWrapped("mesh is invalid: %s", m_Mesh.Validate().c_str());
		ImGui::PopStyleColor();
	}
}

void MeshEditorPanel::DrawSculpt()
	{
	ImGui::Separator();
	ImGui::Text("Sculpt");

	const char *brushes[] = {"Grab", "Smooth", "Inflate", "Flatten", "Crease"};
	int brush = static_cast<int>(m_Brush);
	if (ImGui::Combo("Brush", &brush, brushes, IM_ARRAYSIZE(brushes)))
		m_Brush = static_cast<SculptBrush>(brush);

	ImGui::SliderFloat("Radius", &m_BrushRadius, 0.01f, 5.0f, "%.3f");
	ImGui::SliderFloat("Strength", &m_BrushStrength, 0.0f, 1.0f, "%.2f");

	if (!m_HasPick)
	{
		ImGui::TextDisabled("pick a face to place the brush");
		return;
	}

	ImGui::TextDisabled("at %.3f, %.3f, %.3f", m_LastHit[0], m_LastHit[1],
						 m_LastHit[2]);

	if (ImGui::Button("Apply Brush"))
	{
		m_History.Push(m_Mesh);
		Wasteland::SculptStroke stroke;
		stroke.Brush = m_Brush;
		stroke.Center[0] = m_LastHit[0];
		stroke.Center[1] = m_LastHit[1];
		stroke.Center[2] = m_LastHit[2];
		stroke.Radius = m_BrushRadius;
		stroke.Strength = m_BrushStrength;
		// Grab drags along the brush normal -- the viewport's view direction
		// is not available here, and a grab along a known axis is at least
		// predictable. Recorded as a limitation, not hidden.
		stroke.Direction[0] = 0.0f;
		stroke.Direction[1] = 0.0f;
		stroke.Direction[2] = 1.0f;
		stroke.PlaneNormal[0] = 0.0f;
		stroke.PlaneNormal[1] = 0.0f;
		stroke.PlaneNormal[2] = 1.0f;
		stroke.PlanePoint[0] = m_LastHit[0];
		stroke.PlanePoint[1] = m_LastHit[1];
		stroke.PlanePoint[2] = m_LastHit[2];

		const float mean = Wasteland::SculptStrokeApply(m_Mesh, stroke);
		if (mean < 0.0f)
		{
			m_Status = "brush refused: the radius or strength is invalid";
		}
		else
		{
			m_Status = "brush mean displacement " + std::to_string(mean);
		}
	}
	}

void MeshEditorPanel::DrawHistory()
	{
	ImGui::Separator();
	ImGui::Text("History");

	if (!m_History.CanUndo())
	{
		ImGui::TextDisabled("nothing to undo");
		return;
	}

	ImGui::TextDisabled("%d snapshot(s), depth %d",
						m_History.GetDepth(), m_History.GetMaxDepth());
	if (ImGui::Button("Undo"))
	{
		if (!m_History.Undo(m_Mesh))
		{
			m_Status = "undo failed";
		}
		else
		{
			// An undo can restore a mesh whose selection indices no longer
			// exist, so the selection is dropped rather than clamped -- a
			// clamped index set would silently point at different faces.
			ClearSelection();
			m_Status = "undone";
		}
	}
	}

void MeshEditorPanel::SaveTarget()
{
ImGui::Separator();
ImGui::Text("Save");

	// Default to the SOURCE path's stem with a .obj suffix, so "save" means
	// "leave the engine with an editable OBJ" rather than "pick a name".
	if (m_SavePath.empty())
	{
		std::filesystem::path stem = std::filesystem::path(m_TargetPath).stem();
		m_SavePath = (std::filesystem::path("models") / (stem.string() + ".obj")).string();
	}

	char buffer[512];
	std::snprintf(buffer, sizeof(buffer), "%s", m_SavePath.c_str());
	if (ImGui::InputText("Path", buffer, sizeof(buffer)))
	{
		m_SavePath = buffer;
	}

	if (ImGui::Button("Save OBJ"))
	{
		std::string error;
		const std::filesystem::path absolute =
			Project::Get().GetAssetPath(m_SavePath);
		std::error_code createError;
		std::filesystem::create_directories(absolute.parent_path(), createError);
		if (!MeshWriter::SaveOBJ(absolute, m_Mesh, &error))
		{
			m_Status = "save failed: " + error;
			WL_CORE_WARN("MeshEditorPanel: save failed: {0}", error);
		}
		else
		{
			m_Status = "saved " + m_SavePath;
			WL_CORE_INFO("MeshEditorPanel: wrote {0}", absolute.string());
		}
	}
}

} // namespace Wasteland
