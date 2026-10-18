#pragma once

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "Wasteland/Audio/AudioClip.h"
#include "Wasteland/Core/UUID.h"

namespace Wasteland::Audio
{

	// What a source is asking for when it starts a sound.
	struct PlayRequest
	{
		// Which clip. Either a registered name or a direct handle; Name wins
		// when both are set, so a script can name a clip without loading one.
		std::string ClipName;
		Ref<AudioClip> Clip;

		// NOT clamped here or at the mixer: a Volume above 1.0 is multiplied
		// straight into the samples, so a caller passing 2.0 gets a genuine
		// +6 dB rather than a saturated-at-unity value.
		float Volume = 1.0f;
		// A Pitch of exactly 0 would freeze FramePosition forever and the
		// voice would never retire. Nothing in this header rejects it; the
		// clamp has to live in the .cpp.
		float Pitch = 1.0f;
		bool Loop = false;

		// Higher wins when voices are scarce. The whole point of a voice cap
		// is that a firefight must not silence the UI, and that only works if
		// UI and effects are on different tiers.
		// Higher wins, so the useful range is negative-low through positive-
		// high. Default 0 puts everything in one tier, which means the cap
		// below cannot distinguish a footstep from a gunshot unless the
		// caller opts in -- the default is neutral, not prioritised.
		int Priority = 0;

		// 2D sounds ignore the listener entirely: no attenuation, no panning.
		// Footsteps and UI belong here; anything the player should be able to
		// locate belongs in 3D.
		bool Spatial = false;

		// World position for a 3D sound. Ignored when Spatial is false.
		glm::vec3 Position = {0.0f, 0.0f, 0.0f};

		// Distance window over which a 3D sound fades. MinDistance is the
		// distance at which volume is exactly Volume; beyond MaxDistance it is
		// silent. Equal values mean no falloff, which is the honest way to say
		// "this sound is 3D but omnipresent".
		float MinDistance = 1.0f;
		float MaxDistance = 30.0f;
	};

	// One playing sound. Public so a UI can inspect what is playing, but the
	// mixer owns it: do not construct one and expect it to make a sound.
	struct Voice
	{
		Ref<AudioClip> Clip;
		int VoiceId = -1;

		// Playback position in FRAMES, not samples or seconds. Frames are the
		// only unit that survives a pitch change without accumulating drift.
		// double, not float: this advances by seconds*pitch*sampleRate every
		// tick, so a long clip accumulates far more mantissa pressure than
		// float's ~7 digits can absorb without audible periodic jitter.
		double FramePosition = 0.0;

		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		int Priority = 0;
		bool Spatial = false;
		glm::vec3 Position = {0.0f, 0.0f, 0.0f};
		float MinDistance = 1.0f;
		float MaxDistance = 30.0f;

		// Per-voice fade, used when a source is stopped with a fade rather than
		// cut. Cutting a gunshot dead is audible and cheap-sounding; a short
		// fade is the difference between "effects" and "a game with effects".
		float FadeRemaining = 0.0f;
		float FadeDuration = 0.0f;

		// Cached so a voice that has finished mixing can be identified without
		// comparing whole clips.
		UUID ClipId;

		// The sentinel test. VoiceId is -1 in a retired slot and a positive
		// id in a live one -- m_NextVoiceId starts at 1, so 0 is never
		// handed out and `>= 0` is in practice the same as `> 0`.
		bool IsPlaying() const { return VoiceId >= 0; }
	};

	// A 3D sound's resolved gain and stereo position for the current listener.
	struct SpatialGains
	{
		float Left = 1.0f;
		float Right = 1.0f;
		// Distance attenuation, before the per-side pan. Kept separate so a
		// caller can attenuate without panning (a mono output device, or a
		// test asserting distance falloff in isolation).
		float Attenuation = 1.0f;
	};

