// PCH first, by house rule: it supplies the standard library and the
// engine's logging/instrumentation headers to every translation unit.
// Includes deliberately kept to one project header -- the types below
// (RotationTrack, BoneTrack, AnimationClip) are all declared there.
#include "wlpch.h"
#include "Wasteland/Animation/AnimationClip.h"

// std::lower_bound / upper_bound / distance. The key lists are kept
// sorted by time precisely so lookups are binary searches; Evaluate below
// runs once per animated bone per frame.
#include <algorithm>
// std::abs for the same-time tolerance checks below.
#include <cmath>

// Definitions live in Wasteland::Animation to match the header, so these
// out-of-line bodies resolve their unqualified member names.
namespace Wasteland {

	// Nested namespace to match AnimationClip.h. C++17 would allow the
	// nested-definition form, but the header opens both braces separately.
	namespace Animation {

		// ---------------------------------------------------------------------
		// RotationTrack: a keyframed quaternion channel.
		// ---------------------------------------------------------------------
		// ---------------------------------------------------------------------
		// RotationTrack
		// ---------------------------------------------------------------------

		// Out-of-line definition; the default argument for `mode` lives in the
		// header only, since repeating it here would be a redeclaration error.
		//
		// Takes a const ref so the caller's quaternion is not copied before it
		// has been validated -- the parameter is normalized into a local below.
		void RotationTrack::AddKey(float time, const glm::quat &value,
								   Interpolation mode)
		{
			// Normalize on the way in. A quaternion handed in with a magnitude
			// other than 1 (from a scale baked into an exported transform, or
			// accumulated float error) is not a rotation, and slerping it is
			// undefined. Normalizing at the boundary means every key in the
			// track is guaranteed valid, so Evaluate never has to check.
			// Working copy: the normalization must not touch what the caller passed
			// in, since that object may be a rest pose or a shared cached value.
			glm::quat normalized = value;
			// Squared, not actual, length: the comparison only needs "is this near
			// zero", and dot() is one multiply-add versus a sqrt.
			float lengthSquared = glm::dot(normalized, normalized);
			// 1e-8 on the SQUARED length is a magnitude threshold of 1e-4. Chosen
			// well above float denormals so a quaternion that merely lost precision
			// to accumulated error still normalizes, and well below any real
			// rotation so a genuinely zeroed one is caught.
			if (lengthSquared > 1e-8f)
				// glm::normalize divides by the length; the guard above is what keeps
				// that division from producing NaN/inf on a degenerate input.
				normalized = glm::normalize(normalized);
			// Zero (or all-denormal) quaternion: there is no rotation to recover,
			// so fall back to identity rather than propagating a non-finite value
			// that would poison every downstream slerp.
			else
				// glm::quat's constructor is (w, x, y, z), so this is identity, not a
				// 1.0 on the x axis.
				normalized = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			// lower_bound with a value/element comparator: finds the first key whose
			// Time is >= `time`, i.e. exactly the insertion point for a new key.
			auto it = std::lower_bound(m_Keys.begin(), m_Keys.end(), time,
									   // The comparator compares ONLY Time. Key is never consulted, which is
									   // what makes this a valid strict weak ordering for lower_bound.
									   [](const Key &k, float t) { return k.Time < t; });

			// DEFECT (asymmetric dedupe): this only inspects the key at or AFTER
			// `time`. If a key already sits just BEFORE `time` but within the 1e-6
			// tolerance -- say 0.9999999 when adding at 1.0 -- lower_bound skips it
			// and returns end(), so a SECOND key is inserted one 1e-7 apart instead
			// of replacing the first. Evaluate then degrades that pair to a step
			// (see the dt guard below) and two "equal" times exist with no defined
			// order between them. AnimationCurve::AddKey has the identical shape, so
			// this is a shared trait of the pair, not a one-off.
			if (it != m_Keys.end() && std::abs(it->Time - time) <= 1e-6f)
			{
				// Replace in place rather than insert: two keys at one time have no
				// defined order, so which one sampling sees would depend on insertion
				// sequence. Last write wins is the predictable rule.
				it->Value = normalized;
				// Mode is overwritten with the value. Unlike AnimationCurve, there are
				// no tangents to preserve, so nothing is deliberately left alone.
				it->Mode = mode;
			}
			else
			{
				// `it` is already the correct slot, so insert() does not re-search.
				m_Keys.insert(it, Key(time, normalized, mode));
			}
		}

