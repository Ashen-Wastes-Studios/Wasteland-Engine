// PCH first. The mixer is pure computation with no platform dependency, but it
// still needs the engine's Ref<T> and glm headers that the PCH supplies.
#include "wlpch.h"

// Its own header, which declares PlayRequest, Voice, SpatialGains and the Mixer
// class. Note Mix() and Update() are both public: a device mixes a buffer, and
// something else advances time, and conflating them is what causes drift.
#include "Mixer.h"

// std::max/std::min throughout -- clamping volumes, gains and pan coefficients.
// Not <core> or <utility>: this file is explicit about what it uses.
#include <algorithm>
// std::cos/std::sin for the equal-power pan, and std::max on doubles in
// ComputeAttenuation's denominator guard.
#include <cmath>

// Nested namespace: all audio types live under Audio so a Mixer cannot collide
// with, say, an input or animation mixer of the same name.
namespace Wasteland::Audio
{

	// File-local constants. Anonymous namespace rather than static members: these
	// are implementation details, not part of the Mixer's published surface.
	namespace
	{
		// Below this the voice has faded to nothing worth keeping.
		// UNUSED: nothing in this file (or the header) references kFadeFloor. A voice
		// below this amplitude is not retired early -- MixVoice just skips it via the
		// `baseVolume <= 0.0f` check, which is exact rather than approximate. The
		// constant and its comment describe an intent that was never implemented.
		constexpr float kFadeFloor = 0.0001f;

		// Guard against a zero or negative sample rate turning a voice into an
		// infinite loop. A clip with no rate cannot be played; treating it as
		// finished is better than hanging the audio thread.
		// UNUSED, and it documents a guard that does not exist. The comment claims this
		// prevents a zero sample rate becoming an infinite loop in AdvanceVoice. It
		// cannot: AdvanceVoice divides by clip->GetFrameCount(), not by the sample rate,
		// so a 0 Hz clip has 0 frames and is already rejected in Play() by the
		// `GetFrameCount() == 0` check.
		constexpr float kMinSampleRate = 1.0f;
	}

	// Called by AudioSystem::Update once per frame from the camera transform, and
	// from the Mixer constructor. It derives Right from Forward/Up so ComputeSpatial
	// does not have to recompute it per voice.
	void Mixer::Listener::RecomputeBasis()
	{
		// A zero-length forward vector has no meaningful "right", and
		// normalising it produces NaN that then poisons every gain downstream.
		// Fall back to -Z, which is what a default-constructed listener means
		// anyway.
		// Squared length avoids the sqrt just to test for a degenerate vector. The 1e-8
		// threshold is on the SQUARE, so it corresponds to a length of ~1e-4.
		float lengthSquared = glm::dot(Forward, Forward);
		// Also catches NaN, since NaN comparisons are false -- a NaN Forward would
		// skip this branch and then poison every gain downstream.
		if (lengthSquared < 1e-8f)
			Forward = glm::vec3(0.0f, 0.0f, -1.0f);
		// Normalised so the dot products in ComputeSpatial measure real angles rather
		// than being scaled by however far the camera happened to be translated.
		else
			Forward = glm::normalize(Forward);

		// Right = Forward x Up. In a right-handed Y-up system with Forward = -Z and
		// Up = +Y this yields +X, which is the engine's right-hand direction.
		Right = glm::cross(Forward, Up);
		// Reached when Forward is parallel to Up -- looking straight up or down. The
		// cross product is then the zero vector and there is no meaningful "right".
		if (glm::dot(Right, Right) < 1e-8f)
		{
			// Forward is parallel to Up. Pick any perpendicular so the basis is
			// still orthonormal; the listener's roll is undefined in that pose
			// and this only affects stereo image.
			// First fallback: cross with world +Z. For Forward = +Y this gives -X, and for
			// Forward = -Y it gives +X -- either way a perpendicular, so the basis stays
			// orthonormal and only the roll is arbitrary.
			Right = glm::cross(Forward, glm::vec3(0.0f, 0.0f, 1.0f));
			// Second fallback: Forward is parallel to Z as well, which given the first test
			// means Forward is near-zero -- but Forward was normalised above, so this is
			// defensive. It keeps normalize() below from dividing by zero.
			if (glm::dot(Right, Right) < 1e-8f)
				// The engine's +X. Chosen over -X arbitrarily: with an undefined roll, the
				// only observable effect is which ear a sound plays through.
				Right = glm::vec3(1.0f, 0.0f, 0.0f);
		}
		// Normalised last, after both fallbacks. ComputeSpatial's `lateral` is a dot
		// product against this vector, so a non-unit Right would scale pan with speed.
		Right = glm::normalize(Right);
	}

	// explicit in the header, so there is no accidental int -> Mixer conversion.
	Mixer::Mixer(int maxVoices)
		// A cap of 0 or negative would make every Play() steal or drop. Clamping to 1
		// keeps the mixer usable and makes the failure visible (one voice) rather than
		// silent (no audio at all).
		: m_MaxVoices(maxVoices < 1 ? 1 : maxVoices)
	{
		// So Right is valid before the first Play. Without this the first frame's panning
		// would use the header's default Right against an un-normalised Forward.
		m_Listener.RecomputeBasis();
	}

