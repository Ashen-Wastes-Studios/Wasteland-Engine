#pragma once

// glm, for the types the .cpp interpolates.
//
// DEFECT: unnecessary in this header. Every member here is float,
// Interpolation, Extrapolation or std::vector<Keyframe> -- no glm type
// appears in a single declaration. If it is present for the .cpp's
// convenience, that is a coupling this header does not need, and it widens
// the include cost of every file that only wants to read keyframe data.
#include <glm/glm.hpp>
// DEFECT: unused here, and more clearly so -- there is no quaternion or any
// other glm type in this header at all. Rotation curves are documented as
// one curve PER COMPONENT AXIS, so orientation is handled by four scalar
// curves rather than by anything in this type. This looks like a remnant of
// an earlier design that templated the curve over glm types.
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Wasteland {

	namespace Animation {

		// How a curve interpolates between two keyframes.
		// How a segment interpolates.
		//
		// Scoped, so Interpolation::Step cannot collide with another name in the
		// namespace, and uint8_t because these values are serialised -- an explicit
		// width means the on-disk format does not depend on the compiler's choice of
		// underlying type.
		enum class Interpolation : uint8_t
		{
			// Hold the left key's value until the right key is reached. Used
			// for step-like data (a visibility toggle, a discrete blend switch).
			// Hold the left key's value until the right key is reached.
			//
			// The zero value, so a default-constructed or zero-filled Interpolation is
			// Step -- a safe choice for an unset field, where a flat-tangent cubic would
			// not be.
			Step = 0,
			// Straight line between the two values.
			Linear = 1,
			// Cubic Hermite using each key's in/out tangent. Smooth, and what
			// almost all authored motion actually wants.
			// Cubic Hermite using the in/out tangents. The default for new keys, per
			// Keyframe::Mode below: smooth motion is the common authoring intent.
			Cubic = 2,
		};

		// One keyframe on a curve.
		//
		// Tangents are per-key (out on the left key, in on the right), which is
		// how both Blender and Unity model curves: the slope leaving a key and
		// the slope arriving at the next one are authored separately, so a
		// sharp start can follow a soft arrival.
		// One key. Plain aggregate with constructors, no virtuals and no enforced
		// invariants, so it stays cheap to copy -- which matters because
		// RecalculateAutoTangents rewrites it in place.
		struct Keyframe
		{
			// Position on the curve, kept ascending by AddKey. That ordering is the
			// invariant EvaluateSegment relies on.
			//
			// DEFECT: the invariant is only maintained by AddKey, and the mutable
			// GetKeys() accessor below hands out the whole vector. A caller that pushes
			// through that reference can break the ordering, after which the segment
			// search returns the wrong segment silently.
			float Time = 0.0f;
			float Value = 0.0f;

			// The two tangents, in units per second.
			//
			// Meaningless for Step, as the comment says -- a step has no slope. They are
			// still stored and serialised, so a key switched from Cubic to Step and back
			// does not lose its authored tangents.
			// Slope of the curve arriving at this key (used by the segment to
			// its left) and leaving it (used by the segment to its right).
			// Meaningless for Step.
			float InTangent = 0.0f;
			float OutTangent = 0.0f;

			// Interpolation of the segment STARTING at this key.
			// The mode of the segment STARTING at this key, not the one ending here.
			//
			// That asymmetry is the load-bearing detail: the last key's Mode is stored
			// but never consulted, since no segment starts there. Two adjacent keys with
			// different modes use the LEFT key's.
			Interpolation Mode = Interpolation::Cubic;

			// Keeps the struct default-constructible so vector operations and resize()
			// work; the in-class initialisers above give it usable contents.
			Keyframe() = default;
			// The two-argument convenience constructor.
			//
			// DEFECT: tangents are left at their 0.0f defaults, which under Cubic
			// produces a FLAT segment -- a curve built only from these constructors and
			// never passed through RecalculateAutoTangents is eased rather than smooth.
			// Silent, because the result is still a valid curve; the author has to know
			// to call RecalculateAutoTangents.
			Keyframe(float time, float value)
				: Time(time), Value(value) {}
			// The three-argument form. Tangents are still left at zero, for the same
			// reason as above.
			//
			// Note there is no constructor taking tangents, so an author who wants
			// specific slopes must set them afterwards through the mutable GetKeys().
			Keyframe(float time, float value, Interpolation mode)
				: Time(time), Value(value), Mode(mode) {}
		};

		// A keyframed scalar function of time.
		//
		// Deliberately scalar rather than a template over glm types: every
		// channel that is not translation and rotation (opacity, blend weights,
		// scalar parameters, morph targets) is a plain float, and one
		// implementation covering all of them beats four near-identical ones.
		// Translation and rotation curves store one curve per component axis
		// instead, which is also how the DCC tools export them.
		// A keyframed scalar function of time.
		//
		// Deliberately scalar, as the comment above argues: one implementation
		// covering opacity, blend weights, morph targets and plain parameters beats
		// four near-identical ones over glm types.
		class AnimationCurve
		{
		public:
			// An empty curve. Evaluating one is the interesting case -- it yields 0
			// rather than crashing, the fail-closed choice for a value that will be
			// used as a weight or an opacity.
			AnimationCurve() = default;

			// ---- Authoring ----

			// Inserts a key, keeping the list sorted by time. A key at an
			// existing time replaces its value rather than adding a duplicate;
			// two keys at one time have no defined order between them and would
			// make sampling ambiguous.
			// Insert or replace, keeping the vector sorted by time.
			//
			// A key at an existing time REPLACES rather than duplicating, because two
			// keys at one time have no defined order and would make sampling ambiguous.
			//
			// @param time   Position on the curve.
			// @param value  Value at that position.
			// @param mode   Interpolation for the segment starting here.
			void AddKey(float time, float value, Interpolation mode = Interpolation::Cubic);

			// Removes every key at `time`. Returns how many were removed.
			// Removes every key at `time` and returns how many.
			//
			// RETURNING A COUNT from a remove is unusual -- most such APIs return void
			// or bool. Here it makes the remove-all-duplicates behaviour observable,
			// which matters because AddKey replaces rather than duplicates, so in
			// practice this returns 0 or 1 and the count is the only way a caller can
			// tell that it found nothing.
			int RemoveKeyAtTime(float time);

			// Inline, because clear() is trivial.
			//
			// DEFECT: does NOT call InvalidateCache(). The cached start and end below
			// are left holding the old range, so GetStartTime and GetEndTime report a
			// value that corresponds to no key -- and GetDuration, derived from them,
			// reports a stale non-zero duration for an empty curve.
			void RemoveAllKeys() { m_Keys.clear(); }

			// Replaces the tangents on each key so the curve passes smoothly
			// through it, the "Auto" / "Smooth" tangent mode in Blender and
			// Unity.
			//
			// A Catmull-Rom style slope through the neighbouring keys, which is
			// what makes auto tangents pass through the key instead of
			// flattening it.
			// Blender/Unity-style 'Auto' tangents: a Catmull-Rom slope through the
			// neighbouring keys.
			//
			// Rewrites BOTH tangents on EVERY key, so calling it discards hand-authored
			// slopes with no warning and no way to recover them. There is no per-key
			// opt-out and no record of which keys were manual.
			void RecalculateAutoTangents();

			// ---- Queries ----

			// The curve's value at `time`.
			//
			// Clamps to the first/last key outside the keyed range rather than
			// extrapolating. Extrapolation is a deliberate opt-in (see
			// Extrapolation) because a naive linear guess past the ends is
			// almost never what an animator intended.
			// The value at `time`.
			//
			// The hot path: once per track per bone per frame, thousands of times on a
			// real character. Hence the cached key range below.
			//
			// Clamps outside the keyed range rather than extrapolating, which is the
			// safe default; Extrapolation below is the deliberate opt-in.
			float Evaluate(float time) const;

			bool IsEmpty() const { return m_Keys.empty(); }
			// Inline trivial getter, alongside IsEmpty above.
			size_t GetKeyCount() const { return m_Keys.size(); }

			// Key range. Returns 0 for an empty curve.
			// The first key's time, read from the CACHE rather than m_Keys.front().
			//
			// So an empty curve reports 0, which the comment above states -- and so
			// does a curve whose cache has been invalidated but not yet recomputed,
			// depending on which path the .cpp takes for each.
			float GetStartTime() const;
			float GetEndTime() const;
			float GetDuration() const;

			// The key nearest `time`, for editor gizmos. Null when empty.
			// The key NEAREST `time`, not the one exactly at it -- the comment above is
			// explicit that this exists for editor gizmos.
			//
			// @return  A pointer into m_Keys, or null when the curve is empty.
			//
			// A raw pointer into the vector: it dangles if a key is added or the curve
			// cleared while the caller holds it. Fine for a gizmo drawn immediately;
			// unsafe for anything that stores the pointer.
			const Keyframe *FindKey(float time) const;

			// Direct access for authoring tools and tests.
			// The const overload, for readers.
			//
			// Paired with the mutable one below so the two can never disagree about
			// which vector they name.
			const std::vector<Keyframe> &GetKeys() const { return m_Keys; }
			// The MUTABLE overload, and the most consequential line in the header.
			//
			// It hands out write access to the key list, so a caller can insert keys out
			// of time order or change a Time, and every invariant this class relies on --
			// sorted order for the segment search, and the cached range -- is silently
			// invalidated. Nothing calls InvalidateCache() on the caller's behalf.
			//
			// The comment above frames this as 'for authoring tools and tests', a
			// legitimate reason. It is also why the RemoveAllKeys defect above is
			// observable at all: a test reaching in through this accessor is exactly how
			// a stale cache gets noticed. Returning by value, or keeping it private and
			// friending the authoring tool, would preserve the invariants.
			std::vector<Keyframe> &GetKeys() { return m_Keys; }

			// ---- Range behaviour ----

			// What happens outside the keyed range.
			//
			// Scoped and uint8_t for the same reasons as Interpolation, including the
			// serialisation reason.
			enum class Extrapolation : uint8_t
			{
				// Hold the end values. The zero value, and the member default below, so a
				// freshly constructed curve clamps -- the conservative choice.
				Clamp = 0,
				Linear = 1,
				// Wrap the time.
				//
				// DEFECT: there is no period parameter anywhere in the class, so 'loop' can
				// only mean wrapping by the curve's own authored duration. Ping-pong, and
				// any repeat length other than one pass, are not expressible. If a DCC
				// export later needs a repeat count, this enum has nowhere to put it and
				// the setter signature would have to change.
				Loop = 2,
			};

			Extrapolation GetExtrapolation() const { return m_Extrapolation; }
			// Inline setter, and the only mutator besides the key methods.
			//
			// No cache invalidation is needed, since the range does not change -- but
			// note this can change Evaluate's result outside the keyed range with no
			// authoring change at all, which matters when deciding what makes a curve
			// dirty.
			void SetExtrapolation(Extrapolation mode) { m_Extrapolation = mode; }

			// Recomputes the cached key range. Call after authoring changes.
			// Cached because Evaluate runs once per track per bone per frame,
			// which is thousands of calls a frame on a real character.
			//
			// NOT marked mutable: a curve shared between threads while being
			// authored is a data race either way, and hiding the write behind
			// `mutable` would only make it harder to see. Evaluate is
			// const-correct and has no lazy state.
			// Marks the cached range stale after an authoring change.
			//
			// Not `mutable`, and the comment above is emphatic that this is deliberate:
			// a curve shared while being authored is a race either way, and hiding the
			// write behind mutable would only make it harder to see. Evaluate stays
			// const-correct with no lazy state.
			//
			// DEFECT: the burden falls on every caller. RemoveAllKeys above, and the
			// mutable GetKeys() below, both change the range without calling this -- so
			// a caller that forgets leaves the cache reporting a range no key has.
			void InvalidateCache() { m_CacheValid = false; }

		private:
			// Evaluates the segment between keys[i] and keys[i+1]. `keys` must
			// be sorted, which AddKey guarantees.
			// The segment evaluator.
			//
			// @param keys  Sorted keys.
			// @param i     Index of the LEFT key of the segment.
			// @param time  Time within that segment.
			//
			// Takes the vector as a PARAMETER rather than reading the member, which is
			// what lets it be exercised on a temporary or a test fixture. The sorted
			// precondition comes from AddKey -- and, per GetKeys() above, from nothing
			// that stops a caller breaking it.
			float EvaluateSegment(const std::vector<Keyframe> &keys, size_t i,
								  float time) const;

			// The keys, sorted ascending by time. The class's only real state beyond
			// the two cached floats and the extrapolation mode.
			std::vector<Keyframe> m_Keys;
			Extrapolation m_Extrapolation = Extrapolation::Clamp;

			// The cached range, cached because Evaluate runs thousands of times a frame
			// and a front()/back() pair per call is measurable.
			//
			// DEFECT: zero-initialised, so before the first cache fill both reads are 0.0
			// and a curve whose keys begin at time 10 has a cached start
			// indistinguishable from an empty curve's until the cache has been
			// recomputed. m_CacheValid below is what disambiguates, so correctness
			// depends on every read path checking it first.
			float m_CachedStart = 0.0f;
			float m_CachedEnd = 0.0f;
			// The validity flag, false on construction so the first read recomputes.
			//
			// DEFECT: nothing sets it back to true in this header, and RemoveAllKeys
			// does not clear it. Every update to it lives in the .cpp, so the invariant
			// 'm_CacheValid implies m_CachedStart and m_CachedEnd match m_Keys' is
			// spread across two files with nothing documenting it.
			bool m_CacheValid = false;
		};

	} // namespace Animation

} // namespace Wasteland
