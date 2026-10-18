// ScriptInspectorPanel.h -- the editor panel that shows and edits the live
// instance variables of a selected entity's Python script.
//
// It is a thin state holder: the entity to inspect plus a visibility flag.
// All drawing lives in OnImGuiRender() (ScriptInspectorPanel.cpp), which
// reaches back into the script engine to read and write pybind11 objects.
// EditorLayer drives both halves -- SetEntity() every frame from the scene
// hierarchy selection, OnImGuiRender() only while the Scripting workflow is
// active.
#pragma once

// Entity by value, not by reference or handle: it is just an entt handle plus
// a Scene back-pointer, so copying is cheap and the default-constructed
// member below stays a safe "nothing selected" state.
#include "Wasteland/Scene/Entity.h"

namespace Wasteland
{


	// No base class and no interface: the editor owns this panel by value as an
	// EditorLayer member (EditorLayer.h: m_ScriptInspectorPanel), so it needs no
	// lifetime management of its own.
	//
	// The indentation in this header is one tab deeper than the namespace's --
	// cosmetic, but consistent with the rest of the editor panel headers.
	class ScriptInspectorPanel
	{

	public:
		// defaulted rather than user-provided: both members have initialisers, so a
		// default construction is already well-defined. Declaring it explicitly
		// documents that the panel is cheap to default-construct in-place.
		ScriptInspectorPanel() = default;


		// Takes the Entity BY VALUE, then copy-assigns. The copy is two words, so the
		// parameter could just as well be a const reference; as written it lets the
		// caller pass a temporary from GetSelectedEntity() without a named local.
		//
		// No validity check here. OnImGuiRender() calls m_Entity.IsValid() before
		// touching components, which is what makes a stale or default entity safe.
		void SetEntity(Entity entity) { m_Entity = entity; }
		// Defined in the .cpp because it pulls in ImGui and pybind11, which this
		// header deliberately does not.
		//
		// Called directly by EditorLayer rather than being driven by the Layer
		// system -- the panel is a member of EditorLayer, not a layer of its own.
		void OnImGuiRender();

		// Read by EditorLayer to decide whether to draw the menu checkbox for this
		// panel (EditorLayer.cpp:1060-1062), which then writes it back through
		// SetVisible -- so this flag is the single source of truth for the panel's
		// on-screen state.
		bool IsVisible() const { return m_Visible; }
		// Plain setter, no side effects. It does NOT open or close an ImGui window;
		// m_Visible is also passed by address to ImGui::Begin() in the .cpp, so the
		// panel's own close-button writes this member directly too.
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		// Default-constructed to entt::null, which is exactly the "no selection"
		// sentinel that Entity::operator bool and IsValid() test. That is what lets
		// this header omit any explicit initialiser.
		Entity m_Entity;
		// NSDMI, so the panel is visible by default -- the editor shows it as soon as
		// the Scripting workflow is entered, rather than requiring a first toggle.
		bool m_Visible = true;
	};

}