	// Name -> clip, so the same file is decoded once and referenced by string from
	// script. Registered clips keep their Ref alive for the mixer's lifetime.
	// ---- Clip registry ----

	// Silently ignores bad input rather than logging: registration happens during
	// asset load, where a failure is already reported by the loader.
	void Mixer::RegisterClip(const std::string &name, const Ref<AudioClip> &clip)
	{
		// IsValid() is AudioClip's own flag, set by its decoder. Registering an invalid
		// clip would store a handle that Play() later rejects anyway.
		if (name.empty() || !clip || !clip->IsValid())
			return;

		// Re-registering replaces. Deliberate: a hot-reloaded clip should take
		// effect, and refusing would leave a stale file in place with no
		// diagnostic.
		m_Clips[name] = clip;
	}

	// Returns nullptr for an unknown name. Callers check, or pass the result to Play,
	// which treats a null clip as a dropped voice rather than a crash.
	Ref<AudioClip> Mixer::GetClip(const std::string &name) const
	{
		auto it = m_Clips.find(name);
		return it != m_Clips.end() ? it->second : nullptr;
	}

	// Existence check without a Ref copy. AudioSystem uses this to avoid issuing a
	// Play for a clip that was never imported.
	bool Mixer::HasClip(const std::string &name) const
	{
		return m_Clips.find(name) != m_Clips.end();
	}

	// ---- Spatial ----

	// static because it needs no mixer state -- only the listener's distance, which
	// the caller has already computed. Public in the header so a test can assert
	// the falloff curve directly without a listener.
	float Mixer::ComputeAttenuation(float distance, float minDistance, float maxDistance)
	{
		// No falloff window, or a degenerate one, means "always audible".
		// Returning 1.0 is the honest reading of a max that is not above the
		// min: the alternative is dividing by ~0 and producing a click.
		// Two degenerate cases folded into one branch: an empty or inverted window, and
		// a non-positive min. Both are read as "no falloff".
		if (maxDistance <= minDistance || minDistance <= 0.0f)
			return 1.0f;

		// Inside the inner radius: full volume. The caller passes minDistance as the
		// distance at which the sound is exactly at Volume.
		if (distance <= minDistance)
			return 1.0f;
		// Outside the outer radius: silent. Exact 0, not an epsilon, so a sound at the
		// boundary contributes nothing and cannot ring.
		if (distance >= maxDistance)
			return 0.0f;

		// Inverse-distance rolloff, NORMALISED to the [min, max] window.
		//
		// Raw inverse gain is min/distance, which is 1.0 at minDistance but
		// only min/maxDistance at maxDistance -- it never reaches silence, so a
		// sound would still be faintly audible at any range. The normalisation
		// below remaps that curve onto exactly [1, 0]:
		//
		//     (min/d - min/max) / (1 - min/max)
		//
		// An earlier version multiplied the raw gain by (1 - t) where t was the
		// normalised distance. That applied distance falloff TWICE -- the
		// gain already encoded distance -- so a sound at 10 units with a
		// 1..100 window came out at 0.091 instead of 0.100, and the error grew
		// with range. Normalising the curve, rather than scaling it, is what
		// makes the endpoints exact.
		// The raw inverse-distance curve's value AT maxDistance. This is the number the
		// normalisation below subtracts to make the far end exactly zero.
		float minGain = minDistance / maxDistance;
		// The raw curve, with the distance floored at minDistance. The floor is redundant
		// given the early return above -- it is belt-and-braces against a caller that
		// bypassed the window check.
		float rawGain = minDistance / std::max(distance, minDistance);
		// Remaps [minGain, 1] onto [0, 1]. At distance == minDistance this is 1.0 and
		// at distance == maxDistance it is exactly 0.0, which the clamps below preserve.
		float normalised = (rawGain - minGain) / (1.0f - minGain);
		// Clamped because float rounding at the endpoints can land a hair outside [0,1],
		// and a gain above 1 would let a voice exceed full volume.
		return std::max(0.0f, std::min(1.0f, normalised));
	}

