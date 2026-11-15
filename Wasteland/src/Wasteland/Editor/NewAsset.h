#pragma once

// Task F.11 — the asset types the Content Browser's New menu offers.
//
// The MENU is editor UI; this is the FILE side, which is the part that can be
// wrong in a way the user cannot see. A new asset must be a VALID EMPTY FILE,
// not a stub: a stub that a lenient reader accepts is rejected by the compiler
// that consumes it, and the failure surfaces later, in a different tool, with
// the file's provenance long gone.
//
// Every header is produced by the matching serializer's own Save/Emit on a
// default-constructed asset. That is deliberate: hand-written headers drift the
// moment a serializer gains a required field, and a drift is invisible until a
// user creates an asset and tries to open it. Producing it through the
// serializer means the file is valid BY CONSTRUCTION, and if a serializer
// cannot emit a usable empty asset, that is a defect in the serializer --
// which is exactly what this function is here to expose.

#include <string>

namespace Wasteland::Editor
{
	// Write a new empty asset of the kind implied by `absolutePath`'s
	// extension. `assetName` is the display name stored inside the file (the
	// serializers embed one); it may be empty, in which case the file stem is
	// used.
	//
	// Returns false with `error` set for an unsupported extension, a failure to
	// create the parent directory, or a serializer that refuses the empty asset.
	bool CreateEmptyAsset(const std::string &absolutePath,
						  const std::string &assetName, std::string &error);
}