		// Sample the channel. Clamps rather than extrapolates outside the keyed
		// range, matching AnimationCurve's default -- a naive linear guess past
		// the ends is almost never what was authored.
		glm::quat RotationTrack::Evaluate(float time) const
		{
			// Degenerate cases are handled before any search, so the binary search
			// below can assume at least two keys and a time strictly inside them.
			if (m_Keys.empty())
				// An empty track has no opinion; identity is the one value that is
				// actually neutral here, so this composes correctly rather than merely
				// being a safe-looking default.
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			// Single key: no segment exists to interpolate, and no reason to clamp
			// either -- the lone key is the answer at every time.
			if (m_Keys.size() == 1)
				// Note this IGNORES `time` entirely. A one-key track returns that key
				// for the whole timeline, which is why a mis-authored track with a
				// single key holds a constant rotation instead of failing visibly.
				return m_Keys[0].Value;

			// Clamp to the first key. <= rather than < so a sample exactly on the
			// first key returns it from here rather than falling into the segment
			// path with t == 0 (which would land on the same value anyway).
			if (time <= m_Keys.front().Time)
				// First key's value.
				return m_Keys.front().Value;
			// Clamp to the last key, symmetric with the bound above. Having both
			// bounds means the segment search below can never run off either end.
			if (time >= m_Keys.back().Time)
				// Last key's value.
				return m_Keys.back().Value;

			// upper_bound, not lower_bound: we want the bracketing PAIR, so this
			// must return the first key strictly AFTER `time`, making the previous
			// key the segment's left end.
			auto it = std::upper_bound(m_Keys.begin(), m_Keys.end(), time,
									   [](float t, const Key &k) { return t < k.Time; });
			// Distance in elements, not bytes; the cast is to the index type the
			// container itself uses.
			size_t index = (size_t)std::distance(m_Keys.begin(), it);
			// DEFENSIVE, and unreachable: the `time <= front` clamp above already
			// returned, so `time > front.Time` and upper_bound cannot return begin.
			// Harmless, but it exists to make the index-1 subscript below safe
			// without re-deriving why.
			if (index == 0)
				index = 1;
			// DEFENSIVE, and unreachable for the mirror reason: the `time >= back`
			// clamp means `time < back.Time`, so upper_bound cannot return end.
			if (index >= m_Keys.size())
				index = m_Keys.size() - 1;

			// The bracketing pair. Both are const refs into m_Keys -- no copies, and
			// nothing below mutates the track, so the references stay valid.
			const Key &a = m_Keys[index - 1];
			// `b` is the key the segment arrives at.
			const Key &b = m_Keys[index];

			// Step reads `a`'s mode, not `b`'s, because a key's Mode describes the
			// segment STARTING at it (see AnimationCurve.h). Using b.Mode would
			// apply one key too late.
			if (a.Mode == Interpolation::Step)
				// Hold the left value for the whole segment: a step track is flat until
				// the next key, which is the only correct rendering of "discrete".
				return a.Value;

			// Segment length in seconds.
			float dt = b.Time - a.Time;
			// Two keys at (numerically) the same time give no usable t, and the
			// division below would be a divide-by-zero. Hold the left value. This
			// is the safety net that keeps the forward-only dedupe defect above from
			// turning into a NaN rotation.
			if (dt <= 1e-6f)
				return a.Value;

			// Normalized position within the segment. The clamps above guarantee
			// this lands in (0, 1).
			float t = (time - a.Time) / dt;

			// Linear still slerps -- it is the same arc, just without the
			// smoothstep ease in and out. Interpolating a rotation by lerping
			// its components when the author asked for "linear" is the bug
			// this whole type exists to prevent.
			// DEFECT (unreachable Linear/Cubic branch): Interpolation::Linear and
			// Interpolation::Cubic are not distinguished here -- both slerp the same
			// arc at the same rate. Unlike a scalar curve there is no tangent to
			// scale by dt, so a rotation authored "linear" gets no easing knob.
			// Deliberate per the note above, but it means the enum value is inert
			// for rotation and a caller switching it sees no change.
			glm::quat result = glm::slerp(a.Value, b.Value, t);

			// glm::slerp takes the short arc, so the result is already
			// normalized. Re-normalizing is cheap insurance against the
			// near-parallel fallback path, which returns an unnormalized blend.
			// Re-normalize as insurance: glms slerp already returns unit length on
			// the normal path, but its near-parallel fallback blends linearly and
			// does NOT re-normalize, so this closes the only path that could leak
			// a non-unit quaternion into a pose.
			return glm::normalize(result);
		}