	// The mixer. Pure computation, NO device.
	//
	// This separation is the whole design. Everything that decides *what* you
	// hear -- voice allocation, priority, attenuation, panning, looping,
	// fades -- lives here, with no platform dependency at all. The device only
	// ever hands the mixer a buffer to fill. That is what makes the audio
	// system testable with no sound card: a test can mix 512 frames and assert
	// on the samples.
	//
	// No buses, no mixing snapshots, no occlusion, no convolution reverb. Those
	// are deferred until something needs them; an empty DSP graph costs
	// maintenance and buys nothing.
	class Mixer
	{
	public:
		// The listener's transform, updated per frame by the engine from the
		// primary camera.
		struct Listener
		{
			glm::vec3 Position = {0.0f, 0.0f, 0.0f};
			// Default faces -Z, matching the engine's right-handed convention.
			// A listener left at these defaults therefore hears correctly with no
			// setup call at all.
			glm::vec3 Forward = {0.0f, 0.0f, -1.0f};
			glm::vec3 Up = {0.0f, 1.0f, 0.0f};

			// An orthonormal basis derived from Forward/Up, cached because it
			// is used per voice per frame.
			// Derived from Forward/Up by RecomputeBasis and cached, because
			// ComputeSpatial needs it once per spatial voice per frame and the
			// cross product is not free. The cost is that changing Forward or
			// Up without calling RecomputeBasis leaves a STALE basis -- which is
			// why the setter above is a method rather than two public fields.
			glm::vec3 Right = {1.0f, 0.0f, 0.0f};

			// Recompute Right from Forward/Up. Call after changing either.
			void RecomputeBasis();
		};

		// Above this, a new sound steals a voice instead of being dropped.
		// Chosen so a normal scene never hits the cap (so the cap is invisible
		// in practice) while a pathological one cannot allocate without bound.
		// static constexpr, so it is a compile-time constant usable in a
		// constant expression; no out-of-line definition is needed in C++17
		// and later.
		static constexpr int kDefaultMaxVoices = 32;
		// The fixed engine-facing mix rate. Clips retain their own source rates;
		// Mix() converts their read step into these output frames.
		static constexpr int kEngineMixRate = 48000;

		Mixer() = default;
		// The explicit keyword is load-bearing: without it a bare integer
		// would implicitly convert to a Mixer and the two-argument form
		// would be ambiguous with a copy.
		explicit Mixer(int maxVoices);

		// ---- Clip registry ----

		// Register a clip under a name so sources can reference it by string
		// and the same file is not decoded twice.
		void RegisterClip(const std::string &name, const Ref<AudioClip> &clip);
		Ref<AudioClip> GetClip(const std::string &name) const;
		bool HasClip(const std::string &name) const;
		// Legacy reference view: do not retain/read it concurrently with playback
		// mutations. Use GetClipsSnapshot() when the device callback is running.
		const std::unordered_map<std::string, Ref<AudioClip>> &GetClips() const { return m_Clips; }
		std::unordered_map<std::string, Ref<AudioClip>> GetClipsSnapshot() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_Clips;
		}

		// ---- Playback ----

		// Start a sound. Returns the voice id, or -1 if it could not be
		// started.
		//
		// A -1 is a real outcome, not an error to ignore: at the voice cap with
		// everything higher-priority, a footstep SHOULD be dropped. A caller
		// that needs to know should check, and a caller that does not care
		// (most effects) can ignore it safely.
		int Play(const PlayRequest &request);

		// Stop a voice, optionally over a fade. A zero-or-negative fade cuts
		// immediately, which is correct for UI and wrong for a gunshot.
		// fadeSeconds <= 0 means an immediate cut. The header says so, but
		// note the asymmetry with StopAll: a caller passing a NEGATIVE
		// fade is treated the same as zero rather than rejected.
		void Stop(int voiceId, float fadeSeconds = 0.0f);
		void StopAll(float fadeSeconds = 0.0f);

		// Stop every voice whose clip matches. Used when a source is destroyed
		// or a weapon is swapped.
		void StopClip(const AudioClip *clip, float fadeSeconds = 0.0f);