	// Resolves attenuation AND pan together, because both depend on the same
	// listener-relative vector and computing it twice would be wasteful.
	SpatialGains Mixer::ComputeSpatial(const glm::vec3 &position, bool spatial,
									   float minDistance, float maxDistance) const
	{
		// Default-constructed to Left = Right = Attenuation = 1.0, so a 2D sound takes
		// the early return below and is full volume, centred.
		SpatialGains gains;
		// 2D: the defaults are already the answer, so nothing else is computed. This
		// is why Spatial is per-request rather than a global switch.
		if (!spatial)
			return gains;  // full volume, centred

		// Listener-relative. Computed once and used for BOTH the distance and the lateral
		// component, so they cannot disagree about where the sound is.
		glm::vec3 toSound = position - m_Listener.Position;
		// Euclidean distance, not squared. The sqrt is cheap and the pan formula below
		// needs the true direction, which comes from toSound/distance.
		float distance = glm::length(toSound);
		// Computed BEFORE the near-zero check below, so a sound at the listener still
		// gets the right attenuation (1.0 for any sane window).
		gains.Attenuation = ComputeAttenuation(distance, minDistance, maxDistance);

		// At the listener the direction is undefined -- any unit vector would do -- so
		// the sound stays centred rather than picking an arbitrary pan.
		if (distance < 1e-4f)
			return gains;  // at the listener: centred, no pan

		// Equal-power panning. A linear split between left and right would sum
		// to less than 1 in between, so a sound sweeping past the player would
		// dip in loudness -- the classic "hole in the middle".
		// Signed projection onto the listener's right vector: +1 is fully right, -1 fully
		// left, 0 dead ahead. The division is safe because of the 1e-4 guard above.
		float lateral = glm::dot(toSound / distance, m_Listener.Right);
		// Clamped against float error. Right is normalised, so the dot cannot exceed 1 in
		// exact arithmetic -- but an unnormalised Right would, hence the clamp.
		lateral = std::max(-1.0f, std::min(1.0f, lateral));

		// Constant-power: panAngle of 0 is centre, +/-1 is hard left/right.
		// sqrt(0.5), the constant-power factor. Combined with the sqrt(2) below, the two
		// cancel for the per-side gains, so this is really a readable way to express
		// "equal power, not equal amplitude".
		const float kSqrtHalf = 0.70710678f;
		// Maps lateral [-1,1] onto [0, PI/2]: 0 is hard left, PI/4 is centre, PI/2 is
		// hard right. The 0.25 is 1/4 because the range spans a quarter turn.
		float panAngle = (lateral + 1.0f) * 0.25f * 3.14159265f;
		// At centre (PI/4) this gives 0.707, i.e. -3 dB per side -- the correct
		// constant-power centre, where a linear split would dip 3 dB in total.
		gains.Left = std::cos(panAngle) * kSqrtHalf * 1.41421356f;
		// sin at the complementary angle. Left^2 + Right^2 == 1 at every pan position,
		// which is the property that makes the sweep past the player sound level-flat.
		gains.Right = std::sin(panAngle) * kSqrtHalf * 1.41421356f;

		// Attenuation is left unmultiplied into Left/Right: keeping it separate lets a
		// caller attenuate without panning, as the header notes.
		return gains;
	}

	// ---- Voice allocation ----

	// Called only when the cap is already reached, so it is off the hot path.
	int Mixer::FindStealCandidate(int incomingPriority) const
	{
		// A voice may be stolen when it is lower priority than the incoming
		// sound, when priority is TIED, or when it is already fading.
		//
		// Ties count as stealable deliberately. The cap exists to bound
		// simultaneous one-shots -- a firefight, not a music player -- and if
		// equal-priority footsteps could not displace each other then the
		// fourth footstep in a burst would simply be lost, which is exactly
		// the stutter the cap is supposed to prevent. A STRICTLY higher
		// priority voice is still protected: a footstep never silences UI.
		//
		// Among valid candidates prefer the QUIETEST. Two equal-priority
		// footsteps: dropping the one already inaudible is better than the one
		// the player can currently hear. Ties keep the earlier voice, which is
		// the one furthest through its life.
		// -1 means "nothing may be stolen", which Play() turns into a dropped voice.
		// Deliberately not 0: index 0 is a real voice.
		int best = -1;
		// Only meaningful once best >= 0. The `best < 0 ||` guard below makes the first
		// candidate win regardless of this initial value.
		float bestVolume = 0.0f;

		// Linear scan over at most m_MaxVoices entries (32 by default). A heap would
		// cost more than it saves at this size.
		for (size_t i = 0; i < m_Voices.size(); ++i)
		{
			// Const reference: this function only selects, never modifies. The caller reuses
			// the slot by assigning a fresh Voice in Play().
			const Voice &voice = m_Voices[i];
			// A retired slot is not a candidate. Retired slots exist precisely so they can
			// be reused, and they are reachable here only because AllocateVoiceSlot() ran first.
			if (!voice.IsPlaying())
				continue;

			// A looping voice is never stolen. Music and ambience are
			// long-lived by design, and cutting a loop to make room for a
			// footstep is the worst possible outcome.
			// Loops are permanent by design. This is the single most important line in the
			// function: without it, a burst of footsteps would cut the music.
			if (voice.Loop)
				continue;

			// A strictly-higher-priority voice is protected. A footstep must
			// never silence UI. A TIE is stealable: the cap exists to bound
			// simultaneous one-shots, and if two equal-priority footsteps
			// could not displace each other the fourth in a burst would be
			// lost, which is the stutter the cap is meant to prevent.
			//
			// An already-fading voice is always fair game, whatever its
			// priority: it is on its way out, and reusing the slot early is
			// exactly right.
			// A voice on its way out is free to reclaim early, whatever its priority -- it is
			// already being silenced, so the slot is effectively unused.
			const bool alreadyFading = voice.FadeRemaining > 0.0f;
			// <= not <, so a tie is stealable. See the block comment above for why: the cap
			// exists to bound simultaneous one-shots, and refusing ties would lose the fourth
			// footstep in a burst instead.
			const bool notMoreImportant = voice.Priority <= incomingPriority;
			// The OR is the whole policy: a voice is a candidate if EITHER it is fading OR
			// it is no more important than the incoming sound.
			if (!alreadyFading && !notMoreImportant)
				continue;

			// Strictly-less keeps the EARLIER voice on an exact volume tie, which is the one
			// furthest through its life -- the better one to drop.
			if (best < 0 || voice.Volume < bestVolume)
			{
				// Narrowing from size_t. Safe because m_Voices.size() <= m_MaxVoices and the cap
				// is clamped to >= 1, so the vector cannot exceed INT_MAX.
				best = (int)i;
				bestVolume = voice.Volume;
			}
		}

		// -1 when every candidate was filtered out. Play() counts that as dropped, not
		// stolen, so GetDroppedVoiceCount() and GetStealedVoiceCount() stay meaningful.
		return best;
	}

