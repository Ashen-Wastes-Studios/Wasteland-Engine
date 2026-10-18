// Precompiled header first, by engine convention.
#include "wlpch.h"
// This class's declaration, including the Keyframe struct and the
// Interpolation / Extrapolation enums used throughout.
#include "Wasteland/Animation/AnimationCurve.h"

// lower_bound, upper_bound, remove_if, distance -- the ordering assumptions
// this file depends on.

// Note the cache fields are touched through const_cast in the accessors
// below, which is a language-level escape hatch rather than an algorithm,
// but it is the one thing here that needs justifying.
#include <algorithm>
// abs, fmod, radians -- all used on the hot sampling path.
#include <cmath>

namespace Wasteland {

	// Nested inside Wasteland. A second namespace level here, unlike the flat
	// Wasteland used by the renderer.
	namespace Animation {

		// Adds or replaces a keyframe at `time`.

		// Not const, obviously -- this mutates m_Keys.
		void AnimationCurve::AddKey(float time, float value, Interpolation mode)
		{
			// A key at an existing time REPLACES rather than appending. Two keys
			// at one time have no defined order between them, so which one
			// wins would depend on insertion sequence -- and sampling would
			// depend on that too. Overwriting makes the last write win, which
			// is predictable and is what an animator dragging a key expects.
			// lower_bound finds the first key at or AFTER `time`, which is either the
			// key we are replacing or the insertion point for a new one.

			// The lambda supplies the comparison: keys sort by Time alone, so two keys at
			// the same time compare equal and lower_bound finds the FIRST of them. That
			// is why the replacement below compares within an epsilon rather than testing
			// `it->Time == time` -- exact equality would miss a key written as 0.1 vs an
			// accumulated 0.100000001.

			// Lower bound rather than upper bound (Evaluate below uses the opposite, for a
			// different reason): for a replace-or-insert operation the first match is
			// what we want.
			auto it = std::lower_bound(m_Keys.begin(), m_Keys.end(), time,
									   [](const Keyframe &k, float t) { return k.Time < t; });

			// Found an existing key within tolerance. REPLACE rather than append.

			// The epsilon is the same 1e-6f used by RemoveKeyAtTime and FindKey below,
			// consistently applied throughout this file.

			// Both conditions matter: `it != end()` because lower_bound legitimately
			// returns the past-the-end iterator when `time` exceeds every key, and touching
			// it then would be undefined behaviour.
			if (it != m_Keys.end() && std::abs(it->Time - time) <= 1e-6f)
			{
				// Overwrite in place, keeping the key's existing Time.

				// Preserving Time rather than writing `time` is deliberate: they can differ
				// by up to the epsilon, and the stored key is the one already sitting in the
				// sorted array.
				it->Value = value;
				// The interpolation mode IS overwritten, unlike the tangents below.
				it->Mode = mode;
				// Tangents are left alone deliberately: a re-added key usually
				// comes from an editor drag, and recomputing them here would
				// discard hand-authored slopes the animator set.
			}
			else
			{
				// Construct the new key with defaults for the tangents.

				// So a freshly inserted key starts with tangent 0, meaning a flat segment
				// until RecalculateAutoTangents is called. Sampling before then produces a
				// visibly wrong curve rather than a wrong-but-plausible one.
				Keyframe key(time, value, mode);
				// Insert at the found position, preserving sorted order.

				// O(n) for the shift, but AddKey is an authoring-time operation; the per-
				// frame cost is Evaluate, which is a binary search.
				m_Keys.insert(it, key);
			}

			// Drop the cached start/end times.

			// Called on EVERY path, including the replace case -- correct, since the
			// replaced key could be the first or last and change the bounds.
			InvalidateCache();
		}

		// Removes every key within tolerance of `time`, returning how many went.

