#pragma once

#include <glm/glm.hpp>

namespace Wasteland {

	// Concrete 2D camera. Note it does NOT derive from Wasteland::Camera --
	// it is a standalone type with its own projection matrix, so it cannot
	// be passed where a Camera base reference is expected.
	class OrthographicCamera
	{
	public:
		// Orthographic = parallel projection, no perspective divide. Width/height
		// are supplied directly in world units rather than derived from fov,
		// which is what makes it usable for tilemaps and UI pixel-perfect work.
		OrthographicCamera(float left, float right, float bottom, float top);

		// Re-derives the projection matrix from new bounds.
		// DEFECT-ish asymmetry: this does NOT refresh m_ViewProjectionMatrix.
		// SetPosition/SetRotation both call RecalculateViewMatrix (line 24),
		// but changing the projection bounds alone leaves the cached product
		// stale, so the camera can render with the old aspect ratio until the
		// next position/rotation change happens to run. Not fixed here.
		void SetProjection(float left, float right, float bottom, float top);

		// Returns BY VALUE, unlike Camera::GetProjection which returns const&.
		// A 3-float copy is cheap so this is only a style inconsistency, but it
		// means a caller can never mutate the camera through the return value.
		const glm::vec3 GetPosition() const { return m_Position; }
		// Position-only setter: deliberately does NOT recompute the projection.
		// The trailing RecalculateViewMatrix is what keeps the view and the
		// cached view-projection product consistent after the move.
		void SetPosition(const glm::vec3& position) { m_Position = position; RecalculateViewMatrix(); }

		// Scalar in radians (glm convention), zero-initialised at line 31.
		float GetRotation() const { return m_Rotation; }
		// Rotation is applied about the camera position by the view matrix;
		// the projection matrix is orthogonal and rotation-invariant, so only
		// the view half needs rebuilding.
		void SetRotation(float rotation) { m_Rotation = rotation; RecalculateViewMatrix(); }

		// By-value return again (a 4x4 = 16 floats copied per call). The two
		// matrix accessors below share this pattern.
		const glm::mat4 GetProjectionMatrix() const { return m_ProjectionMatrix; }
		// Cached view transform, refreshed only by RecalculateViewMatrix.
		const glm::mat4 GetViewMatrix() const { return m_ViewMatrix; }
		// The pre-multiplied product the renderer binds as u_ViewProjection.
		// DEFECT: because SetProjection (line 12) never touches this cache, it
		// can disagree with GetProjectionMatrix() * GetViewMatrix() whenever
		// the projection was changed after the last position/rotation update.
		const glm::mat4 GetViewProjectionMatrix() const { return m_ViewProjectionMatrix; }
	// First of two back-to-back private: labels on lines 23 and 25 --
	// redundant but harmless.
	private:
		// Declared, not defined here: the translation/rotation composition and
		// the m_ViewProjectionMatrix = projection * view product live in the
		// matching .cpp. Keeping it private stops callers skipping the cache.
		void RecalculateViewMatrix();
	private:
		// Unlike Camera::m_Projection this has NO in-class initialiser, so it
		// is default-initialised to whatever glm leaves in the type (identity for
		// glm default ctors). It is populated by the ctor/SetProjection.
		glm::mat4 m_ProjectionMatrix;
		glm::mat4 m_ViewMatrix;
		// Pure cache: derived state recomputed from the two members above it.
		// Nothing sets it except RecalculateViewMatrix.
		glm::mat4 m_ViewProjectionMatrix;

		// Camera sits at the world origin by default, so with a symmetric
		// ortho box the first frame shows the area centred on (0,0).
		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		// Zero rotation: an orthographic view is naturally axis-aligned, so the
		// initial view matrix is an identity.
		float m_Rotation = 0.0f;
	};

}