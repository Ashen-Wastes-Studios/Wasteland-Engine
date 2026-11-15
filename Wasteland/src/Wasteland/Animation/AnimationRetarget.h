// Retargeting: playing a clip authored on one rig on a different rig.
//
// Scope is deliberately narrow, matching the plan: a bone-NAME mapping plus a
// proportional scale derived from the two skeletons' proportions. There is no
// motion extraction and no IK -- a retargeted clip is the same curve data
// re-keyed onto different bones, with translations scaled so a 2 m character
// does not take metre-long strides on a 1 m one.
//
// ## Why a remapped CLIP rather than a remap at sample time
//
// AnimationClip::Sample already does the right thing with a clip whose tracks
// reference bones the skeleton does not have: it seeds every bone from the rest
// pose and skips out-of-range track indices. So a retargeted clip that simply
// has no track for the extra bone leaves that bone at rest -- which is exactly
// the behaviour the plan's verification asks for, and it needs no change to the
// hot sampling path.
//
// The alternative (a mapping consulted inside Sample) would put a name lookup on
// the per-frame path for every animated bone, on the one function the animation
// system calls most.

#pragma once

#include <string>
#include <vector>

#include "AnimationClip.h"
#include "Skeleton.h"

namespace Wasteland
{
	namespace Animation
	{
		class AnimationRetarget
		{
		public:
			// One source-bone-name -> target-bone-name pairing. An override
			// exists because automatic matching is a convenience, and a rig that
			// names its bones "spine_01" on one side and "Spine01" on the other
			// is exactly the case a human has to be able to correct.
			struct Mapping
			{
				std::string SourceBone;
				std::string TargetBone;
			};

			// Automatic matching, used for any source bone an override does not
			// name. Exact match first, then case-insensitive, then normalised
			// (non-alphanumerics stripped, lowercased) so `spine_01` meets
			// `Spine01`. Returns only pairs that resolved on BOTH sides; a
			// source bone with no counterpart is reported by the caller, not
			// silently dropped here.
			static std::vector<Mapping> BuildMap(
				const Skeleton &source,
				const Skeleton &target,
				const std::vector<Mapping> &overrides = {});

			// The scale applied to translation tracks: the target's overall
			// height over the source's. Height is the length of the longest
			// downward chain of rest translations, which is robust to a rig
			// whose bones all sit at the origin (a common authoring state where
			// the pose carries the shape and the translations are zero).
			//
			// Returns 1.0 when either height is zero -- a scale computed by
			// dividing by zero would be worse than no scale, and an unriged
			// skeleton retargets as a pure re-key.
			static float HeightRatio(const Skeleton &source, const Skeleton &target);

			// Produces a clip whose tracks are re-keyed onto `targetSkeleton`.
			//
			// - Tracks are matched by the source bone's NAME through the map;
			//   an unnamed source bone is dropped (it has no counterpart, and a
			//   track keyed to the wrong index would be worse than no track).
			// - Position channels are multiplied by `positionScale`; rotation
			//   and scale channels are copied unchanged. A rotation cannot be
			//   scaled -- there is no meaningful proportional rotation -- and a
			//   limb-length difference is already expressed by the translation
			//   scale.
			// - The returned clip keeps the source's name, so a state machine
			//   that references clips by name keeps resolving after a retarget.
			// - A source clip with no tracks produces an empty clip, not a
			//   failure: "nothing to retarget" is a valid answer.
			static AnimationClip RetargetClip(
				const AnimationClip &source,
				const Skeleton &sourceSkeleton,
				const Skeleton &targetSkeleton,
				const std::vector<Mapping> &overrides = {},
				float positionScale = 1.0f);

			// The inverse lookup, for an editor showing "this target bone comes
			// from that source bone". Returns an empty string when unmapped.
			static std::string SourceBoneForTarget(
				const std::vector<Mapping> &map, const std::string &targetBone);

			// Normalised comparison key: lowercased, non-alphanumerics removed.
			// Public because an editor that lists unmatched bones needs the same
			// notion of "these two names are probably the same bone".
			static std::string NormaliseBoneName(const std::string &name);
		};
	}
}