		// Returning the count (rather than void or bool) lets a caller distinguish
		// removing the last key from removing nothing.
		int AnimationCurve::RemoveKeyAtTime(float time)
		{
			int removed = 0;
			// Erase-remove rather than erase(it): every key within tolerance of
			// `time` goes, not just the first match found.
			// The erase-remove idiom: remove_if compacts matching elements to the front
			// and returns the new logical end; erase then discards the tail.

			// erase(it) alone would delete only ONE key, so both halves are required.
			auto it = std::remove_if(m_Keys.begin(), m_Keys.end(),
									 [time](const Keyframe &k) { return std::abs(k.Time - time) <= 1e-6f; });
			// The distance BEFORE the erase -- after this line it would be 0, because
			// erase has already removed the tail.

			// The (int) cast narrows ptrdiff_t to int. A curve with more than 2^31 keys
			// is not a practical concern, but the cast is unchecked in principle.
			removed = (int)std::distance(it, m_Keys.end());
			// Discard the compacted-away tail.

			// Destroying Keyframes with shift-assignment or move, which is why a key
			// holding owned resources would need care here.
			m_Keys.erase(it, m_Keys.end());
			// Invalidate ONLY if something actually went.

			// An optimisation: removing nothing leaves the bounds unchanged, so the
			// cache is still valid. Correct, and it avoids a needless recompute on a
			// missed removal.
			if (removed > 0)
				InvalidateCache();
			return removed;
		}

		// Recomputes every key's tangents from its neighbours -- the "auto" in auto
		// tangents, as against hand-authored slopes.

		// Only affects AUTO tangents in the sense that it overwrites ALL of them,
		// including any an animator set by hand. AddKey deliberately preserves
		// hand-authored tangents (see the note there); this function does not.
		void AnimationCurve::RecalculateAutoTangents()
		{
			// Cached size, since the loop below indexes from it repeatedly.
			const size_t n = m_Keys.size();
			// Empty curve: nothing to compute, and the loops below would read m_Keys[0]
			// unconditionally.
			if (n == 0)
				return;

			// Every key, endpoints included -- they get one-sided tangents rather than
			// being skipped.
			for (size_t i = 0; i < n; ++i)
			{
				// First key: slope toward the next. Last key: slope from the
				// previous. Interior: the chord through the neighbours, halved
				// -- this is the Catmull-Rom tangent, and halving is what makes
				// the curve pass *through* the key instead of merely near it.
				// Zero-initialised so a key with no left neighbour keeps a flat incoming
				// slope instead of an uninitialised value.
				float inTangent = 0.0f;
				// Same for the outgoing side.
				float outTangent = 0.0f;

				// Interior or last key: has a predecessor.
				if (i > 0)
				{
					// Time delta to the previous key.
					float dt = m_Keys[i].Time - m_Keys[i - 1].Time;
					// Slope from the previous key, guarded against a near-zero interval.

					// The guard uses the SAME 1e-6f epsilon as the duplicate-time check in
					// AddKey. Two keys within epsilon of each other therefore produce a zero
					// tangent rather than an enormous slope -- so the epsilon is load-bearing
					// here, not merely a comparison tolerance.
					inTangent = (dt > 1e-6f)
									? (m_Keys[i].Value - m_Keys[i - 1].Value) / dt
									: 0.0f;
				}

				// Interior or first key: has a successor.
				if (i + 1 < n)
				{
					float dt = m_Keys[i + 1].Time - m_Keys[i].Time;
					// Slope to the next key, same epsilon guard.
					outTangent = (dt > 1e-6f)
									 ? (m_Keys[i + 1].Value - m_Keys[i].Value) / dt
									 : 0.0f;
				}

				// Interior keys only -- both neighbours exist.

				// This branch OVERWRITES the one-sided tangents computed above, so for an
				// interior key the earlier in/out calculations are wasted work. Keeping them
				// gives the flat default for endpoints without a separate path, at the cost of
				// some redundant arithmetic on the majority of keys.
				if (i > 0 && i + 1 < n)
				{
					// The chord spans BOTH neighbours, skipping this key entirely.

					// That is what makes it Catmull-Rom: the tangent is determined by where the
					// curve is heading before and after, not by the local slope.
					float span = m_Keys[i + 1].Time - m_Keys[i - 1].Time;
					// Chord slope through the two neighbours, guarded like the others.
					float chord = (span > 1e-6f)
									  ? (m_Keys[i + 1].Value - m_Keys[i - 1].Value) / span
									  : 0.0f;
					inTangent = chord;
					outTangent = chord;
				}

				// Write the recomputed incoming slope back.

				// Written unconditionally for every key, so any hand-authored tangent is
				// lost -- see the note on the function itself.
				m_Keys[i].InTangent = inTangent;
				m_Keys[i].OutTangent = outTangent;
			}

			InvalidateCache();
		}

