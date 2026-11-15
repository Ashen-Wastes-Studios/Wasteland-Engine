// BoneThumbnail.h -- Task F.7 v1: a bone-silhouette thumbnail raster.
//
// v1 of the plan's animation thumbnail is a CPU raster of the bone hierarchy's
// first pose: cheap, recognizable, and -- like the graph thumbnail (F.4) -- pure
// CPU, so it is verifiable HEADLESSLY with no GL device. v2 (a rendered skinned
// mesh) waits on the drawing editor.
//
// Input is the WORLD-SPACE bone positions and their parent indices, which the
// caller computes from a pose via Skeleton::ComputeWorldMatrices -- that keeps
// this raster independent of the animation sampling, and makes it testable with
// any pose the Animation API can produce. Draws each bone as a segment from the
// bone's position to its parent's, with a filled joint marker on the root.
//
// Output is a top-row-first RGBA8 buffer (ThumbnailCache's convention), size x
// size. Deterministic: same bones in, identical bytes out (the F.7 verification).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland
{
	struct BoneThumbnailResult
	{
		std::vector<std::uint8_t> Pixels; // size*size*4, RGBA, top row first
		int Size = 0;
		int BoneCount = 0;
	};

	// Rasterizes a bone hierarchy into `size` x `size` RGBA. `worldPositions` and
	// `parents` are parallel arrays; `parents[i] < 0` marks a root. Positions are
	// fit so the largest extent maps to (size - 2*margin) pixels. A hierarchy with
	// no bones yields a background buffer and `BoneCount == 0`.
	BoneThumbnailResult MakeBoneHierarchyThumbnail(
	    const std::vector<float> &worldPositions,   // x,y,z per bone
	    const std::vector<std::int32_t> &parents,   // per bone, -1 = root
	    int size);
}