	// Two outcomes: reuse a retired slot, or grow. Only when both fail does Play()
	// consider stealing.
	int Mixer::AllocateVoiceSlot()
	{
		// Reuse a retired slot before growing, so a long session does not
		// accumulate a vector sized to the loudest moment ever heard.
		// First-fit over the whole vector. Linear, but bounded by the cap.
		for (size_t i = 0; i < m_Voices.size(); ++i)
		{
			// Retired slots are found here rather than compacted away, so the vector keeps
			// its high-water mark without holding any live voice.
			if (!m_Voices[i].IsPlaying())
				return (int)i;
		}
		// Growth is capped. Note the vector NEVER shrinks, so its size is the loudest
		// moment ever heard -- GetActiveVoiceCount() exists because of exactly that.
		if ((int)m_Voices.size() < m_MaxVoices)
		{
			// Default-constructed Voice has VoiceId = -1, i.e. not playing. Play() then
			// assigns a real id immediately after getting the index back.
			m_Voices.emplace_back();
			// Index of the element just created. -1 cannot be confused with a valid slot,
			// because the vector is never empty here (size was >= 0 and just grew).
			return (int)m_Voices.size() - 1;
		}
		// At the cap with no free slot. Play() treats this as the signal to look for a
		// steal candidate, NOT as an error.
		return -1;
	}

	// The single entry point for starting a sound. Returns a voice id for the caller
	// to Stop or SetVoiceVolume later, or -1.
	int Mixer::Play(const PlayRequest &request)
	{
		// A direct handle takes priority over the name lookup below.
		Ref<AudioClip> clip = request.Clip;
		// Fall back to the registry. The `!clip` test means an explicitly-set handle wins,
		// so a caller can override a stale name.
		if (!clip && !request.ClipName.empty())
			clip = GetClip(request.ClipName);

		// A missing clip is a content bug (a sound referenced but never
		// imported). It is a NORMAL return of -1 rather than an assert: a
		// missing footstep should not take the game down mid-firefight.
		// Invalid includes "not found": GetClip returned nullptr. Counted as dropped so
		// the diagnostic in the header (watch GetDroppedVoiceCount) surfaces content bugs.
		if (!clip || !clip->IsValid())
		{
			// Diagnostic counter, never reset except by ResetCounters(). A monotonically
			// rising count is the signal that sounds are being lost.
			m_DroppedVoices++;
			return -1;
		}

		// A loop of an empty clip would never end and would permanently
		// consume a voice slot.
		// GetFrameCount is samples.size(). A zero-length clip would either never advance
		// (silence) or, if looped, spin forever in AdvanceVoice -- so it is rejected here.
		if (clip->GetFrameCount() == 0)
		{
			m_DroppedVoices++;
			return -1;
		}

		// Free or newly grown slot, or -1 at the cap. This is the common path: most Play
		// calls never reach the steal logic below.
		int slot = AllocateVoiceSlot();

		// At the cap. Only now is the expensive/controversial steal path considered.
		if (slot < 0)
		{
			// At the cap. Try to make room before giving up.
			int candidate = FindStealCandidate(request.Priority);
			if (candidate < 0)
			{
				m_DroppedVoices++;
				return -1;
			}
			// The stolen slot is reused directly rather than freed and reallocated. The old
			// voice's data is overwritten wholesale by the `voice = Voice()` line below,
			// which is what makes the steal silent rather than a click.
			slot = candidate;
			// Counted separately from drops so a debugger can tell "we were too busy" from
			// "the sound was missing".
			m_StealedVoices++;
		}
		// DEFECT: unreachable. AllocateVoiceSlot returns either -1 (handled above) or an
		// index < m_Voices.size() <= m_MaxVoices, and FindStealCandidate only returns
		// indices below m_Voices.size(). So `slot >= m_MaxVoices` can never be true and
		// this defensive branch can never drop a voice.
		else if ((int)m_Voices.size() >= m_MaxVoices && slot >= m_MaxVoices)
		{
			// Defensive: a slot past the cap should be impossible.
			m_DroppedVoices++;
			return -1;
		}

		// Reference into the vector. Nothing below resizes m_Voices, so this stays valid.
		Voice &voice = m_Voices[slot];
		// Full reset to defaults, which clears the stolen voice's FadeRemaining,
		// FadeDuration, FramePosition and Volume in one assignment.
		voice = Voice();
		voice.Clip = clip;
		// Cached so a finished voice can be identified without comparing clip pointers.
		voice.ClipId = clip->GetId();
		// Monotonic ids, never reused. That is what lets a caller hold a stale id safely:
		// once the voice retires, IsPlaying() is false for that id forever.
		voice.VoiceId = m_NextVoiceId++;
		// DEFECT: this overflow check is dead code. m_NextVoiceId is a signed int, and
		// signed overflow is undefined behaviour -- by the time it wraps, the compiler
		// is entitled to have assumed it could not, so the guard may never execute. An
		// unsigned counter with a skip of 0 would be well-defined.
		if (m_NextVoiceId <= 0)
			m_NextVoiceId = 1;  // wrapped; keep ids positive
		// Explicit, though `voice = Voice()` already set it. Stated because it is the
		// field everything else in playback depends on.
		voice.FramePosition = 0.0;
		// A negative volume or pitch is meaningless; clamp rather than
		// producing a reversed or silent voice.
		// Negative volume clamped rather than rejected: a script asking for -1 almost
		// certainly means "silent", and rejecting the whole Play would be worse.
		voice.Volume = std::max(0.0f, request.Volume);
		// Pitch 0 would freeze FramePosition forever and loop would spin; negative would
		// run the clip backwards past index 0. Both are meaningless, so both become 1.
		voice.Pitch = request.Pitch <= 0.0f ? 1.0f : request.Pitch;
		// Stored verbatim. FindStealCandidate reads it to protect long-lived sounds.
		voice.Loop = request.Loop;
		// Compared against other voices' priorities at steal time, never at mix time.
		voice.Priority = request.Priority;
		// Decides whether ComputeSpatial applies attenuation and panning for this voice.
		voice.Spatial = request.Spatial;
		voice.Position = request.Position;
		// Copied per voice rather than referenced, so a reused PlayRequest cannot change
		// the falloff of a sound already playing.
		voice.MinDistance = request.MinDistance;
		voice.MaxDistance = request.MaxDistance;
		voice.FadeRemaining = 0.0f;
		// Explicitly cleared: a fresh voice is not fading. The pairing with
		// FadeRemaining above is what MixVoice's ratio divides on.
		voice.FadeDuration = 0.0f;

		// The new id, NOT the slot index. Callers must use this -- Stop(voiceId) and
		// SetVoiceVolume look up by id, and the two differ once stealing reorders slots.
		return voice.VoiceId;
	}

