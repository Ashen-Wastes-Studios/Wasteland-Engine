#pragma once

#include <glm/glm.hpp>

namespace Wasteland {

	// Minimal camera abstraction: it holds ONLY a projection matrix and
	// offers no view/transform state at all, so anything that needs a view
	// must keep that on the concrete camera type instead of here.
	class Camera
	{
	// Implicitly virtual is not needed on the destructor: ~Camera is already
	// virtual (see line 14), which is what lets a derived camera be deleted
	// through a Camera* without a UB base-destructor call.
	public:
		// Leaves m_Projection at its in-class default (identity, line 18), so a
		// default-constructed Camera projects as if nothing was set up yet.
		Camera() = default;
		// Convenience ctor for callers that already computed the projection
		// themselves -- stores a COPY of the matrix, it does not alias the
		// caller's. 'const &' only avoids a copy on the way in.
		Camera(const glm::mat4& projection)
			// Member-init list: runs before the (empty) body. Equivalent to
			// assigning in the body, but initialises the 16 floats once instead
			// of default-constructing then overwriting them.
			: m_Projection(projection) {}

		// virtual + defaulted: the vtable slot exists so deletion through a
		// base pointer runs the derived destructor, and = default avoids an
		// out-of-line definition for an already-trivial destructor.
		virtual ~Camera() = default;

		// Returns a REFERENCE -- callers get the live matrix with no copy. The
		// const overload blocks writes, so the projection is read-only from
		// outside even though the member is protected and mutable.
		const glm::mat4& GetProjection() const { return m_Projection; }
	// Protected rather than private so derived cameras can mutate their own
	// projection without going back through this base (which has no setter).
	protected:
		// Identity default. glm::mat4(1.0f) is a diagonal matrix of ones, so an
		// unconfigured camera maps clip space to itself rather than to
		// uninitialised memory.
		glm::mat4 m_Projection = glm::mat4(1.0f);
	};

}