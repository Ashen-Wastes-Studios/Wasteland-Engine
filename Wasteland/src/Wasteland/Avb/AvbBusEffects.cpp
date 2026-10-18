#include "wlpch.h"

#include "Wasteland/Avb/AvbBusEffects.h"
#include "Wasteland/Audio/AudioClip.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace Wasteland::Avb
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kMaxInternalSample = 1000000.0f;

		float FiniteClamp(float value, float low, float high, float fallback) noexcept
		{
			return std::clamp(std::isfinite(value) ? value : fallback, low, high);
		}

		float SafeSample(float value) noexcept
		{
			return FiniteClamp(value, -kMaxInternalSample, kMaxInternalSample, 0.0f);
		}

		constexpr float kCrossoverQ = 0.70710678f;

		Dsp::Biquad MakeBiquad(Dsp::BiquadType type, float frequency, float q = 0.70710678f)
		{
			return Dsp::Biquad(kAvbAudioSampleRate, type, frequency, q);
		}

		std::array<Dsp::Biquad, 4> MakeLinkwitzRiley4(Dsp::BiquadType type, float frequency)
		{
			return {MakeBiquad(type, frequency, kCrossoverQ), MakeBiquad(type, frequency, kCrossoverQ),
			        MakeBiquad(type, frequency, kCrossoverQ), MakeBiquad(type, frequency, kCrossoverQ)};
		}

		void ConfigureLinkwitzRiley4(std::array<Dsp::Biquad, 4>& sections,
		                             Dsp::BiquadType type, float frequency) noexcept
		{
			for (Dsp::Biquad& section : sections)
				section.Configure(type, frequency, kCrossoverQ);
		}

		float ProcessLinkwitzRiley4(std::array<Dsp::Biquad, 4>& sections,
		                            std::size_t channel, float input) noexcept
		{
			const std::size_t firstSection = channel * 2;
			return sections[firstSection + 1].ProcessSample(sections[firstSection].ProcessSample(input));
		}
	}

	SubmixDelay::SubmixDelay(float delaySeconds, float feedback, float wet, float maxDelaySeconds)
		: m_Delay(kAvbAudioSampleRate, maxDelaySeconds)
	{
		SetParameters(delaySeconds, feedback, 1.0f - FiniteClamp(wet, 0.0f, 1.0f, 0.35f), wet);
	}

	void SubmixDelay::SetParameters(float delaySeconds, float feedback, float dry, float wet) noexcept
	{
		m_Delay.SetDelaySeconds(delaySeconds, delaySeconds);
		m_Delay.SetFeedback(feedback);
		m_Delay.SetMix(FiniteClamp(dry, -4.0f, 4.0f, 1.0f),
		               FiniteClamp(wet, -4.0f, 4.0f, 0.35f));
	}

	void SubmixDelay::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (interleavedStereo)
			m_Delay.Process(interleavedStereo, interleavedStereo, frames);
	}

	void SubmixDelay::Reset() noexcept
	{
		m_Delay.Reset();
	}

	Compressor::Compressor()
		: m_Compressors{Dsp::Compressor(kAvbAudioSampleRate), Dsp::Compressor(kAvbAudioSampleRate)}
	{
	}

	void Compressor::SetParameters(float thresholdDb, float ratio, float kneeDb,
	                               float attackSeconds, float releaseSeconds,
	                               float lookaheadSeconds, Dsp::DetectorMode mode) noexcept
	{
		for (Dsp::Compressor& compressor : m_Compressors)
			compressor.SetParameters(thresholdDb, ratio, kneeDb, attackSeconds,
			                         releaseSeconds, lookaheadSeconds, mode);
	}

	void Compressor::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (!interleavedStereo || frames == 0)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			float& left = interleavedStereo[frame * 2];
			float& right = interleavedStereo[frame * 2 + 1];
			const float inputLeft = SafeSample(left);
			const float inputRight = SafeSample(right);
			const float detector = (std::max)(std::fabs(inputLeft), std::fabs(inputRight));
			float outputLeft = 0.0f;
			float outputRight = 0.0f;
			m_Compressors[0].Process(&inputLeft, &detector, &outputLeft, nullptr, 1);
			m_Compressors[1].Process(&inputRight, &detector, &outputRight, nullptr, 1);
			left = SafeSample(outputLeft);
			right = SafeSample(outputRight);
		}
	}

	void Compressor::Reset() noexcept
	{
		for (Dsp::Compressor& compressor : m_Compressors)
			compressor.Reset();
	}

	Eq::Eq()
		: m_LowShelf{MakeBiquad(Dsp::BiquadType::LowShelf, 250.0f), MakeBiquad(Dsp::BiquadType::LowShelf, 250.0f)},
		  m_Mid{MakeBiquad(Dsp::BiquadType::Peaking, 1200.0f), MakeBiquad(Dsp::BiquadType::Peaking, 1200.0f)},
		  m_HighShelf{MakeBiquad(Dsp::BiquadType::HighShelf, 7000.0f), MakeBiquad(Dsp::BiquadType::HighShelf, 7000.0f)}
	{
	}

	void Eq::SetParameters(float lowFrequencyHz, float lowGainDb,
	                       float midFrequencyHz, float midGainDb,
	                       float highFrequencyHz, float highGainDb, float midQ) noexcept
	{
		for (std::size_t channel = 0; channel < 2; ++channel)
		{
			m_LowShelf[channel].Configure(Dsp::BiquadType::LowShelf, lowFrequencyHz, 0.70710678f, lowGainDb);
			m_Mid[channel].Configure(Dsp::BiquadType::Peaking, midFrequencyHz, midQ, midGainDb);
			m_HighShelf[channel].Configure(Dsp::BiquadType::HighShelf, highFrequencyHz, 0.70710678f, highGainDb);
		}
	}

	void Eq::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (!interleavedStereo)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			for (std::size_t channel = 0; channel < 2; ++channel)
			{
				const std::size_t index = frame * 2 + channel;
				float sample = m_LowShelf[channel].ProcessSample(SafeSample(interleavedStereo[index]));
				sample = m_Mid[channel].ProcessSample(sample);
				interleavedStereo[index] = SafeSample(m_HighShelf[channel].ProcessSample(sample));
			}
		}
	}

	void Eq::Reset() noexcept
	{
		for (std::size_t channel = 0; channel < 2; ++channel)
		{
			m_LowShelf[channel].Reset();
			m_Mid[channel].Reset();
			m_HighShelf[channel].Reset();
		}
	}

	MultibandCompressor::MultibandCompressor()
		: m_LowPass(MakeLinkwitzRiley4(Dsp::BiquadType::LowPass, 250.0f)),
		  m_MidHighPass(MakeLinkwitzRiley4(Dsp::BiquadType::HighPass, 250.0f)),
		  m_MidLowPass(MakeLinkwitzRiley4(Dsp::BiquadType::LowPass, 4000.0f)),
		  m_HighPass(MakeLinkwitzRiley4(Dsp::BiquadType::HighPass, 4000.0f)),
		  m_LowAllpassLowPass(MakeLinkwitzRiley4(Dsp::BiquadType::LowPass, 4000.0f)),
		  m_LowAllpassHighPass(MakeLinkwitzRiley4(Dsp::BiquadType::HighPass, 4000.0f)),
		  m_Compressors{Dsp::Compressor(kAvbAudioSampleRate), Dsp::Compressor(kAvbAudioSampleRate),
		                Dsp::Compressor(kAvbAudioSampleRate)}
	{
		SetBandParameters(MultibandBand::Low, -18.0f, 2.0f);
		SetBandParameters(MultibandBand::Mid, -18.0f, 2.0f);
		SetBandParameters(MultibandBand::High, -18.0f, 2.0f);
	}

	void MultibandCompressor::SetCrossovers(float lowHz, float highHz) noexcept
	{
		lowHz = FiniteClamp(lowHz, 20.0f, 10000.0f, 250.0f);
		highHz = FiniteClamp(highHz, lowHz + 10.0f, 20000.0f, 4000.0f);
		highHz = (std::max)(highHz, lowHz + 10.0f);
		ConfigureLinkwitzRiley4(m_LowPass, Dsp::BiquadType::LowPass, lowHz);
		ConfigureLinkwitzRiley4(m_MidHighPass, Dsp::BiquadType::HighPass, lowHz);
		ConfigureLinkwitzRiley4(m_MidLowPass, Dsp::BiquadType::LowPass, highHz);
		ConfigureLinkwitzRiley4(m_HighPass, Dsp::BiquadType::HighPass, highHz);
		ConfigureLinkwitzRiley4(m_LowAllpassLowPass, Dsp::BiquadType::LowPass, highHz);
		ConfigureLinkwitzRiley4(m_LowAllpassHighPass, Dsp::BiquadType::HighPass, highHz);
	}

	void MultibandCompressor::SetBandParameters(MultibandBand band, float thresholdDb,
	                                             float ratio, float attackSeconds,
	                                             float releaseSeconds, float kneeDb) noexcept
	{
		std::size_t index = 0;
		switch (band)
		{
		case MultibandBand::Low: index = 0; break;
		case MultibandBand::Mid: index = 1; break;
		case MultibandBand::High: index = 2; break;
		default: return;
		}
		m_Compressors[index].SetParameters(thresholdDb, ratio, kneeDb, attackSeconds, releaseSeconds);
	}

	void MultibandCompressor::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (!interleavedStereo || frames == 0)
			return;
		const float silence = 0.0f;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			float low[2];
			float mid[2];
			float high[2];
			for (std::size_t channel = 0; channel < 2; ++channel)
			{
				const float input = SafeSample(interleavedStereo[frame * 2 + channel]);
				const float lowerLow = ProcessLinkwitzRiley4(m_LowPass, channel, input);
				const float lowerHigh = ProcessLinkwitzRiley4(m_MidHighPass, channel, input);
				const float lowAllpassLow = ProcessLinkwitzRiley4(m_LowAllpassLowPass, channel, lowerLow);
				const float lowAllpassHigh = ProcessLinkwitzRiley4(m_LowAllpassHighPass, channel, lowerLow);
				low[channel] = lowAllpassLow + lowAllpassHigh;
				mid[channel] = ProcessLinkwitzRiley4(m_MidLowPass, channel, lowerHigh);
				high[channel] = ProcessLinkwitzRiley4(m_HighPass, channel, lowerHigh);
			}

			float* bands[] = {low, mid, high};
			for (std::size_t band = 0; band < 3; ++band)
			{
				const float detector = (std::max)(std::fabs(bands[band][0]), std::fabs(bands[band][1]));
				float ignoredOutput = 0.0f;
				float gain = 1.0f;
				m_Compressors[band].Process(&silence, &detector, &ignoredOutput, &gain, 1);
				bands[band][0] *= gain;
				bands[band][1] *= gain;
			}
			interleavedStereo[frame * 2] = SafeSample(low[0] + mid[0] + high[0]);
			interleavedStereo[frame * 2 + 1] = SafeSample(low[1] + mid[1] + high[1]);
		}
	}

	void MultibandCompressor::Reset() noexcept
	{
		for (Dsp::Biquad& section : m_LowPass)
			section.Reset();
		for (Dsp::Biquad& section : m_MidHighPass)
			section.Reset();
		for (Dsp::Biquad& section : m_MidLowPass)
			section.Reset();
		for (Dsp::Biquad& section : m_HighPass)
			section.Reset();
		for (Dsp::Biquad& section : m_LowAllpassLowPass)
			section.Reset();
		for (Dsp::Biquad& section : m_LowAllpassHighPass)
			section.Reset();
		for (auto& compressor : m_Compressors)
			compressor.Reset();
	}

	FdnReverb::FdnReverb()
	{
		constexpr std::array<std::size_t, kLineCount> lineFrames = {1423, 1783, 1979, 2099};
		for (std::size_t line = 0; line < kLineCount; ++line)
			m_Lines[line].assign(lineFrames[line], 0.0f);
		SetParameters();
	}

	void FdnReverb::SetParameters(float decaySeconds, float dampingHz, float wet, float dry) noexcept
	{
		decaySeconds = FiniteClamp(decaySeconds, 0.1f, 20.0f, 1.5f);
		dampingHz = FiniteClamp(dampingHz, 0.0f, 20000.0f, 8000.0f);
		m_Wet = FiniteClamp(wet, 0.0f, 4.0f, 0.35f);
		m_Dry = FiniteClamp(dry, 0.0f, 4.0f, 1.0f);
		m_DampingCoefficient = dampingHz <= 0.0f ? 0.0f :
			std::exp(-2.0f * kPi * dampingHz / static_cast<float>(kAvbAudioSampleRate));
		for (std::size_t line = 0; line < kLineCount; ++line)
		{
			const float seconds = static_cast<float>(m_Lines[line].size()) / static_cast<float>(kAvbAudioSampleRate);
			m_Feedback[line] = std::clamp(std::pow(10.0f, -3.0f * seconds / decaySeconds), 0.0f, 0.9995f);
		}
	}

	void FdnReverb::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (!interleavedStereo)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			const float left = SafeSample(interleavedStereo[frame * 2]);
			const float right = SafeSample(interleavedStereo[frame * 2 + 1]);
			float delayed[kLineCount];
			for (std::size_t line = 0; line < kLineCount; ++line)
			{
				const float value = m_Lines[line][m_WriteIndices[line]];
				m_DampingState[line] = SafeSample(m_DampingCoefficient * m_DampingState[line] +
				                                  (1.0f - m_DampingCoefficient) * value);
				delayed[line] = m_DampingState[line];
			}

			const float mixed[kLineCount] = {
				0.5f * (delayed[0] + delayed[1] + delayed[2] + delayed[3]),
				0.5f * (delayed[0] - delayed[1] + delayed[2] - delayed[3]),
				0.5f * (delayed[0] + delayed[1] - delayed[2] - delayed[3]),
				0.5f * (delayed[0] - delayed[1] - delayed[2] + delayed[3]),
			};
			const float monoInput = 0.5f * (left + right);
			const float input[kLineCount] = {monoInput * 0.25f, monoInput * 0.25f,
			                                 monoInput * 0.25f, monoInput * 0.25f};
			for (std::size_t line = 0; line < kLineCount; ++line)
			{
				m_Lines[line][m_WriteIndices[line]] = SafeSample(input[line] + m_Feedback[line] * mixed[line]);
				m_WriteIndices[line] = (m_WriteIndices[line] + 1) % m_Lines[line].size();
			}

			const float wetLeft = 0.5f * (delayed[0] + delayed[1]);
			const float wetRight = 0.5f * (delayed[2] + delayed[3]);
			interleavedStereo[frame * 2] = SafeSample(m_Dry * left + m_Wet * wetLeft);
			interleavedStereo[frame * 2 + 1] = SafeSample(m_Dry * right + m_Wet * wetRight);
		}
	}

	void FdnReverb::Reset() noexcept
	{
		for (auto& line : m_Lines)
			std::fill(line.begin(), line.end(), 0.0f);
		m_WriteIndices.fill(0);
		m_DampingState.fill(0.0f);
	}

	ConvolutionReverb::ConvolutionReverb(const Wasteland::Audio::AudioClip* impulseResponse,
	                                     float wet, float dry)
	{
		SetMix(wet, dry);
		SetImpulseResponse(impulseResponse);
	}

	void ConvolutionReverb::SetImpulseResponse(const Wasteland::Audio::AudioClip* impulseResponse)
	{
		if (!impulseResponse || !impulseResponse->IsValid() || impulseResponse->GetSampleRate() <= 0 ||
		    impulseResponse->GetSamples().empty())
		{
			SetImpulseResponse(nullptr, 0, 0);
			return;
		}
		const auto& samples = impulseResponse->GetSamples();
		SetImpulseResponse(samples.data(), samples.size(), static_cast<std::uint32_t>(impulseResponse->GetSampleRate()));
	}

	void ConvolutionReverb::SetImpulseResponse(const float* monoSamples, std::size_t sampleCount,
	                                          std::uint32_t sampleRate)
	{
		if (!monoSamples || sampleCount == 0 || sampleRate == 0)
		{
			m_HasImpulseResponse = false;
			m_PartitionCount = 0;
			m_BlockFill = 0;
			m_OutputAvailable = false;
			return;
		}

		const double lastSourceFrame = static_cast<double>(sampleCount - 1) *
			static_cast<double>(kAvbAudioSampleRate) / static_cast<double>(sampleRate);
		const double requestedFrames = (std::max)(0.0, std::floor(lastSourceFrame)) + 1.0;
		const std::size_t outputFrames = static_cast<std::size_t>((std::min)(
			static_cast<double>(kMaxImpulseFrames), requestedFrames));
		if (outputFrames == 0)
		{
			m_HasImpulseResponse = false;
			m_PartitionCount = 0;
			return;
		}

		std::vector<float> impulse(outputFrames, 0.0f);
		for (std::size_t frame = 0; frame < outputFrames; ++frame)
		{
			const double sourcePosition = static_cast<double>(frame) * static_cast<double>(sampleRate) /
				static_cast<double>(kAvbAudioSampleRate);
			const std::size_t first = (std::min)(static_cast<std::size_t>(sourcePosition), sampleCount - 1);
			const std::size_t second = (std::min)(first + 1, sampleCount - 1);
			const float fraction = static_cast<float>(sourcePosition - static_cast<double>(first));
			const float a = std::isfinite(monoSamples[first]) ? monoSamples[first] : 0.0f;
			const float b = std::isfinite(monoSamples[second]) ? monoSamples[second] : 0.0f;
			impulse[frame] = SafeSample(a + (b - a) * fraction);
		}
		BuildImpulse(impulse);
	}

	void ConvolutionReverb::BuildImpulse(const std::vector<float>& monoImpulse)
	{
		m_HasImpulseResponse = false;
		const std::size_t partitions = (monoImpulse.size() + kPartitionFrames - 1) / kPartitionFrames;
		m_Twiddles.resize(kFftFrames / 2);
		for (std::size_t index = 0; index < m_Twiddles.size(); ++index)
		{
			const float angle = -2.0f * kPi * static_cast<float>(index) / static_cast<float>(kFftFrames);
			m_Twiddles[index] = std::complex<float>(std::cos(angle), std::sin(angle));
		}

		m_ImpulseSpectra.assign(partitions * kFftFrames, std::complex<float>{0.0f, 0.0f});
		for (std::size_t partition = 0; partition < partitions; ++partition)
		{
			std::vector<std::complex<float>> spectrum(kFftFrames, std::complex<float>{0.0f, 0.0f});
			const std::size_t first = partition * kPartitionFrames;
			const std::size_t count = (std::min)(kPartitionFrames, monoImpulse.size() - first);
			for (std::size_t index = 0; index < count; ++index)
				spectrum[index] = std::complex<float>(monoImpulse[first + index], 0.0f);
			Transform(spectrum.data(), false);
			std::copy(spectrum.begin(), spectrum.end(), m_ImpulseSpectra.begin() + partition * kFftFrames);
		}

		m_InputSpectra.assign(2 * partitions * kFftFrames, std::complex<float>{0.0f, 0.0f});
		for (std::size_t channel = 0; channel < 2; ++channel)
		{
			m_InputBlocks[channel].assign(kPartitionFrames, 0.0f);
			m_OutputBlocks[channel].assign(kPartitionFrames, 0.0f);
			m_Overlap[channel].assign(kPartitionFrames, 0.0f);
			m_Work[channel].assign(kFftFrames, std::complex<float>{0.0f, 0.0f});
			m_Sums[channel].assign(kFftFrames, std::complex<float>{0.0f, 0.0f});
		}
		m_PartitionCount = partitions;
		m_HistoryWrite = 0;
		m_BlockFill = 0;
		m_OutputRead = 0;
		m_OutputAvailable = false;
		m_HasImpulseResponse = true;
	}

	void ConvolutionReverb::SetMix(float wet, float dry) noexcept
	{
		m_Wet = FiniteClamp(wet, 0.0f, 4.0f, 1.0f);
		m_Dry = FiniteClamp(dry, 0.0f, 4.0f, 0.0f);
	}

	std::size_t ConvolutionReverb::LatencyFrames() const noexcept
	{
		return m_HasImpulseResponse ? kPartitionFrames : 0;
	}

	void ConvolutionReverb::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (!interleavedStereo || frames == 0 || !m_HasImpulseResponse)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			const float inputLeft = SafeSample(interleavedStereo[frame * 2]);
			const float inputRight = SafeSample(interleavedStereo[frame * 2 + 1]);
			float wetLeft = 0.0f;
			float wetRight = 0.0f;
			if (m_OutputAvailable && m_OutputRead < kPartitionFrames)
			{
				wetLeft = m_OutputBlocks[0][m_OutputRead];
				wetRight = m_OutputBlocks[1][m_OutputRead];
				++m_OutputRead;
				if (m_OutputRead == kPartitionFrames)
					m_OutputAvailable = false;
			}

			interleavedStereo[frame * 2] = SafeSample(m_Dry * inputLeft + m_Wet * wetLeft);
			interleavedStereo[frame * 2 + 1] = SafeSample(m_Dry * inputRight + m_Wet * wetRight);
			m_InputBlocks[0][m_BlockFill] = inputLeft;
			m_InputBlocks[1][m_BlockFill] = inputRight;
			++m_BlockFill;
			if (m_BlockFill == kPartitionFrames)
				ProcessCompletedPartition();
		}
	}

	void ConvolutionReverb::ProcessCompletedPartition() noexcept
	{
		ProcessChannelPartition(0);
		ProcessChannelPartition(1);
		m_HistoryWrite = (m_HistoryWrite + 1) % m_PartitionCount;
		m_BlockFill = 0;
		m_OutputRead = 0;
		m_OutputAvailable = true;
	}

	void ConvolutionReverb::ProcessChannelPartition(std::size_t channel) noexcept
	{
		auto& work = m_Work[channel];
		std::fill(work.begin(), work.end(), std::complex<float>{0.0f, 0.0f});
		for (std::size_t index = 0; index < kPartitionFrames; ++index)
			work[index] = std::complex<float>(m_InputBlocks[channel][index], 0.0f);
		Transform(work.data(), false);

		const std::size_t historyBase = (channel * m_PartitionCount + m_HistoryWrite) * kFftFrames;
		std::copy(work.begin(), work.end(), m_InputSpectra.begin() + historyBase);
		auto& sum = m_Sums[channel];
		std::fill(sum.begin(), sum.end(), std::complex<float>{0.0f, 0.0f});
		for (std::size_t partition = 0; partition < m_PartitionCount; ++partition)
		{
			const std::size_t historyIndex = (m_HistoryWrite + m_PartitionCount - partition) % m_PartitionCount;
			const std::size_t inputBase = (channel * m_PartitionCount + historyIndex) * kFftFrames;
			const std::size_t impulseBase = partition * kFftFrames;
			for (std::size_t bin = 0; bin < kFftFrames; ++bin)
				sum[bin] += m_ImpulseSpectra[impulseBase + bin] * m_InputSpectra[inputBase + bin];
		}
		Transform(sum.data(), true);
		for (std::size_t index = 0; index < kPartitionFrames; ++index)
		{
			m_OutputBlocks[channel][index] = SafeSample(m_Overlap[channel][index] + sum[index].real());
			m_Overlap[channel][index] = SafeSample(sum[index + kPartitionFrames].real());
		}
	}

	void ConvolutionReverb::Transform(std::complex<float>* values, bool inverse) noexcept
	{
		for (std::size_t index = 1, reversed = 0; index < kFftFrames; ++index)
		{
			std::size_t bit = kFftFrames >> 1;
			for (; reversed & bit; bit >>= 1)
				reversed ^= bit;
			reversed ^= bit;
			if (index < reversed)
				std::swap(values[index], values[reversed]);
		}

		for (std::size_t length = 2; length <= kFftFrames; length <<= 1)
		{
			const std::size_t half = length / 2;
			const std::size_t twiddleStep = kFftFrames / length;
			for (std::size_t start = 0; start < kFftFrames; start += length)
			{
				for (std::size_t offset = 0; offset < half; ++offset)
				{
					std::complex<float> twiddle = m_Twiddles[offset * twiddleStep];
					if (inverse)
						twiddle = std::conj(twiddle);
					const std::complex<float> even = values[start + offset];
					const std::complex<float> odd = values[start + offset + half] * twiddle;
					values[start + offset] = even + odd;
					values[start + offset + half] = even - odd;
				}
			}
		}
		if (inverse)
		{
			const float scale = 1.0f / static_cast<float>(kFftFrames);
			for (std::size_t index = 0; index < kFftFrames; ++index)
				values[index] *= scale;
		}
	}

	void ConvolutionReverb::Reset() noexcept
	{
		for (auto& block : m_InputBlocks)
			std::fill(block.begin(), block.end(), 0.0f);
		for (auto& block : m_OutputBlocks)
			std::fill(block.begin(), block.end(), 0.0f);
		for (auto& overlap : m_Overlap)
			std::fill(overlap.begin(), overlap.end(), 0.0f);
		std::fill(m_InputSpectra.begin(), m_InputSpectra.end(), std::complex<float>{0.0f, 0.0f});
		for (auto& work : m_Work)
			std::fill(work.begin(), work.end(), std::complex<float>{0.0f, 0.0f});
		for (auto& sum : m_Sums)
			std::fill(sum.begin(), sum.end(), std::complex<float>{0.0f, 0.0f});
		m_HistoryWrite = 0;
		m_BlockFill = 0;
		m_OutputRead = 0;
		m_OutputAvailable = false;
	}

	BusReverb::BusReverb(const Wasteland::Audio::AudioClip* impulseResponse)
		: m_Convolution(impulseResponse)
	{
		m_UseConvolution = m_Convolution.HasImpulseResponse();
	}

	void BusReverb::SetImpulseResponse(const Wasteland::Audio::AudioClip* impulseResponse)
	{
		m_Convolution.SetImpulseResponse(impulseResponse);
		m_UseConvolution = m_Convolution.HasImpulseResponse();
		m_Fdn.Reset();
	}

	void BusReverb::SetFdnParameters(float decaySeconds, float dampingHz,
	                                 float wet, float dry) noexcept
	{
		m_Fdn.SetParameters(decaySeconds, dampingHz, wet, dry);
	}

	void BusReverb::SetConvolutionMix(float wet, float dry) noexcept
	{
		m_Convolution.SetMix(wet, dry);
	}

	void BusReverb::Process(float* interleavedStereo, std::size_t frames) noexcept
	{
		if (m_UseConvolution)
			m_Convolution.Process(interleavedStereo, frames);
		else
			m_Fdn.Process(interleavedStereo, frames);
	}

	void BusReverb::Reset() noexcept
	{
		m_Fdn.Reset();
		m_Convolution.Reset();
	}
}
