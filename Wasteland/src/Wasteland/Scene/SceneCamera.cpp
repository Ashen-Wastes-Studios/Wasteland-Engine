#include "wlpch.h"
#include "SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Wasteland {

	// Builds the initial projection from the field defaults.
	//
	// Calling RecalculateProjection here -- rather than leaving m_Projection
	// to be built lazily -- means the projection is at least consistent with
	// the defaults from the first moment. It is NOT, however, usable: with
	// m_AspectRatio at its 0.0f default and m_ProjectionType at
	// Orthographic, this takes the orthographic branch below and GLM's ortho
	// divides by (right - left) == 0, leaving m_Projection full of infinities.
	//
	// Nothing crashes from that -- infinities are just numbers -- but any
	// vertex transformed by it produces NaN, so a camera used before
	// SetViewportSize renders nothing at all. The Scene repairs this by
	// calling SetViewportSize on every camera it collects, which is why it has
	// never surfaced as a visible bug.
	SceneCamera::SceneCamera()
	{
		RecalculateProjection();
	}

	// Switches to orthographic and sets its parameters in one call.
	//
	// Sets the type first so the single trailing RecalculateProjection builds
	// the orthographic projection, not the outgoing one -- one rebuild rather
	// than two.
	//
	// @param size      Vertical extent of the view volume, in world units.
	//                 The horizontal extent becomes size * aspect ratio, so
	//                 this is the visible height, not a half-extent.
	// @param nearClip  Near depth plane. May be negative -- this class's
	//                 orthographic path uses a depth slab centred on the
	//                 camera, unlike the perspective pair.
	// @param farClip   Far depth plane.
	void SceneCamera::SetOrthographic(float size, float nearClip, float farClip)
	{
		// Type first, so the rebuild at the end uses the orthographic branch.
		m_ProjectionType = ProjectionType::Orthographic;

		// Stored raw -- no unit conversion on any of the three. `size` is in
		// world units and the clip planes are in world units, both taken as
		// given.
		m_OrthographicSize = size;
		m_OrthographicNear = nearClip;
		m_OrthographicFar = farClip;

		RecalculateProjection();
	}

	// Switches to perspective and sets its parameters in one call.
	//
	// @param verticalFOV  Vertical field of view **IN RADIANS**. Stored raw:
	//                    no degree conversion happens here, and the field's
	//                    own default is glm::radians(45.0f), which is the only
	//                    thing that reveals the unit. Passing 45 meaning
	//                    degrees yields a ~2578-degree projection.
	// @param nearClip     Near depth plane, positive, in world units.
	// @param farClip      Far depth plane, in world units.
	void SceneCamera::SetPerspective(float verticalFOV, float nearClip, float farClip)
	{
		// Type first, for the same reason as the orthographic setter.
		m_ProjectionType = ProjectionType::Perspective;

		// Stored raw. See the warning above -- the FOV unit is undocumented
		// anywhere except the field's default value.
		m_PerspectiveFOV = verticalFOV;
		m_PerspectiveNear = nearClip;
		m_PerspectiveFar = farClip;

		RecalculateProjection();
	}

	// Sets the viewport's pixel dimensions and derives the aspect ratio.
	//
	// The only route to m_AspectRatio; there is no SetAspectRatio, so the ratio
	// is always derived from real dimensions rather than authored independently
	// and drifting out of step with them.
	//
	// A height of 0 divides by zero here and produces an infinite aspect
	// ratio, which then poisons m_Projection. The engine's main camera is
	// protected by Application::OnWindowResize returning early on a 0x0
	// (minimized) window; anything else calling this with a possibly-zero
	// height needs its own guard.
	//
	// @param width   Viewport width in pixels.
	// @param height  Viewport height in pixels.
	void SceneCamera::SetViewportSize(uint32_t width, uint32_t height)
	{
		// Both operands are cast to float BEFORE the division. Without the
		// casts this would be integer division, so 1600x900 would yield an
		// aspect ratio of 1 instead of 1.78 -- a subtly wrong projection
		// rather than an obvious one.
		//
		// A zero width or height is rejected outright. Division by zero gives
		// an infinite (or NaN, for 0/0) aspect ratio, and glm asserts on that
		// inside perspective* -- which aborted the whole editor when a scene
		// was loaded before its first viewport resize, because the camera was
		// still at the initial 0x0 viewport size. Keeping the previous aspect
		// and returning leaves a sane projection for the first real frame.
		if (width == 0 || height == 0)
			return;

		m_AspectRatio = (float)width / (float)height;

		RecalculateProjection();
	}

	// Rebuilds m_Projection from the active parameter set.
	//
	// Requires a non-zero m_AspectRatio in the orthographic case; see the
	// constructor's note on why the defaults violate that.
	void SceneCamera::RecalculateProjection()
	{
		// Explicitly `if`/`else if` rather than a switch, so adding an
		// enumerator would produce a compiler warning about an unhandled
		// case. Here, an out-of-range type would silently leave m_Projection
		// stale from the previous rebuild instead.
		if (m_ProjectionType == ProjectionType::Perspective)
		{
			// GLM's perspective takes radians and an aspect ratio directly,
			// which is why the FOV field is radians and why the ratio has to be
			// computed rather than passed as pixel dimensions.
			//
			// A zero aspect ratio here would collapse the horizontal scale to
			// zero: not a division by zero, but a projection that maps every
			// point onto the vertical axis.
			m_Projection = glm::perspective(m_PerspectiveFOV, m_AspectRatio, m_PerspectiveNear, m_PerspectiveFar);
		}
		else if (m_ProjectionType == ProjectionType::Orthographic)
		{
			// Orthographic has no lens, so there is no FOV -- the view volume is
			// defined directly as a box, derived from `size` and the aspect
			// ratio.
			//
			// `size` is the VERTICAL extent, halved here, so the full height is
			// size. Horizontal follows the aspect ratio to keep the box square
			// in pixels: a wider window sees more world horizontally, which is
			// what keeps the projection from stretching.
			//
			// This is the division-by-zero site: with m_AspectRatio at 0, both
			// left and right evaluate to 0, and GLM's ortho computes
			// 2 / (right - left).
			float orthoLeft = -m_OrthographicSize * m_AspectRatio * 0.5f;
			float orthoRight = m_OrthographicSize * m_AspectRatio * 0.5f;
			float orthoBottom = -m_OrthographicSize * 0.5f;
			float orthoTop = m_OrthographicSize * 0.5f;

			// The vertical pair is independent of the aspect ratio, so bottom
			// and top stay finite even in the degenerate case above -- it is
			// only the horizontal pair that collapses.
			m_Projection = glm::ortho(orthoLeft, orthoRight,
				orthoBottom, orthoTop, m_OrthographicNear, m_OrthographicFar);
		}
	}

}