		// Range query for the track itself. Empty means 0.0f rather than a
		// sentinel -- see the callers below for why that choice is load-bearing
		// and where it bites.
		float RotationTrack::GetStartTime() const
		{
			// DEFECT SOURCE: this returns 0.0f both for "no keys" and for "first
			// key happens to be at t=0". The caller cannot tell them apart.
			return m_Keys.empty() ? 0.0f : m_Keys.front().Time;
		}

		// End of the keyed range; symmetric with GetStartTime above.
		float RotationTrack::GetEndTime() const
		{
			// Same ambiguity as GetStartTime: an empty track and a track ending at
			// t=0 are indistinguishable.
			return m_Keys.empty() ? 0.0f : m_Keys.back().Time;
		}

		// ---------------------------------------------------------------------
		// BoneTrack
		// ---------------------------------------------------------------------

		// Earliest key across ALL SEVEN channels of this bone (3 position,
		// 1 rotation, 3 scale). A bone's range is the union of its channels
		// because each channel clamps independently at Sample time.
		float BoneTrack::GetStartTime() const
		{
			// `found` exists purely to distinguish "minimum of nothing" from
			// "minimum happens to be 0.0f"... but see the note at the return below:
			// the flag is written and never read, so it does not actually do that.
			float start = 0.0f;
			// UNUSED FIELD: `found` is assigned by the lambda and by the loop, and
			// never read. The function returns `start` regardless. Dead weight, and
			// it looks load-bearing enough to mislead the next reader into thinking
			// the empty case is handled.
			bool found = false;

			// Local fold helper: each channel contributes its start time and this
			// keeps the running minimum, so adding channels later cannot be
			// forgotten here. Captures by reference to mutate start/found.
			auto consider = [&start, &found](float t) {
				if (!found || t < start)
				{
					start = t;
					found = true;
				}
			};

			// Three components per vector channel; rotation is handled separately
			// because it is not an array.
			for (int i = 0; i < 3; ++i)
			{
				// IsEmpty() guard is REQUIRED, not an optimization: an empty curve
				// reports GetStartTime() == 0.0f, so calling it unconditionally would
				// pull this bone's start time to 0 whenever any single component was
				// unkeyed.
				if (!Position[i].IsEmpty())
					consider(Position[i].GetStartTime());
				if (!Scale[i].IsEmpty())
					consider(Scale[i].GetStartTime());
			}
			// Same guard for the quaternion channel.
			if (!Rotation.IsEmpty())
				consider(Rotation.GetStartTime());

			// Returns 0.0f when every channel is empty, and that is exactly the
			// value that makes AnimationClip::GetStartTime() wrong -- see the note
			// there.
			return start;
		}

		// Latest key across all seven channels; mirror of GetStartTime above.
		float BoneTrack::GetEndTime() const
		{
			// Same unused `found` flag, same reason.
			float end = 0.0f;
			bool found = false;

			// Running maximum this time, so the sign of the comparison below flips.
			auto consider = [&end, &found](float t) {
				if (!found || t > end)
				{
					end = t;
					found = true;
				}
			};

			// Same three-component walk as the start-time version.
			for (int i = 0; i < 3; ++i)
			{
				if (!Position[i].IsEmpty())
					consider(Position[i].GetEndTime());
				if (!Scale[i].IsEmpty())
					consider(Scale[i].GetEndTime());
			}
			if (!Rotation.IsEmpty())
				consider(Rotation.GetEndTime());

			// 0.0f when the bone is entirely unkeyed. Less damaging than on the
			// start side (0.0 only lowers the max), but still indistinguishable
			// from a real key at t=0.
			return end;
		}