		// The first key's time, or 0 for an empty curve.

		// CONST method that writes to members via const_cast. Legal C++, but it
		// means the cache is shared mutable state with no synchronisation -- two
		// threads calling this on the same curve race, and `m_CacheValid` could be
		// observed true before `m_CachedStart` is written on a weakly ordered machine.
		float AnimationCurve::GetStartTime() const
		{
			// Compute once, reuse thereafter. Invalidation happens in AddKey and
			// RemoveKeyAtTime.
			if (!m_CacheValid)
			{
				// The const_cast. Needed because the cache is a performance detail of a
				// logically-const query.

				// The write is repeated verbatim in all four lines below rather than factored
				// into a private non-const helper -- four copies of one expression, which is
				// why an edit to the caching rule has to be made in five places.
				const_cast<AnimationCurve *>(this)->m_CachedStart =
					m_Keys.empty() ? 0.0f : m_Keys.front().Time;
				const_cast<AnimationCurve *>(this)->m_CachedEnd =
					m_Keys.empty() ? 0.0f : m_Keys.back().Time;
				const_cast<AnimationCurve *>(this)->m_CacheValid = true;
			}
			return m_CachedStart;
		}

		// The last key's time. An exact duplicate of GetStartTime above, differing
		// only in which cached field it returns.
		float AnimationCurve::GetEndTime() const
		{
			if (!m_CacheValid)
			{
				// Recomputes BOTH cached bounds, not just the end.

				// Correct but redundant: a caller that only ever asks for the end time
				// still pays for both writes. Harmless, since it happens once per
				// invalidation.
				const_cast<AnimationCurve *>(this)->m_CachedStart =
					m_Keys.empty() ? 0.0f : m_Keys.front().Time;
				const_cast<AnimationCurve *>(this)->m_CachedEnd =
					m_Keys.empty() ? 0.0f : m_Keys.back().Time;
				const_cast<AnimationCurve *>(this)->m_CacheValid = true;
			}
			// The cached value, computed above if the cache was cold.
			return m_CachedEnd;
		}

		// Last time minus first time.

		// NOT cached, unlike its two siblings -- it reads m_Keys directly.
		float AnimationCurve::GetDuration() const
		{
			// Empty curve: zero length rather than an exception or a garbage read from
			// front()/back() on an empty vector.
			if (m_Keys.empty())
				return 0.0f;
			// front() and back() are the extremes because m_Keys is kept sorted by AddKey
			// and insert.
			return m_Keys.back().Time - m_Keys.front().Time;
		}

		// Linear search for a key at `time`, or nullptr.

		// LINEAR, unlike the binary searches elsewhere in this file. O(n) per call.
		// Fine for authoring tools and for occasional queries, but it is the one
		// search here that would not scale to a densely keyed curve.
		const Keyframe *AnimationCurve::FindKey(float time) const
		{
			if (m_Keys.empty())
				return nullptr;
			// Range-for over the sorted vector, returning on the first match.

			// A binary search is available -- the vector is sorted by Time -- so this is
			// a missed optimisation rather than a necessity.
			for (const Keyframe &k : m_Keys)
			{
				// Same epsilon as everywhere else in this file.

				// Returns a pointer INTO m_Keys, so it dangles if a key is added or removed
				// before the caller uses it. There is no const_cast trickery here, but the
				// aliasing hazard is the same category.
				if (std::abs(k.Time - time) <= 1e-6f)
					return &k;
			}
			// No key within tolerance.
			return nullptr;
		}

