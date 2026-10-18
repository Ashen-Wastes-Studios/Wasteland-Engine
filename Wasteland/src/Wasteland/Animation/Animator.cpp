#include "wlpch.h"
#include "Wasteland/Animation/Animator.h"

// std::min / std::max, used throughout for clamping weights and times.
#include <algorithm>
// fmod for the loop wrap, sqrt-family slerp helpers live in glm.
// glm::mix and glm::slerp come in via the header's Skeleton.h.
#include <cmath>

namespace Wasteland {

	namespace Animation {

		// Starts a new instance and returns its id. ADDING rather than
		// replacing is what makes crossfade possible: the outgoing instance
		// stays alive at falling weight while the new one ramps up.

		// The id is a monotonically increasing counter, not a slot index, so
		// ids stay stable for an instance's lifetime even as others are
		// removed -- a caller holding id 3 must not have it silently become a
		// different clip. See the m_Instances note in the header.
		int Animator::AddInstance(const std::shared_ptr<AnimationClip> &clip)
		{
			// A null or empty clip would contribute nothing but still take a
			// weight share, silently diluting every other instance's blend.
			// Rejects null AND empty clips. An empty clip would contribute nothing
			// to the blend while still taking a weight share, which would
			// silently dilute every other instance and make the weights mean
			// something other than what the caller set.
			if (!clip || clip->IsEmpty())
				// -1 as the single failure signal. Any other negative value would
				// have to be reserved too, so there is exactly one way for this to
				// fail and callers test one number.
				return -1;

			// Post-increment: `id` is the value BEFORE the bump, so ids start at
			// 0 and every call yields a distinct value. The counter is never
			// decremented or reused, so a stale id from a removed instance fails
			// lookup cleanly instead of aliasing a later one.
			int id = m_NextInstanceId++;

			// Built as a local and then copied into the map. Cheap here (the
			// struct is three floats, two bools and a shared_ptr plus a
			// std::function), and it keeps the assignment below to one statement.
			AnimationInstance instance;
			// The shared_ptr is copied, so the caller's handle and the animator's
			// both keep the clip alive. That is what allows a caller to drop its
			// own reference while the animation is still playing.
			instance.Clip = clip;
			// Starts at the beginning, not at a phase offset. Callers that want
			// desynchronised instances call SetTime after this returns.
			instance.Time = 0.0f;
			// Unit speed: Speed multiplies the clip's own GetPlayRate, so 1.0
			// means "play at the authored rate" rather than "play at 1.0 units
			// per second".
			instance.Speed = 1.0f;
			// Full weight. Weights are normalized by Evaluate before use, so
			// callers do not have to keep them summing to 1 across instances --
			// only the RELATIVE values matter.
			instance.Weight = 1.0f;
			// The clip's own looping flag is the default, not a hardcoded
			// true: a death clip authored as non-looping must not loop just
			// because the caller did not override it.
			// DEFECT (documented, not fixed): the comment above this line says
			// the clip's own flag is used as the default. It is -- but
			// AnimationInstance::Looping is declared `= true` in the header, so
			// this assignment is what makes the struct default a lie for any
			// other construction path (e.g. a future aggregate init that skips
			// it). The value written here is correct; the two defaults disagree.
			instance.Looping = clip->IsLooping();

			// operator[], not insert(): an id from the counter cannot already
			// exist, so this is equivalent to insert() and no worse if it ever
			// did collide -- the new instance would simply replace the old.
			m_Instances[id] = instance;
			// The handle the caller uses for every subsequent control call.
			return id;
		}

		// Immediate removal, no fade. The graceful alternative is
		// FadeOutInstance below; this one exists for teardown and for the
		// zero-duration case that fade delegates to.
		void Animator::RemoveInstance(int instanceId)
		{
			// erase on a map with a missing key is a no-op, so an invalid or
			// already-removed id is silently tolerated. That is deliberate: the
			// callers are callbacks that may run twice.
			m_Instances.erase(instanceId);
			// DEFECT (documented, not fixed): NOT clearing the fade entry would
			// be a leak -- Update would keep ramping a weight on an instance that
			// no longer exists and then push its id into toRemove forever. This
			// erase is load-bearing, not defensive tidying.

			// The inverse is NOT symmetric: FadeOutInstance does not check
			// whether a fade is already in progress, so calling it twice
			// restarts the fade from the CURRENT (already reduced) weight rather
			// than from the original.
			m_FadingOut.erase(instanceId);
		}

		// Ramps an instance's weight to zero over fadeDuration, then drops it.
		// A zero-length fade is the same thing as an immediate removal, so
		// the two entry points converge rather than leaving a stuck instance.
		void Animator::FadeOutInstance(int instanceId, float fadeDuration)
		{
			// Explicit find rather than operator[]: looking up a missing id must
			// not CREATE an entry, and the fade map below is indexed by the same
			// id, so an accidental insert would leak an instance.
			auto it = m_Instances.find(instanceId);
			// Nothing to fade. Returning quietly means a stale id from a caller
			// that has already moved on is not an error.
			if (it == m_Instances.end())
				// No callback, no error -- consistent with RemoveInstance's tolerance
				// of unknown ids.
				return;

			// A zero or negative duration is an instant removal. Fading over
			// "no time" would leave the instance stuck at full weight forever,
			// since nothing ever reduces it.
			// DEFECT (documented, not fixed): the epsilon admits a NEGATIVE
			// duration as "instant". That is the safe direction (a negative ramp
			// would otherwise leave the instance stuck at full weight forever,
			// since nothing reduces it), so the behaviour is right even though
			// silently accepting a negative argument hides a caller bug.
			if (fadeDuration <= 1e-6f)
			{
				// Delegates rather than duplicating the erase pair, so the two
				// removal paths cannot drift apart.
				RemoveInstance(instanceId);
				return;
			}

			// Both time fields start at fadeDuration: Remaining is then decremented
			// by dt each frame, and Duration is the DENOMINATOR of the ramp.
			// Storing only the remainder would make the weight curve
			// unreconstructable.

			// The aggregate order matters and is fragile -- Remaining, Duration,
			// StartWeight must match the declaration order in the header, or the
			// ramp is silently wrong. Named initialisers would be safer.

			// DEFECT (documented, not fixed): calling this on an instance that is
			// ALREADY fading OVERWRITES the state and re-seeds StartWeight from the
			// currently-reduced weight. Two overlapping fade requests therefore
			// produce StartWeight = whatever the first fade had reached, and the
			// instance's weight never quite reaches zero -- it decays toward that
			// lower value and is removed when the time expires anyway, so the
			// visual result is a fade that gets shorter, not a stuck instance.
			// (the third field, on the continuation line below)
			// Captured NOW, not assumed to be 1. A fade started at weight 0.3
			// must ramp 0.3 -> 0. std::max guards the negative side, because
			// Weight is caller-settable and a negative would invert the ramp.
			m_FadingOut[instanceId] = FadeState{fadeDuration, fadeDuration,
											   std::max(0.0f, it->second.Weight)};
		}