		// ---------------------------------------------------------------------
		// AnimationClip
		// ---------------------------------------------------------------------

		// The generic authoring entry point: channel + component index + a
		// scalar. Used by parsers, which address keys positionally. The typed
		// wrappers below are what hand-written code should use.
		void AnimationClip::AddKey(BoneIndex bone, TrackChannel channel,
								   int component, float time, float value,
								   Interpolation mode)
		{
			// An invalid bone index would key into m_Tracks under -1, creating
			// an entry no sample path could ever find -- a silent no-op that
			// looks like a bug in the caller.
			// A default-constructed BoneIndex holds -1 and IsValid() is Value >= 0,
			// so this catches both "unset" and "negative from a bad file".
			if (!bone.IsValid())
				return;

			// DEFECT: operator[] DEFAULT-CONSTRUCTS. Reaching this line is enough
			// to put an entry into m_Tracks, even if the switch below ends up
			// adding no key at all -- an out-of-range component, or the Rotation
			// case, leaves a PHANTOM empty BoneTrack behind. Consequences:
			//   - GetTrackCount() and GetAnimatedBones() report a bone that
			//     animates nothing,
			//   - GetTrack() returns non-null for it while HasTrack() says false,
			//   - AnimationClip::GetStartTime() folds its 0.0f in and pins the
			//     clip start to 0 (see the note there).
			// A non-inserting lookup, or validating before the subscript, would
			// avoid creating the entry at all.
			BoneTrack &track = m_Tracks[bone.Value];

			// All three enumerators are covered, so no default is needed; a value
			// outside the enum (a corrupt import) simply adds nothing.
			switch (channel)
			{
			case TrackChannel::Position:
				// Bound-checked rather than asserted: `component` usually comes from a
				// file parser, and one malformed key should drop that key, not abort
				// the import.
				if (component >= 0 && component < 3)
					// component indexes the fixed-size array directly, so the check above
					// is what makes this subscript safe.
					track.Position[component].AddKey(time, value, mode);
				break;
			// Scale is structurally identical to position -- both are three
			// AnimationCurves -- which is why these two cases read the same.
			case TrackChannel::Scale:
				if (component >= 0 && component < 3)
					// Same as the position case above.
					track.Scale[component].AddKey(time, value, mode);
				break;
			// Deliberate no-op, documented at the case body: one float cannot name
			// a quaternion component unambiguously.
			case TrackChannel::Rotation:
				// Handled by AddRotationKey, which takes a quaternion.
				// Reached only if a caller passes a scalar for rotation, where
				// the meaning of one float is ambiguous, so it is ignored
				// rather than guessed at.
				break;
			}
		}

		// Typed wrapper: fans a vec3 out to the three scalar channels, so
		// callers never construct a component index by hand.
		void AnimationClip::AddPositionKey(BoneIndex bone, float time,
										   const glm::vec3 &value, Interpolation mode)
		{
			if (!bone.IsValid())
				return;
			// operator[] inserts here too, but that is correct in this function:
			// all three components are written immediately below.
			BoneTrack &track = m_Tracks[bone.Value];
			// x, y, z in that order. Three independent keys, not one vec key --
			// a DCC exporter may key only some axes, and each curve dedupes on
			// its own.
			track.Position[0].AddKey(time, value.x, mode);
			track.Position[1].AddKey(time, value.y, mode);
			track.Position[2].AddKey(time, value.z, mode);
		}

		// Typed wrapper for the quaternion channel; the only correct way to
		// author rotation, given the scalar AddKey path drops it.
		void AnimationClip::AddRotationKey(BoneIndex bone, float time,
										   const glm::quat &value, Interpolation mode)
		{
			if (!bone.IsValid())
				return;
			// No intermediate BoneTrack reference needed -- the only channel being
			// written is the rotation track.
			m_Tracks[bone.Value].Rotation.AddKey(time, value, mode);
		}