		// Interpolates within one segment, keys[i] to keys[i+1].

		// Takes the vector as a PARAMETER rather than reading m_Keys, so it can be
		// exercised directly against a test vector.
		float AnimationCurve::EvaluateSegment(const std::vector<Keyframe> &keys,
											  size_t i, float time) const
		{
			// The left key. Bounds are the caller's responsibility -- there is no check
			// that i+1 is in range, since this is a private helper only Evaluate calls.
			const Keyframe &a = keys[i];
			const Keyframe &b = keys[i + 1];
			// Segment duration.
			float dt = b.Time - a.Time;

			// Two keys at effectively the same time give no interval to
			// interpolate across. Holding the left value is the only
			// defined answer; dividing by dt here would produce an infinity.
			// Two keys at effectively the same time: no interval to interpolate across.

			// Returning a.Value holds the left value rather than dividing by dt, which
			// would be an infinity. Not defensive -- it is the correct answer for a
			// degenerate segment.
			if (dt <= 1e-6f)
				return a.Value;

			// Step interpolation: the left key's value holds until the right key.

			// Testing a.Mode rather than b.Mode is correct -- the mode describes how to
			// LEAVE a -- and worth noting because testing the wrong one is an easy and
			// silent bug.
			if (a.Mode == Interpolation::Step)
				return a.Value;

			// Normalised position within the segment, in [0,1] for a time inside it.

			// No clamp: a caller passing a time outside the segment gets an extrapolated
			// result. Evaluate guarantees the range before calling, so this is a private-
			// helper precondition rather than a checked invariant.
			float t = (time - a.Time) / dt;

			// Linear: straight interpolation between the two values.

			// Returns early, so the cubic path below is only reached for Smooth.
			if (a.Mode == Interpolation::Linear)
				return a.Value + (b.Value - a.Value) * t;

			// Cubic Hermite. The basis functions give:
			//   h00 = 2t^3 - 3t^2 + 1     h10 = t^3 - 2t^2 + t
			//   h01 = -2t^3 + 3t^2        h11 = t^3 - t^2
			// which sum to 1 at t=0 and t=1 (so the segment is exact at both
			// ends) and to 0 in between (so a key's value is unaffected by
			// what follows it).
			//
			// The tangents are scaled by dt so that a slope authored as
			// "value per second" behaves the same regardless of segment
			// length -- otherwise a slow segment and a fast one between the
			// same two keys would have visibly different curvature.
			// Powers of t, computed once and reused by the four basis functions below.
			float t2 = t * t;
			float t3 = t2 * t;

			// The four cubic Hermite basis functions. The block comment above derives
			// them; the useful properties are that they sum to 1 at both endpoints (so the
			// segment is exact at the keys) and to 0 in between (so one key's value does
			// not depend on what follows it).
			float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
			// Pairs with a.OutTangent.
			float h10 = t3 - 2.0f * t2 + t;
			// Pairs with b.Value.
			float h01 = -2.0f * t3 + 3.0f * t2;
			// Pairs with b.InTangent.

			// Note the pairs are cross-sided: h10 multiplies the LEFT key's outgoing
			// tangent while h11 multiplies the RIGHT key's incoming one. Both multiply dt,
			// which is what makes a slope authored per-second behave identically on short
			// and long segments -- without that scaling, the same two keys would show
			// visibly different curvature depending on the time between them.
			float h11 = t3 - t2;

			// The Hermite evaluation, expanded one term per line.
			return h00 * a.Value
				 + h10 * a.OutTangent * dt
				 + h01 * b.Value
				 + h11 * b.InTangent * dt;
		}

