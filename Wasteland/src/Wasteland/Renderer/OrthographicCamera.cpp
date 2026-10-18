// PCH first, by house rule: the standard library headers and the engine's
// logging facilities. The real header follows it deliberately.
#include "wlpch.h"
// OrthographicCamera.h owns the member layout and the inline
// SetPosition/SetRotation, both of which call RecalculateViewMatrix below.
#include "OrthographicCamera.h"

// glm::ortho, translate, rotate and inverse. glm/glm.hpp itself arrives via
// the header, so naming the four functions actually used stays explicit.
#include <glm/gtc/matrix_transform.hpp>

// Named namespace rather than anonymous: the class is exported and other
// translation units construct it.
namespace Wasteland {

	// A 2D orthographic camera -- the four extents are the visible rectangle in
	// view units. Near/far are not parameters because a 2D camera has one usable
	// slice, and exposing them invites a z-range that clips everything.
	// The WL_PROFILE_FUNCTION() in the body leads it, so the timer covers
	// everything the function does.
	OrthographicCamera::OrthographicCamera(float left, float right, float bottom, float top)
		// Built in the init list, so m_ProjectionMatrix is valid before the body.
		// m_ViewMatrix(1.0f) is the IDENTITY and that is correct, not a shortcut: the
		// header defaults m_Position to (0,0,0) and m_Rotation to 0, so
		// RecalculateViewMatrix would compute the identity anyway.
		//
		// DEFECT (maintenance): near/far are hardcoded to -1.0f/1.0f here and the SAME
		// two literals appear again in SetProjection. Change one and the camera's
		// z-extent differs between construction and the first resize, with no compile
		// error and no symptom until something is unexpectedly culled. Not fixed here.
		: m_ProjectionMatrix(glm::ortho(left, right, bottom, top, -1.0f, 1.0f)), m_ViewMatrix(1.0f)
	{
		WL_PROFILE_FUNCTION();

		// The combined matrix is CACHED rather than built on demand, since
		// GetViewProjectionMatrix in the header returns this member: one multiply per
		// change instead of one per draw call. The invariant carried here --
		// m_ViewProjectionMatrix == m_ProjectionMatrix * m_ViewMatrix after EVERY
		// mutation of either half -- is maintained in three places, which is why
		// RecalculateViewMatrix refreshes it too and not just the view half.
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}

	// Re-projects the visible rectangle: called on resize and on ortho zoom, and
	// the view half is untouched because panning and rotation are independent of
	// the extents.
	void OrthographicCamera::SetProjection(float left, float right, float bottom, float top)
	{
		WL_PROFILE_FUNCTION();

		// The near/far literals are duplicated from the constructor -- see the DEFECT
		// note there. Left inline so both functions read like glm's own signature.
		// The next line re-publishes the cache although only the projection changed:
		// new projection * existing view is a different matrix, and leaving the cache
		// stale here is the most likely way this class silently renders wrong.
		m_ProjectionMatrix = glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}

	// Recomputed from position and rotation rather than applied incrementally, so
	// there is no accumulated error and the inputs cannot drift out of sync with
	// the published matrix. Reached only via the header's SetPosition/SetRotation,
	void OrthographicCamera::RecalculateViewMatrix()
	{
		WL_PROFILE_FUNCTION();

		// World -> camera is translate * rotate: the camera transform is applied AFTER
		// the local one. Reversed, a 2D camera would orbit the origin instead of the
		// object being viewed. The continuation converts degrees to radians, since
		// glm::rotate wants radians and m_Rotation is stored in degrees.
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), m_Position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0, 0, 1));

		// inverse, not transpose: position is baked into the same matrix, so there is
		// no orthonormal-plus-translation shortcut for the whole 4x4. GLM does a general
		// cofactor inverse -- correct as written, and once per change rather than per
		// frame. The line below re-publishes the constructor's cache invariant.
		m_ViewMatrix = glm::inverse(transform);
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}

}