	// FadeSeconds == 0 (the default) cuts immediately. Both paths below leave the
	// voice retired rather than erasing it, so the vector's size never shrinks.
	void Mixer::Stop(int voiceId, float fadeSeconds)
	{
		// Linear scan by id. A map would be faster but the vector is capped at ~32, and
		// keeping voices contiguous is what lets Play reuse a slot without bookkeeping.
		for (Voice &voice : m_Voices)
		{
			// Both conditions: a retired slot may still hold the id from before it retired,
			// and matching it again would resurrect a voice that has already finished.
			if (voice.VoiceId != voiceId || !voice.IsPlaying())
				continue;

			// A fade is a countdown, not a flag: MixVoice multiplies volume by the remaining
			// fraction, and Update retires the voice when the countdown hits zero.
			if (fadeSeconds > 0.0f)
			{
				// Do not lengthen a fade already in progress: calling Stop with
				// a 2s fade after a 0.1s one should not extend it.
				// Overwritten, not min()'d. The block comment's claim that a fade is not LENGTHENED
				// is therefore wrong -- a later Stop with a longer fade does extend this one.
				voice.FadeDuration = fadeSeconds;
				// Restarted from full, so the ratio in MixVoice starts at 1.0 and falls to 0.
				voice.FadeRemaining = fadeSeconds;
			}
			// Immediate stop. The two assignments are the exact retirement state: IsPlaying()
			// returns false once VoiceId is -1, and dropping the Ref lets the clip unload.
			else
			{
				// IsPlaying() tests `VoiceId >= 0`, so -1 is the single source of truth for
				// "this slot is free". Every other function keys off it.
				voice.VoiceId = -1;
				// Releases the clip's reference. ClipId is deliberately left stale -- nothing
				// reads it, and the next Voice assignment overwrites it.
				voice.Clip = nullptr;
			}
			return;
		}
	}

	// Identical body to Stop, but without the early `return`, so every live voice is
	// touched. Used on scene teardown and level change.
	void Mixer::StopAll(float fadeSeconds)
	{
		for (Voice &voice : m_Voices)
		{
			if (!voice.IsPlaying())
				continue;
			if (fadeSeconds > 0.0f)
			{
				voice.FadeDuration = fadeSeconds;
				voice.FadeRemaining = fadeSeconds;
			}
			else
			{
				voice.VoiceId = -1;
				voice.Clip = nullptr;
			}
		}
	}

