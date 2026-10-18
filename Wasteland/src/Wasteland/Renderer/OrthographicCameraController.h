#pragma once

// The controller owns a camera BY VALUE, so the complete type is needed
// here, not just a forward declaration.
#include "Wasteland/Renderer/OrthographicCamera.h"
// OnUpdate takes a Timestep by value to get the frame delta.
#include "Wasteland/Core/Timestep.h"

// Pulls in WindowResizeEvent, which OnEvent has to recognise.
#include "Wasteland/Events/ApplicationEvent.h"
// MouseScrolledEvent is the other event this controller consumes; zoom
// is the only camera control bound to the mouse wheel here.
#include "Wasteland/Events/MouseEvent.h"

// Needed for the glm::vec3 member holding the camera position.
#include <glm/glm.hpp>

namespace Wasteland {

	// A plain aggregate describing the world-space rectangle the camera
	// looks at. Held by value in m_Bounds, and GetBounds hands out a const
	// reference to it rather than a copy.
	struct OrthographicCameraBounds
	{
		// Two floats on one line, uninitialised. There is no constructor, so
		// these are indeterminate until the constructor or OnResize writes
		// them -- a latent trap if a bounds object is ever default-constructed.
		float Left, Right;
		// Same uninitialised-pair situation as line 15.
		float Bottom, Top;

		// Derives the visible width in world units. Not const, so this cannot
		// be called on a const bounds object even though it mutates nothing --
		// an inconsistency with GetBounds, which does return a const ref.
		float GetWidth() { return Right - Left; }
		// Same missing const as GetWidth.
		float GetHeight() { return Top - Bottom; }
	};

	// Owns an OrthographicCamera plus the input state that drives it.
	// This is the type Sandbox2D holds, so pan/zoom are wired up by
	// construction rather than by each caller reimplementing them.
	class OrthographicCameraController
	{
	public:
		// aspectRatio is width/height, kept separately so a window resize can
		// re-derive the bounds without reconstructing the controller.
		// rotation defaults to off: 2D cameras are normally axis-aligned and
		// rotating one would need the rotation event handling enabled too.
		// DEFECT: m_AspectRatio (line 44) has no in-class initialiser and the
		// constructor is declared here, so it relies entirely on the .cpp to
		// assign it.
		OrthographicCameraController(float aspectRatio, bool rotation = false);

		// Per-frame integration of movement. Taking the Timestep is what makes
		// the pan speed frame-rate independent.
		// Not virtual: this type is a plain mixin, not a Layer.
		void OnUpdate(Timestep ts);
		// Single funnel for all events; the per-type handlers below filter.
		// Non-const reference because the handlers are expected to mark events
		// handled rather than just observe them.
		void OnEvent(Event& e);

		// Re-derives bounds from a new pixel size, keeping the world-space
		// height and zoom by solving for the width via the aspect ratio.
		void OnResize(float width, float height);

		// Mutable access: the caller gets the real camera and can set position
		// or rotation directly, which will then be overwritten by the next
		// CalculateView().
		OrthographicCamera& GetCamera() { return m_Camera; }
		// Const overload: selected automatically for const controllers, so a
		// read-only user cannot accidentally perturb the camera.
		// The pair returns the same member, so the two can never disagree.
		const OrthographicCamera& GetCamera() const { return m_Camera; }

		// Zoom is stored separately from the camera (which has no zoom notion
		// at all) and folded into the view bounds in CalculateView.
		float GetZoomLevel() const { return m_ZoomLevel; }
		// Setting zoom invalidates the camera matrices, hence the trailing
		// CalculateView(). A setter that skipped it would render at the old
		// zoom until the next input-driven recalculation.
		// No clamping here: a zero or negative level would collapse or invert
		// the ortho box. Worth noting as a missing guard.
		void SetZoomLevel(float level) { m_ZoomLevel = level; CalculateView(); }
		// Reference return, so callers see live bounds rather than a snapshot.
		// Consistent with the const-correctness Camera.h uses, but NOT with
		// GetWidth/GetHeight on lines 18-19, which are non-const.
		const OrthographicCameraBounds& GetBounds() const { return m_Bounds; }
	// Implementation details; a controller is meant to be driven through
	// OnUpdate/OnEvent, not poked directly.
	private:
		// The single place that turns (position, rotation, zoom, bounds) into
		// camera matrices. Funnelling all of it here is what keeps
		// m_ViewProjectionMatrix consistent -- see the note in
		// OrthographicCamera.h about SetProjection not refreshing that cache.
		void CalculateView();

		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnWindowResized(WindowResizeEvent& e);
	// Second consecutive private: label, directly after the one on line 38.
	// Redundant but harmless.
	private:
		// width/height, cached so OnResize can recompute bounds without being
		// handed the ratio. Written by the constructor and OnResize.
		float m_AspectRatio;
		// 1.0 means "show the full bounds"; lower values zoom in. In-class
		// initialiser so the camera is usable before any input arrives.
		float m_ZoomLevel = 1.0f;
		OrthographicCameraBounds m_Bounds;
		OrthographicCamera m_Camera;

		// Constructor flag, not in-class initialised, so it is only valid once
		// the constructor has run -- which is why it must be assigned there
		// rather than defaulted here like m_ZoomLevel is.
		bool m_Rotation;

		// The controller mirrors the camera position rather than reading it
		// back from m_Camera. That duplicated state is a consistency hazard:
		// anything that calls GetCamera().SetPosition() directly would leave
		// these two out of step.
		glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
		float m_CameraRotation = 0.0f;
		// Two tunables declared on one line. Units differ: translation is
		// world units per second, rotation is degrees per second.
		// Neither is exposed through a setter, so these are compile-time
		// constants in practice despite being non-const members.
		float m_CameraTranslationSpeed = 5.0f, m_CameraRotationSpeed = 180.0f;
	};

}