#include "wlpch.h"

#include "Wasteland/Animation/AnimationRetarget.h"

// std::isalnum / std::tolower for NormaliseBoneName.
#include <cctype>
// std::max.
#include <algorithm>
// std::unordered_map / std::unordered_set for the name lookups.
#include <unordered_map>
#include <unordered_set>

namespace Wasteland
{
	namespace Animation
	{
		std::string AnimationRetarget::NormaliseBoneName(const std::string &name)
		{
			std::string out;
			out.reserve(name.size());
			for (char c : name)
			{
				if (std::isalnum(static_cast<unsigned char>(c)))
				{
					out.push_back(
						static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
			}
			return out;
		}

		std::vector<AnimationRetarget::Mapping> AnimationRetarget::BuildMap(
			const Skeleton &source, const Skeleton &target,
			const std::vector<Mapping> &overrides)
		{
			// Name -> index on the target, for both exact and normalised lookup.
			// Built once per call; a retarget is an authoring-time operation, so
			// the O(n) table is not worth optimising.
			std::unordered_map<std::string, BoneIndex> targetByName;
			std::unordered_map<std::string, BoneIndex> targetByNormalised;
			for (int i = 0; i < target.GetBoneCount(); ++i)
			{
				const BoneIndex index((int32_t)i);
				const std::string name = target.GetBoneName(index);
				targetByName.emplace(name, index);
				// The FIRST name wins on a collision. Two bones normalising to
				// the same key ("spine_01" and "Spine01" in one rig) is a rig
				// error, and silently preferring the later one would make the
				// mapping depend on bone order.
				targetByNormalised.emplace(NormaliseBoneName(name), index);
			}

			std::vector<Mapping> map;
			std::unordered_set<std::string> claimedTargets;

			// Overrides first, so an explicit pairing beats an automatic one
			// however good the automatic match looked.
			for (const Mapping &override : overrides)
			{
				if (override.SourceBone.empty() || override.TargetBone.empty())
				{
					continue;
				}
				// An override naming a target bone that does not exist is
				// skipped rather than guessed at: a typo in a hand-authored
				// mapping must not silently retarget onto the wrong bone.
				const auto it = targetByName.find(override.TargetBone);
				if (it == targetByName.end())
				{
					continue;
				}
				// Resolve the source side too, so the mapping records the pair
				// even when the source rig has since been re-exported under a
				// different name -- the caller reports unmatched sources
				// separately, and a mapping that cannot be honoured is worse
				// than no mapping.
				map.push_back(override);
				claimedTargets.insert(override.TargetBone);
			}

			for (int i = 0; i < source.GetBoneCount(); ++i)
			{
				const BoneIndex sourceIndex((int32_t)i);
				const std::string sourceName = source.GetBoneName(sourceIndex);

				bool alreadyMapped = false;
				for (const Mapping &m : map)
				{
					if (m.SourceBone == sourceName)
					{
						alreadyMapped = true;
						break;
					}
				}
				if (alreadyMapped)
				{
					continue;
				}

				// Exact, then case-insensitive, then normalised.
				auto exact = targetByName.find(sourceName);
				if (exact != targetByName.end())
				{
					map.push_back({sourceName, sourceName});
					claimedTargets.insert(sourceName);
					continue;
				}

				auto normalised =
					targetByNormalised.find(NormaliseBoneName(sourceName));
				if (normalised != targetByNormalised.end())
				{
					const std::string targetName =
						target.GetBoneName(normalised->second);
					if (claimedTargets.insert(targetName).second)
					{
						map.push_back({sourceName, targetName});
					}
				}
				// A source bone with no counterpart on the target is dropped
				// here. It is not an error: the whole point of a retarget is
				// that the rigs differ, and the caller reports the unmatched
				// list so an editor can show it.
			}

			return map;
		}

		float AnimationRetarget::HeightRatio(const Skeleton &source,
											 const Skeleton &target)
		{
			// Both skeletons' heights are the length of their longest downward
			// chain of rest translations, which is robust to a rig whose bones
			// all sit at the origin -- there the pose carries the shape and the
			// ratio is meaningless, so the caller gets 1.0 rather than a
			// division by zero.
			//
			// Implemented as an explicit stack rather than recursion: a rig
			// with a few hundred bones is fine either way, but a corrupted
			// parent index forming a cycle would recurse forever. The visited
			// set is what actually bounds it.
			auto chainHeight = [](const Skeleton &skeleton) -> float
			{
				const int boneCount = skeleton.GetBoneCount();
				if (boneCount <= 0)
				{
					return 0.0f;
				}

				const Pose &rest = skeleton.GetRestPose();

				// Children lists, so the walk is top-down from the roots
				// rather than a repeated scan for the next bone whose parent
				// is the current one.
				std::vector<std::vector<int>> children(boneCount);
				std::vector<int> roots;
				std::vector<bool> seen(boneCount, false);
				for (int i = 0; i < boneCount; ++i)
				{
					const BoneIndex parent = skeleton.GetParent(BoneIndex((int32_t)i));
					if (parent.IsValid() && parent.Value < boneCount)
					{
						children[parent.Value].push_back(i);
					}
					else
					{
						roots.push_back(i);
					}
				}

				// Accumulated chain length to each bone, computed top-down.
				std::vector<float> depth(boneCount, 0.0f);
				std::vector<int> stack = roots;
				while (!stack.empty())
				{
					const int bone = stack.back();
					stack.pop_back();
					if (seen[bone])
					{
						continue;
					}
					seen[bone] = true;

					for (int child : children[bone])
					{
						if (seen[child])
						{
							continue;
						}
						const glm::vec3 delta =
							rest.Bones[child].Translation - rest.Bones[bone].Translation;
						depth[child] = depth[bone] + glm::length(delta);
						stack.push_back(child);
					}
				}

				float best = 0.0f;
				for (float d : depth)
				{
					best = std::max(best, d);
				}
				return best;
			};

			const float sourceH = chainHeight(source);
			const float targetH = chainHeight(target);
			if (sourceH <= 0.0f || targetH <= 0.0f)
			{
				return 1.0f;
			}
			return targetH / sourceH;
		}

		AnimationClip AnimationRetarget::RetargetClip(
			const AnimationClip &source, const Skeleton &sourceSkeleton,
			const Skeleton &targetSkeleton, const std::vector<Mapping> &overrides,
			float positionScale)
		{
			AnimationClip result(source.GetName());
			result.SetLooping(source.IsLooping());
			result.SetPlayRate(source.GetPlayRate());

			// Name -> target index, so a track is re-keyed by identity rather
			// than by position in the bone array. This is the entire point of
			// the feature: the same clip on two rigs whose bones are ordered
			// differently must animate the same bones.
			std::unordered_map<std::string, BoneIndex> targetIndex;
			for (int i = 0; i < targetSkeleton.GetBoneCount(); ++i)
			{
				targetIndex.emplace(targetSkeleton.GetBoneName(BoneIndex((int32_t)i)),
									BoneIndex((int32_t)i));
			}

			const std::vector<Mapping> map =
				BuildMap(sourceSkeleton, targetSkeleton, overrides);

			for (BoneIndex sourceBone : source.GetAnimatedBones())
			{
				const BoneTrack *track = source.GetTrack(sourceBone);
				if (track == nullptr || track->IsEmpty())
				{
					continue;
				}

				// Resolve the source bone's name through the map. A source bone
				// with no counterpart has no target index and is dropped: a
				// track keyed to a guess would animate the wrong bone.
				std::string targetName;
				for (const Mapping &m : map)
				{
					if (m.SourceBone == sourceSkeleton.GetBoneName(sourceBone))
					{
						targetName = m.TargetBone;
						break;
					}
				}
				if (targetName.empty())
				{
					continue;
				}
				const auto targetIt = targetIndex.find(targetName);
				if (targetIt == targetIndex.end())
				{
					continue;
				}
				const BoneIndex targetBone = targetIt->second;

				// Position: copied key-for-key, with translations scaled. The
				// tangents are copied through the mutable accessor because a
				// retarget that silently flattened every curve to a linear
				// interpolation would change the motion, not just the skeleton
				// it is applied to.
				for (int c = 0; c < 3; ++c)
				{
					const AnimationCurve &curve = track->Position[c];
					for (const Keyframe &key : curve.GetKeys())
					{
						result.AddKey(targetBone, TrackChannel::Position, c,
									  key.Time, key.Value * positionScale, key.Mode);
					}
					// AddKey cannot carry a tangent, so the keys are copied
					// first and the tangents written afterwards.
					AnimationCurve &written = result.GetTrack(targetBone)->Position[c];
					const std::vector<Keyframe> &sourceKeys = curve.GetKeys();
					std::vector<Keyframe> &writtenKeys = written.GetKeys();
					for (size_t k = 0; k < sourceKeys.size() && k < writtenKeys.size();
						 ++k)
					{
						writtenKeys[k].InTangent = sourceKeys[k].InTangent * positionScale;
						writtenKeys[k].OutTangent = sourceKeys[k].OutTangent * positionScale;
					}
				}

				// Rotation: copied unchanged. A rotation cannot be scaled
				// proportionally -- there is no meaningful "1.3x this rotation"
				// -- so a limb-length difference shows up only in the
				// translation scale, which is the correct behaviour for a
				// retarget without IK.
				for (const RotationTrack::Key &key : track->Rotation.GetKeys())
				{
					result.AddRotationKey(targetBone, key.Time, key.Value, key.Mode);
				}

				// Scale: copied unchanged, and NOT scaled by the position
				// scale. A track that already authorises a bone's relative size
				// is a deliberate choice; multiplying it by the height ratio
				// would double-apply a proportion the rig already carries.
				for (int c = 0; c < 3; ++c)
				{
					const AnimationCurve &curve = track->Scale[c];
					for (const Keyframe &key : curve.GetKeys())
					{
						result.AddKey(targetBone, TrackChannel::Scale, c,
									  key.Time, key.Value, key.Mode);
					}
					AnimationCurve &written = result.GetTrack(targetBone)->Scale[c];
					const std::vector<Keyframe> &sourceKeys = curve.GetKeys();
					std::vector<Keyframe> &writtenKeys = written.GetKeys();
					for (size_t k = 0; k < sourceKeys.size() && k < writtenKeys.size();
						 ++k)
					{
						writtenKeys[k].InTangent = sourceKeys[k].InTangent;
						writtenKeys[k].OutTangent = sourceKeys[k].OutTangent;
					}
				}
			}

			return result;
		}

		std::string AnimationRetarget::SourceBoneForTarget(
			const std::vector<Mapping> &map, const std::string &targetBone)
		{
			for (const Mapping &m : map)
			{
				if (m.TargetBone == targetBone)
				{
					return m.SourceBone;
				}
			}
			return std::string();
		}
	}
}