	// Stops by clip rather than id: when a source is destroyed or a weapon swapped,
	// the caller knows the clip, not the voice ids.
	void Mixer::StopClip(const AudioClip *clip, float fadeSeconds)
	{
		// Null-tolerant because the argument comes from GetClip(), which returns nullptr
		// for an unknown name.
		if (!clip)
			return;
		for (Voice &voice : m_Voices)
		{
			// Pointer identity, not ClipId comparison. ClipId is cached for debuggers but is
			// not used here -- a raw AudioClip* is what the caller has to hand.
			if (!voice.IsPlaying() || voice.Clip.get() != clip)
				continue;
			if (fadeSeconds > 0.0f)
			{
				voice.FadeDuration = fadeSeconds;
				voice.FadeRemaining = fadeSeconds;
			}
			else
			{
				voice.VoiceId = -1;
				voice.Clip = nullptr;
			}
		}
	}

	// Returns false if the voice has already finished. AudioSystem treats that as
	// normal, so a caller must not treat false as an error.
	bool Mixer::SetVoiceVolume(int voiceId, float volume)
	{
		// Same by-id scan as Stop, but returns on the first match instead of continuing.
		for (Voice &voice : m_Voices)
		{
			if (voice.VoiceId == voiceId && voice.IsPlaying())
			{
				// Clamped for the same reason as in Play: a negative volume means silent, not
				// an invalid request.
				voice.Volume = std::max(0.0f, volume);
				return true;
			}
		}
		return false;
	}

	// Pitch is NOT clamped here the way volume is -- it is replaced with 1.0, because
	// pitch 0 and negative pitch are structurally broken rather than merely quiet.
	bool Mixer::SetVoicePitch(int voiceId, float pitch)
	{
		for (Voice &voice : m_Voices)
		{
			if (voice.VoiceId == voiceId && voice.IsPlaying())
			{
				// Changing pitch mid-voice does not rescale FramePosition, so there is no click,
				// but the clip's remaining length now changes with the new rate.
				voice.Pitch = pitch <= 0.0f ? 1.0f : pitch;
				return true;
			}
		}
		return false;
	}

	// Bulk version of SetVoiceVolume: every playing voice from this clip, including
	// looped music, so a music-volume slider can use it directly.
	void Mixer::SetClipVolume(const AudioClip *clip, float volume)
	{
		if (!clip)
			return;
		for (Voice &voice : m_Voices)
		{
			// No early return -- all matching voices are updated, which is the point of this
			// function versus the by-id variants.
			if (voice.IsPlaying() && voice.Clip.get() == clip)
				voice.Volume = std::max(0.0f, volume);
		}
	}

	// Read-only query for scripts and debug views. False for an id that has finished,
	// which is the normal answer for a stale id rather than an error.
	bool Mixer::IsPlaying(int voiceId) const
	{
		for (const Voice &voice : m_Voices)
			if (voice.VoiceId == voiceId && voice.IsPlaying())
				return true;
		return false;
	}

	// Advance-then-retire. Separate from Mix() so a device that mixes a different
	// frame count than wall-clock elapsed cannot cause permanent audio/video drift.
	// ---- Update ----

	// Called from Update only. Returns false to mean "this voice has finished";
	// Update then retires it. Position only -- no samples are touched here.
	bool Mixer::AdvanceVoice(Voice &voice, double frames)
	{
		// Defensive: Update already checks IsPlaying, but a Stop between the two calls
		// would leave a retired voice with a null clip.
		if (!voice.IsPlaying() || !voice.Clip)
			return false;

		// `frames` is already seconds * sample rate, so multiplying by Pitch here gives
		// the frame advance. Doing it in double keeps a long session from drifting.
		voice.FramePosition += frames * (double)voice.Pitch;

		// Cast to double so the comparisons below and the modulo-style subtraction are
		// done in the same precision as FramePosition.
		const double total = (double)voice.Clip->GetFrameCount();
		// Looping voices never return false from here, so they are never retired by this
		// path -- they are stopped explicitly or live until StopAll.
		if (voice.Loop)
		{
			// Wrap rather than clamp: a looping voice whose position stops at
			// the end is a voice that goes silent on the second loop.
			// DEFECT (benign): Play rejects zero-length clips, so total is always > 0 here.
			// Without this guard the while loop below would subtract 0 forever. The guard is
			// correct; it is simply unreachable given Play's check.
			if (total > 0.0)
			{
				// A while, not an if: a large time delta or a high pitch can overshoot by more
				// than one clip length in a single step.
				while (voice.FramePosition >= total)
					// Subtraction rather than fmod, so no library call and no precision loss beyond
					// the double arithmetic itself.
					voice.FramePosition -= total;
			}
			// A looping voice always survives. The false return below is the non-looping exit.
			return true;
		}

		// Non-looping: still playing while the position is before the end. Note this
		// runs AFTER the position was advanced, so it tests post-advance.
		return voice.FramePosition < total;
	}

