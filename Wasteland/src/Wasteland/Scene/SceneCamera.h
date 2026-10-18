// SceneCamera.h -- the scene-level camera: a projection state holder that the
// Scene collects from every entity carrying a CameraComponent.
//
// Deliberately thin. It owns no transform: position and orientation come from
// the entity's TransformComponent, while this class holds only how the world
// is projected through the lens. Keeping those separate is what lets the editor
// preview from a different place than any entity's gameplay camera.

#pragma once

#include "Wasteland/Renderer/Camera.h"

namespace Wasteland {

	// A camera with a switchable perspective/orthographic projection.
	//
	// Holds two complete sets of projection parameters and swaps between them,
	// rather than keeping one set and overwriting it. That is what makes
	// toggling projection type back and forth lossless -- a game can flip to
	// orthographic for a top-down map and back to perspective without the
	// perspective FOV having been clobbered while it was in orthographic mode.
	//
	// Every mutator calls the private RecalculateProjection, so m_Projection
	// can never drift out of sync with the fields. There is no path that
	// changes a field without rebuilding.
	class SceneCamera : public Camera
	{
	public:
		// Which projection the next RecalculateProjection builds.
		enum class ProjectionType { Perspective = 0, Orthographic = 1 };

	public:
		// Builds the initial projection from the field defaults.
		//
		// WARNING: the resulting projection is DEGENERATE. m_AspectRatio
		// defaults to 0, and with the default Orthographic projection type that
		// makes orthoLeft == orthoRight == 0, so GLM divides by zero and
		// m_Projection contains infinities. A default-constructed SceneCamera
		// renders nothing.
		//
		// This is masked in practice because the Scene calls SetViewportSize
		// on every camera it collects, before any of them is used -- so a
		// CameraComponent is always repaired before it renders. But anything
		// that constructs a SceneCamera and reads its projection without a
		// viewport size gets infinities, not a sensible default. An aspect
		// ratio of 1.0f would make the defaults usable.
		SceneCamera();

		// Virtual so the base Camera can be deleted through a SceneCamera*.
		// Defined inline and defaulted: the base class owns the projection
		// state, so there is nothing to release here.
		virtual ~SceneCamera() = default;

		// Switches to orthographic and sets its parameters in one call.
		//
		// @param size      Vertical extent of the view volume, in world units.
		//                 The horizontal extent is size * aspect ratio, so
		//                 this is the HEIGHT of the visible region, not a
		//                 half-extent and not a radius.
		// @param nearClip  Near depth plane, which may be NEGATIVE -- see the
		//                 clip-plane note on the field declarations below.
		// @param farClip   Far depth plane.
		void SetOrthographic(float size, float nearClip, float farClip);

		// Switches to perspective and sets its parameters in one call.
		//
		// @param verticalFOV  Vertical field of view **IN RADIANS**, despite
		//                   the name. See the warning on the field below --
		//                   this is the single most error-prone value in the
		//                   class.
		// @param nearClip     Near depth plane, in world units. Must be
		//                    positive here; the orthographic path's
		//                    negative-capable convention does not apply.
		// @param farClip      Far depth plane.
		void SetPerspective(float verticalFOV, float nearClip, float farClip);

		// Sets the viewport's pixel dimensions and derives the aspect ratio
		// from them.
		//
		// The only way to set the aspect ratio -- there is no SetAspectRatio --
		// because the ratio is always derived rather than authored.
		//
		// @param width   Viewport width in pixels.
		// @param height  Viewport height in pixels. A height of 0 divides by
		//                 zero and yields an infinite aspect ratio, which
		//                 propagates infinities straight into m_Projection.
		//                 The engine's main camera avoids this because
		//                 Application::OnWindowResize returns early on a 0x0
		//                 (minimized) window rather than resizing; a secondary
		//                 camera or an editor viewport must make the same
		//                 guard itself.
		void SetViewportSize(uint32_t width, uint32_t height);

		// The vertical field of view **in radians**.
		//
		// Despite the name, this is radians, not degrees. It matches the
		// m_PerspectiveFOV default of glm::radians(45.0f) and the raw store in
		// SetPerspective.
		float GetPerspectiveVerticalFOV() const { return m_PerspectiveFOV; }

		// Sets the vertical FOV in radians and rebuilds the projection.
		//
		// Note this is an alias that behaves identically to SetPerspective's
		// FOV argument -- it does NOT switch the projection type to
		// perspective, and does not touch the clip planes. On an orthographic
		// camera it changes a field that is not currently in use, so it has no
		// visible effect until SetProjectionType(Perspective) is called.
		void SetPerspectiveVerticalFOV(float verticalFOV) { m_PerspectiveFOV = verticalFOV; RecalculateProjection(); }

