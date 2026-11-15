// MeshSculpt.h -- sculpt brushes over an EditableMesh.
//
// SCOPE (the plan's, verbatim): Grab, Smooth, Inflate, Flatten, Crease, Mask,
// with a radius and a strength. NO multires, NO dynamic tessellation, NO clay
// strips, NO pose brushes -- those are P1.
//
// WHY A FIXED BRUSH SET AND NOT A SCRIPTABLE ONE
// ----------------------------------------------
// A brush is a displacement function over a vertex neighbourhood. The six
// here cover the standard sculpt vocabulary, and every one of them is a
// *local* operation: it moves vertices within a radius by an amount derived
// from the local geometry. That locality is what makes them testable without a
// window -- the same property a dynamic-tessellation or multires implementation
// would destroy, because the answer would depend on the subdivision level
// rather than on the mesh.
//
// THE FALLOFF
// -----------
// `w = (1 - (d/r)^2)^2` -- a smoothstep-squared. It is 1 at the centre, 0 at
// the rim, and has zero derivative at both ends, so a brush stroke does not
// leave a visible crease where it stops. A linear falloff does leave one, and
// that is the single most obvious defect in a hand-rolled sculpt brush.
//
// THE CLAMP
// ---------
// Every displacement is clamped to the brush radius. That is not cosmetic: it
// is the invariant the plan's verification asserts ("bounded by the brush
// radius"), and it also bounds the damage a single stroke can do to a mesh --
// a smooth step can otherwise pull a vertex across the whole model on a
// high-curvature surface.
#pragma once

#include <cstdint>
#include <vector>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	enum class SculptBrush
	{
		Grab,
		Smooth,
		Inflate,
		Flatten,
		Crease,
	};

	struct SculptStroke
	{
		SculptBrush Brush = SculptBrush::Smooth;

		// The brush centre in asset space. For Grab it is the first hit; for
		// Flatten it defines the plane together with PlaneNormal.
		float Center[3] = {0.0f, 0.0f, 0.0f};

		float Radius = 1.0f;
		float Strength = 0.5f;

		// Grab: the drag vector. Its magnitude is how far the surface moves at
		// the centre of the brush.
		float Direction[3] = {0.0f, 0.0f, 0.0f};

		// Flatten: the plane to project onto, defined by a point and a normal.
		float PlaneNormal[3] = {0.0f, 1.0f, 0.0f};
		float PlanePoint[3] = {0.0f, 0.0f, 0.0f};

		// A per-vertex weight in [0,1], indexed by VERTEX index. EMPTY means no
		// mask. A masked-out vertex (weight 0) is not touched by ANY brush --
		// the mask is applied after the brush's own falloff, so it composes
		// rather than replacing the falloff. This is why the mask is a struct
		// member and not a parameter of each brush: six brushes would otherwise
		// need six identical mask checks, and five of them would be forgotten.
		//
		// A vector rather than a raw pointer because this is a VALUE struct that
		// a caller copies; a borrowed pointer into a caller's array would dangle
		// the moment the stroke outlived the array, and the brush would then read
		// whatever the allocator put there.
		std::vector<float> Mask;
	};

	// Applies one stroke.
	//
	// Returns the mean displacement MAGNITUDE over the affected vertices, or -1
	// when an argument is invalid (negative or NaN radius, non-finite strength,
	// a null mask pointer with a non-empty stroke). A return value of exactly 0
	// means the brush touched nothing -- which is a legitimate outcome, not an
	// error, so callers must compare against -1 rather than testing for truth.
	float SculptStrokeApply(EditableMesh &mesh, const SculptStroke &stroke);

	// The falloff weight for a distance and a radius. Exposed because it is the
	// part a caller needs to reproduce when previewing a brush footprint, and
	// because it is the piece with a real shape worth testing.
	float SculptFalloff(float distance, float radius);

	// Recomputes normals after a stroke. Exposed because a stroke that moves
	// vertices leaves the normals stale, and a caller doing several strokes in
	// a row wants to defer it; but the default expectation is that a stroke
	// leaves the mesh shaded correctly, so SculptStrokeApply calls it.
	void SculptCommit(EditableMesh &mesh);

} // namespace Wasteland