	// Advances every voice by wall-clock seconds. Called once per frame from
	// AudioSystem::Update, NOT once per audio buffer -- that separation is what
	// keeps audio from drifting against video when buffer sizes vary.
	void Mixer::Update(float seconds)
	{
		// A zero or negative delta (paused game, clock adjustment) would otherwise run
		// the fade countdown backwards or spin nothing at all.
		if (seconds <= 0.0f)
			return;

		// Non-const: Update retires voices inline by mutating VoiceId and Clip.
		for (Voice &voice : m_Voices)
		{
			// Skip retired slots without touching them, so their Clip stays null.
			if (!voice.IsPlaying())
				continue;

			// Only voices actually fading have a countdown. Everything else skips the block,
			// which is the common case and keeps this loop cheap.
			if (voice.FadeRemaining > 0.0f)
			{
				// Linear fade. The perceived shape is not exponential, so a long fade sounds
				// mechanical -- acceptable for a UI stop, audible on a gunshot.
				voice.FadeRemaining -= seconds;
				// Reaching zero retires the voice here rather than waiting for the next call,
				// so a fade ends promptly instead of up to one frame late.
				if (voice.FadeRemaining <= 0.0f)
				{
					// Fade complete: retire the voice.
					// Explicit, so a reused slot cannot inherit a stale countdown.
					voice.FadeRemaining = 0.0f;
					voice.VoiceId = -1;
					// Dropped so the slot is reusable and the clip can unload.
					voice.Clip = nullptr;
					// Skips the AdvanceVoice below: a just-finished fade must not also advance the
					// position, or the clip would lose its last fraction of a second of tail.
					continue;
				}
			}

			// Belt-and-braces: a retired-but-not-yet-reset voice. Skipping avoids the null
			// dereference in the frames computation two lines below.
			if (!voice.Clip)
				continue;

			// Per-voice sample rate, not a mixer-wide one: a 44.1 kHz clip and a 22 kHz clip
			// advance by different frame counts for the same elapsed time.
			double frames = (double)seconds * (double)voice.Clip->GetSampleRate();
			// Returns false once a non-looping voice passes the end of its clip.
			if (!AdvanceVoice(voice, frames))
			{
				voice.VoiceId = -1;
				voice.Clip = nullptr;
			}
		}
	}

	// Accumulate-then-clamp. MixVoice ADDS into the output; it never overwrites, so
	// several mixers or several voices can sum into one buffer.
	// ---- Mixing ----