		// Typed wrapper for scale.
		void AnimationClip::AddScaleKey(BoneIndex bone, float time,
										const glm::vec3 &value, Interpolation mode)
		{
			if (!bone.IsValid())
				return;
			// Same safe insert rationale as the position wrapper.
			BoneTrack &track = m_Tracks[bone.Value];
			track.Scale[0].AddKey(time, value.x, mode);
			track.Scale[1].AddKey(time, value.y, mode);
			// z -- the third and last of the scale components.
			track.Scale[2].AddKey(time, value.z, mode);
		}

		// Fan the tangent recompute out to every scalar curve in the clip.
		// Authoring-time only: this is O(tracks x 6 curves x keys) and there
		// is no dirty flag, so it must be called explicitly after a bulk edit
		// rather than lazily per frame.
		void AnimationClip::RecalculateAutoTangents()
		{
			// Non-const iteration: RecalculateAutoTangents writes tangents.
			for (auto &entry : m_Tracks)
			{
				// entry.second is the BoneTrack; entry.first is the bone index.
				BoneTrack &track = entry.second;
				// Position and scale share the loop body because both are 3-element
				// arrays of the same curve type.
				for (int i = 0; i < 3; ++i)
				{
					// Each curve invalidates its own cached key range as part of the
					// recompute, so no cache fix-up is needed here.
					track.Position[i].RecalculateAutoTangents();
					// Scale tangents, same reason.
					track.Scale[i].RecalculateAutoTangents();
				}
			}

			// The loop above already covered every scalar channel, and there is
			// deliberately no rotation work here. Nothing is left for this
			// function to do, which is why the body ends without a return.
			// RotationTrack has no per-key tangents to recompute: slerp has no
			// tangent parameter. Ease in and out of a rotation is applied at
			// blend time by the animator, not baked into the keys.
		}

		// Clip range = min over bones of that bone's min over channels.
		float AnimationClip::GetStartTime() const
		{
			// Note this duplicates the pattern in BoneTrack::GetStartTime but
			// WITHOUT the per-channel IsEmpty() guards that function uses.
			float start = 0.0f;
			bool found = false;
			// DEFECT (empty-track poisoning): unlike BoneTrack::GetStartTime, this
			// fold never checks entry.second.IsEmpty(). An empty BoneTrack reports
			// 0.0f, so 0.0f is compared against the running minimum and wins --
			// pinning the clip start to 0.0f and inflating GetDuration() by the
			// clip's true start time. Reachable, because AnimationClip::AddKey's
			// operator[] creates exactly such empty entries (see the note there).
			for (const auto &entry : m_Tracks)
			{
				// One call per bone per query; nothing is cached, unlike
				// AnimationCurve's cached key range.
				//
				// The very next line, `if (!found || t < start)`, is the only place in
				// this file where a `found` flag is actually READ -- so unlike the
				// BoneTrack version, the empty-map case IS handled here. That flag
				// exists to keep an empty m_Tracks from returning a bogus minimum.
				//
				// The `return start` at the end of this function falls back to 0.0f for
				// a clip with no tracks -- indistinguishable from a clip whose first
				// key is at t=0, but harmless here because both are valid clip starts.
				float t = entry.second.GetStartTime();
				if (!found || t < start)
				{
					start = t;
					found = true;
				}
			}
			return start;
		}

		// Clip range, max side. Same shape as GetStartTime above.
		float AnimationClip::GetEndTime() const
		{
			float end = 0.0f;
			bool found = false;
			// Same loop; same reasoning.
			for (const auto &entry : m_Tracks)
			{
				// An empty BoneTrack contributes 0.0f, which can only ever LOWER a
				// maximum, so unlike the start-time path this particular defect is
				// benign -- unless every real end time is negative, in which case it
				// would clamp the clip end to 0.0f.
				float t = entry.second.GetEndTime();
				if (!found || t > end)
				{
					end = t;
					found = true;
				}
			}
			// 0.0f when there are no tracks.
			return end;
		}