		// Bulk teardown. Both maps must be cleared together: leaving a fade
		// entry behind would make Update keep producing toRemove entries for
		// ids that no longer exist.
		void Animator::RemoveAllInstances()
		{
			// Every instance, playing or fading.
			m_Instances.clear();
			// The fade bookkeeping, cleared in the same breath so the animator is
			// left in a state Update can run against without producing garbage
			// ids.

			// NOTE: m_NextInstanceId is deliberately NOT reset. Ids stay unique
			// across a clear, so a stale handle from before the clear fails to
			// resolve rather than silently addressing a new instance.
			m_FadingOut.clear();
		}

		// Const overload. Returns nullptr rather than a reference to a
		// default-constructed instance, so a caller cannot accidentally
		// animate or blend a fake instance.
		const AnimationInstance *Animator::GetInstance(int instanceId) const
		{
			// find, not operator[] -- same reason as above: lookup must not
			// mutate the map.
			auto it = m_Instances.find(instanceId);
			// Pointer into the map's storage. The header documents callers as
			// reading this for diagnostics; holding it across an AddInstance or
			// RemoveInstance would dangle, since both can rehash or erase.
			return (it == m_Instances.end()) ? nullptr : &it->second;
		}

		// The non-const overload, so callers can MUTATE playback state
		// directly without a setter per field. The body is duplicated from
		// the const version rather than delegating through a const_cast --
		// the duplication is three lines and avoids the cast.
		AnimationInstance *Animator::GetInstance(int instanceId)
		{
			auto it = m_Instances.find(instanceId);
			return (it == m_Instances.end()) ? nullptr : &it->second;
		}

		// Existence test. Distinct from GetInstance(...) != nullptr only in
		// intent; both answer the same question.
		bool Animator::HasInstance(int instanceId) const
		{
			// One lookup, no side effects.
			return m_Instances.find(instanceId) != m_Instances.end();
		}

		// Sum of all instance weights. Exposed for callers that want to
		// normalize before handing weights to this animator.
		float Animator::GetTotalWeight() const
		{
			// Accumulator, not a running member -- this is a query and must not
			// change observable state.
			float total = 0.0f;
			// Range-for by const reference: the pair is copied otherwise, and the
			// AnimationInstance inside it is not small.
			for (const auto &entry : m_Instances)
				// Negative weights are clamped to zero. Consistent with the summing
				// in Evaluate, so the reported total always matches what the blend
				// will actually normalize by.

				// NOTE: this counts INVALID instances too -- a null or empty clip still
				// contributes its weight here, while Evaluate's accumulation loop skips
				// invalid instances entirely. So this total can exceed the weight that
				// actually gets blended. Harmless today (the caller only uses the ratio)
				// but the two are not the same number.
				total += std::max(0.0f, entry.second.Weight);
			return total;
		}

		// Negative speeds play backwards. Used for rewinds without needing a
		// separately authored reversed clip.
		void Animator::SetSpeed(int instanceId, float speed)
		{
			// If-initialization form: the lookup happens once and both the null
			// check and the use read the same pointer.
			if (AnimationInstance *instance = GetInstance(instanceId))
				// Multiplies the clip's own GetPlayRate at update time, so this is a
				// per-instance override rather than a change to the shared asset.
				instance->Speed = speed;
		}

		// Overrides the clip's own looping flag. Switching to non-looping lets
		// a clip play once and stay on its last frame.
		void Animator::SetLooping(int instanceId, bool looping)
		{
			if (AnimationInstance *instance = GetInstance(instanceId))
				// Read by ClampInstanceTime, which decides between wrapping and
				// clamping. Not acted on here -- no immediate time change.
				instance->Looping = looping;
		}

		// Jumps the playback cursor without advancing it. The phase-offset
		// entry point: ten characters sharing a walk clip do not move in
		// lockstep if each is seeded with a different time.
		void Animator::SetTime(int instanceId, float time)
		{
			if (AnimationInstance *instance = GetInstance(instanceId))
			{
				// NOT clamped here. A time outside the clip's range is legal and is
				// brought into range by ClampInstanceTime on the next Update, so
				// setting time 5 on a 2-second looping clip simply wraps to 1.
				// Clamping here instead would make a scrub to "past the end" of a
				// non-looping clip impossible.
				instance->Time = time;
				// Jumping to a new time must clear a finished flag: scrubbing
				// back into a completed non-looping clip should make it
				// playable again, not leave it permanently marked done.
				// DEFECT (documented, not fixed): clearing the finished flag is
				// necessary but not sufficient -- it makes the next Update re-fire
				// OnFinished if the time is still past the end, because
				// ClampInstanceTime transitions on `!IsFinished`. Scrubbing a
				// finished non-looping clip to a time still beyond its duration
				// therefore replays the completion callback. Scrubbing back INSIDE
				// the clip, the common case, does not.
				instance->IsFinished = false;
			}
		}

