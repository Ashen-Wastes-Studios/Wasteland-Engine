#pragma once

// Math.h -- small math helpers that sit on top of GLM.
//
// Everything here is a thin wrapper over GLM rather than a replacement for
// it; GLM does the actual linear algebra. The reason these wrappers exist is
// that the raw GLM entry points are awkward at call sites -- decompose in
// particular writes through three out-parameters and reports failure by
// return value rather than by throwing.

#include <glm/glm.hpp>

namespace Wasteland {
	namespace Math {

		// Splits a 4x4 transform matrix into its translation, rotation
		// (Euler angles, radians) and scale components.
		//
		// This is a copy of glm::decompose from GLM's matrix_decompose.inl,
		// relocated here so the engine controls the signature and so the
		// experimental GLM extension need not be included at every call site.
		// See Math.cpp for the implementation and for its known limitations.
		//
		// @param transform   The matrix to decompose, taken by const reference
		//                    so the caller's matrix is not copied.
		// @param translation  Out: the translation component, read from the
		//                    matrix's fourth column.
		// @param rotation     Out: rotation as Euler angles in radians.
		// @param scale        Out: the per-axis scale factors.
		//
		// @returns true if the matrix was decomposed, false if it was
		//          singular. The out-parameters are written only on success,
		//          so a false return leaves the caller's variables untouched.
		bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

	}
}