		// Duration in seconds, computed as end - start rather than cached.
		// A clip-level cache would need invalidation on every AddKey, and the
		// answer is already an O(tracks) walk over data that is in cache.
		float AnimationClip::GetDuration() const
		{
			// Guards the degenerate clip; without it the two folds below would
			// return 0.0f anyway, so this is clarity rather than correctness.
			if (m_Tracks.empty())
				return 0.0f;
			// Two full walks over m_Tracks per call. Callers that need this every
			// frame (an editor scrub bar) pay for it; the animator uses the
			// start/end pair directly instead.
			return GetEndTime() - GetStartTime();
		}

		// Distinguishes "this bone is animated" from "an entry exists for
		// this bone" -- the second is not the same thing, see AddKey above.
		bool AnimationClip::HasTrack(BoneIndex bone) const
		{
			if (!bone.IsValid())
				return false;
			auto it = m_Tracks.find(bone.Value);
			// The IsEmpty() half of the check is what makes this function disagree
			// with GetTrack() below, which returns a pointer to empty tracks.
			return it != m_Tracks.end() && !it->second.IsEmpty();
		}

		// Const overload. Returns null rather than an empty track so the
		// caller can distinguish "no such bone" -- but NOT "bone exists with
		// no keys", which is the asymmetry noted below.
		const BoneTrack *AnimationClip::GetTrack(BoneIndex bone) const
		{
			if (!bone.IsValid())
				return nullptr;
			auto it = m_Tracks.find(bone.Value);
			// Returns a usable pointer even when the BoneTrack is empty, so
			//   GetTrack(b) != nullptr && !HasTrack(b)
			// is reachable. Callers must check IsEmpty() themselves.
			return (it == m_Tracks.end()) ? nullptr : &it->second;
		}

		// Non-const overload; exists so authoring code can add keys through
		// the returned reference. Same lookup, same null convention.
		BoneTrack *AnimationClip::GetTrack(BoneIndex bone)
		{
			if (!bone.IsValid())
				return nullptr;
			auto it = m_Tracks.find(bone.Value);
			return (it == m_Tracks.end()) ? nullptr : &it->second;
		}

		// Collect the animated bones for an editor bone list or a bind-pose
		// query.
		std::vector<BoneIndex> AnimationClip::GetAnimatedBones() const
		{
			// Returned by value: callers iterate it once, and a vector of 8-byte
			// handles is cheap enough that sharing storage would not pay.
			std::vector<BoneIndex> bones;
			// Exact reserve, since one bone produces exactly one entry below -- no
			// reallocation, and no capacity guess to be wrong about.
			bones.reserve(m_Tracks.size());
			for (const auto &entry : m_Tracks)
				// entry.first IS the BoneIndex; the map is keyed by bone index, so no
				// lookup is needed to recover it.
				bones.push_back(BoneIndex(entry.first));
			// HAZARD: m_Tracks is an unordered_map, so this order is whatever the
			// hash happens to produce -- not sorted by bone index, not insertion
			// order, and not stable across implementations. Anything downstream
			// that assumes ascending bone indices has to sort for itself. It also
			// includes phantom empty tracks from the AddKey defect above.
			return bones;
		}

