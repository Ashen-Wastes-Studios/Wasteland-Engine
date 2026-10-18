#pragma once

#include "Wasteland/Animation/AnimationCurve.h"
#include "Wasteland/Animation/Skeleton.h"

// Needed for m_Name and the const std::string& ctor parameter.
// Note glm is NOT included here even though glm::quat appears on
// line 41 -- it arrives transitively via AnimationCurve.h or
// Skeleton.h. That is a fragile arrangement: reordering those
// includes, or a header that stops pulling glm in, breaks this one.
#include <string>
// The bone-to-track map. Hash map rather than vector because BoneIndex
// is a sparse identifier, not a dense array index.
#include <unordered_map>
#include <vector>

// Outer namespace, then a nested Animation namespace on line 12.
// Both closing braces are named at the bottom (lines 188 and 190),
// which is good practice for a two-deep nest.
namespace Wasteland {

	// Nested rather than folded into Wasteland. The inner one is indented
	// by one extra tab throughout, which is why the members below sit
	// three tabs deep.
	namespace Animation {

		// Which transform channels a track animates.
		// The explicit uint8_t underlying type is worth noting: three
		// enumerators fit in a byte, so the enum is compact. This matters
		// because a channel can plausibly end up in a serialised key.
		// Scoped, so TrackChannel::Position cannot leak.
		enum class TrackChannel : uint8_t
		{
			Position = 0,
			Rotation = 1,
			// Ordinals 0/1/2 are written out explicitly even though declaration
			// order would produce the same values. That is the defensive choice --
			// it means inserting a channel later cannot silently renumber the
			// other two.
			// DEFECT: there is no DEFECT here yet, but nothing else in this header
			// validates a channel value, so AddKey (line 122) must handle an
			// out-of-range channel. Its comment says out-of-range COMPONENTS are
			// ignored; it says nothing about an out-of-range channel.
			Scale = 2,
		};

		// A keyframed quaternion channel.
		//
		// Its own type rather than four scalar curves because a quaternion must NOT
		// be interpolated componentwise. Lerping x,y,z,w between two rotations
		// does not rotate uniformly: the result is not unit-length (so it no
		// longer represents a rotation), and the angular rate varies through the
		// interval -- a limb visibly accelerates and decelerates mid-swing, which
		// reads as a hitch rather than motion.
		//
		// Slerp interpolates along the arc of constant angular velocity, which is
		// what a 180-degree turn should actually look like. Storing quaternion
		// keys keeps that correct and costs one fewer structure than four
		// parallel curves plus the reconstruction logic.
		class RotationTrack
		{
		public:
			struct Key
			{
				// Seconds from the clip start. Not normalised, so key times are
				// absolute within the clip and GetStartTime/GetEndTime exist to
				// recover the actual range.
				// Zero default means a default-constructed Key sits at the clip origin
				// with the identity rotation -- a coherent no-op key.
				float Time = 0.0f;
				// glm::quat is constructed in (w, x, y, z) order by convention, so this
				// is w=1 with zero vector part: the IDENTITY rotation.
				// DEFECT: the brace initialiser relies on that component order being
				// remembered correctly. Written in the visually-natural (x, y, z, w) it
				// would silently become a 180-degree rotation about X. A named constant
				// or a constructor call would remove the hazard.
				glm::quat Value = {1.0f, 0.0f, 0.0f, 0.0f};
				// Per-key, not per-track: a key can be eased differently from its
				// neighbours, which is what animation tools expose and what makes
				// hand-keyed motion readable.
				// Cubic by default, matching the default arguments on AddKey below --
				// so an un-authored key and an authored key agree.
				Interpolation Mode = Interpolation::Cubic;