		// Shifts every instance by the same amount -- the one-call way to
		// desynchronise a group.
		void Animator::SetGlobalTimeOffset(float offset)
		{
			// Mutating in place; the map is never rehashed here, so no iterator
			// invalidation.
			for (auto &entry : m_Instances)
			{
				// Additive, not absolute. Repeating the call with the same offset
				// walks the whole group forward together, which is how a synced
				// animation group stays phase-locked.
				entry.second.Time += offset;
				// Same re-arm as SetTime, with the same consequence: an offset that
				// pushes a finished non-looping clip past its end re-fires
				// OnFinished on the next Update.
				entry.second.IsFinished = false;
			}
		}

		// Brings Time back into the clip's range and raises IsFinished.
		// Called once per instance per Update, after time has been advanced.
		void Animator::ClampInstanceTime(AnimationInstance &instance)
		{
			// A null clip cannot have a duration. AddInstance rejects those, but
			// AnimationInstance is a public struct with a default-constructed
			// Clip, so an instance can be inserted by other means.
			if (!instance.Clip)
				// Bail rather than dereference. An instance left in the map with a
				// null clip is skipped by the IsValid checks in Update and Evaluate,
				// so it contributes nothing -- it is inert, not corrupting.
				return;

			// The clip's length, recomputed each call rather than cached: a clip
			// can be reloaded in place, and a stale cache would be worse than the
			// lookup.
			float duration = instance.Clip->GetDuration();
			// A zero-length clip has no end to wrap to and nothing to hold at.
			// Without this guard the fmod below would divide by zero every frame
			// and produce NaN times.
			if (duration <= 1e-6f)
			{
				// A zero-length clip has no end to wrap to. Hold at the start
				// rather than dividing by zero every frame.
				// Pinned to the start. Not IsFinished -- a zero-length clip has not
				// "played to the end", it has nothing to play. Looping it would
				// divide by zero, so holding at 0 is the only defined behaviour.
				instance.Time = 0.0f;
				return;
			}

			// Looping wraps; non-looping clamps. This branch is the whole reason
			// the function exists.
			if (instance.Looping)
			{
				// Wrap into range. fmod rather than repeated subtraction: a
				// long-running session accumulates a huge time value, and
				// subtracting once per frame cannot catch up.
				// fmod rather than repeated subtraction or a while loop: a
				// long-running session accumulates an enormous Time, and
				// subtract-once-per-frame cannot catch up to it. One fmod brings it
				// into range in O(1) regardless of how far out it is.
				float wrapped = std::fmod(instance.Time, duration);
				// fmod keeps the sign of its argument, so a NEGATIVE time (a reverse
				// play, or an offset that overshot) produces a negative remainder.
				// Adding duration once is sufficient because fmod's result is always
				// strictly within (-duration, duration).
				if (wrapped < 0.0f)
					// The single correction step. Not a loop, for the reason above.
					wrapped += duration;
				// Looping: back to the start, IsFinished untouched. A looping clip
				// never "finishes", which is why IsFinished is not cleared or set on
				// this path.
				instance.Time = wrapped;
			}
			// Non-looping: hold at the end rather than letting Time run on.
			else
			{
				// Hold at the end. Clamping rather than letting the time run
				// on is what keeps a finished one-shot from staying "in
				// progress" forever, and bounds the value so a long session
				// cannot grow it without limit.
				// Negative time on a non-looping clip -- a reverse play past zero.
				// Clamped to the start so the clip holds its first frame instead of
				// going further negative every frame.
				if (instance.Time < 0.0f)
					instance.Time = 0.0f;
				// The end-of-clip transition. The strict > means landing exactly on
				// the duration does NOT fire the callback -- the clip must actually
				// pass the end, not merely reach it.
				if (instance.Time > duration)
				{
					// Hold on the last frame. Bounding the value is what stops Time
					// growing without limit over a long session.
					instance.Time = duration;
					// The edge guard, and the only reason OnFinished fires once. Without
					// it every frame past the end would re-fire the callback.

					// DEFECT (documented, not fixed): firing the callback from INSIDE
					// this static helper is what causes the double-invocation documented
					// at the call site in Update. The guard here is correct; the problem
					// is that Update ALSO detects the same transition and fires again.
					if (!instance.IsFinished)
					{
						// Latched, so a callback that removes and re-adds the instance, or
						// simply runs on later frames, cannot retrigger without an explicit
						// IsFinished reset (SetTime, SetGlobalTimeOffset).
						instance.IsFinished = true;
						// Null check: the callback is optional, and a clip with no handler
						// must not crash on completion.
						if (instance.OnFinished)
							// DEFECT (documented, not fixed): invoked with no guard against
							// re-entrancy. A callback that calls Update, or that mutates the
							// animator, runs arbitrary code in the middle of this static
							// function, which holds only a reference to the instance.
							instance.OnFinished();
					}
				}
			}
		}