		// The one hot function in this file: writes the pose for `time`.
		void AnimationClip::Sample(const Skeleton &skeleton, float time,
								   Pose &outPose) const
		{
			// Bone count comes from the SKELETON, not from the clip -- a clip may
			// have been authored for a different rig, and the pose must match what
			// the caller is going to skin.
			const size_t boneCount = (size_t)skeleton.GetBoneCount();

			// DEFECT (header contract not honoured): AnimationClip.h documents
			// Sample as leaving bones the clip does not animate "at whatever they
			// already held", on the stated assumption the caller seeds the rest
			// pose. This implementation unconditionally overwrites every bone from
			// the rest pose instead. A caller that pre-blends two poses and samples
			// this clip on top silently loses its blend -- the discarded value is
			// never read. Either the code should skip un-animated bones, or the
			// header should state that Sample fully owns the pose.
			// Seed from the rest pose before applying this clip's keys.
			//
			// This is what makes a PARTIAL clip work: a walk cycle that
			// animates only the legs must leave the spine exactly where the
			// rest pose put it. Resizing to identity instead would snap every
			// unanimated bone to the skeleton root.
			// Resize fills with identity, and the loop immediately below overwrites
			// every element -- so the identity fill is pure allocation cost here.
			// Kept because Pose::Resize is the sizing API and Sample should not
			// be reaching into Bones.assign() behind it.
			outPose.Resize(boneCount);
			// Seeding loop. O(bones) unconditionally, even for a clip that animates
			// one bone -- this is the price of the partial-clip contract above.
			for (size_t i = 0; i < boneCount; ++i)
				// GetRestTransform returns by value, so this is a copy into the pose;
				// the int32_t cast is a narrowing of the loop index, safe because the
				// loop bound came from the same skeleton.
				outPose.Bones[i] = skeleton.GetRestTransform(BoneIndex((int32_t)i));

			for (const auto &entry : m_Tracks)
			{
				// Per-bone pass. Unsorted order is harmless here because each iteration
				// writes one independent bone -- unlike GetAnimatedBones, nothing here
				// depends on the sequence.
				//
				// Hoisted so the range check below reads as one comparison.
				const int32_t boneValue = entry.first;

				// A clip authored for a different skeleton can name a bone this
				// one does not have. Skipping it is right: the bone was
				// authored somewhere else and simply has no counterpart here.
				// Retargeting guard: a clip authored for a bigger rig legitimately names
				// bones this skeleton does not have. The negative half is unreachable
				// through the public API (AddKey rejects invalid BoneIndex) but costs
				// nothing to keep, and m_Tracks is not a private-by-construction type.
				if (boneValue < 0 || (size_t)boneValue >= boneCount)
					continue;

				// Const: Sample does not modify the clip.
				const BoneTrack &track = entry.second;
				// `target` already holds the rest transform from the seeding loop, so
				// this is an in-place overwrite rather than a fresh default.
				BoneTransform &target = outPose.Bones[(size_t)boneValue];

				// A channel is applied only if the clip has keys for it. An
				// unkeyed position means "do not move this bone", so writing
				// the curve's default 0 would slam it to the origin.
				// DEFECT (per-channel gate, per-component write): the guard is an OR
				// over the three components, but the body writes all three. A clip that
				// keys only Z passes this test and then evaluates X and Y on EMPTY
				// curves -- and AnimationCurve::Evaluate returns 0.0f for an empty
				// curve, so those two axes are slammed to the origin. That is precisely
				// the failure the comment below attributes to "an unkeyed position",
				// except here it happens on PARTIALLY keyed positions. The fix is a
				// per-component gate, or writing only the components that have keys.
				if (!track.Position[0].IsEmpty() || !track.Position[1].IsEmpty() ||
					!track.Position[2].IsEmpty())
				{
					// Evaluates all three curves; the two that are empty contribute 0.0f
					// per the defect above.
					target.Translation = glm::vec3(
						track.Position[0].Evaluate(time),
						track.Position[1].Evaluate(time),
						track.Position[2].Evaluate(time));
				}

				// Same defect as the position gate above, with scale added: an empty
				// scale curve evaluates to 0.0f, which collapses the bone to zero size
				// rather than leaving it at the rest scale.
				if (!track.Scale[0].IsEmpty() || !track.Scale[1].IsEmpty() ||
					!track.Scale[2].IsEmpty())
				{
					// The three scale evaluations; an unkeyed component yields 0.0f.
					target.Scale = glm::vec3(
						track.Scale[0].Evaluate(time),
						track.Scale[1].Evaluate(time),
						track.Scale[2].Evaluate(time));
				}

				// Rotation is a single channel, so a single IsEmpty() test is exactly
				// right here -- this is the case the position/scale guards above should
				// have been written as.
				if (!track.Rotation.IsEmpty())
					// Replaces rotation outright. Because the channel is non-empty, the
					// identity fallback inside Evaluate is unreachable on this path.
					target.Rotation = track.Rotation.Evaluate(time);
			}
		}

	// ---------------------------------------------------------------------
	// End of namespace Animation.
	// ---------------------------------------------------------------------
	} // namespace Animation

} // namespace Wasteland