		// Change a playing voice's volume/pitch. Returns false if the voice has
		// already finished, which is normal and not an error.
		bool SetVoiceVolume(int voiceId, float volume);
		bool SetVoicePitch(int voiceId, float pitch);

		// Change volume on every playing voice from a given clip.
		void SetClipVolume(const AudioClip *clip, float volume);

		bool IsPlaying(int voiceId) const;

		// Count of voices ACTUALLY PLAYING, not the size of the slot vector.
		//
		// These differ: a retired slot keeps its memory so the next Play can
		// reuse it without reallocating, so the vector is sized to the
		// loudest moment ever heard. Returning m_Voices.size() reports stale
		// slots as live voices, which makes the voice cap look permanently
		// full and turns a debugging aid into a lie.
		// O(voices) per call rather than a cached counter. At the default cap
		// of 32 that is trivial, and it avoids having to keep a live count
		// correct across steal, retire and fade -- the correctness win is
		// worth more than the scan.
		int GetActiveVoiceCount() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			int count = 0;
			for (const Voice &voice : m_Voices)
			{
				if (voice.IsPlaying())
					count++;
			}
			return count;
		}

		int GetMaxVoices() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_MaxVoices;
		}
		// Clamps the low end only. Lowering the cap below the current active
		// count does NOT retire anything immediately: the excess keeps
		// playing until each voice finishes on its own, and nothing steals
		// until the cap is exceeded again. So SetMaxVoices(1) mid-firefight
		// leaves every gunshot audible to its natural end.
		void SetMaxVoices(int max)
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_MaxVoices = max < 1 ? 1 : max;
		}

		// ---- Mixing ----

		// Legacy direct access is intended for single-threaded/headless use. Prefer
		// the synchronized snapshot/setter while the output callback is active.
		Listener &GetListener() { return m_Listener; }
		const Listener &GetListener() const { return m_Listener; }
		// Thread-safe alternatives for audio systems that update/read the listener
		// while the device callback is mixing.
		Listener GetListenerSnapshot() const;
		void SetListenerTransform(const glm::vec3 &position,
			const glm::vec3 &forward, const glm::vec3 &up);

		// Master volume, applied to everything. 1.0 is unmodified.
		//
		// Clamped to [0, 1] on BOTH ends, deliberately. Unity is the ceiling:
		// master volume is a user-facing attenuation of already-clamped mixed
		// output, so a value above 1 can only add gain the mix cannot use.
		//
		// The upper clamp is load-bearing rather than cosmetic. Mix() hard-clamps
		// every sample to +/-1 at the end of the buffer, so an unclamped
		// SetMasterVolume(50.0f) does not exceed full scale -- it SATURATES,
		// turning any signal already near the ceiling into a square wave that
		// sounds like hard clipping. A mid-level input is affected just as
		// visibly: 0.05 * 50 pins a sample that should sit at 0.05 to 1.0, so a
		// single runaway UI slider or script call destroys dynamics for every
		// sound at once. Clamping here means that value degrades to unity gain
		// instead of to distortion.
		void SetMasterVolume(float volume)
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			if (volume < 0.0f)
				volume = 0.0f;
			else if (volume > 1.0f)
				volume = 1.0f;
			m_MasterVolume = volume;
		}
		float GetMasterVolume() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_MasterVolume;
		}

		// Mix into a stereo interleaved float buffer at kEngineMixRate. Source
		// clips keep their native sample rates and are linearly resampled per
		// voice while reading. The buffer is ADDED to, not overwritten.
		void Mix(float *output, int frameCount);

		// Real-time device path. Mixes at kEngineMixRate and advances fade and
		// retirement state from the frames actually rendered. Do not also call
		// Update() to advance voice positions for the same device frames.
		void MixRealtime(float *output, int frameCount);

		// Advance every voice's position and fade state by `seconds`, retiring
		// finished ones.
		//
		// Separate from Mix because a device may mix a different number of
		// frames than elapsed time accounts for (buffer sizes drift, and a
		// device can underrun). Advancing by the MIXED frame count instead of
		// wall-clock keeps audio and video from drifting apart permanently.
		// Advancing by MIXED frames rather than wall-clock is the whole
		// reason this is separate from Mix: a device that underruns mixes
		// fewer frames than elapsed time implies, and following wall-clock
		// would desynchronise audio from video permanently and silently.
		void Update(float seconds);

		// Resolve a 3D sound's gain and pan for the current listener. Public so
		// it can be tested directly, and so a UI can visualise falloff.
		SpatialGains ComputeSpatial(const glm::vec3 &position, bool spatial,
									float minDistance, float maxDistance) const;

		// Pure distance attenuation, 1 at MinDistance falling to 0 at
		// MaxDistance. Inverse-distance rather than linear because linear
		// sounds like a volume knob turning; inverse sounds like distance.
		// The header documents equal min/max as the no-falloff case, so the
		// implementation must special-case maxDistance <= minDistance before
		// it divides by the width. Nothing at the call site enforces the
		// ordering -- MinDistance defaults to 1 and MaxDistance to 30, but a
		// caller can set them either way round and get a negative or
		// infinite falloff rather than an error.
		static float ComputeAttenuation(float distance, float minDistance, float maxDistance);

		// Voices dropped because the cap was reached. A diagnostic, and the
		// number to watch if audio is ever reported as "not playing".
		int GetDroppedVoiceCount() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_DroppedVoices;
		}
		// Both counters are plain ints that are never reset automatically,
		// so they are lifetime totals for the Mixer object. ResetCounters
		// exists precisely because there is no per-frame rollover.
		int GetStealedVoiceCount() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_StealedVoices;
		}
		void ResetCounters()
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_DroppedVoices = 0;
			m_StealedVoices = 0;
		}

		// Iterate live voices, for a debug view or a script that wants to know
		// what is playing.
		// DEFECT (stale comment): the comment above says "iterate live
		// voices", but this returns m_Voices, which KEEPS retired slots so
		// Play can reuse them without reallocating (that is the reasoning
		// spelled out above GetActiveVoiceCount). So this vector contains
		// dead entries whose VoiceId is -1. A caller that treats every
		// element as playing will act on finished voices; it must test
		// Voice::IsPlaying() on each. Either the accessor should filter, or
		// the comment should say it returns slots.
		// Legacy reference view: do not retain/read it concurrently with playback.
		// A synchronized copy is available for concurrent diagnostics.
		const std::vector<Voice> &GetVoices() const { return m_Voices; }
		std::vector<Voice> GetVoicesSnapshot() const
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			return m_Voices;
		}

	private:
		// Mix one voice into the buffer. `writeGain` is the master/attenuation
		// product applied on top of the voice's own volume.
		void MixVoice(Voice &voice, float *output, int frameCount, float masterGain);

		// Free a voice slot, retiring the finished one if any.
		int AllocateVoiceSlot();

		// Find the lowest-priority, quietest voice to steal. Returns an index
		// into m_Voices, or -1 if nothing may be stolen.
		//
		// Prefers the quietest, not merely the lowest priority: among two
		// equal-priority footsteps, dropping the one already inaudible is
		// better than the one the player can currently hear.
		int FindStealCandidate(int incomingPriority) const;

		// Advance one voice's position, returning false when it has finished.
		bool AdvanceVoice(Voice &voice, double frames);

		std::vector<Voice> m_Voices;
		mutable std::recursive_mutex m_Mutex;
		std::unordered_map<std::string, Ref<AudioClip>> m_Clips;
		Listener m_Listener;
		float m_MasterVolume = 1.0f;
		int m_MaxVoices = kDefaultMaxVoices;
		// Monotonic counter, never reused, so a stale id from a retired
		// voice cannot accidentally address a different one -- which is
		// what makes Stop()/SetVoiceVolume() safe against a late caller.
		int m_NextVoiceId = 1;
		int m_DroppedVoices = 0;
		int m_StealedVoices = 0;
	};

}
