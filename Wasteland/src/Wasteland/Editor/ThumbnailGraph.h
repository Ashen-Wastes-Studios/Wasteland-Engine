#pragma once

// Graph thumbnails (Task F.4): a node graph rasterized from its node positions.
//
// No text is rasterized, deliberately. A graph is recognizable by its SHAPE --
// the fan-out of a branch, the chain of a sequencer, the long wire across a
// layout -- and text at 256px would be unreadable anyway. It also means this
// needs no font, no glyph atlas and no ImGui, so it runs headlessly and cannot
// drift with an ImGui version.
//
// The implementation lives in ThumbnailCache.cpp rather than its own file
// because it shares that file's Raster; a second copy of the box/line rasterizer
// would be a second thing to keep correct.

#include <cstdint>
#include <string>

#include "Wasteland/Editor/ThumbnailCache.h"

namespace Wasteland::Editor
{
	struct GraphThumbnailResult
	{
		ThumbnailEntry Entry;
		std::string Error;

		// Observable because the verification needs them: "both the wire pixels
		// and the node pixels are present" is only checkable if the caller can
		// tell a wire-only image from a node-only one.
		int NodeCount = 0;
		int WireCount = 0;

		bool Ok() const { return Error.empty(); }
	};

	// Load a .wlblocks or .wlbtree and rasterize it into a `size` x `size`
	// square. `size` is normally ThumbnailCache::kThumbnailSize.
	//
	// Wires are drawn first (under the nodes), then node boxes, then the
	// event/root node in the accent colour -- matching the draw order the
	// designer sees in the canvas.
	//
	// On a load failure the result carries the serializer's message and an empty
	// entry; the caller falls back to GenerateIcon for the graph's kind.
	GraphThumbnailResult MakeGraphThumbnail(const std::string &path, uint32_t size);
}
