#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Wasteland::Avb::Dsp
{
	// Rate-dependent processors use a fixed integer sample rate in [8 kHz, 384 kHz].
	// WaveShaper, StereoPanner, and StereoMixer are rate-independent. Mono Process()
	// methods consume one float per frame; stereo methods consume interleaved L,R
	// pairs and their frame count remains a count of frames. Process methods allocate
	// no memory. A zero-frame call is a no-op and may receive null pointers; otherwise
	// callers provide buffers of the documented size. Non-finite audio samples are
	// treated as silence at each input boundary.
	inline constexpr std::uint32_t kMinSampleRate = 8000;
	inline constexpr std::uint32_t kMaxSampleRate = 384000;
	inline constexpr float kMaxDelaySeconds = 10.0f;

	enum class BiquadType : std::uint8_t
	{
		LowPass,
		HighPass,
		BandPass,
		Notch,
		Peaking,
		LowShelf,
		HighShelf
	};

	enum class OnePoleType : std::uint8_t { LowPass, HighPass };
	enum class WaveShape : std::uint8_t { Sine, InverseTangent, HyperbolicTangent, Cubic, HardClip };
	enum class DetectorMode : std::uint8_t { Peak, RMS };
	enum class StereoDelayMode : std::uint8_t { Normal, Cross, PingPong };
	enum class PanLaw : std::uint8_t { EqualPower, Linear };
	enum class OscillatorWaveform : std::uint8_t { Sine, Triangle, Saw, Square };
	enum class EnvelopeStage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };

	struct StateVariableOutput
	{
		float Low = 0.0f;
		float Band = 0.0f;
		float High = 0.0f;
		float Notch = 0.0f;
	};

	class Biquad
	{
	public:
		explicit Biquad(std::uint32_t sampleRate, BiquadType type = BiquadType::LowPass,
		               float cutoffHz = 1000.0f, float q = 0.70710678f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void Configure(BiquadType type, float cutoffHz, float q = 0.70710678f, float gainDb = 0.0f) noexcept;
		void Reset() noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;

	private:
		std::uint32_t m_SampleRate;
		BiquadType m_Type;
		float m_CutoffHz;
		float m_Q;
		float m_GainDb = 0.0f;
		float m_B0 = 1.0f, m_B1 = 0.0f, m_B2 = 0.0f;
		float m_A1 = 0.0f, m_A2 = 0.0f;
		float m_Z1 = 0.0f, m_Z2 = 0.0f;
	};

	class StateVariableFilter
	{
	public:
		explicit StateVariableFilter(std::uint32_t sampleRate, float cutoffHz = 1000.0f, float q = 0.70710678f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float cutoffHz, float q) noexcept;
		void Reset() noexcept;
		StateVariableOutput ProcessSample(float input) noexcept;
		void Process(const float* input, StateVariableOutput* output, std::size_t frames) noexcept;

	private:
		std::uint32_t m_SampleRate;
		float m_CutoffHz;
		float m_Q;
		float m_G = 0.0f;
		float m_K = 1.0f;
		float m_Integrator1 = 0.0f;
		float m_Integrator2 = 0.0f;
		void UpdateCoefficients() noexcept;
	};

	class OnePole
	{
	public:
		explicit OnePole(std::uint32_t sampleRate, OnePoleType type = OnePoleType::LowPass, float cutoffHz = 1000.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetCutoff(float cutoffHz) noexcept;
		void Reset() noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;

	private:
		std::uint32_t m_SampleRate;
		OnePoleType m_Type;
		float m_CutoffHz;
		float m_Coefficient = 0.0f;
		float m_PreviousLow = 0.0f;
		void UpdateCoefficient() noexcept;
	};

	class LadderFilter
	{
	public:
		explicit LadderFilter(std::uint32_t sampleRate, float cutoffHz = 1000.0f, float resonance = 0.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float cutoffHz, float resonance) noexcept;
		void Reset() noexcept;
		float ProcessSample(float input) noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;

	private:
		std::uint32_t m_SampleRate;
		float m_CutoffHz;
		float m_Resonance;
		float m_Coefficient = 0.0f;
		float m_Stages[4] = {};
		void UpdateCoefficient() noexcept;
	};

	class WaveShaper
	{
	public:
		void SetParameters(WaveShape shape, float amount = 1.0f, float bias = 0.0f, float outputGain = 1.0f) noexcept;
		float ProcessSample(float input) const noexcept;
		void Process(const float* input, float* output, std::size_t frames) const noexcept;
		float Amount() const noexcept { return m_Amount; }
	private:
		WaveShape m_Shape = WaveShape::HardClip;
		float m_Amount = 1.0f;
		float m_Bias = 0.0f;
		float m_OutputGain = 1.0f;
	};

	class AdEnvelope
	{
	public:
		explicit AdEnvelope(std::uint32_t sampleRate, float attackSeconds = 0.01f, float decaySeconds = 0.1f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float attackSeconds, float decaySeconds, float attackCurve = 1.0f, float decayCurve = 1.0f) noexcept;
		void SetLooping(bool looping) noexcept { m_Looping = looping; }
		bool IsLooping() const noexcept { return m_Looping; }
		void Trigger() noexcept;
		void Reset() noexcept;
		float ProcessSample() noexcept;
		void Process(float* output, std::size_t frames) noexcept;
		float Value() const noexcept { return m_Value; }
	private:
		std::uint32_t m_SampleRate;
		std::uint32_t m_AttackFrames = 1, m_DecayFrames = 1;
		float m_AttackCurve = 1.0f, m_DecayCurve = 1.0f;
		std::uint32_t m_PhaseFrame = 0;
		float m_Value = 0.0f;
		EnvelopeStage m_Stage = EnvelopeStage::Idle;
		bool m_Looping = false;
	};

	class AdsrEnvelope
	{
	public:
		explicit AdsrEnvelope(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float attackSeconds, float decaySeconds, float sustain,
		                   float releaseSeconds, float attackCurve = 1.0f,
		                   float decayCurve = 1.0f, float releaseCurve = 1.0f) noexcept;
		void TriggerAttack() noexcept;
		void TriggerRelease() noexcept;
		void Reset() noexcept;
		float ProcessSample() noexcept;
		void Process(float* output, std::size_t frames) noexcept;
		float Value() const noexcept { return m_Value; }
		EnvelopeStage Stage() const noexcept { return m_Stage; }
	private:
		std::uint32_t m_SampleRate;
		std::uint32_t m_AttackFrames = 1, m_DecayFrames = 1, m_ReleaseFrames = 1;
		float m_Sustain = 0.7f;
		float m_AttackCurve = 1.0f, m_DecayCurve = 1.0f, m_ReleaseCurve = 1.0f;
		std::uint32_t m_PhaseFrame = 0;
		float m_Value = 0.0f;
		float m_ReleaseStart = 0.0f;
		EnvelopeStage m_Stage = EnvelopeStage::Idle;
	};

	class Compressor
	{
	public:
		explicit Compressor(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetParameters(float thresholdDb, float ratio, float kneeDb, float attackSeconds,
		                   float releaseSeconds, float lookaheadSeconds = 0.0f,
		                   DetectorMode mode = DetectorMode::Peak) noexcept;
		void Reset() noexcept;
		// Mono program and optional mono sidechain buffers. nullptr sidechain uses
		// the program input. GainEnvelope, when non-null, receives the applied
		// linear gain per frame. Lookahead is capped at 100 ms and preallocated.
		void Process(const float* input, const float* sidechain, float* output,
		             float* gainEnvelope, std::size_t frames) noexcept;
		float Gain() const noexcept { return m_Gain; }
	private:
		std::uint32_t m_SampleRate;
		float m_ThresholdDb = -18.0f, m_Ratio = 4.0f, m_KneeDb = 0.0f;
		float m_AttackSeconds = 0.01f, m_ReleaseSeconds = 0.1f, m_LookaheadSeconds = 0.0f;
		float m_Detector = 0.0f, m_Gain = 1.0f;
		DetectorMode m_Mode = DetectorMode::Peak;
		std::vector<float> m_Lookahead;
		std::size_t m_LookaheadIndex = 0, m_LookaheadFrames = 0;
	};

	class Limiter
	{
	public:
		explicit Limiter(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		// Mono limiter with a hard output ceiling and at most 100 ms lookahead.
		void SetParameters(float ceiling = 0.98f, float releaseSeconds = 0.05f,
		                   float lookaheadSeconds = 0.0f) noexcept;
		void Reset() noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
		float Ceiling() const noexcept { return m_Ceiling; }
	private:
		std::uint32_t m_SampleRate;
		float m_Ceiling = 0.98f, m_ReleaseSeconds = 0.05f, m_Gain = 1.0f;
		std::vector<float> m_Lookahead;
		std::size_t m_LookaheadIndex = 0, m_LookaheadFrames = 0;
	};

	class Delay
	{
	public:
		// Mono fractional delay; storage is allocated once for at most 10 seconds.
		// Delay time clamps to [0, configured maximum], and feedback to [-0.99, 0.99].
		explicit Delay(std::uint32_t sampleRate, float maxDelaySeconds = 2.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		float MaxDelaySeconds() const noexcept { return m_MaxDelaySeconds; }
		void SetDelaySeconds(float seconds) noexcept;
		void SetFeedback(float feedback) noexcept;
		void SetMix(float dry, float wet) noexcept;
		float Feedback() const noexcept { return m_Feedback; }
		void Reset() noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
	private:
		std::uint32_t m_SampleRate;
		float m_MaxDelaySeconds;
		float m_DelaySamples = 1.0f, m_Feedback = 0.0f, m_Dry = 1.0f, m_Wet = 0.5f;
		std::vector<float> m_Buffer;
		std::size_t m_WriteIndex = 0;
		float ReadDelay() const noexcept;
	};

	class StereoDelay
	{
	public:
		// Interleaved stereo fractional delay; fixed storage is bounded to 10 seconds.
		// Per-channel delays clamp to the configured maximum, feedback to [-0.99, 0.99].
		explicit StereoDelay(std::uint32_t sampleRate, float maxDelaySeconds = 2.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetDelaySeconds(float leftSeconds, float rightSeconds) noexcept;
		void SetFeedback(float feedback) noexcept;
		void SetMix(float dry, float wet) noexcept;
		void SetMode(StereoDelayMode mode) noexcept { m_Mode = mode; }
		void Reset() noexcept;
		// Input and output are exactly two interleaved floats per frame: L,R.
		void Process(const float* interleavedInput, float* interleavedOutput, std::size_t frames) noexcept;
	private:
		std::uint32_t m_SampleRate;
		float m_MaxDelaySeconds;
		float m_DelaySamples[2] = {1.0f, 1.0f};
		float m_Feedback = 0.0f, m_Dry = 1.0f, m_Wet = 0.5f;
		StereoDelayMode m_Mode = StereoDelayMode::Normal;
		std::vector<float> m_LeftBuffer, m_RightBuffer;
		std::size_t m_WriteIndex = 0;
		float ReadDelay(const std::vector<float>& buffer, float delaySamples) const noexcept;
	};

	class StereoPanner
	{
	public:
		// Rate-independent, mono-in to exactly two interleaved output channels.
		void Process(const float* monoInput, float* interleavedStereoOutput, std::size_t frames,
		             float pan, PanLaw law = PanLaw::EqualPower) const noexcept;
		static void Gains(float pan, PanLaw law, float& left, float& right) noexcept;
	};

	class StereoMixer
	{
	public:
		// Rate-independent 2-in/1-out stereo mixer; every buffer is interleaved
		// L,R. Gains are deliberately unclamped and may attenuate or invert.
		void Process(const float* stereoA, const float* stereoB, float* stereoOutput,
		             std::size_t frames, float gainA = 1.0f, float gainB = 1.0f) const noexcept;
	};

	class Lfo
	{
	public:
		explicit Lfo(std::uint32_t sampleRate, float frequencyHz = 1.0f,
		             OscillatorWaveform waveform = OscillatorWaveform::Sine, float phaseRadians = 0.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetFrequency(float frequencyHz) noexcept;
		void SetWaveform(OscillatorWaveform waveform) noexcept { m_Waveform = waveform; }
		void SetPhase(float phaseRadians) noexcept;
		void Reset(float phaseRadians = 0.0f) noexcept;
		float ProcessSample() noexcept;
		void Process(float* output, std::size_t frames) noexcept;
	private:
		std::uint32_t m_SampleRate;
		float m_FrequencyHz = 1.0f, m_Phase = 0.0f;
		OscillatorWaveform m_Waveform = OscillatorWaveform::Sine;
	};

	class Sine
	{
	public:
		explicit Sine(std::uint32_t sampleRate, float frequencyHz = 440.0f, float amplitude = 1.0f);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void SetFrequency(float frequencyHz, float glideSeconds = 0.0f) noexcept;
		void SetAmplitude(float amplitude) noexcept;
		float CurrentFrequency() const noexcept { return m_CurrentFrequency; }
		float TargetFrequency() const noexcept { return m_TargetFrequency; }
		void Reset() noexcept;
		float ProcessSample(bool sync = false) noexcept;
		// Optional sync buffer has one byte per frame; a nonzero byte resets phase
		// at that exact sample before it is rendered.
		void Process(float* output, std::size_t frames, const std::uint8_t* sync = nullptr) noexcept;
	private:
		std::uint32_t m_SampleRate;
		float m_CurrentFrequency, m_TargetFrequency, m_Amplitude;
		float m_Phase = 0.0f, m_GlideStep = 0.0f;
		std::uint32_t m_GlideFramesRemaining = 0;
	};

	class Noise
	{
	public:
		explicit Noise(std::uint32_t sampleRate, std::uint32_t seed = 0xA341316Cu);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		void Reset(std::uint32_t seed) noexcept;
		float ProcessSample() noexcept;
		void Process(float* output, std::size_t frames) noexcept;
	private:
		std::uint32_t m_SampleRate;
		std::uint32_t m_State;
	};

	class BiquadEq
	{
	public:
		explicit BiquadEq(std::uint32_t sampleRate);
		std::uint32_t SampleRate() const noexcept { return m_SampleRate; }
		// Mono three-band EQ: low/high shelves and a peaking mid band.
		void SetParameters(float lowFrequencyHz, float lowGainDb, float midFrequencyHz,
		                   float midGainDb, float highFrequencyHz, float highGainDb,
		                   float midQ = 0.70710678f) noexcept;
		void Reset() noexcept;
		void Process(const float* input, float* output, std::size_t frames) noexcept;
	private:
		std::uint32_t m_SampleRate;
		Biquad m_LowShelf;
		Biquad m_Mid;
		Biquad m_HighShelf;
	};
}
