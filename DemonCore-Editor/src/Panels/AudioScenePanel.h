// AudioScenePanel -- every audio component in the open scene.
//
// The scene hierarchy lists entities, and the properties view edits the
// components of ONE selected entity. Neither answers "where is the sound in this
// level": a gunshot wired to the wrong clip, a listener on the wrong entity, a
// 3D source with MaxDistance smaller than its MinDistance. This panel is that
// audit, and it is the reason an audio workflow is worth a layout.
//
// Read-only. The panel has no editor controls on purpose: changing an
// AudioSourceComponent here would duplicate the properties view and let the two
// disagree, and the mixer's runtime state (the voice table) is not editable from
// a scene view at all. What it does offer is the one action that belongs to a
// LIST of sources -- focus the entity in the hierarchy, so the component can be
// edited where it is already editable.
#pragma once

// Entity is stored by value below, so the complete type is required -- the same
// reasoning SceneHierarchyPanel gives for including it in its own header.
#include "Wasteland/Scene/Entity.h"

#include <string>
#include <vector>

namespace Wasteland
{

	// One audio-bearing entity as the panel needs it. Flat rather than an
	// Entity handle plus a re-lookup, so the row is a point-in-time fact about
	// the scene rather than a live view into a registry that may change.
	struct AudioSourceRow
	{
		// Display name, from TagComponent. Falls back to a UUID-derived label
		// when the entity has no tag -- an unnamed entity is a real state and the
		// row must still be identifiable. A short UUID fragment is used rather
		// than the entity index because the index is not stable across a destroy.
		std::string EntityName;

		// Which component this row is about. A scene can legitimately hold both on
		// one entity, and they are different things: a source makes sound, a
		// listener hears it.
		bool IsListener = false;

		// Source fields. Only meaningful when IsListener is false.
		std::string ClipName;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		int Priority = 0;
		bool Spatial = false;
		float MinDistance = 0.0f;
		float MaxDistance = 0.0f;
		bool PlayOnStart = false;

		// Listener's Enabled, reused in the source rows' Loop slot when
		// IsListener is true. Two fields would be clearer and one is cheaper to
		// keep correct; the flags column is the only reader of this in that case.
		bool ListenerEnabled = true;

		// True when the authoring values contradict each other. Surfaced as a
		// warning rather than silently corrected: the panel does not know whether
		// an inverted distance window is a mistake or an intentional omnipresent
		// 3D sound, and guessing is how a level loses a sound.
		bool HasDistanceWarning = false;

		// True when ClipName is set but the mixer has no such clip. This is the
		// single most common "why is there no sound" cause, and it is checkable
		// without playing anything.
		bool ClipMissing = false;

		// The entity this row came from, so the panel can offer to focus it.
		Entity Handle;
	};

	// The scene-wide audio audit.
	class AudioScenePanel
	{
	public:
		AudioScenePanel() = default;

		void OnImGuiRender(class Scene &scene);

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		// The last entity the panel was asked to focus, or a default handle.
		// Read by the layer, which owns the hierarchy panel's selection.
		const Entity &GetPendingFocus() const { return m_PendingFocus; }
		void ClearPendingFocus() { m_PendingFocus = Entity(); }

	private:
		void Capture(Scene &scene);

		bool m_Visible = false;

		// One-shot, so the panel's first size is applied once rather than every
		// frame: it is re-docked on every layout switch.
		bool m_SizedOnce = false;

		// Listeners hidden by default. A scene with one listener among forty
		// sources makes the sources hard to scan, and the listener is rarely the
		// thing being debugged.
		bool m_HideListeners = false;

		std::vector<AudioSourceRow> m_Rows;

		// Counts, so the header can say what the scene contains without the user
		// counting rows.
		int m_SourceCount = 0;
		int m_ListenerCount = 0;
		int m_MissingClipCount = 0;

		Entity m_PendingFocus;

		std::string m_Status;
	};

}