				// Needed because the other two constructors on lines 45 and 46 are
				// user-declared, which would otherwise suppress the implicit default.
				// It is redundant given every member has an in-class initialiser, but
				// the explicit = default documents that the default is meaningful
				// rather than accidentally aggregate-initialisable.
				Key() = default;
				// Two-argument convenience: the mode then takes its in-class default
				// of Cubic, so this and the three-argument form below differ only in
				// being explicit about it.
				// Empty body: all initialisation is in the list, which is the
				// cheaper form (no default-construct-then-assign).
				Key(float t, const glm::quat &v) : Time(t), Value(v) {}
				// The full constructor, with the initialiser list wrapped onto the
				// next line by a tab.
				Key(float t, const glm::quat &v, Interpolation m)
					// Declaration order matches initialiser order (Time, Value, Mode),
					// which is what -Wreorder checks for. Getting this wrong is only a
					// performance issue when the initialisers are independent, but the
					// ordering here is correct regardless.
					: Time(t), Value(v), Mode(m) {}
			};

			// Replaces a key at the same time, keeping the list sorted.
			// Insert-or-replace, keeping m_Keys sorted by time -- the invariant
			// Evaluate on line 65 and GetStartTime/GetEndTime both rely on.
			// DEFECT (ambiguous contract): "replaces a key at the same time" --
			// at the same FLOAT time. A key at 0.1 and one at 0.10000001 are
			// distinct and both retained, so two keys can sit arbitrarily close
			// together and make interpolation ill-conditioned. There is no epsilon
			// comparison, and nothing in this header mentions one.
			void AddKey(float time, const glm::quat &value,
						Interpolation mode = Interpolation::Cubic);

			void RemoveAllKeys() { m_Keys.clear(); }

			// One of three inline predicates here (IsEmpty, GetKeyCount, and
			// RemoveAllKeys above) that keep the trivial accessors out of the
			// .cpp.
			bool IsEmpty() const { return m_Keys.empty(); }
			// size_t rather than int, so a track with more keys than INT_MAX keys
			// is representable -- which cannot happen in practice, but the correct
			// type costs nothing.
			size_t GetKeyCount() const { return m_Keys.size(); }

			// The rotation at `time`.
			//
			// Clamped to the end keys outside the keyed range. Identity for an
			// empty track, matching AnimationCurve's "no opinion reads as zero"
			// rule -- except identity IS the neutral rotation, so it composes
			// correctly rather than merely being a safe default.
			glm::quat Evaluate(float time) const;

			float GetStartTime() const;
			float GetEndTime() const;

			// const REFERENCE: no copy for an editor UI walking the keys.
			// DEFECT (no mutable overload, unlike AnimationClip::GetTrack which
			// has both on lines 172-173): a caller holding the reference can
			// still invalidate it by calling AddKey or RemoveAllKeys, and there
			// is no const-iterator begin/end here either. Iterating the returned
			// reference while adding a key is a silent iterator invalidation.
			const std::vector<Key> &GetKeys() const { return m_Keys; }

		// One member, so private is doing real work here rather than just
		// sectioning.
		private:
			// Sorted by Time at all times, maintained by AddKey.
			// DEFECT (reserve): nothing ever reserves, so an imported clip with
			// thousands of keys per channel reallocates and copies repeatedly
			// during load -- O(n log n) copies on top of the insertions.
			// A single reserve() in the first AddKey call, or a size-taking
			// overload, would avoid it.
			std::vector<Key> m_Keys;
		};

		// Keyframes for one bone, one channel.
		//
		// Split per channel rather than one vector of keys carrying a channel
		// tag, because the channels interpolate differently: position and scale
		// are scalar curves, rotation is a slerped quaternion.
		struct BoneTrack
		{
			// A fixed C array of three, not a std::array and not a glm::vec3 of
			// curves. Three consequences: BoneTrack is not assignable-by-array but
			// is copyable, the type is trivially destructible, and a caller can
			// index it out of range with no check.
			// The trailing comment names the component order, which is the one
			// thing a bare array member most needs.
			AnimationCurve Position[3]; // x, y, z
			RotationTrack Rotation;
			AnimationCurve Scale[3];	 // x, y, z