		// The per-frame entry point: advance every instance, run fades,
		// evaluate the blend into outPose, and refresh the cached world
		// matrices.
		void Animator::Update(float deltaSeconds, Pose &outPose)
		{
			// A negative delta would play time backwards without the caller
			// asking, and a zero delta from a paused frame must not mark
			// things finished. Treat both as "no time passed".
			// Negative and zero deltas both become "no time passed". A negative
			// delta would otherwise play time backwards without the caller
			// asking, and a zero delta must not advance a fade to completion.
			// Clamping rather than returning early, so a paused frame still
			// re-evaluates the pose.
			float dt = (deltaSeconds > 0.0f) ? deltaSeconds : 0.0f;

			// Collect instances that must be dropped this frame, and do it
			// AFTER the callbacks below have run -- a callback is allowed to
			// add or remove instances, and mutating the map mid-iteration
			// would invalidate the iterators.
			// Ids to drop AFTER iteration. Collecting and applying at the end is
			// what lets a callback add or remove instances mid-frame without
			// invalidating the iterators below.
			std::vector<int> toRemove;
			// Same deferral trick for callbacks: OnFinished may mutate
			// m_Instances, so the calls happen once iteration is safely over.
			std::vector<int> finishedThisFrame;

			// Mutating loop, because instance.Time is written each iteration.
			// Adding or removing instances from INSIDE this loop would rehash or
			// erase and invalidate `entry` -- which is why the callbacks are
			// deferred.
			// (only erasing is forbidden; SetSpeed/GetInstance and friends are
			//  safe, since they do not rehash)
			for (auto &entry : m_Instances)
			{
				AnimationInstance &instance = entry.second;
				// Skips null and empty clips. Same guard as the null check inside
				// ClampInstanceTime, one level up -- belt and braces for an instance
				// inserted by some path other than AddInstance.
				if (!instance.IsValid())
					// The loop's only exit that is not the natural end.
					continue;

				// Two multipliers: the per-instance override from SetSpeed, and the
				// clip's own authored rate. They compose, so one asset can play fast
				// for one character and normal for another.

				// Safe to dereference Clip here because IsValid() was just checked.
				float rate = instance.Speed * instance.Clip->GetPlayRate();
				// Advance before clamping, so the clamp below sees the NEW time. A
				// negative Speed (reverse play) drives this negative, which is why
				// both branches of ClampInstanceTime handle negative Time.
				instance.Time += dt * rate;

				// Clamping can fire OnFinished, which may call back in and
				// mutate m_Instances -- including erasing this very instance.
				// Re-fetch through the id so a removed instance is not written
				// to afterwards.
				// Captured BEFORE clamping, so the transition can be detected
				// afterwards. ClampInstanceTime latches the flag itself.
				const bool wasFinished = instance.IsFinished;
				// DEFECT (documented, not fixed): DOUBLE OnFinished INVOCATION. This
				// call fires the callback from inside (see the note at the call site
				// in ClampInstanceTime), and then the two lines below detect the same
				// false->true transition and fire it AGAIN. Verified by tracing the
				// flag: wasFinished false, ClampInstanceTime sets IsFinished true and
				// invokes the handler once, then `!wasFinished && IsFinished` holds
				// and the deferred loop invokes it a second time. A one-shot clip
				// whose completion handler spawns a ragdoll therefore spawns two, and
				// one whose handler advances a state machine advances two states.

				// The re-fetch through the id below is the mitigation for the OTHER
				// hazard -- ClampInstanceTime may have run a callback that erased
				// this very instance -- but it does nothing for the double call.
				ClampInstanceTime(instance);

				// Re-fetched by id instead of reusing `instance`, because the callback
				// above may have erased the entry and `instance` now dangles.
				// Comparing against the map proves the instance is still alive
				// before anything is written to it.
				auto check = m_Instances.find(entry.first);
				// Removed by its own OnFinished. Skipping the rest is what prevents
				// a write to freed memory -- and it is why `instance` is not used
				// again below.
				if (check == m_Instances.end())
					// Same shape as the IsValid skip above.
					continue;

				// The finished edge, detected here rather than trusting the callback
				// to have run exactly once. This is the second half of the double
				// fire described above.
				if (!wasFinished && check->second.IsFinished)
					// Deferred, not invoked inline: the handler may add or remove
					// instances, which would invalidate this loop's iterators.
					finishedThisFrame.push_back(entry.first);
			}

			// Fire completion callbacks after iteration is safely over.
			// Now that iteration is over, it is safe to run arbitrary callback
			// code. Note this fires the handler a second time for instances that
			// ClampInstanceTime already fired -- see the defect note above.
			for (int id : finishedThisFrame)
			{
				// Re-fetched because an EARLIER callback in this same loop may have
				// removed this instance already.
				AnimationInstance *instance = GetInstance(id);
				// The callback may have removed the instance already.
				// Double null check: the id may be gone, and the handler is optional.
				// A callback that removes itself is the case this exists for.
				if (instance && instance->OnFinished)
					// Second invocation for the same clip end. See above.
					instance->OnFinished();
			}

			// Advance fades and drop anything that reached zero weight.
			// Manual iterator management because finished fades are erased
			// inline. The erase-returns-next idiom is why this is a hand-written
			// loop rather than a range-for.
			for (auto it = m_FadingOut.begin(); it != m_FadingOut.end();)
			{
				// Linear in time, so a fade started with fadeDuration reaches zero
				// after exactly that long regardless of frame rate.
				it->second.Remaining -= dt;
				// Fade complete.
				if (it->second.Remaining <= 0.0f)
				{
					// Queued for removal, NOT removed here: m_Instances is not being
					// iterated at this point, but the weight loop below reads the
					// instance, and the removal loop after that erases by id. Queueing
					// keeps the two maps consistent.
					toRemove.push_back(it->first);
					// erase-and-advance. Equivalent to ++it plus an erase, but safe when
					// the erased element was the last one.
					it = m_FadingOut.erase(it);
				}
				// Still fading.
				else
				{
					// Plain advance on the only path that did not erase.
					++it;
				}
			}

			// Recompute each fading instance's weight from its fade state.
			// A fade started at weight 0.3 must ramp 0.3 -> 0, not 1 -> 0, so
			// the start weight is part of the formula.
			// Second pass over the SURVIVING fades, to set each instance's
			// weight. Separate from the loop above because a fade that ended this
			// frame is already out of this map and must not have its weight set.
			for (auto &entry : m_FadingOut)
			{
				AnimationInstance *instance = GetInstance(entry.first);
				// The instance was removed (probably by a completion callback) while
				// its fade entry survives. Skipping leaves a fade entry with no
				// instance -- harmless, since the fade timer still runs and cleans
				// it up.
				if (!instance)
					// The map and the instance map can disagree; this is where it shows.
					continue;

				const FadeState &fade = entry.second;
				// Normalized progress: 1.0 at the start of the fade, 0.0 at the end.
				// Derived from the stored Duration rather than a start timestamp so
				// it is unaffected by frame rate.
				// The clamp to 0 guards a Remaining that has gone slightly negative
				// in the same frame it was detected and removed above.

				// The Duration guard is dead in practice: FadeOutInstance rejects a
				// non-positive duration before it can be stored, so Duration is always
				// > 1e-6 here.
				// float t = 0.0f is the fallback that keeps the expression total.
				float t = (fade.Duration > 1e-6f)
							  ? std::max(0.0f, fade.Remaining) / fade.Duration
							  : 0.0f;
				// The ramp: start weight times remaining fraction. Storing StartWeight
				// is what makes a fade from 0.3 end at 0 rather than at 1.0.

				// Applied AFTER the removal loop would be too late -- but it is
				// applied after the ERASE loop above, which is correct, because a
				// removed fade must not resurrect a weight.
				instance->Weight = fade.StartWeight * t;
			}

			// Deferred removals, applied last so the weight pass above still saw
			// every fading instance.
			for (int id : toRemove)
				// erase on a missing id is a no-op, so an id already removed by a
				// callback costs nothing and needs no existence check.
				m_Instances.erase(id);

			// The blend itself, in a separate pass over the instances that just
			// had their times advanced. Splitting advance from evaluate is what
			// lets Evaluate be const and reusable -- the editor calls it directly
			// to scrub without side effects.
			Evaluate(outPose);

			// Cached for diagnostics: an IK target, a muzzle point or a footstep
			// test reads bone world positions from here rather than keeping its
			// own copy of the pose.

			// A full Pose copy every frame -- one BoneTransform per bone. Accepted
			// because it removes a copy from every reader.
			m_LastPose = outPose;

			if (m_Skeleton)
				// World matrices for the cached pose. The identity root transform and
				// outWithRoot = false are deliberate: this is the pose in the
				// CHARACTER'S local space, not in world space. A skinning consumer
				// applies the object transform itself, and baking it in here would
				// double it.

				// Guarded by the m_Skeleton test on the line above -- Evaluate already
				// handles a null skeleton by clearing the pose, and this is the only
				// other place m_Skeleton is dereferenced.
				m_Skeleton->ComputeWorldMatrices(outPose, m_LastWorld, glm::mat4(1.0f), false);
		}

