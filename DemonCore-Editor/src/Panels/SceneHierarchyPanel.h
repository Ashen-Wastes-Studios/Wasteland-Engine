// Editor-only: this panel lives in the editor target, not the engine.
#pragma once

// Core types: Ref, the smart handle used throughout, plus the shared
// definitions. Quoted so the engine's own include layout is honoured.
#include "Wasteland/Core/Core.h"
// Logging for the diagnostics this panel emits while walking a scene.
#include "Wasteland/Core/Log.h"
// The scene graph itself, and Entity -- the handle a selection is expressed
// in. Both are needed in the header because the panel STORES them.
#include "Wasteland/Scene/Scene.h"
#include "Wasteland/Scene/Entity.h"

// In the engine namespace rather than the editor's, so this panel uses the
// same types as the runtime it inspects.
namespace Wasteland {

		// The outliner plus the properties view, as one panel object. The two are
		// toggled independently (see the visibility accessors below) but share the
		// selection, which is why they are not two classes.
	class SceneHierarchyPanel
	{
		// Defaulted: a panel with no scene still constructs, so it can be created
		// before a project is loaded. OnImGuiRender then has nothing to walk.
	public:
				// The constructing overload takes the scene by Ref, so the panel shares
				// ownership rather than holding a pointer the project could invalidate.
		SceneHierarchyPanel() = default;
		SceneHierarchyPanel(const Ref<Scene>& context);

				// Swaps the scene the panel displays. The previous Ref is released when
				// this returns.
				// ---
				// DEFECT (documented, not fixed): this does NOT clear m_SelectionContext.
				// A selection made in the old scene survives the swap as a handle into a
				// scene the panel no longer shows. Whether that is dangerous depends on
				// how Entity is defined -- if it is a pointer, the properties view will
				// read freed memory for whatever was selected.
		void SetContext(const Ref<Scene>& context);

				// The ImGui pass. No Event& and no update tick: this is a pure function of
				// engine state, called once per frame by the editor layer.
		void OnImGuiRender();

				// The visibility pair. Plain bools rather than an enum or a bitmask, so a
				// caller toggling one does not have to read-modify-write a mask.
		bool IsHierarchyVisible() const { return m_HierarchyVisible; }
		void SetHierarchyVisible(bool visible) { m_HierarchyVisible = visible; }
		bool IsPropertiesVisible() const { return m_PropertiesVisible; }
		void SetPropertiesVisible(bool visible) { m_PropertiesVisible = visible; }

				// Returns the selection BY VALUE. Defensible only because Entity is a small
				// handle; if it is a pointer this is a copy of a pointer, which is fine.
				// If it were a struct wrapping a pointer plus an id it would start costing
				// more than the accessor saves.
		Entity GetSelectedEntity() const { return m_SelectionContext; }
		void SetSelectedEntity(Entity entity);
		// First of two `private:` sections, one for methods and one for data. The
		// split has no effect in C++ -- both are the same access -- but it does
		// separate the two kinds of member by sight.
	private:
				// One recursive call per entity to draw the tree row; the recursion into
				// children lives in the .cpp.
		void DrawEntityNode(Entity entity);
				// Draws the component list for whatever is currently selected, which is
				// why it takes no Entity argument: it reads m_SelectionContext itself.
		void DrawComponents(Entity entity);
	private:
				// A Ref, so the panel keeps the scene alive. This is the reason the class
				// can be constructed with no scene and filled in later -- the default is a
				// null Ref that every method must tolerate.
		Ref<Scene> m_Context;
				// The current selection. DEFECT (documented, not fixed): it is NOT
				// initialised here, so it default-constructs. If Entity's default
				// constructor leaves an invalid handle rather than a null one, that is
				// what SetSelectedEntity must be expected to replace before any draw.
		Entity m_SelectionContext;
				// Both panels start visible. A fresh editor opens with the outliner shown,
				// which is the useful default for someone opening the app.
		bool m_HierarchyVisible = true;
		bool m_PropertiesVisible = true;
	};

}