#pragma once

#include "Wasteland/Animation/AnimationClip.h"
#include "Wasteland/Animation/Skeleton.h"

#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wasteland {

	namespace Animation {

		// One playing clip within an Animator.
		//
		// A struct rather than a bare clip pointer because playback state (time,
		// speed, weight, looping) is per-instance, not per-asset: the same walk
		// cycle plays on ten enemies with ten different phases.
		struct AnimationInstance
		{
			std::shared_ptr<AnimationClip> Clip;

			// Playback cursor, in the clip's own time units.
			float Time = 0.0f;

			// Playback speed multiplier on top of the clip's own rate. Negative
			// plays backwards, which is how a rewind plays without a separate
			// reversed clip.
			float Speed = 1.0f;

			// 0..1 contribution when several instances are blended together.
			// Weights are normalized before use, so relative values are what
			// matter and callers do not have to keep them summing to 1.
			float Weight = 1.0f;

			// When true, Time wraps at the clip's end. When false it holds
			// there and IsFinished becomes true.
			bool Looping = true;

			// Set by the update loop, and the flag RemoveAllInstances and
			// ClampInstanceTime both consult. Not derived from Time, so a caller
			// that sets Time past the end via SetTime does not get IsFinished
			// until the next Update runs.
			bool IsFinished = false;

			// Called when playback completes a non-looping clip, so a
			// death animation can fire a ragdoll or a state change.
			// std::function, so a capture can be anything. DEFECT (lifetime):
			// nothing here checks whether the owning object is still alive when
			// this fires. A lambda capturing `this` on a destroyed entity is a
			// use-after-free the moment its death animation completes, and the
			// type system cannot catch it.
			std::function<void()> OnFinished;

			// Cheap guard for a struct whose Clip is a shared_ptr that may be
			// null after a move or a failed AddInstance. Both halves matter: a
			// valid but EMPTY clip would otherwise divide by a zero duration.
			bool IsValid() const { return Clip && !Clip->IsEmpty(); }
			// Returns 0 for a null clip rather than dereferencing it, so this
			// is safe to call on an invalid instance -- but a caller using the
			// 0 to advance Time would silently stall that instance instead of
			// retiring it, which is why IsValid is checked separately.
			float GetDuration() const { return Clip ? Clip->GetDuration() : 0.0f; }
		};

		// Drives instances over a skeleton and produces one blended pose.
		//
		// This is the runtime equivalent of Unity's Animator and Blender's
		// Action editor: it owns playback time, blending and layer weighting,
		// and hands back a Pose. It deliberately knows nothing about HOW a
		// pose is drawn -- no meshes, no skinning, no renderer -- so the same
		// animator drives a skinned character, a swinging door, and a turret
		// barrel.
		class Animator
		{
		public:
			Animator() = default;

			// ---- Skeleton ----

			void SetSkeleton(std::shared_ptr<Skeleton> skeleton) { m_Skeleton = std::move(skeleton); }
			std::shared_ptr<Skeleton> GetSkeleton() const { return m_Skeleton; }

			// ---- Instance management ----

			// Starts a clip as a new instance and returns its id, or -1 if the
			// clip is null or empty.
			//
			// Adding rather than replacing is what makes crossfade possible:
			// the old instance stays alive at decreasing weight while the new
			// one ramps up, so the two overlap instead of snapping.
			int AddInstance(const std::shared_ptr<AnimationClip> &clip);

			// Removes an instance immediately, without a fade.
			void RemoveInstance(int instanceId);

			// Removes an instance by fading its weight to zero, then dropping
			// it. The graceful way to end a state, and what a transition
			// should call.
			void FadeOutInstance(int instanceId, float fadeDuration);

			void RemoveAllInstances();

			// m_Instances.size(), which counts only live instances if a fade-out
			// removes the entry immediately. FadeOutInstance keeps the instance
			// alive while its weight ramps, so during a fade this number
			// includes instances contributing almost nothing.
			int GetInstanceCount() const { return (int)m_Instances.size(); }
			const AnimationInstance *GetInstance(int instanceId) const;
			AnimationInstance *GetInstance(int instanceId);
			bool HasInstance(int instanceId) const;

			// Total weight of all instances. Used to normalize.
			float GetTotalWeight() const;

			// ---- Playback control ----

			// Sets an instance's playback speed.
			void SetSpeed(int instanceId, float speed);
			void SetLooping(int instanceId, bool looping);

			// Jumps an instance's cursor. Used for phase offsets, so ten
			// characters walking the same clip do not move in lockstep.
			// An unconditional jump, not a relative one, and not clamped here.
			// SetTime beyond the clip length is accepted; ClampInstanceTime
			// exists precisely to pull it back so a long session cannot grow
			// the cursor without bound.
			void SetTime(int instanceId, float time);

			// Jumps every instance by the same amount, so a group of enemies
			// desynchronizes with one call.
			void SetGlobalTimeOffset(float offset);

			// ---- Update ----

			// Advances every instance by `deltaSeconds`, drops finished
			// non-looping instances, then blends the survivors into
			// `outPose`.
			//
			// `outPose` is resized to the skeleton size. With no instances it
			// holds the rest pose, so an animator with nothing playing still
			// produces a valid (rest) pose rather than an empty one.
			void Update(float deltaSeconds, Pose &outPose);

			// Blends the current instances into `outPose` without advancing
			// time. Separate from Update so a paused game can still evaluate a
			// pose, and so the editor can scrub without side effects.
			void Evaluate(Pose &outPose) const;

			// ---- Blending ----

			// How poses are combined.
			enum class BlendMode : uint8_t
			{
				// Replace: later instances override earlier ones.
				Override = 0,
				// Average weighted, matching Unity's default. Position and
				// scale lerp, rotation slerps.
				Interpolate = 1,
				// Add offsets on top of a base pose. Used for recoil layers
				// and additive breathing, where the motion should stack on
				// whatever the base animation is already doing.
				Additive = 2,
			};

			// The base pose that Additive instances stack onto. Defaults to the
			// rest pose.
			void SetAdditiveBase(const Pose &base);
			const Pose &GetAdditiveBase() const { return m_AdditiveBase; }

			// ---- Diagnostics ----

			// The pose produced by the most recent Update. Kept so gameplay
			// code can read bone world positions (for an IK target, a muzzle
			// point, a footstep test) without keeping its own copy.
			const Pose &GetLastPose() const { return m_LastPose; }

			// The most recent bone world matrices, or empty before the first
			// Update.
			const std::vector<glm::mat4> &GetLastWorldMatrices() const { return m_LastWorld; }

		private:
			// Blends `src` into `dst` scaled by `weight`.
			static void BlendPose(Pose &dst, const Pose &src, float weight,
								  BlendMode mode, const Pose &additiveBase);

			// Stops an instance's time from exceeding its clip length in a way
			// that would keep growing across a long session.
			// Static despite taking the instance by reference: it mutates only
			// playback state, so no Animator member is needed to do it.
			static void ClampInstanceTime(AnimationInstance &instance);

			std::shared_ptr<Skeleton> m_Skeleton;

			// Sparse so ids are stable for the lifetime of an instance even as
			// others are removed -- a caller holding instance 3 must not have
			// it silently become a different clip.
			std::unordered_map<int, AnimationInstance> m_Instances;
			// Starts at 0, so instance id 0 is legitimate. That is what makes the
			// -1 in AddInstance's contract unambiguous -- a caller must not
			// treat 0 as "no instance".
			int m_NextInstanceId = 0;

			// Instances being faded out. Both the remaining time AND the
			// weight the fade started from are needed:
			//
			// weight = startWeight * (remaining / totalDuration)
			//
			// Storing only the remainder loses the denominator, so the ramp
			// cannot be reconstructed -- and a fade started from weight 0.3
			// must fade 0.3 -> 0, not 1 -> 0.
			struct FadeState
			{
				float Remaining = 0.0f;
				float Duration = 1.0f;
				float StartWeight = 1.0f;
			};
			// A SEPARATE map from m_Instances, keyed by the same ids. An
			// instance is therefore present in both while fading, and the
			// update loop is what has to consult both to know it still exists.
			// Two maps to keep in step is the cost of not disturbing the live
			// instance's weight bookkeeping mid-fade.
			std::unordered_map<int, FadeState> m_FadingOut;

			// DEFECT (unreachable member): m_BlendMode is private, the only
			// constructor is `= default` above, and no method in this class
			// assigns it -- BlendPose takes the mode as a PARAMETER instead.
			// So BlendMode::Override and BlendMode::Additive can never be
			// selected by any caller: every blend is Interpolate. Either add a
			// SetBlendMode accessor or pass the mode into Update/Evaluate.
			BlendMode m_BlendMode = BlendMode::Interpolate;

			Pose m_AdditiveBase;
			Pose m_LastPose;
			std::vector<glm::mat4> m_LastWorld;
		};

	} // namespace Animation

} // namespace Wasteland