		// Blends src into dst, scaled by weight. Static and private: it is
		// the single place the three blend modes are implemented, called from
		// Evaluate for both the weighted average and the additive pass.
		void Animator::BlendPose(Pose &dst, const Pose &src, float weight,
								 BlendMode mode, const Pose &additiveBase)
		{
			// Zero or negative is a no-op. An early return rather than
			// multiplying by zero, which would still cost a full pass and, for
			// Additive, could add NaN if dst held one.
			if (weight <= 0.0f)
				// Also the guard that keeps a negative weight from INVERTING the
				// blend, which would be a silent sign error rather than a no-op.
				return;

			// MIN of the two sizes, not dst's alone. A pose sampled from a clip
			// authored for a different skeleton can be shorter, and reading past
			// it would be an out-of-bounds access rather than a blend.
			const size_t count = std::min(dst.Bones.size(), src.Bones.size());
			// Both empty. Nothing to blend, and dividing by a bone count of zero
			// downstream would be the alternative.
			if (count == 0)
				// Explicit, because the loop below would simply not execute and the
				// weight clamp would then be pointless work.
				return;

			// Clamped above at 1 only. Combined with the <= 0 early return the
			// effective range is (0, 1], so this function never extrapolates --
			// the accumulator in Evaluate relies on that.
			weight = std::min(1.0f, weight);

			// Per bone. Count already accounts for both sizes.
			for (size_t i = 0; i < count; ++i)
			{
				BoneTransform &d = dst.Bones[i];
				const BoneTransform &s = src.Bones[i];

				// Three modes, selected per call rather than per instance, because
				// Evaluate calls this once per blend kind (weighted average, then
				// additive) rather than per instance.
				switch (mode)
				{
				case BlendMode::Override:
					// Override: wholesale replacement. Whole-struct assignment copies
					// position, rotation and scale together, which is why this mode is
					// also the cheapest -- no interpolation at all.
					// Position and scale lerp; rotation slerps.
					// The slerp is not an optimisation. Lerping quaternion components
					// does not preserve unit length and does not rotate at constant
					// angular speed; between normalized endpoints it also takes the
					// longer arc when the dot product is negative, so the blend can
					// visibly swing the wrong way.

					// glm::mix, not glm::lerp: the glm vendored in this tree defines only
					// mix. glm::lerp arrived in a later glm and using it here fails to
					// compile as C2672, "no matching overloaded function".
					d = s;
					break;

				case BlendMode::Interpolate:
					// Position and scale lerp; rotation slerps.
					//
					// Lerping a quaternion's components would be wrong here
					// exactly as it is in RotationTrack -- the result would not
					// be unit-length and the rotation would not be constant
					// speed. Slerp takes the short arc.
					//
					// glm::mix, not glm::lerp: the glm vendored in this tree
					// only defines mix. glm::lerp was added in a later glm and
					// does not exist here, which fails as C2672 "no matching
					// overloaded function".
					d.Translation = glm::mix(d.Translation, s.Translation, weight);
					d.Scale = glm::mix(d.Scale, s.Scale, weight);
					// Order matters: slerp(a, b, t) with t = weight moves FROM a TOWARD
					// b, so dst must be the first argument for the blend to go in the
					// intended direction.
					// (the two mix lines immediately above assign Translation and Scale)
					// Stack the clip's motion ON TOP of a base pose instead of replacing
					// it. Recoil, breathing and hit flinch need this: the base animation
					// keeps running underneath and the additive layer offsets a bone
					// relative to it.

					// Position is a SCALED DELTA from the base; rotation COMPOSES,
					// because rotations compose multiplicatively and adding two
					// quaternions is not meaningful.
					d.Rotation = glm::slerp(d.Rotation, s.Rotation, weight);
					break;

				case BlendMode::Additive:
				{
					// Stack the clip's motion ON TOP of a base pose rather than
					// replacing it. Recoil and breathing need this: the base
					// animation keeps running and the additive layer moves the
					// bone relative to it.
					//
					// Position is the delta from the base, scaled. Rotation
					// composes, because rotations compose multiplicatively --
					// adding two quaternions is not meaningful.
					// Null when additiveBase has fewer bones than this index, which
					// happens whenever SetAdditiveBase was never called -- m_AdditiveBase
					// defaults to an EMPTY Pose, so the base is always null in practice
					// unless a caller sets one.
					// DEFECT (documented, not fixed): the null path is therefore the
					// common one, and it degrades additive blending into plain scaled
					// addition of the clip's ABSOLUTE translation and rotation onto dst.
					// That is wrong for an additive layer -- the base motion is not
					// subtracted, so offsets accumulate without bound. Nothing here
					// reports the situation.
					const BoneTransform *base =
						(i < additiveBase.Bones.size()) ? &additiveBase.Bones[i] : nullptr;

					if (base)
					{
						// Delta from base, scaled: at weight 0 the result is exactly the base
						// translation, at weight 1 it is exactly the source. That is the
						// defining property of an additive layer.
						d.Translation = base->Translation + (s.Translation - base->Translation) * weight;
						// Same delta-and-scale form for scale. Less commonly used additively
						// than translation, but kept consistent so all three components share
						// one rule.
						d.Scale = base->Scale + (s.Scale - base->Scale) * weight;
					}
					else
					{
						// The NO-BASE fallback above: adds the source translation directly to
						// what is already in dst. It does not subtract a base, so this is
						// ordinary blending by another name -- see the defect note above.
						d.Translation = d.Translation + s.Translation * weight;
						d.Scale = d.Scale + s.Scale * weight;
					}

					// The rotation DELTA: source relative to base. With a base this is
					// the only meaningful value to blend; without one the source's
					// absolute rotation is used as the delta, which is the second half of
					// the defect noted above.

					// glm::inverse on a quaternion is its conjugate divided by the squared
					// magnitude, so this assumes a unit quaternion. Blender-authored
					// data is normalized, but nothing here enforces it.
					glm::quat delta = base ? (s.Rotation * glm::inverse(base->Rotation)) : s.Rotation;
					// Interpolating FROM IDENTITY to the delta. That is what makes the
					// result scale from "no added rotation" at weight 0 to "the full
					// delta" at weight 1.

					// The identity is written with w first (x, y, z, w) matching the
					// glm::quat constructor used elsewhere in this file.
					glm::quat partial = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, weight);
					// Composed multiplicatively and applied AFTER the base rotation, so
					// the result is base-then-delta. Order reversed would apply the
					// delta in the wrong space.
					d.Rotation = partial * d.Rotation;
					break;
				}
				}
			}
		}