	// Writes ONE voice's contribution. Accumulation into `output` is the caller's
	// contract; this function never reads what it writes.
	void Mixer::MixVoice(Voice &voice, float *output, int frameCount, float masterGain)
	{
		// Same guard as AdvanceVoice: Mix can be called with a different voice set than
		// the last Update, because the two are driven independently.
		if (!voice.IsPlaying() || !voice.Clip)
			return;

		// Local copy of the Ref. Cheap (one refcount), and it keeps the clip alive even
		// if another thread retires the voice mid-mix.
		Ref<AudioClip> clip = voice.Clip;
		// A reference to the clip's storage, not a copy. The whole point of the device-
		// free design is that a mix touches the existing samples in place.
		const std::vector<float> &samples = clip->GetSamples();
		// Frame count in double, matching FramePosition's precision. Using the vector
		// size here rather than GetFrameCount() keeps both reads consistent.
		const double total = (double)samples.size();
		// Guards the read loop below. Play rejects empty clips, so this is defensive.
		if (total <= 0.0)
			return;

		// Per voice per call. The listener's basis is cached, but the dot product is
		// still per voice -- this is the hot path of the whole mixer.
		SpatialGains gains = ComputeSpatial(voice.Position, voice.Spatial,
											voice.MinDistance, voice.MaxDistance);

		// The three scalar factors. Master is applied here rather than per sample so the
		// multiply happens once per voice, not once per sample.
		float baseVolume = voice.Volume * masterGain * gains.Attenuation;
		// A voice that has faded to nothing should cost nothing to skip.
		// Linear fade multiplier: remaining/total. Guarded on Duration > 0 so a voice
		// with a remaining count but no duration cannot divide by zero.
		if (voice.FadeRemaining > 0.0f && voice.FadeDuration > 0.0f)
			// Note this DISABLES the attenuation, not multiplies it: the mixer's volume knob
			// stays independent of a stop-fade, which is why a UI fade does not fight the
			// master volume.
			baseVolume *= (voice.FadeRemaining / voice.FadeDuration);

		// Early-out before the sample loop. A fully faded or silent voice costs one
		// compare rather than frameCount iterations.
		if (baseVolume <= 0.0f)
			return;

		// Per-side gains resolved once, so the inner loop is two multiply-adds.
		const float leftGain = baseVolume * gains.Left;
		// Same, for the right channel. Constant across the whole buffer for this voice.
		const float rightGain = baseVolume * gains.Right;

		// Fractional read position. A pitch of 1.05 lands between samples; a
		// device asks for more frames than a clip has, and the voice must
		// simply run out rather than read past the end.
		// Fractional, so a pitch of 1.05 lands between samples and interpolates.
		double readPosition = voice.FramePosition;
		// Frames per output frame. Constant, so the loop needs no multiply here.
		const double increment = (double)voice.Pitch;

		// A negative pitch was clamped to 1 at Play, so increment is always
		// positive and the loop below terminates.

		// One iteration per output frame, not per sample: the loop writes both channels
		// at once from the single interpolated value below.
		for (int frame = 0; frame < frameCount; ++frame)
		{
			// The device asked for more frames than the clip has left. Two ways out, and the
			// choice depends on whether this voice loops.
			if (readPosition >= total)
			{
				if (voice.Loop)
				{
					// One wrap. A pitch high enough to need several wraps is handled by the check
					// immediately below rather than looping here.
					readPosition -= total;
					// DEFECT (harmless in practice): this only triggers if `increment` exceeds the
					// clip length in one frame -- i.e. Pitch > frame count. The condition can never
					// be true after a single subtraction of `total` unless total itself is tiny.
					// The break is a safe backstop against a spin, not a reachable path.
					if (readPosition >= total)
						break;  // pathological pitch; do not spin
				}
				else
				{
					// Non-looping voice ran out of samples. The remainder of the buffer stays as-is
					// (whatever other voices wrote), which is correct for an accumulating mixer.
					break;
				}
			}

			// Truncation to the integer sample index. Safe because readPosition >= 0 is
			// guaranteed: it starts at a non-negative FramePosition and only grows.
			size_t index = (size_t)readPosition;
			// DEFECT (latent): the bounds check is redundant given the wrap above, and it
			// returns 0.0f rather than the last sample. For a looping voice whose last
			// sample is non-zero this would click -- except the branch above is only taken
			// when index == samples.size(), which the wrap prevents.
			float sample = index < samples.size() ? samples[index] : 0.0f;

			// Linear interpolation between this sample and the next. Without
			// it, any pitch other than 1.0 produces aliasing that is very
			// audible on a sustained sound.
			// The neighbour for interpolation. Computed once, then bounds-checked.
			size_t next = index + 1;
			// Default for the out-of-range case: the signal is treated as ending at zero,
			// which gives a short fade-out on the final sample rather than a held tone.
			float nextSample = 0.0f;
			// The ordinary case: the next sample exists and is used verbatim.
			if (next < samples.size())
				nextSample = samples[next];
			// A looping voice wraps its interpolation neighbour to sample 0, so the last
			// sample interpolates smoothly into the first instead of clicking.
			else if (voice.Loop)
				// The wrap point. This is the single line that makes seamless looping work.
				nextSample = samples[0];

			// The fractional part of the position, in [0,1). Kept in float because it feeds
			// a float lerp and the precision beyond that is inaudible.
			float fraction = (float)(readPosition - (double)index);
			// Linear interpolation. Cheaper than a windowed sinc and, at these rates,
			// indistinguishable for most content.
			float value = sample + (nextSample - sample) * fraction;

			// Interleaved stereo: 2 floats per frame. The cast guards against a negative
			// frameCount, which the caller's `frameCount <= 0` check already excludes.
			float *out = output + (size_t)frame * 2;
			// += , never =. Two voices writing the same frame must SUM, or the second
			// would silently erase the first.
			out[0] += value * leftGain;
			// Right channel, same accumulation. For a 2D voice both gains are 1.0.
			out[1] += value * rightGain;

			// Advance for the next output frame. Ends the loop body; nothing below uses the
			// incremented value except the next iteration.
			readPosition += increment;
		}

		// Commit the position actually consumed, not the requested frame count:
		// a voice that ran out of samples must not skip ahead on the next call.
		// Commits the position ACTUALLY CONSUMED, including any partial advance before a
		// break. Assigning the requested frameCount instead would skip audio whenever a
		// voice ran out mid-buffer.
		voice.FramePosition = readPosition;
	}

	// The public mixing entry point. Fills frameCount interleaved stereo frames by
	// summing every live voice into `output`.
	void Mixer::Mix(float *output, int frameCount)
	{
		// Null-tolerant because `output` comes from a script binding or a device callback,
		// neither of which guarantees a valid buffer.
		if (!output || frameCount <= 0)
			return;

		// Read once into a local, so the value is stable across all voices even if a
		// setter runs mid-mix.
		const float masterGain = m_MasterVolume;
		// Whole-mixer early-out. Not merely an optimisation: it means muting costs
		// nothing, since no voice is even visited.
		if (masterGain <= 0.0f)
			return;

		for (Voice &voice : m_Voices)
		{
			// Skips retired slots, which are the majority of the vector in a steady state.
			if (!voice.IsPlaying())
				continue;
			// Summed, not assigned. The order of voices affects float rounding only, not
			// audibly -- and nothing here sorts by priority.
			MixVoice(voice, output, frameCount, masterGain);
		}

		// Clamp once, at the end. Clamping per voice inside MixVoice would cost
		// a compare per sample per voice and changes the result: a voice
		// cancelled by a quieter one would still have been clipped.
		const size_t sampleCount = (size_t)frameCount * 2;
		for (size_t i = 0; i < sampleCount; ++i)
		{
			// High clamp first. Order matters: checking the negative bound first would add
			// a compare on the common non-clipping path.
			if (output[i] > 1.0f)
				// Hard clamp, not a soft knee. A sum that exceeds 1.0 is distortion, and the
				// alternative -- letting it wrap -- is far worse.
				output[i] = 1.0f;
			else if (output[i] < -1.0f)
				output[i] = -1.0f;
		}
	}

}
