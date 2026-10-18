#pragma once

// The base class. It carries ONLY a projection matrix and knows nothing
// about a view or a position -- the entire view state lives here.
// Included rather than forward-declared so the in-class member defaults
// below compile.
#include "Camera.h"
// The frame delta, used by OnUpdate. Small header pulled in for one
// parameter type.
#include "Wasteland/Core/Timestep.h"
// The event base plus the mouse event type OnEvent dispatches. The concrete
// mouse event is needed in the HEADER, not just the .cpp, because
// OnMouseScroll takes a MouseScrolledEvent& and that signature is
// declared here.
#include "Wasteland/Events/Event.h"
#include "Wasteland/Events/MouseEvent.h"

// glm, for the mat4/vec3/quat members below.
#include <glm/glm.hpp>

namespace Wasteland {

		// An orbit camera: it always looks at m_FocalPoint from m_Distance away,
		// with the position DERIVED from the focal point, pitch, yaw and distance
		// rather than integrated over time. That is why there is no velocity, no
		// damping and no inertia anywhere in this class.
		// ---
		// An editor camera rather than a game camera: it takes raw mouse deltas,
		// uses a hardcoded left-ALT modifier, and exposes no way to script it.
		// A game would use OrthographicCameraController for the 2D equivalent.
	class EditorCamera : public Camera
	{
	public:
				// Defaulted: leaves the class at its in-class member defaults. DEFECT
				// (documented, not fixed): m_ViewMatrix is a glm::mat4 with no initialiser,
				// so a default-constructed EditorCamera has an UNINITIALISED view matrix
				// until the first UpdateView() call. The only caller in the tree constructs
				// with explicit parameters, so this is latent rather than live.
		EditorCamera() = default;
				// The real constructor. DEFECT (documented, not fixed): it takes an
				// aspectRatio but ALSO derives the aspect from the viewport in
				// UpdateProjection, and the two can disagree. The constructor seeds the
				// base class projection from this argument; the first
				// SetViewportSize() call then silently replaces it.
		EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

				// The per-frame tick. Recomputes the view matrix unconditionally at the
				// end, so the matrix is always current even when no input arrived.
		void OnUpdate(Timestep ts);
				// Takes the event by non-const reference because EventDispatcher dispatches
				// through it and may mark it handled.
		void OnEvent(Event& e);

				// Distance to the focal point, with a setter that does NO clamping.
				// SetDistance therefore accepts 0 or negative values, which collapses the
				// camera onto its focal point. The setter is unused by the .cpp --
				// MouseZoom is the only writer -- so the hazard is not currently reachable
				// from the editor.
		inline float GetDistance() const { return m_Distance; }
		inline void SetDistance(float distance) { m_Distance = distance; }

