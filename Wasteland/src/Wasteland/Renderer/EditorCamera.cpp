// Precompiled header, and it must stay first: the engine's wlpch.h comment
#include "wlpch.h"
// The class under implementation. Pulled in before the project headers
#include "EditorCamera.h"

// Input is the only engine service this .cpp talks to directly; everything
// else comes through the glm or standard headers.
#include "Wasteland/Core/Input.h"
// For WL_KEY_LEFT_ALT, the modifier that gates all three drag gestures in
#include "Wasteland/Core/KeyCodes.h"
#include "Wasteland/Core/MouseButtonCodes.h"

// GLFW is included but NEVER referenced in this file -- every input query
// goes through the Input facade. DEFECT (documented, not fixed): a dead
// third-party include. It costs compile time and re-exposes GLFW macros
// (GLFW_TRUE, GLFW_KEY_*) into this translation unit, which is exactly the
// coupling the Input facade exists to avoid.
#include <GLFW/glfw3.h>

// Required BEFORE the gtx include: GLM gates its experimental extension
#define GLM_ENABLE_EXPERIMENTAL
// glm::toMat4(quat) and glm::rotate(quat, vec3) both live in this header.
#include <glm/gtx/quaternion.hpp>

// Everything below is inside namespace Wasteland, matching the header.
namespace Wasteland {

