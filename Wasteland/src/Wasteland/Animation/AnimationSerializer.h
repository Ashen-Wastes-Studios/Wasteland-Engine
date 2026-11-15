#pragma once

// .wlanim -- the animation asset format.
//
// One file carries a skeleton, the clips authored on it, and an optional state
// machine. That is the same shape a DCC export produces: a rig, the actions
// authored against it, and the controller that picks between them. Splitting
// them would mean a character's rig and its actions could not be shipped or
// versioned together.
//
// YAML, on the same substrate as .wlblocks and .wastescene, under the same two
// rules:
//
//   1. STABLE STRING KEYS everywhere -- bone names, clip names, state names.
//      Never indices. A file authored before a field is added must still load,
//      and an index would silently retarget to a different bone after a re-export.
//   2. The loader REPORTS instead of throwing. A malformed key drops that key
//      with a diagnostic; it never takes the process down mid-load.
//
// The skeleton is mandatory in practice (a clip sampled without one has nowhere
// to write), but the parser tolerates its absence so a clips-only file can be
// authored and the skeleton bound at runtime.

#include "Wasteland/Animation/AnimationClip.h"
// AnimationRetarget::Mapping, the element type of AnimationAsset::Retarget.
// A real include rather than a forward declaration: Mapping is a NESTED
// type, so a forward-declared AnimationRetarget cannot name it. There is no
// cycle -- AnimationRetarget.h reaches AnimationClip.h and Skeleton.h,
// neither of which includes this header.
#include "Wasteland/Animation/AnimationRetarget.h"
#include "Wasteland/Animation/AnimationStateMachine.h"
#include "Wasteland/Animation/Skeleton.h"

#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace Wasteland::Animation
{
	// A structured error, so a load failure says WHY rather than only "no".
	// Same shape as VisualBlocks::GraphError, and for the same reason: a bool
	// return is not something a designer can act on.
	struct AnimationError
	{
		std::string Message;
		// The bone, clip or state the failure is about, when there is one.
		// Empty for document-level problems, so a caller must test the string
		// rather than assume it names something.
		std::string Context;
		// Always true here. Reserved so a future warning path has somewhere to
		// go without changing the struct's layout.
		bool IsError = true;

		// "file: clip 'Idle': message" -- one line a designer can act on.
		std::string Format(const std::string &fileName) const;
	};

	// The contents of one .wlanim file.
	struct AnimationAsset
	{
		// The asset's own name, which is also the file's stem in practice.
		std::string Name;

		// The rig. shared_ptr because an Animator and every SkinnedMesh bound to
		// the same rig share one instance.
		std::shared_ptr<Skeleton> Skeleton;

		// The clips authored on that rig, in authoring order.
		std::vector<std::shared_ptr<AnimationClip>> Clips;

		// Optional. A rig with clips and no machine is directly controllable;
		// the machine is what makes it playable without code.
		//
		// A VALUE, matching how AnimationStateMachine is owned everywhere else
		// ("a character owns one, and copying a machine copies its whole state
		// table"), and registered with pybind's default holder. A shared_ptr
		// here would mismatch that registration and throw at runtime.
		AnimationStateMachine StateMachine;
		// Presence flag, because a default-constructed machine is itself a
		// coherent state (no states, no transitions) and cannot be used to mean
		// "absent" without making an explicitly-empty machine unrepresentable.
		bool HasStateMachine = false;

		// Authoring-only node positions for the state machine, keyed by state
		// name.
		//
		// Deliberately NOT a field of AnimationState: a position is an editor
		// concern, and a machine that knew where its states sat on somebody's
		// screen would carry that layout with it into every copy. Keyed by name
		// for the same reason every other field in this format is.
		//
		// A missing entry means "never positioned", and the editor lays that
		// state out itself -- so an older file and a state added later behave the
		// same way rather than landing on top of the origin.
		struct StatePosition
		{
			float X = 0.0f;
			float Y = 0.0f;
		};
		std::unordered_map<std::string, StatePosition> StatePositions;

		// --- retargeting ---

		// The authored bone-name mapping, applied when this asset is retargeted
		// onto a rig other than the one it was authored for. Authoring metadata
		// rather than a runtime input: nothing in the playback path reads it, and
		// a retargeted clip is stored in Clips like any other.
		//
		// A vector rather than a map because the pairing has no natural key --
		// source and target are both meaningful, and a source bone can
		// legitimately map to more than one target on a rig with duplicated
		// chains. The serializer sorts it by source name so the file is stable.
		std::vector<AnimationRetarget::Mapping> Retarget;

		// Multiplier for translation tracks during a retarget. Defaults to 1.0,
		// which is a pure re-key; AnimationRetarget::HeightRatio supplies the
		// proportional value an author wants.
		float RetargetPositionScale = 1.0f;
	};

	// Load, save, and convert .wlanim.
	class AnimationSerializer
	{
	public:
		// Load from disk. Returns false and fills `error` on failure.
		// Never partially loads: `outAsset` is left untouched on failure, so a
		// caller that ignores the bool cannot end up with half an asset.
		static bool Load(const std::string &path, AnimationAsset &outAsset,
						 AnimationError &error);

		// Parse from text -- the seam an importer or an agent authors through,
		// and the same code path a loaded file takes. Diverging the two would
		// mean an imported asset bypasses the errors the file path reports.
		static bool Parse(const std::string &text, AnimationAsset &outAsset,
						  AnimationError &error);

		// Serialize to text. Deterministic: the same asset always produces
		// byte-identical output, which is what makes the format diffable and
		// safe to commit.
		static std::string Emit(const AnimationAsset &asset);

		// Write to disk. Returns false with `error` set on failure.
		static bool Save(const std::string &path, const AnimationAsset &asset,
						 AnimationError &error);
	};
}
