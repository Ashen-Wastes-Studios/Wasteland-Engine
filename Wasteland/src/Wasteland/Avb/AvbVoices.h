#pragma once

#include "Wasteland/Avb/AvbDsp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Wasteland::Avb::Dsp
{
	inline constexpr std::size_t kMaxModalModes = 24;
	inline constexpr std::size_t kMaxCombCount = 8;
	inline constexpr std::size_t kMaxGrainCount = 100;

	struct ModalStrikeParameters
	{
		float F1Hz = 420.0f;
		std::array<float, kMaxModalModes> Ratios = {
			1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.84f, 32.0f,
			40.12f, 49.20f, 59.24f, 70.24f, 82.20f, 94.11f, 107.0f, 120.9f,
			135.7f, 151.5f, 168.2f, 185.8f, 204.4f, 223.8f, 244.2f, 265.5f
		};
		std::uint32_t ModeCount = 14;
		float T60Seconds = 0.9f;
		// Normalized Rayleigh damping: 0 means equal decay; values near 1
		// shorten upper-mode T60 while preserving the fundamental T60.
		float Damping = 0.35f;
		float ExciteLengthMs = 1.2f;
		float ExciteTiltDbPerOct = 4.5f;
		float StrikePosition = 0.5f;
		float PitchDriftCents = 4.0f;
		float Amplitude = 1.0f;
	};

	// Seeded, allocation-free-per-sample struck-object model. Configure and
	// trigger on the control side; Process() is safe to call on the render thread.
	class ModalStrike
	{
	public:
		explicit ModalStrike(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(const ModalStrikeParameters& parameters) noexcept;
		void Trigger(std::uint32_t seed = 0xA341316Cu) noexcept;
		void Reset() noexcept;
		float ProcessSample() noexcept;
		void Process(float* output, std::size_t frames) noexcept;
		std::uint32_t ModeCount() const noexcept { return m_ModeCount; }
		float ModeFrequencyHz(std::size_t mode) const noexcept;
		float ModeT60Seconds(std::size_t mode) const noexcept;
		float ModeExcitationGain(std::size_t mode) const noexcept;

	private:
		struct ModeState
		{
			float FrequencyHz = 0.0f;
			float T60Seconds = 0.0f;
			float Decay = 0.0f;
			float Gain = 0.0f;
			float Phase = 0.0f;
			float Amplitude = 0.0f;
			std::uint32_t AgeFrames = 0;
			std::uint32_t T60Frames = 0;
		};

		std::uint32_t m_SampleRate;
		ModalStrikeParameters m_Parameters;
		std::array<ModeState, kMaxModalModes> m_Modes{};
		Noise m_ExcitationNoise;
		OnePole m_ExcitationHighPass;
		std::uint32_t m_ModeCount = 14;
		std::uint32_t m_ExcitationFrames = 1;
		std::uint32_t m_ExcitationFrame = 0;
		float m_PreviousExcitation = 0.0f;
		float m_ExcitationTilt = 0.0f;
		bool m_Active = false;
	};

	struct CombBankParameters
	{
		float FundamentalHz = 310.0f;
		std::array<float, kMaxCombCount> Ratios = {
			1.0f, 1.41421356f, 1.73205081f, 2.23606798f,
			2.64575131f, 3.0f, 3.31662479f, 3.60555128f
		};
		std::uint32_t CombCount = 4;
		float Feedback = 0.93f;
		float LoopLpHz = 6500.0f;
		float OutputGain = 0.35f;
	};

	// Serial bank of feedback combs. The buffers are sized in the constructor;
	// SetParameters() and all render methods are allocation-free.
	class CombBank
	{
	public:
		explicit CombBank(std::uint32_t sampleRate, float maxDelaySeconds = 1.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(const CombBankParameters& parameters) noexcept;
		void Reset() noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
		std::uint32_t CombCount() const noexcept { return m_CombCount; }
		float Feedback() const noexcept { return m_Feedback; }

	private:
		struct CombState
		{
			float DelaySamples = 1.0f;
			float AllpassCoefficient = 1.0f;
			float PreviousAllpassInput = 0.0f;
			float PreviousAllpassOutput = 0.0f;
		};

		std::uint32_t m_SampleRate;
		float m_MaxDelaySeconds;
		std::array<std::vector<float>, kMaxCombCount> m_DelayLines;
		std::array<OnePole, kMaxCombCount> m_LoopFilters;
		std::array<CombState, kMaxCombCount> m_Combs{};
		std::uint32_t m_CombCount = 4;
		std::size_t m_WriteIndex = 0;
		float m_Feedback = 0.93f;
		float m_OutputGain = 0.35f;
	};

	// Stable second-order resonator with optional inharmonic frequency stretch.
	class Resonator
	{
	public:
		explicit Resonator(std::uint32_t sampleRate, float frequencyHz = 440.0f,
		                   float bandwidthHz = 30.0f, float stretch = 1.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float frequencyHz, float bandwidthHz, float stretch = 1.0f) noexcept;
		void Reset() noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
		float FrequencyHz() const noexcept { return m_FrequencyHz; }
	private:
		std::uint32_t m_SampleRate;
		float m_FrequencyHz = 440.0f;
		float m_BandwidthHz = 30.0f;
		float m_Stretch = 1.0f;
		float m_Radius = 0.0f;
		float m_Cosine = 1.0f;
		float m_Previous1 = 0.0f;
		float m_Previous2 = 0.0f;
	};

	enum class GrainEnvelope : std::uint8_t
	{
		Gaussian,
		Triangle,
		Down,
		Up,
		ExpDecay,
		ExpAttack
	};

	// Delay-line granular voice. Up to 100 grains live in fixed storage; the
	// circular audio buffer is allocated once in the constructor. Random start
	// jitter comes only from the per-instance seeded PRNG.
	class GrainDelay
	{
	public:
		explicit GrainDelay(std::uint32_t sampleRate, float maxDelaySeconds = 2.0f,
		                    std::uint32_t seed = 0xA341316Cu);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetMaxGrainCount(std::uint32_t count) noexcept;
		std::uint32_t MaxGrainCount() const noexcept { return m_MaxGrainCount; }
		void SetFeedback(float feedback) noexcept;
		float Feedback() const noexcept { return m_Feedback; }
		void Reset(std::uint32_t seed = 0xA341316Cu) noexcept;
		bool SpawnGrain(float delayMs, float durationMs, float pitchShiftSemitones = 0.0f,
		                GrainEnvelope envelope = GrainEnvelope::Gaussian,
		                float randomStartJitterMs = 0.0f) noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
		std::uint32_t ActiveGrainCount() const noexcept { return m_ActiveGrainCount; }
		static float EnvelopeValue(GrainEnvelope envelope, float phase) noexcept;

	private:
		struct Grain
		{
			float ReadPosition = 0.0f;
			float ReadIncrement = 1.0f;
			float Age = 0.0f;
			float DurationFrames = 1.0f;
			GrainEnvelope Envelope = GrainEnvelope::Gaussian;
			bool Active = false;
		};

		float ReadAt(float position) const noexcept;
		std::uint32_t m_SampleRate;
		float m_MaxDelaySeconds;
		std::vector<float> m_Buffer;
		std::array<Grain, kMaxGrainCount> m_Grains{};
		Noise m_Random;
		std::size_t m_WriteIndex = 0;
		std::uint32_t m_ActiveGrainCount = 0;
		std::uint32_t m_MaxGrainCount = 16;
		float m_Feedback = 0.0f;
		float m_PreviousWet = 0.0f;
	};

	struct WavePlayerFrame
	{
		float Sample = 0.0f;
		float PlaybackLocation = 0.0f;
		bool OnFinished = false;
		bool OnNearlyFinished = false;
		bool OnLooped = false;
	};

	// Non-owning mono PCM player. Call ProcessSample with trigger bits at the
	// exact frame where Play/Stop should occur; it interpolates fractional pitch
	// and returns sample-accurate lifecycle events without allocating.
	class WavePlayer
	{
	public:
		explicit WavePlayer(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetSource(const float* monoSamples, std::size_t frames) noexcept;
		void SetSource(const float* monoSamples, std::size_t frames,
		               std::uint32_t sourceSampleRate) noexcept;
		void SetPitchSemitones(float semitones) noexcept;
		float PitchSemitones() const noexcept { return m_PitchSemitones; }
		void SetLoop(bool enabled) noexcept;
		bool Loop() const noexcept { return m_Loop; }
		void SetLoopRegion(float startSeconds, float durationSeconds) noexcept;
		void SetNearlyFinishedLeadSeconds(float seconds) noexcept;
		void Stop() noexcept;
		WavePlayerFrame ProcessSample(bool playTrigger = false, bool stopTrigger = false,
		                              float startTimeSeconds = 0.0f) noexcept;
		bool IsPlaying() const noexcept { return m_Playing; }
	private:
		float SampleAt(double position) const noexcept;
		std::uint32_t m_SampleRate;
		std::uint32_t m_SourceSampleRate = 0;
		const float* m_Samples = nullptr;
		std::size_t m_FrameCount = 0;
		double m_Playhead = 0.0;
		double m_Step = 1.0;
		double m_LoopStart = 0.0;
		double m_LoopEnd = 0.0;
		float m_PitchSemitones = 0.0f;
		float m_NearlyFinishedLeadSeconds = 0.02f;
		bool m_Loop = false;
		bool m_Playing = false;
		bool m_NearlySent = false;
	};
}