	// The four projection parameters are stored verbatim AND used immediately
	EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
		// Member-init list order FOLLOWS DECLARATION order in the header, not the
		// order written here: m_FOV, m_AspectRatio, m_NearClip, m_FarClip are
		// declared together (header line 145), so this happens to match.
		: m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip), Camera(glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip))
	{
		// Builds the view matrix immediately, because the header's comment on
		UpdateView();
	}

	// The only place m_Projection is recomputed from the viewport. Reached
	void EditorCamera::UpdateProjection()
	{
		// A zero viewport dimension is rejected: it divides to an infinite (or
		// NaN) aspect ratio, and glm asserts on that inside perspective(). The
		// editor viewport legitimately reports 0 height for a frame or two while
		// ImGui is laying out panels.
		// Both dimensions are tested, not just the height: the division below is
		// width/height, so either being zero poisons the result.
		if (m_ViewportWidth <= 0.0f || m_ViewportHeight <= 0.0f)
			return;

		// Overwrites m_AspectRatio, so the constructor's aspectRatio argument is
		m_AspectRatio = m_ViewportWidth / m_ViewportHeight;
		// glm::perspective builds the projection in place and returns it; the
		m_Projection = glm::perspective(glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
	}

	// Called from three places in this file: the constructor, the end of
	void EditorCamera::UpdateView()
	{
		// m_Yaw = m_Pitch = 0.0f; // Lock the camera's rotation
		// Position is DERIVED, never accumulated. The whole class is a pure
		// function of (focal point, pitch, yaw, distance), which is why there is
		// no velocity or damping anywhere -- and also why GetUpDirection() is
		// valid to call later in the same frame without going stale.
		m_Position = CalculatePosition();

		// Rebuilt from Euler angles every call (see GetOrientation). Stored in a
		glm::quat orientation = GetOrientation();
		// Builds the camera-to-world transform as translate * rotate, then the
		// next line inverts it. Deriving the view as an inverse of a composed TRS
		// matrix is the standard trick: it gets the inverted basis and the negated
		m_ViewMatrix = glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(orientation);
		// glm::inverse is a general 4x4 inverse (cofactor/adjugate), NOT a
		// transpose-and-negate shortcut. For a rigid transform the view is just
		// transpose(rotation) with the translation rotated and negated, which
		m_ViewMatrix = glm::inverse(m_ViewMatrix);
	}

	// Returns a PAIR rather than two out-params because both factors come
	std::pair<float, float> EditorCamera::PanSpeed() const
	{
		// Normalised viewport dimension: pixels/1000, capped at 2.4 (a ~2400px
		// viewport). The cap keeps the curve below inside its fitted domain;
		// ---
		// DEFECT (documented, not fixed): the cap is asymmetric with ZoomSpeed's
		float x = std::min(m_ViewportWidth / 1000.0f, 2.4f); // max = 2.4f
		// A quadratic fit, not a linear scale: 0.0366x^2 - 0.1778x + 0.3021.
		// Evaluated at x=0 it is 0.3021, and at the x=2.4 cap it is ~0.086 -- so
		float xFactor = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		// Same normalisation on the other axis, from the HEIGHT. Deliberately not
		float y = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		// The identical curve applied to y. Duplicated rather than factored into a
		float yFactor = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		// { xFactor, yFactor } -- order matters, because the caller's structured
		return { xFactor, yFactor };
	}

	// A CONSTANT. Unlike PanSpeed this does not scale with the viewport, so
	float EditorCamera::RotationSpeed() const
	{
		// 0.8 radians of pitch/yaw per unit of scaled mouse delta. Paired with the
		return 0.8f;
	}

	// Quadratic in the orbit distance, capped. The shape is what makes zoom
	float EditorCamera::ZoomSpeed() const
	{
		// 20% of the orbit radius as the zoom step scale. Multiplying by m_Distance
		float distance = m_Distance * 0.2f;
		// Clamps a NEGATIVE m_Distance to 0, so the square below cannot produce a
		distance = std::max(distance, 0.0f);
		// speed = (0.2 * distance)^2. At the default m_Distance of 10 this is
		float speed = distance * distance;
		// Caps the OUTPUT at 100, reached when 0.2*distance = 10, i.e. m_Distance
		// ---
		// DEFECT (documented, not fixed): the cap is asymmetric with PanSpeed's
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}

	// The per-frame tick. Called unconditionally from EditorLayer.cpp.
	// ---
	// DEFECT (documented, not fixed): the Timestep ts parameter is UNUSED.
	// Every delta below is a raw mouse-delta sample, not scaled by dt, so the
	void EditorCamera::OnUpdate(Timestep ts)
	{
		// LEVEL-triggered, not edge-triggered: Input.h documents IsKeyPressed as
		if (Input::IsKeyPressed(WL_KEY_LEFT_ALT))
		{
			// A const reference to a braced temporary. Lifetime-extended to the end of
			const glm::vec2& mouse{ Input::GetMouseX(), Input::GetMouseY() };
			// Pixel delta times 0.003: the one place mouse sensitivity is defined for
			// rotation. The factor is applied BEFORE PanSpeed's own normalisation, so
			glm::vec2 delta = (mouse - m_InitialMousePosition) * 0.003f;
			// The anchor for the NEXT frame's delta, updated every frame ALT is held
			// rather than on button-press. That makes delta a per-frame motion rather
			m_InitialMousePosition = mouse;

			// Middle = pan. The three branches are mutually exclusive else-ifs, so a
			if (Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_MIDDLE))
				// Pan moves m_FocalPoint; the new m_Position is not recomputed here but by
				MousePan(delta);
			// Left = rotate. Checked before right because a right-button click is
			else if (Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_LEFT))
				MouseRotate(delta);
			// Right = zoom, taking ONLY the vertical component of the two-axis delta.
			else if (Input::IsMouseButtonPressed(WL_MOUSE_BUTTON_RIGHT))
				// A vertical drag zooms. The sign convention comes from MouseZoom, which
				MouseZoom(delta.y);
		}

		// Unconditional, so the view matrix is always current even on a frame with
		UpdateView();
	}

	// The event entry point. EditorLayer.cpp forwards every event to both the
	void EditorCamera::OnEvent(Event& e)
	{
		// Short-lived by design -- Event.h documents it as construct, dispatch,
		EventDispatcher dispatcher(e);
		// MouseScrolledEvent is the ONLY type dispatched. Note the return value
		// of Dispatch is discarded here, so a non-scroll event is simply skipped
		// -- correct, but it means this class silently ignores every other event
		// type rather than rejecting them.
		// ---
		// DEFECT (documented, not fixed): Event.h's Dispatch has a duplicated type
		dispatcher.Dispatch<MouseScrolledEvent>(WL_BIND_EVENT_FN(EditorCamera::OnMouseScroll));
	}

	// Private, reached only through the dispatcher above.
	bool EditorCamera::OnMouseScroll(MouseScrolledEvent& e)
	{
		// Wheel delta scaled by 0.1. Not gated by ALT, unlike the drag gestures,
		float delta = e.GetYOffset() * 0.1f;
		// Same MouseZoom the right-drag path uses, so the zoom floor and the
		MouseZoom(delta);
		// Recomputes here rather than waiting for the next OnUpdate, so the
		UpdateView();
		// Returning FALSE means 'not handled', which Event.h documents as letting
		// the event keep propagating to further listeners. The camera has in fact
		// consumed the scroll, so this is arguably wrong -- but the editor relies
		// on propagation here, since EditorLayer forwards the same event to the
		// game camera controller as well.
		return false;
	}

	// Pan in the camera's OWN right/up plane (see the header), so the drag
	void EditorCamera::MousePan(const glm::vec2& delta)
	{
		// Structured binding over the pair PanSpeed returns; C++17, so this is
		auto [xSpeed, ySpeed] = PanSpeed();
		// NEGATED right, positive up. Negating X is what makes dragging right
		// move the scene right -- without it the world would slide the opposite
		// way under the cursor.
		m_FocalPoint += -GetRightDirection() * delta.x * xSpeed * m_Distance;
		// The Y half of the pan. Not negated, because the vertical mouse axis is
		m_FocalPoint += GetUpDirection() * delta.y * ySpeed * m_Distance;
	}

	// Rotate changes pitch and yaw only. The header documents that neither is
	void EditorCamera::MouseRotate(const glm::vec2& delta)
	{
		// The classic yaw-inversion guard: once the camera's up vector points
		// downward in world space, dragging right must turn the other way or the
		// controls invert.
		// ---
		// DEFECT (documented, not fixed): this is a binary test on GetUpDirection().y
		// with no clamp on m_Pitch, so it flips discontinuously the instant pitch
		float yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
		// Horizontal turn, scaled by the yawSign just computed. RotationSpeed() is
		m_Yaw += yawSign * delta.x * RotationSpeed();
		// Vertical turn, NOT sign-corrected: the pitch axis has no inversion
		m_Pitch += delta.y * RotationSpeed();
	}

	// The one movement helper that moves the focal point as well as the
	void EditorCamera::MouseZoom(float delta)
	{
		// SUBTRACTED, so a positive delta zooms OUT. The wheel handler feeds it
		m_Distance -= delta * ZoomSpeed();
		// The floor. At 1.0 world unit the camera is close enough to the focal
		if (m_Distance < 1.0f)
		{
			// Shifts the TARGET by one whole unit of forward travel, then line 122
			// re-clamps the distance. Together they make a zoom-in past the limit
			// translate along the view direction rather than stall.
			m_FocalPoint += GetForwardDirection();
			// The clamp itself. Note this DISCARDS however far past the floor the
			m_Distance = 1.0f;
		}
	}

	// Three basis accessors, each rotating a local axis through the current
	glm::vec3 EditorCamera::GetUpDirection() const
	{
		// The world-space up axis: local +Y rotated by the orientation. Returns
		// by value, so this is a fresh vec3 the caller owns.
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetRightDirection() const
	{
		// The right axis, local +X. Same shape as GetUpDirection.
		return glm::rotate(GetOrientation(), glm::vec3(1.0f, 0.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetForwardDirection() const
	{
		// The forward axis, local -Z -- negative because OpenGL/glm cameras look
		// down -Z. The sign is the one place the handedness convention leaks
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 0.0f, -1.0f));
	}

	// The single function the whole class orbits around: eye = target -
	glm::vec3 EditorCamera::CalculatePosition() const
	{
		// The subtraction, not the addition, is what puts the camera BEHIND the
		return m_FocalPoint - GetForwardDirection() * m_Distance;
	}

	// The single conversion point from the stored pitch/yaw pair to a
	glm::quat EditorCamera::GetOrientation() const
	{
		// glm::quat(vec3) is the EULER constructor: type_quat.inl shows it is a
		// closed-form cos/sin expansion, not a composition of two axis rotations.
		// The third component (roll) is always 0 -- this camera has no roll axis,
		// so roll can never be introduced.
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}

}