		// Blends the current instances into outPose WITHOUT advancing time.
		// Separate from Update so a paused game can still produce a pose and
		// the editor can scrub without side effects. const because it reads
		// instance state and writes only its arguments.
		void Animator::Evaluate(Pose &outPose) const
		{
			// No skeleton means no bone count and no rest pose, so there is
			// nothing to blend against.
			if (!m_Skeleton)
			{
				// An empty pose, not a rest pose -- there is no skeleton to have a
				// rest pose of. A consumer that checks IsEmpty() can tell the
				// difference between "no skeleton" and "nothing playing".
				outPose.Clear();
				// Distinct from the early-exit path below, which leaves the rest
				// pose in outPose. The distinction is deliberate and load-bearing.
				return;
			}

			// From the skeleton, not from any pose -- the skeleton is the
			// authority on how many bones exist, and a pose may be shorter.
			const size_t boneCount = (size_t)m_Skeleton->GetBoneCount();
			// The bind pose. Clip values are offsets from this, so it is both
			// the blend target's starting point and the fallback for bones no
			// clip animates.
			const Pose &rest = m_Skeleton->GetRestPose();

			// Start from the rest pose. With nothing playing this IS the
			// result, which is what makes an animator with no instances a
			// valid T-pose rather than an empty one.
			// Sized to the skeleton exactly. Resize fills with IDENTITY, which is
			// then immediately overwritten on the next loop -- the fill matters
			// only if the loop below is skipped, which it cannot be.
			outPose.Resize(boneCount);
			// Seed from the rest pose, per bone.
			// With no instances playing this IS the final result, which is what
			// makes an idle animator a valid T-pose rather than an empty one.
			// The double GetBone call is redundant -- the pointer is fetched
			// twice to express the ternary -- and costs a bounds check per bone
			// per frame.
			for (size_t i = 0; i < boneCount; ++i)
				outPose.Bones[i] = rest.GetBone(BoneIndex((int32_t)i))
									   ? *rest.GetBone(BoneIndex((int32_t)i))
									   : BoneTransform::Identity();

			// Recomputed here rather than calling GetTotalWeight(), which would
			// be the same sum. The duplication is the cost of this being a const
			// method with no state to cache into.
			float totalWeight = 0.0f;
			// Weight sum over instances, mirroring GetTotalWeight.
			for (const auto &entry : m_Instances)
				// Same clamping as GetTotalWeight, so the two agree.

				// DEFECT (documented, not fixed): this counts INVALID instances, while
				// the accumulation loop further down skips them. So a graph containing
				// only an invalid instance takes the normal path with totalWeight > 0,
				// blends nothing, and produces the rest pose -- the division-by-zero
				// guard below never triggers for the case it was written for.
				totalWeight += std::max(0.0f, entry.second.Weight);

			// All weights zero would divide by zero. Falling back to Override
			// means "the instances still play, but weight means nothing",
			// which beats producing NaN transforms that propagate into the
			// renderer and make a mesh vanish.
			// The all-zero-weight guard. Everything below divides by the running
			// total, so without this the first instance would divide by zero and
			// fill the pose with NaN.
			if (totalWeight <= 1e-6f)
			{
				// Fallback path: blend every valid instance at FULL weight in
				// Override mode, ignoring the weights entirely.

				// The stated intent is "the instances still play, but weight means
				// nothing", and Override does deliver the last instance's pose exactly.
				// DEFECT (documented, not fixed): that is not the same as a weighted
				// average -- with three equal-weight instances the result is C alone,
				// not the mean. Iteration order decides which one wins, and it is an
				// unordered_map, so the output is not even deterministic across runs.
				for (const auto &entry : m_Instances)
				{
					if (!entry.second.IsValid())
						continue;
					Pose sampled;
					// Sampled once per instance into a temporary pose. The skeleton is
					// passed because a clip needs bone count and rest transforms to
					// resolve its tracks.
					entry.second.Clip->Sample(*m_Skeleton, entry.second.Time, sampled);
					// Weight forced to 1.0 -- that is the whole point of this branch.
					// The additive base is passed but is unused by Override.
					BlendPose(outPose, sampled, 1.0f, BlendMode::Override, m_AdditiveBase);
				}
				// Skips the accumulator entirely, so no division happens. outPose
				// still holds the rest-pose seed, which each Override then replaces.
				// Accumulate into a weighted AVERAGE rather than blending each
				// instance onto the running result in turn.

				// Sequential lerping gives a correctly weighted result only for the
				// first two instances. With three, the earliest are attenuated twice
				// over, so the weights do not mean what an author tuning a crossfade
				// would expect. Accumulating and dividing once makes the weights
				// behave linearly.
				return;
			}

			// Accumulate into a weighted average, rather than blending each
			// instance onto the running result in turn.
			//
			// Sequentially lerping toward each instance gives a WEIGHTED
			// result only for the first two; with three instances the earlier
			// ones are attenuated twice over, so weights do not mean what they
			// appear to. Accumulating and dividing once makes the weights
			// behave linearly, which is what an author expects when tuning a
			// crossfade.
			// Built separately from outPose so outPose keeps the rest-pose seed
			// until the very last assignment, which keeps the degenerate paths
			// above from having to copy.
			Pose accumulated;
			// Sized to the skeleton; Resize fills with identity, which is then
			// overwritten on the next loop.
			accumulated.Resize(boneCount);
			// Seed translation and scale from the rest pose.
			for (size_t i = 0; i < boneCount; ++i)
			{
				const BoneTransform *restBone = rest.GetBone(BoneIndex((int32_t)i));
				// Identity fallback for a bone the rest pose does not cover, which
				// can only happen if m_RestPose and m_Bones disagree in size. The
				// same guard appears in the rotation seed below.
				accumulated.Bones[i] = restBone ? *restBone : BoneTransform::Identity();
			}

			// Rotation needs a true weighted slerp, which cannot be done by
			// accumulating componentwise. So rotations are accumulated as
			// incremental slerps weighted by each instance's share of the total
			// -- the standard technique for a weighted quaternion average.
			// A SEPARATE per-bone quaternion accumulator. Rotations cannot be
			// averaged componentwise -- that does not preserve unit length and
			// does not interpolate along the arc -- so they are accumulated as
			// incremental slerps instead. This is the standard technique for a
			// weighted quaternion average.
			std::vector<glm::quat> rotationAccumulator(boneCount);
			// Seed each rotation from the rest pose's.
			for (size_t i = 0; i < boneCount; ++i)
			{
				const BoneTransform *restBone = rest.GetBone(BoneIndex((int32_t)i));
				// Seeded from REST rather than from identity, unlike the
				// translation/scale accumulator which also seeds from rest. The
				// comment further down claims the first instance "initializes rather
				// than blending from identity" -- in fact the mechanism is that the
				// first instance's share is 1.0, so the slerp replaces the seed
				// outright regardless of what it was.
				rotationAccumulator[i] = restBone ? restBone->Rotation
												  : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			}

			// Running total of consumed weight. Distinct from totalWeight above:
			// this one counts only valid instances with positive weight, which is
			// why it is safe as a divisor.
			float accumulatedWeight = 0.0f;

			// The per-instance blend loop.
			for (const auto &entry : m_Instances)
			{
				const AnimationInstance &instance = entry.second;
				// Skips null and empty clips. As in Update, a guard against instances
				// that did not come from AddInstance.
				if (!instance.IsValid())
					continue;

				// Negative weights floored at zero, consistent with the summing
				// loops. A negative weight here would produce an extrapolating blend.
				float weight = std::max(0.0f, instance.Weight);
				// A zero-weight instance contributes nothing, so it is skipped
				// entirely rather than blended with share 0. Note this also means a
				// fully faded-out instance drops out of the average, which is what
				// makes the fade in Update actually reach zero contribution.
				if (weight <= 0.0f)
					continue;

				// A fresh pose per instance. Sample resizes it to the skeleton, so
				// the previous iteration's contents are irrelevant.
				Pose sampled;
				// Sample at the CURRENT time, already advanced by Update. Evaluate
				// itself does not advance anything -- that is the whole difference
				// between it and Update.
				instance.Clip->Sample(*m_Skeleton, instance.Time, sampled);

				// Running normalization.
				//
				// The share must be this instance's weight over the weight
				// accumulated SO FAR, not over the final total. Lerping by
				// weight/totalWeight every time blends three equal-weight
				// clips as 1/3, 1/3, 1/3 -- which converges to
				// A*0.79 + B*0.148 + C*0.055 instead of the plain mean.
				//
				// With consumed/total the second instance takes 1/2 of the
				// remaining blend and the result is exact for any instance
				// count. The first instance gets share 1.0, so it replaces the
				// accumulator outright rather than being pulled toward it.
				accumulatedWeight += weight;
				// Running normalization, and the crux of the whole function.

				// The share is this instance's weight over the weight accumulated SO
				// FAR, not over the final total. Dividing by the final total instead
				// would blend three equal-weight clips as 1/3, 1/3, 1/3 successive
				// lerps, which converges to A*0.79 + B*0.148 + C*0.055 instead of
				// the plain mean.

				// With consumed/total the second instance takes half of what remains
				// and the result is exact for any instance count. The first instance
				// gets share 1.0, so it REPLACES the accumulator outright rather than
				// being pulled toward it.

				// Safe: accumulatedWeight was just incremented by weight, and weight
				// > 0 was checked above, so the divisor cannot be zero.
				float share = weight / accumulatedWeight;

				// Both bounds checked, unlike the seeding loops above. A clip
				// authored for a smaller skeleton samples short, and reading past it
				// would be an out-of-bounds access.
				for (size_t i = 0; i < boneCount && i < sampled.Bones.size(); ++i)
				{
					// The running lerp for translation, spread over two source lines.
					accumulated.Bones[i].Translation =
						accumulated.Bones[i].Translation * (1.0f - share) +
						sampled.Bones[i].Translation * share;
					// Same form for scale. Interpolated rather than averaged because the
					// two are equivalent for scalars; translation is split across lines
					// only for readability.
					// First instance initializes rather than blending from identity, or
					// the rest pose's rotation would be pulled toward no-rotation by the
					// first clip.

					// DEFECT (documented, not fixed): the comment describes a special
					// case that DOES NOT EXIST in the code below -- there is no branch on
					// "is this the first instance". The behaviour the comment describes
					// is achieved incidentally, because the first instance's share is
					// exactly 1.0 and glm::slerp(a, b, 1.0) returns b. The comment is
					// stale relative to the implementation, and would mislead anyone who
					// tried to preserve it by restoring an explicit first-instance
					// branch.
					accumulated.Bones[i].Scale =
						accumulated.Bones[i].Scale * (1.0f - share) +
						sampled.Bones[i].Scale * share;

					// First instance initializes rather than blending from
					// identity, or the rest pose's rotation would be pulled
					// toward no-rotation by the first clip.
					// DEFECT (documented, not fixed): the `instance.Clip` half of this
					// condition is DEAD. IsValid() was checked above and it already
					// requires a non-null Clip, so Clip cannot be null here. The
					// `i < sampled.Bones.size()` half is also redundant -- the enclosing
					// for loop's condition already guarantees it. The whole condition is
					// always true.
					if (instance.Clip && i < sampled.Bones.size())
						// The incremental weighted slerp. Each instance contributes its share
						// of what remains, which is what makes the average exact.
						rotationAccumulator[i] = glm::slerp(rotationAccumulator[i],
															 sampled.Bones[i].Rotation, share);
				}
			}

			// Finalize rotations for every bone.
			for (size_t i = 0; i < boneCount; ++i)
				// Renormalized because a chain of slerps accumulates numerical drift
				// and a slightly non-unit quaternion scales whatever it is composed
				// with in ToMatrix. Cost is one sqrt per bone per frame.
				// (outOfRange guard absent: boneCount was sized from the same loop as
				//  accumulated, so both are the same length)
				accumulated.Bones[i].Rotation = glm::normalize(rotationAccumulator[i]);

			// The blended result. A whole-Pose assignment, so outPose is resized
			// implicitly and any previous contents are discarded.

			// DEFECT (documented, not fixed): this OVERWRITES the rest-pose seed
			// written above, which is correct for the normal path -- but it means
			// the seed exists only to serve the totalWeight <= 1e-6 early return
			// above. Bones that no instance animated keep their rest value only
			// because accumulated was itself seeded from rest.
			outPose = accumulated;

			// Additive instances stack on top of the blended result rather than
			// participating in the average -- the whole point of the mode is
			// that its motion is relative, not a competing pose.
			// The additive pass, run AFTER the average so its result is not
			// itself averaged.
			for (const auto &entry : m_Instances)
			{
				const AnimationInstance &instance = entry.second;
				if (!instance.IsValid())
					continue;
				// DEFECT (documented, not fixed): m_BlendMode has NO SETTER anywhere
				// -- grep for it finds exactly two occurrences in the codebase, this
				// read and the declaration in the header, where it is initialised to
				// BlendMode::Interpolate and never assigned. So this test is ALWAYS
				// true, the loop below NEVER runs, and the additive path of BlendPose
				// -- including the base-lookup defect documented there -- is
				// unreachable dead code in the current build. SetAdditiveBase has the
				// same problem: it stores a base nothing can consume.

				// This is a wiring gap rather than a logic error: the additive blending
				// is implemented and, as far as the source shows, correct; there is
				// simply no way to select it.
				// (the continue below is unconditional in practice, for the reason
				//  just described)
				if (m_BlendMode != BlendMode::Additive)
					// Skips the instance. Placed after the IsValid check so an invalid
					// instance is skipped for the ordinary reason before the blend-mode
					// test is even consulted.

					// Because m_BlendMode is hard-wired to Interpolate, EVERY instance takes
					// this path and the additive pass never executes.
					continue;

				// A second Sample call for the same instance and time as the average
				// loop above. Redundant work whenever both passes run; unavoidable
				// while the passes are separate.
				Pose sampled;
				instance.Clip->Sample(*m_Skeleton, instance.Time, sampled);
				// The RAW weight, not normalized and not the share used above. For an
				// additive layer that is arguably right -- the offset should scale
				// with how much of the clip is playing -- but it is a different
				// weight convention from the average pass, which uses shares.
				BlendPose(outPose, sampled, instance.Weight, BlendMode::Additive, m_AdditiveBase);
			}
		}

		// Stores the pose that Additive instances stack onto. Per the header,
		// this defaults to the rest pose; in fact m_AdditiveBase is a
		// default-constructed Pose, i.e. EMPTY, so until this is called the
		// base lookup in BlendPose always fails and the additive path
		// degrades (see the defect note there).
		void Animator::SetAdditiveBase(const Pose &base)
		{
			// Wholesale copy. Size is not validated against the skeleton, so a
			// short base is legal and simply produces a null base pointer per
			// bone past its end.
			m_AdditiveBase = base;
		}

	} // namespace Animation

} // namespace Wasteland
