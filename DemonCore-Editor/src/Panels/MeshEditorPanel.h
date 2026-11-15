// MeshEditorPanel -- sub-object mesh editing.
//
// WHY THIS PANEL EXISTS SEPARATELY FROM THE VIEWPORT
// -------------------------------------------------
// The editor already picks ENTITIES by pixel readback (EditorLayer reads a
// pixel from the entity-ID attachment). That answers "which entity", not
// "which face", and sub-object editing needs the second answer. So this panel
// owns the per-face raycast (engine MeshPicking), the sub-object selection
// sets, and the modelling operations.
//
// The mesh is loaded from the target entity's own MeshComponent::MeshPath --
// the panel does not go through Scene's MeshAssetCache, because that cache
// stores ProceduralMesh (the renderer's parallel-vector layout) and this panel
// needs the editable half-edge structure. Loading a second copy is the cost of
// that; it is not shared mutable state, so the renderer's cached mesh cannot
// be corrupted by an edit.
//
// VERIFICATION STATUS: this panel has NEVER been drawn. The editor crashes at
// startup on the machine this was written on (DX12 device removed, 0x887a0006),
// so nothing here has been seen on screen. The picking and the operations it
// calls are unit-tested headlessly (tests/test_mesh_editing.py); the ImGui
// wiring, the toolbar, the gizmo placement and the save path are not.
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "Wasteland/Mesh/EditableMesh.h"
#include "Wasteland/Mesh/MeshEditHistory.h"
#include "Wasteland/Mesh/MeshSculpt.h"
#include "Wasteland/Scene/Entity.h"

namespace Wasteland
{

	class Scene;

	enum class MeshEditMode
	{
		Object = 0,
		Vertex,
		Edge,
		Face,
	};

	const char *MeshEditModeName(MeshEditMode mode);

	class MeshEditorPanel
	{
	public:
		MeshEditorPanel() = default;

		void OnImGuiRender(Scene &scene);

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		MeshEditMode GetMode() const { return m_Mode; }

		// The entity being edited, or a default-constructed Entity() when the
		// panel has no target. A panel with no target is a valid state (nothing
		// selected), not an error.
		Entity GetTargetEntity() const { return m_TargetEntity; }
		bool HasTarget() const { return m_HasTarget; }

		// The edited mesh, for the viewport gizmo and for tests. Null when no
		// target is loaded.
		const EditableMesh *GetMesh() const { return m_HasTarget ? &m_Mesh : nullptr; }

		size_t GetSelectedCount() const;

		// Clear the sub-object selection. Exposed so the viewport's escape key
		// and the panel's own button share one path.
		void ClearSelection();

		// Records a pick from the viewport. `additive` is shift-click: it
		// toggles the face into the selection instead of replacing it. The hit
		// point is kept because the sculpt brushes act where the cursor is,
		// and re-deriving it from the face index would mean intersecting twice.
		//
		// The VIEWPORT DOES NOT CALL THIS YET. The mechanism exists and is
		// declared so the viewport wiring has a single, explicit entry point
		// rather than reaching into the selection set; until the viewport
		// calls it, no click can populate the selection.
		void SelectFace(int32_t face, const float hitPoint[3], bool additive);

	private:
		bool LoadTarget(Scene &scene);
		void DrawTargetPicker(Scene &scene);
		void DrawToolbar();
		void DrawSelection();
		void DrawOperations();
		void DrawHistory();
		void DrawSculpt();
		void SaveTarget();

		bool m_Visible = false;

		// One-shot, not a per-frame SetNextWindowSize: the panel is re-docked on
		// every layout switch and an unconditional size would fight the dock
		// node for space on every frame after the first.
		bool m_SizedOnce = false;

		Entity m_TargetEntity;
		bool m_HasTarget = false;
		std::string m_TargetPath;

		EditableMesh m_Mesh;
		std::string m_LoadError;

		MeshEditMode m_Mode = MeshEditMode::Object;
		// The state BEFORE the last operation. A snapshot, not an inverse:
		// every op here re-welds and permutes vertex/edge order, so an inverse
		// would be reconstructing a mesh its delta no longer describes.
		MeshEditHistory m_History{16};

		float m_ExtrudeDelta[3] = {0.0f, 0.0f, 1.0f};
		float m_InsetThickness = 0.1f;

		// Sculpt state. The stroke centre is the LAST PICK's hit point: a
		// brush has to act where the cursor is, and the panel already has that
		// from the per-face pick. A brush with no pick yet has nothing to act
		// on, which is a state the UI must be able to show rather than guess.
		SculptBrush m_Brush = SculptBrush::Smooth;
		float m_BrushRadius = 0.5f;
		float m_BrushStrength = 0.5f;
		bool m_HasPick = false;
		float m_LastHit[3] = {0.0f, 0.0f, 0.0f};

		// Sub-object selection. Face indices in all modes: in Vertex mode the
		// faces touching a selected vertex, in Edge mode the faces touching a
		// selected edge. Storing one representation keeps the operations
		// (delete/merge/flip) mode-independent and keeps a lasso and a click
		// able to add to the same set.
		std::set<int32_t> m_SelectedFaces;

		// Merge-by-distance radius, in the asset's own units. Exposed as a
		// slider rather than a constant: the right epsilon is mesh-dependent
		// (a 1mm weld on a 2m prop is a no-op; 1mm on a 2cm miniature is a
		// collapse), and a hardcoded value would be wrong for one of them.
		float m_MergeDistance = 0.001f;

		bool m_ShowWireframe = true;
		bool m_ShowNormals = false;

		std::string m_Status;

		// The path the next SaveTarget writes to. Empty means "same as the
		// source", which is what an artist editing an imported prop wants.
		std::string m_SavePath;
	};

} // namespace Wasteland