			// ANDs all seven channels, so a track counts as non-empty as soon as
			// any single curve or the rotation track has a key.
			// Note it does NOT check Scale independently of Position -- all
			// three components of each array are tested, so a partially animated
			// track is correctly non-empty.
			bool IsEmpty() const
			{
				return Position[0].IsEmpty() && Position[1].IsEmpty() &&
					   Position[2].IsEmpty() && Rotation.IsEmpty() &&
					   Scale[0].IsEmpty() && Scale[1].IsEmpty() && Scale[2].IsEmpty();
			}

			// The earliest key time across all seven channels of this bone.
			// Out of line because the loop is not a one-liner.
			// DEFECT (undefined for an empty track): nothing in this header says
			// what an empty BoneTrack returns. Compare RotationTrack::Evaluate on
			// line 65, whose comment explicitly says identity for an empty track.
			// A caller cannot tell an empty track from one starting at 0.
			float GetStartTime() const;
			float GetEndTime() const;
		};

		// A named animation: keyframes per bone, plus looping and rate.
		//
		// The unit of authored content. A clip does not know about playback,
		// blending or state -- it only answers "what pose does this skeleton
		// hold at time t". Keeping it that way is what lets the same clip be
		// sampled by the animator, blended with another clip, previewed in the
		// editor, and retargeted, none of which the clip has to be aware of.
		class AnimationClip
		{
		public:
			AnimationClip() = default;
			// explicit because a std::string converts to AnimationClip otherwise,
			// which would let a function taking AnimationClip be called with a
			// bare string.
			// m_Looping, m_PlayRate and m_Tracks take their in-class defaults
			// rather than appearing here.
			explicit AnimationClip(const std::string &name) : m_Name(name) {}

			// ---- Authoring ----

			void SetName(const std::string &name) { m_Name = name; }
			const std::string &GetName() const { return m_Name; }

			// Adds or replaces a key on a bone channel.
			//
			// `component` is 0..2 for position/scale and 0..3 for rotation.
			// An out-of-range component is ignored rather than asserted: the
			// value usually comes from a file parser, and a malformed key in an
			// imported clip should drop that key, not abort the import.
			void AddKey(BoneIndex bone, TrackChannel channel, int component,
						float time, float value,
						Interpolation mode = Interpolation::Cubic);

			// Convenience wrappers for the common authoring cases.
			void AddPositionKey(BoneIndex bone, float time, const glm::vec3 &value,
								Interpolation mode = Interpolation::Cubic);
			void AddRotationKey(BoneIndex bone, float time, const glm::quat &value,
								Interpolation mode = Interpolation::Cubic);
			// Third of the three convenience wrappers.
			// DEFECT (indentation): the continuation line on 132 is indented with
			// spaces, not the tabs used everywhere else in this file. The file is
			// otherwise tab-indented throughout, so this one line is visibly wrong
			// and will not align with its neighbours.
			void AddScaleKey(BoneIndex bone, float time, const glm::vec3 &value,
							 // Same default as the other two wrappers and as AddKey, so all four
							 // authoring entry points agree on what an unspecified mode means.
							 Interpolation mode = Interpolation::Cubic);

			// Recomputes auto-generated tangents for every cubic key in the clip.
			// Must be called after bulk authoring (an importer) or the curves
			// between keys will use whatever tangents AddKey happened to produce.
			// DEFECT: nothing documents whether this is idempotent or whether it
			// clobbers hand-authored tangents -- the two are indistinguishable in
			// the Key struct, since a key with an explicit tangent and one with an
			// auto tangent look identical.
			void RecalculateAutoTangents();

			// ---- Playback settings ----

			// Looping is a property of the clip, not the animator, because it
			// is an authoring decision: a walk cycle loops, a death does not,
			// and the same animator may play either.
			bool IsLooping() const { return m_Looping; }
			void SetLooping(bool looping) { m_Looping = looping; }

