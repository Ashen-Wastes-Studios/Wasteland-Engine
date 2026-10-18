#pragma once

#include "Wasteland/Avb/AudioBus.h"
#include "Wasteland/Avb/AvbDsp.h"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Wasteland::Audio
{
	class AudioClip;
}

namespace Wasteland::Avb
{
	// Bus effects are configured while rendering is stopped. Their Process and
	// Reset methods are audio-thread safe: no locks, allocations, or exceptions.
	class SubmixDelay final : public IAudioBusEffect
	{
	public:
		explicit SubmixDelay(float delaySeconds = 0.25f, float feedback = 0.25f,
		                     float wet = 0.35f, float maxDelaySeconds = 2.0f);
		void SetParameters(float delaySeconds, float feedback, float dry = 1.0f,
		                   float wet = 0.35f) noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		Dsp::StereoDelay m_Delay;
	};

	class Compressor final : public IAudioBusEffect
	{
	public:
		Compressor();
		void SetParameters(float thresholdDb, float ratio, float kneeDb = 0.0f,
		                   float attackSeconds = 0.01f, float releaseSeconds = 0.1f,
		                   float lookaheadSeconds = 0.0f,
		                   Dsp::DetectorMode mode = Dsp::DetectorMode::Peak) noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		std::array<Dsp::Compressor, 2> m_Compressors;
	};

	class Eq final : public IAudioBusEffect
	{
	public:
		Eq();
		void SetParameters(float lowFrequencyHz, float lowGainDb,
		                   float midFrequencyHz, float midGainDb,
		                   float highFrequencyHz, float highGainDb,
		                   float midQ = 0.70710678f) noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		std::array<Dsp::Biquad, 2> m_LowShelf;
		std::array<Dsp::Biquad, 2> m_Mid;
		std::array<Dsp::Biquad, 2> m_HighShelf;
	};

	enum class MultibandBand : std::uint8_t { Low, Mid, High };

	// Three-band crossover/compressor: the bands are split at the configured
	// frequencies, compressed with stereo-linked detectors, then recombined.
	class MultibandCompressor final : public IAudioBusEffect
	{
	public:
		MultibandCompressor();
		void SetCrossovers(float lowHz, float highHz) noexcept;
		void SetBandParameters(MultibandBand band, float thresholdDb, float ratio,
		                       float attackSeconds = 0.01f,
		                       float releaseSeconds = 0.1f,
		                       float kneeDb = 0.0f) noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		// Each array stores two cascaded Butterworth sections per stereo channel:
		// indices [0,1] are left-channel stages; [2,3] are right-channel stages.
		std::array<Dsp::Biquad, 4> m_LowPass;
		std::array<Dsp::Biquad, 4> m_MidHighPass;
		std::array<Dsp::Biquad, 4> m_MidLowPass;
		std::array<Dsp::Biquad, 4> m_HighPass;
		// Apply the upper LR4 all-pass sum to the low band so all three branches
		// share the same phase reference when recombined.
		std::array<Dsp::Biquad, 4> m_LowAllpassLowPass;
		std::array<Dsp::Biquad, 4> m_LowAllpassHighPass;
		std::array<Dsp::Compressor, 3> m_Compressors;
	};

	// A four-line Hadamard-feedback delay network. This is deliberately a
	// separate, always-available space effect; an absent IR never silently
	// changes a ConvolutionReverb into this effect.
	class FdnReverb final : public IAudioBusEffect
	{
	public:
		FdnReverb();
		void SetParameters(float decaySeconds = 1.5f, float dampingHz = 8000.0f,
		                   float wet = 0.35f, float dry = 1.0f) noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		static constexpr std::size_t kLineCount = 4;
		std::array<std::vector<float>, kLineCount> m_Lines;
		std::array<std::size_t, kLineCount> m_WriteIndices{};
		std::array<float, kLineCount> m_Feedback{};
		std::array<float, kLineCount> m_DampingState{};
		float m_DampingCoefficient = 0.0f;
		float m_Wet = 0.35f;
		float m_Dry = 1.0f;
	};

	// Uniform partitioned overlap-add convolution with a fixed 256-frame
	// partition at AVB's 48 kHz rate (one partition of latency). IR data is
	// copied/resampled during setup, so the source AudioClip need not outlive
	// SetImpulseResponse(). A null, invalid, or empty IR is exact pass-through.
	class ConvolutionReverb final : public IAudioBusEffect
	{
	public:
		explicit ConvolutionReverb(const Wasteland::Audio::AudioClip* impulseResponse = nullptr,
		                           float wet = 1.0f, float dry = 0.0f);
		void SetImpulseResponse(const Wasteland::Audio::AudioClip* impulseResponse);
		void SetImpulseResponse(const float* monoSamples, std::size_t sampleCount,
		                       std::uint32_t sampleRate);
		void SetMix(float wet, float dry) noexcept;
		bool HasImpulseResponse() const noexcept { return m_HasImpulseResponse; }
		std::size_t LatencyFrames() const noexcept;
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		static constexpr std::size_t kPartitionFrames = 256;
		static constexpr std::size_t kFftFrames = kPartitionFrames * 2;
		static constexpr std::size_t kMaxImpulseFrames = kAvbAudioSampleRate * 4;

		void BuildImpulse(const std::vector<float>& monoImpulse);
		void ProcessCompletedPartition() noexcept;
		void ProcessChannelPartition(std::size_t channel) noexcept;
		void Transform(std::complex<float>* values, bool inverse) noexcept;

		std::vector<std::complex<float>> m_ImpulseSpectra;
		std::vector<std::complex<float>> m_InputSpectra;
		std::vector<std::complex<float>> m_Twiddles;
		std::array<std::vector<float>, 2> m_InputBlocks;
		std::array<std::vector<float>, 2> m_OutputBlocks;
		std::array<std::vector<float>, 2> m_Overlap;
		std::array<std::vector<std::complex<float>>, 2> m_Work;
		std::array<std::vector<std::complex<float>>, 2> m_Sums;
		std::size_t m_PartitionCount = 0;
		std::size_t m_HistoryWrite = 0;
		std::size_t m_BlockFill = 0;
		std::size_t m_OutputRead = 0;
		bool m_OutputAvailable = false;
		bool m_HasImpulseResponse = false;
		float m_Wet = 1.0f;
		float m_Dry = 0.0f;
	};

	// Default bus reverb: use partitioned convolution when a valid impulse
	// response is configured; otherwise use the built-in FDN space. The lower
	// level ConvolutionReverb remains an exact pass-through for a missing IR.
	class BusReverb final : public IAudioBusEffect
	{
	public:
		explicit BusReverb(const Wasteland::Audio::AudioClip* impulseResponse = nullptr);
		void SetImpulseResponse(const Wasteland::Audio::AudioClip* impulseResponse);
		void SetFdnParameters(float decaySeconds = 1.5f, float dampingHz = 8000.0f,
		                      float wet = 0.35f, float dry = 1.0f) noexcept;
		void SetConvolutionMix(float wet, float dry) noexcept;
		bool UsesConvolution() const noexcept { return m_UseConvolution; }
		void Process(float* interleavedStereo, std::size_t frames) noexcept override;
		void Reset() noexcept override;

	private:
		FdnReverb m_Fdn;
		ConvolutionReverb m_Convolution;
		bool m_UseConvolution = false;
	};
}
