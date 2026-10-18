#include "wlpch.h"
#include "Math.h"

// GLM's decompose lives in an experimental extension header, so the macro has
// to be set before it is included. It is set here rather than in Math.h so
// that translation units which only need the declaration do not pay for the
// experimental include.
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace Wasteland::Math {

		// Decomposes a transform matrix into translation, Euler rotation and
		// per-axis scale. See Math.h for the contract.
		//
		// The body is lifted from glm::decompose (matrix_decompose.inl) with
		// the local-matrix copy and the epsilon helpers kept as-is. The steps
		// below follow GLM's ordering, which matters: each stage depends on
		// the previous stage having already been stripped out of LocalMatrix.
		bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale)
		{
			// From glm::decompose in matrix_decompose.inl

			// Pull the glm names in unqualified so the expressions below read
			// like the upstream source.
			using namespace glm;

			// Single-precision float is assumed throughout; epsilon<T>() below
			// is what makes the near-zero comparisons in this function
			// meaningful at float precision.
			using T = float;

			// Work on a copy. The caller's matrix is taken by const reference,
			// and every subsequent step destructively edits LocalMatrix, so
			// this copy is what keeps the caller's matrix intact.
			mat4 LocalMatrix(transform);

			// Normalize the matrix.
			// A bottom-right element of zero means the matrix collapses a
			// dimension and has no invertible transform, so there is nothing
			// meaningful to decompose. Bailing here rather than dividing by it
			// later is what keeps the function from producing infinities.
			if (epsilonEqual(LocalMatrix[3][3], static_cast<float>(0), epsilon<T>()))
				return false;

			// First, isolate perspective. This is the messiest.
			// Any non-zero value in the fourth column of the first three rows
			// indicates a perspective divide is baked into the matrix. It is
			// removed wholesale, because a matrix that mixes perspective with
			// TRS cannot be represented as the three components being asked
			// for. Note this silently discards the perspective rather than
			// reporting it, so a perspective matrix decomposes to plausible
			// looking TRS values that are not what the matrix actually does.
			if (
				epsilonNotEqual(LocalMatrix[0][3], static_cast<T>(0), epsilon<T>()) ||
				epsilonNotEqual(LocalMatrix[1][3], static_cast<T>(0), epsilon<T>()) ||
				epsilonNotEqual(LocalMatrix[2][3], static_cast<T>(0), epsilon<T>()))
			{
				// Clear the perspective partition
				LocalMatrix[0][3] = LocalMatrix[1][3] = LocalMatrix[2][3] = static_cast<T>(0);
				LocalMatrix[3][3] = static_cast<T>(1);
			}

			// Next take care pf translation (easy).
			// Translation lives in the fourth column, so it can be read
			// directly and then zeroed out, leaving only the linear part.
			translation = vec3(LocalMatrix[3]);
			// Zero the xyz of the fourth column but preserve its w, which the
			// normalization step above may have set to 1.
			LocalMatrix[3] = vec4(0, 0, 0, LocalMatrix[3].w);

			// Row[3] is a working copy of the upper-left 3x3, which at this
			// point still has any shear mixed into it. Pdum3 is only ever used
			// by the disabled coordinate-flip check below, so it is presently
			// dead.
			vec3 Row[3], Pdum3;

			// Now get scale and shear.
			// Copy the 3x3 block into Row so the rest of the function can
			// normalize rows in place without touching LocalMatrix again.
			for (length_t i = 0; i < 3; i++)
				for (length_t j = 0; j < 3; j++)
					Row[i][j] = LocalMatrix[i][j];

			// Compute X scale factor and normalize first row.
			// The length of each row is that axis' scale, because the rows are
			// still mutually orthogonal once shear is absent. Normalizing each
			// row as it is measured leaves an orthonormal basis, which is what
			// makes the rotation extraction below valid.
			scale.x = length(Row[0]);
			Row[0] = detail::scale(Row[0], static_cast<T>(1));
			scale.y = length(Row[1]);
			Row[1] = detail::scale(Row[1], static_cast<T>(1));
			// BUG: writes the Z scale into scale.x, overwriting the X scale
			// measured two lines above, and leaves scale.z never written. The
			// intended target is scale.z. Left as-is because this pass is
			// comments-only, but any caller reading scale.x or scale.z from
			// this function is reading wrong or uninitialized data.
			scale.x = length(Row[2]);
			Row[2] = detail::scale(Row[2], static_cast<T>(1));

			// At this point, the matrix (in rows[]) is orthonormal.
			// Check for a coordinate system flip. If the determinant
			// is -1, then negate the matrix and the scaling factors.
			// Disabled with #if 0. The check detects a left-handed basis
			// introduced by a negative scale on an odd number of axes, and
			// would fold that flip into the scale components as negative
			// values. It is off, so such matrices decompose with no indication
			// that a flip occurred.

#if 0
			Pdum3 = cross(Row[1], Row[2]); // v3Cross(row[1], row[2], Pdum3);
			if (dot(Row[0], Pdum3) < 0)
			{
				for (length_t i = 0; i < 3; i++)
				{
					scale[i] *= static_cast<T>(-1);
					Row[i] *= static_cast<T>(-1);
				}
			}
#endif

			// Extract Euler angles from the now-orthonormal basis.
			//
			// rotation.y comes from Row[0][2] because that element encodes the
			// Y-axis angle once the other rotations are accounted for, and
			// asin is taken rather than atan2 so the result is confined to
			// [-pi/2, pi/2]. The cost of that confinement is gimbal lock at
			// the extremes, which the cos test below detects.
			rotation.y = asin(-Row[0][2]);
			// Not an epsilon comparison against zero: cos(rotation.y) is
			// compared exactly, so a value that is merely very small takes
			// the general branch rather than the degenerate one. In practice
			// that is usually harmless -- both branches agree near the
			// boundary -- but it is a fragile test to rely on.
			if (cos(rotation.y) != 0) {
				// Well-conditioned case: X and Z can be recovered
				// independently from the remaining basis elements.
				rotation.x = atan2(Row[1][2], Row[2][2]);
				rotation.z = atan2(Row[0][1], Row[0][0]);
			}
			else {
				// Gimbal lock: Y is at +/-90 degrees, where X and Z rotate
				// about the same axis and only their sum is recoverable. Z is
				// pinned to zero and X absorbs the combined angle.
				rotation.x = atan2(-Row[2][0], Row[1][1]);
				rotation.z = 0;
			}

			// Reached only when the matrix was invertible. Note this returns
			// true even when perspective was stripped or the flip check above
			// was skipped, so true means "produced TRS values", not "the
			// matrix was a clean TRS transform".
			return true;
		}

}