			// Playback rate is a CLIP property, stored but never used by this
			// class: Sample takes an absolute time, so the animator applies the
			// rate. See the looping note on line 138 for why it lives here.
			float GetPlayRate() const { return m_PlayRate; }
			// DEFECT (no validation): no check for zero, negative or NaN. A
			// negative rate plays the clip backwards, which may be intended, but a
			// rate of 0 would freeze an animator that multiplies its delta by this,
			// and a NaN propagates into every sampled pose.
			// Compare SetZoomLevel in OrthographicCameraController.h, whose own
			// comment flags the same missing clamp.
			void SetPlayRate(float rate) { m_PlayRate = rate; }

			// ---- Range ----

			float GetDuration() const;
			float GetStartTime() const;
			float GetEndTime() const;
			// Tracks-level emptiness, unlike BoneTrack::IsEmpty which is
			// channel-level. A clip with one track holding one key is not empty.
			bool IsEmpty() const { return m_Tracks.empty(); }

			// ---- Sampling ----

			// Samples the clip at `time` into `outPose`.
			//
			// `outPose` is resized to `skeleton.GetBoneCount()`. Bones the clip
			// does not animate are left at whatever they already held -- the
			// caller is expected to seed the pose with the rest pose first, so
			// a partial clip animates only the joints it names.
			//
			// That contract is what makes partial animation work at all: a
			// clip with only a "Head" bone must leave the rest of the skeleton
			// exactly where it was, not snap it to the origin.
			void Sample(const Skeleton &skeleton, float time, Pose &outPose) const;

			// ---- Access ----

			size_t GetTrackCount() const { return m_Tracks.size(); }
			bool HasTrack(BoneIndex bone) const;
			const BoneTrack *GetTrack(BoneIndex bone) const;
			// The mutable overload, completing the pair on lines 172-173.
			// DEFECT: this returns a pointer INTO the unordered_map, and nothing
			// warns that any subsequent AddKey -- which may insert and rehash --
			// invalidates it. unordered_map rehashing invalidates pointers to
			// elements, so a caller holding this pointer across an authoring call
			// is left with a dangling one. This is worse than the vector case noted
			// on RotationTrack::GetKeys.
			BoneTrack *GetTrack(BoneIndex bone);

			// Every bone this clip animates.
			std::vector<BoneIndex> GetAnimatedBones() const;

			void RemoveAllTracks() { m_Tracks.clear(); }

		// Implementation state, following the public accessors.
		private:
			// A real default rather than an empty string, so an unnamed clip is
			// identifiable in a UI. An empty name would also be indistinguishable
			// from an explicitly-blanked one.
			std::string m_Name = "Unnamed";
			// Loops by default. The comment on line 138 gives the reasoning (a
			// walk cycle is the common case), but the default is a guess: a clip
			// authored for a one-shot action and never told otherwise will loop.
			bool m_Looping = true;
			// Normal speed. Combined with m_Looping above, a default-constructed
			// AnimationClip is a normal-speed looping clip with no tracks.
			float m_PlayRate = 1.0f;

			// Sparse key, BoneIndex value.
			// DEFECT (type mismatch): the map is keyed by raw int32_t rather than
			// by BoneIndex, while every public method on lines 122, 171-173 and
			// 176 takes a BoneIndex. So BoneIndex must be a typedef or alias for
			// int32_t -- which means nothing type-checks against passing a bone id
			// from a different indexing scheme (a mesh vertex index, say). Using
			// BoneIndex as the key would be free and would document the intent.
			// BoneTrack is the mapped value, stored BY VALUE. A clip with N
			// animated bones stores N full BoneTracks inline -- seven curves each --
			// so map node overhead is paid per bone.
			std::unordered_map<int32_t, BoneTrack> m_Tracks;
		};

	} // namespace Animation

} // namespace Wasteland