		// Near clip plane for the perspective projection, in world units.
		float GetPerspectiveNearClip() const { return m_PerspectiveNear; }

		// Sets the perspective near clip and rebuilds. See SetPerspective for
		// the unit and the sign convention.
		void SetPerspectiveNearClip(float nearClip) { m_PerspectiveNear = nearClip; RecalculateProjection(); }

		// Far clip plane for the perspective projection, in world units.
		float GetPerspectiveFarClip() const { return m_PerspectiveFar; }

		// Sets the perspective far clip and rebuilds.
		void SetPerspectiveFarClip(float farClip) { m_PerspectiveFar = farClip; RecalculateProjection(); }

		// Vertical extent of the orthographic view volume, in world units.
		// See SetOrthographic for what "size" measures.
		float GetOrthographicSize() const { return m_OrthographicSize; }

		// Sets the orthographic vertical extent and rebuilds.
		void SetOrthographicSize(float size) { m_OrthographicSize = size; RecalculateProjection(); }

		// Near clip plane for the orthographic projection.
		float GetOrthographicNearClip() const { return m_OrthographicNear; }

		// Sets the orthographic near clip and rebuilds.
		void SetOrthographicNearClip(float nearClip) { m_OrthographicNear = nearClip; RecalculateProjection(); }

		// Far clip plane for the orthographic projection.
		float GetOrthographicFarClip() const { return m_OrthographicFar; }

		// Sets the orthographic far clip and rebuilds.
		void SetOrthographicFarClip(float farClip) { m_OrthographicFar = farClip; RecalculateProjection(); }

		// Which projection is currently active.
		ProjectionType GetProjectionType() const { return m_ProjectionType; }

		// Switches projection type and rebuilds using whichever parameter set
		// was last set for it.
		//
		// Lossless in both directions, since the two sets are kept separately --
		// see the class comment for why that matters.
		void SetProjectionType(ProjectionType type) { m_ProjectionType = type; RecalculateProjection(); }

	private:
		// Rebuilds m_Projection from the active type's parameter set.
		//
		// Called by every mutator above and by the constructor, which is what
		// guarantees m_Projection always matches the fields. Never call it
		// with a zero aspect ratio: the orthographic branch computes
		// left/right as size * aspectRatio * 0.5, so a zero aspect gives
		// left == right and GLM's ortho divides by zero to produce infinities.
		void RecalculateProjection();

	private:
		// Which parameter set is live.
		//
		// ORTHOGRAPHIC by default, not perspective. Counterintuitive for a
		// 3D engine, and worth knowing because it interacts badly with the
		// zero default aspect ratio: the default type is exactly the one whose
		// construction path divides by zero.
		ProjectionType m_ProjectionType = ProjectionType::Orthographic;

		// Perspective field of view, IN RADIANS.
		//
		// The radians default is the only thing signalling the unit -- the
		// name says "FOV", which conventionally means degrees, and neither
		// setter's parameter name says radians. Passing 45 expecting 45 degrees
		// stores 45 radians (about 2578 degrees), which is a degenerate
		// projection rather than a wrong-but-usable one. Every caller must
		// pass radians; nothing in the API enforces or states it.
		float m_PerspectiveFOV = glm::radians(45.0f);

		// Perspective clip planes, in world units, both positive.
		//
		// The near default of 0.01 is deliberately small: it lets the camera get
		// very close to geometry without clipping through it, at the cost of
		// depth precision. The far default of 1000 covers a large outdoor scene.
		//
		// Note the ratio: a 0.01-to-1000 depth range is five orders of magnitude,
		// which is where depth-buffer z-fighting shows up first if the far clip
		// is pushed much further.
		float m_PerspectiveNear = 0.01f, m_PerspectiveFar = 1000.0f;

		// Orthographic vertical extent in world units. See SetOrthographic.
		float m_OrthographicSize = 10.0f;

		// Orthographic clip planes, in world units, and NEGATIVE by default.
		//
		// A different convention from the perspective pair above: orthographic
		// here uses a depth slab centred on the camera (default -1 to 1),
		// while perspective uses positive distances in front of it. Callers
		// switching between the two must not assume the clip arguments mean the
		// same thing.
		float m_OrthographicNear = -1.0f, m_OrthographicFar = 1.0f;

		// Viewport aspect ratio (width / height).
		//
		// Zero until SetViewportSize runs, which makes the initial projection
		// degenerate -- see the constructor's warning. A default of 1.0 would
		// make the class usable immediately without any setup.
		float m_AspectRatio = 0.0f;
	};

}