		// The main sampling entry point: extrapolate if needed, locate the segment,
		// interpolate.

		// This is the per-frame hot path -- called for every animated property every
		// frame -- so the binary search and the empty checks matter.
		float AnimationCurve::Evaluate(float time) const
		{
			// An empty curve has no opinion, so it reads as 0. Returning 0
			// rather than throwing keeps an unkeyed optional track (a blend
			// weight nobody authored) from taking down a whole pose.
			// An empty curve reads as 0.

			// Chosen over throwing so an unauthored optional track -- a blend weight
			// nobody set -- degrades to zero contribution rather than taking down the pose
			// that samples it.
			if (m_Keys.empty())
				return 0.0f;

			// A single key has no segment, so return its value for ALL times.

			// Including times outside any range, since there is no range.
			if (m_Keys.size() == 1)
				return m_Keys[0].Value;

			// The keyed range, from the sorted vector's extremes.
			float start = m_Keys.front().Time;
			float end = m_Keys.back().Time;

			// Outside the keyed range: extrapolation.

			// Uses <= and >= rather than < and >, so a time exactly AT a key takes the
			// extrapolation path rather than the segment path. That is handled correctly
			// below -- the comment at the Clamp case explains why the inclusive form
			// matters.
			if (time <= start || time >= end)
			{
				// How to behave beyond the keyed range.
				switch (m_Extrapolation)
				{
				// Wrap the time back into the keyed range.

				// The two NVRHI cases stack with the three DX11/DX12 ones only in the factory
				// switches; here each case is distinct.
				case Extrapolation::Loop:
				{
					// Wrap into the keyed range. A single-frame clip has no
					// span to wrap into, so it just holds.
					// The wrap period.
					float span = end - start;
					// A single-frame clip has no span to wrap into, so it holds.

					// Same epsilon used everywhere else in this file.
					if (span <= 1e-6f)
						return m_Keys.front().Value;
					// Reduce into [0, span).

					// fmod is correct rather than floor-mod here only because the sign is
					// corrected on the next line.
					float wrapped = std::fmod(time - start, span);
					// fmod preserves the sign of the dividend, so a time BEFORE start yields a
					// negative remainder.
					if (wrapped < 0.0f)
						// Add one period to bring it into [0, span). This is what makes looping
						// symmetric in both directions.
						wrapped += span;
					// Back into the keyed range.

					// REASSIGNS THE PARAMETER rather than using a local, so the rest of the
					// function works in wrapped time without a second code path.
					time = start + wrapped;
					break;
				}

				case Extrapolation::Linear:
				{
					// Continue along the first/last segment's slope. Only
					// meaningful with a tangent to continue from, so a Step
					// segment holds instead of inventing a linear ramp.
					// Before the first key: continue along the first segment's slope.

					// Note this uses the possibly-WRAPPED time, so after a Loop wrap this branch
					// is unreachable -- correct, since a wrapped time is inside the range by
					// construction.
					if (time < start)
					{
						// A Step first segment has no slope to continue from, so it holds.

						// Inventing a linear ramp here would visibly contradict the segment itself,
						// which is a constant.
						if (m_Keys[0].Mode == Interpolation::Step)
							return m_Keys.front().Value;
						// Span of the first segment.
						float dt = m_Keys[1].Time - m_Keys[0].Time;
						if (dt <= 1e-6f)
							return m_Keys.front().Value;
						// The slope to extrapolate along.

						// Note this IGNORES the tangents entirely -- even for a Smooth segment whose
						// tangent was authored or auto-computed. So linear extrapolation of a smooth
						// curve can disagree with the curve at the boundary it joins.
						float slope = (m_Keys[1].Value - m_Keys[0].Value) / dt;
						// The extrapolated value. `time - start` is negative out here, so this runs
						// backwards from the first key.
						return m_Keys.front().Value + slope * (time - start);
					}
					else
					{
						// Cached size for the forward-extrapolation branch.
						size_t n = m_Keys.size();
						// Mirror of the backward case: a Step last segment holds rather than ramping.

						// n - 2 is the second-to-last key, so this is the LAST segment's mode.
						if (m_Keys[n - 2].Mode == Interpolation::Step)
							return m_Keys[n - 1].Value;
						float dt = m_Keys[n - 1].Time - m_Keys[n - 2].Time;
						if (dt <= 1e-6f)
							return m_Keys[n - 1].Value;
						float slope = (m_Keys[n - 1].Value - m_Keys[n - 2].Value) / dt;
						// Forward extrapolation from the last key. `time - end` is positive out
						// here.
						return m_Keys[n - 1].Value + slope * (time - end);
					}
				}

				// Hold the nearest key value.
				case Extrapolation::Clamp:
				default:
					// The default, and the safe one: a curve outside its keyed
					// range holds the nearest key.
					//
					// The `time <= start` form matters: writing it as a
					// two-way ternary on `time < start` sent a time exactly
					// AT the first key down the `else` branch and returned the
					// LAST key's value. Sampling a curve at its own start
					// returned its end.
					// The default and the safe mode: a curve outside its range holds.

					// The `time <= start` form is load-bearing. As the comment above explains, a
					// two-way ternary on `time < start` sent a time exactly AT the first key down
					// the else branch and returned the LAST key's value -- so sampling a curve at
					// its own start returned its end. The inclusive comparison keeps an exact
					// boundary hit on the correct side.
					return (time <= start) ? m_Keys.front().Value
										   : m_Keys.back().Value;
				}
			}

			// Find the segment containing `time`. upper_bound gives the first
			// key strictly GREATER than `time`, so the segment containing
			// `time` is the one ENDING there -- that is index-1.
			//
			// The +1 is what puts the search on the following segment. Without
			// it, `index` already points at the left key and `index - 1`
			// selects the segment BEFORE it: at t=0.0 with keys at 0 and 1,
			// upper_bound yields 0, and using index-1 underflows to the last
			// segment -- so Evaluate(0.0) returned the LAST key's value. A
			// curve sampled at its own start returned its end.
			// upper_bound finds the first key STRICTLY GREATER than `time`, so the
			// segment containing `time` ENDS there and starts one earlier.

			// The reversed comparator (`t < k.Time` rather than `k.Time < t`) is what
			// makes upper_bound search by value against key times.
			auto it = std::upper_bound(m_Keys.begin(), m_Keys.end(), time,
									   [](float t, const Keyframe &k) { return t < k.Time; });
			// The index just past the located key.

			// The +1 implicit in the EvaluateSegment call below is what puts the search
			// on the FOLLOWING segment. Without it, `index` already points at the left
			// key and `index - 1` selects the segment BEFORE it -- at t=0.0 with keys at
			// 0 and 1, upper_bound yields 0, and using index-1 underflows to the LAST
			// segment, so Evaluate(0.0) returned the last key's value. The same class of
			// bug the Clamp comment above describes.
			size_t index = (size_t)std::distance(m_Keys.begin(), it);

			// Clamp into the valid segment range [1, n-1]. An explicit `time <=
			// end` guard above already handled the edges, but a time landing
			// exactly on the last key after the loop wrap can still land here.
			// Clamp into [1, n-1].

			// Needed because a time exactly equal to the first key -- which the guard
			// above routes here via the inclusive comparison -- makes upper_bound return
			// index 0, and `index - 1` would then underflow to a huge size_t.
			if (index < 1)
				index = 1;
			// Clamp at the top for the same reason at the other end.
			if (index >= m_Keys.size())
				index = m_Keys.size() - 1;

			// Interpolate in the segment starting one key earlier.
			return EvaluateSegment(m_Keys, index - 1, time);
		}

	// Closing brace, named in the trailing comment.
	} // namespace Animation

// Closing the outer namespace.
} // namespace Wasteland