				// Both assignments then ONE call to UpdateProjection. Splitting it this
				// way matters: recomputing per assignment would derive the aspect ratio
				// from a half-updated pair.
				// ---
				// DEFECT (documented, not fixed): there is no guard here against a zero or
				// negative viewport dimension, but UpdateProjection itself bails out on
				// those. The values are still STORED, so a later successful call computes
				// correctly -- the early return in UpdateProjection is what protects the
				// projection, not this setter.
		inline void SetViewportSize(float width, float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

				// The cached view matrix, by reference so callers avoid a 64-byte copy.
				// It is recomputed every OnUpdate, so a caller that caches it across a
				// frame boundary gets a stale matrix -- but never an uninitialised one,
				// because the constructor calls UpdateView.
		const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
				// Returns BY VALUE, unlike GetViewMatrix above. The multiply is a fresh
				// mat4 each call, so a reference would need somewhere to store it; the
				// copy is 64 bytes and this is called once per view, not per vertex.
		glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

				// The three basis vectors, computed on demand by rotating the local axes
				// through the current orientation. Returning by value is correct here
				// because there is no cache to alias.
		glm::vec3 GetUpDirection() const;
		glm::vec3 GetRightDirection() const;
		glm::vec3 GetForwardDirection() const;
		const glm::vec3& GetPosition() const { return m_Position; }
				// A quaternion rather than a matrix, because the rotation is stored as
				// pitch/yaw floats and converted on demand. Rebuilding the quaternion from
				// Euler angles on every call is redundant work -- MouseRotate updates only
				// pitch and yaw and the camera's view matrix is only rebuilt at the end of
				// OnUpdate, so this runs several times per frame with the same result.
		glm::quat GetOrientation() const;

				// Exposed for editor UI that shows the camera's angles. Read-only, and
				// deliberately so: writing m_Yaw directly would skip the yawSign logic in
				// MouseRotate that keeps rotation from flipping.
		float GetPitch() const { return m_Pitch; }
		float GetYaw() const { return m_Yaw; }
	private:
				// UpdateProjection rebuilds m_Projection in the base class from the FOV,
				// the viewport-derived aspect ratio and the clip planes.
		void UpdateProjection();
				// UpdateView rebuilds m_ViewMatrix from the focal point, the derived
				// position and the orientation.
		void UpdateView();

				// A private event handler, reached through the EventDispatcher in OnEvent.
				// Kept out of the public surface because it takes a concrete event type.
		bool OnMouseScroll(MouseScrolledEvent& e);

				// Pan moves the focal point in the camera's own right/up plane, so the
				// direction of travel follows where the camera is looking rather than
				// world axes. That is what makes panning feel like dragging the scene.
		void MousePan(const glm::vec2& delta);
				// Rotate changes pitch and yaw. Neither is clamped, so repeated rotation
				// can pass vertical -- the view will flip rather than stop.
		void MouseRotate(const glm::vec2& delta);
				// Zoom changes the distance. This is the one movement helper that also
				// MOVES the focal point when it hits the floor, which is how a zoom-in past
				// the limit slides along the ground plane instead of stopping dead.
		void MouseZoom(float delta);

				// The eye position derived from the four orbit parameters. Everything else
				// in the class is a consequence of this one function.
		glm::vec3 CalculatePosition() const;

				// Pan and rotation speed both scale with the viewport, so panning feels
				// the same speed in editor units on a small and a large window.
		std::pair<float, float> PanSpeed() const;
		float RotationSpeed() const;
		float ZoomSpeed() const;
	private:
				// Projection parameters. The defaults describe a 45-degree field of view at
				// a 16:9 aspect with a 0.1 to 10000 depth range -- a 10km far plane, matching
				// SceneCamera's defaults so the editor preview and a scene camera agree on
				// how far the world extends.
				// ---
				// DEFECT (documented, not fixed): m_ViewportWidth and m_ViewportHeight at
				// line 64 are declared as FLOAT but initialised with integer literals
				// (1280, 720). That is not a defect on its own -- the literals convert --
				// but the pairing of float declaration with integer defaults is the kind
				// of thing that hides a truncation when the initialiser is later edited to
				// something like a division result.
		float m_FOV = 45.0f, m_AspectRatio = 1.778f, m_NearClip = 0.1f, m_FarClip = 10000.0f;

				// The cached view matrix. Never handed out as a mutable reference, so the
				// only way to change it is UpdateView.
		glm::mat4 m_ViewMatrix;
				// The orbit target and the eye position. Only m_Position is derived (in
				// UpdateView); m_FocalPoint is the state, m_Position is the consequence.
				// Storing both costs a vec3 and risks the two disagreeing if anything ever
				// writes m_Position directly -- nothing currently does.
		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 m_FocalPoint = { 0.0f, 0.0f, 0.0f };

				// Where the middle mouse button was when the drag began. Written by
				// OnUpdate every frame while ALT is held, so it is a one-frame-old anchor
				// rather than a stored drag origin.
		glm::vec2 m_InitialMousePosition;

				// The orbit radius, with a floor enforced in MouseZoom rather than here.
		float m_Distance = 10.0f;
				// Pitch and yaw in radians, negated in GetOrientation (see the .cpp), so
				// the sign convention lives in exactly one place.
		float m_Pitch = 0.0f, m_Yaw = 0.0f;

				// Viewport size, used for the aspect ratio and for pan speed. DEFECT
				// (documented, not fixed): m_AspectRatio above is DEAD after the first
				// SetViewportSize call -- UpdateProjection overwrites it from these two.
				// The in-class default of 1.778f is only ever used if SetViewportSize is
				// never called.
		float m_ViewportWidth = 1280, m_ViewportHeight = 720;
	};

}