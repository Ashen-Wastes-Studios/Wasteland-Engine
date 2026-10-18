#include "wlpch.h"

#include "Wasteland/Avb/AvbVoices.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Wasteland::Avb::Dsp
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kTwoPi = 2.0f * kPi;
		constexpr float kLn1000 = 6.907755278982137f;
		constexpr float kMaxInternal = 1000000.0f;
		constexpr std::uint32_t kDefaultSeed = 0xA341316Cu;

		std::uint32_t ValidateRate(std::uint32_t sampleRate)
		{
			if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate)
				throw std::invalid_argument("AVB voice sample rate must be in [8000, 384000] Hz");
			return sampleRate;
		}

		float FiniteOr(float value, float fallback = 0.0f) noexcept
		{
			return std::isfinite(value) ? value : fallback;
		}

		float ClampFinite(float value, float low, float high, float fallback) noexcept
		{
			return std::clamp(FiniteOr(value, fallback), low, high);
		}

		float SafeAudio(float value, float limit = kMaxInternal) noexcept
		{
			return std::clamp(FiniteOr(value), -limit, limit);
		}

		std::uint32_t MillisecondsToFrames(float milliseconds, std::uint32_t sampleRate,
		                                   float maximumMs) noexcept
		{
			const float safeMs = ClampFinite(milliseconds, 0.0f, maximumMs, 0.0f);
			const double frames = static_cast<double>(safeMs) * sampleRate * 0.001;
			return static_cast<std::uint32_t>((std::max)(1.0, std::round(frames)));
		}

		float WrapPosition(float position, float size) noexcept
		{
			if (!(size > 0.0f) || !std::isfinite(position))
				return 0.0f;
			position = std::fmod(position, size);
			return position < 0.0f ? position + size : position;
		}

		std::size_t DelayFrames(std::uint32_t sampleRate, float seconds)
		{
			const float safeSeconds = ClampFinite(seconds, 0.001f, 10.0f, 1.0f);
			const double requested = std::ceil(static_cast<double>(sampleRate) * safeSeconds) + 2.0;
			return static_cast<std::size_t>((std::max)(3.0, requested));
		}

		std::array<std::vector<float>, kMaxCombCount> MakeCombLines(std::uint32_t sampleRate,
		                                                             float maxDelaySeconds)
		{
			std::array<std::vector<float>, kMaxCombCount> lines;
			const std::size_t frames = DelayFrames(sampleRate, maxDelaySeconds);
			for (auto& line : lines)
				line.assign(frames, 0.0f);
			return lines;
		}

		std::array<OnePole, kMaxCombCount> MakeCombFilters(std::uint32_t sampleRate)
		{
			return {
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f),
				OnePole(sampleRate, OnePoleType::LowPass, 6500.0f)
			};
		}
	}

	ModalStrike::ModalStrike(std::uint32_t sampleRate)
		: m_SampleRate(ValidateRate(sampleRate)),
		  m_ExcitationNoise(sampleRate, kDefaultSeed),
		  m_ExcitationHighPass(sampleRate, OnePoleType::HighPass, 80.0f)
	{
		SetParameters(ModalStrikeParameters{});
	}

	void ModalStrike::SetParameters(const ModalStrikeParameters& parameters) noexcept
	{
		m_Parameters = parameters;
		m_Parameters.F1Hz = ClampFinite(parameters.F1Hz, 10.0f,
		                                 static_cast<float>(m_SampleRate) * 0.45f, 420.0f);
		m_Parameters.ModeCount = static_cast<std::uint32_t>(std::clamp<std::size_t>(
			parameters.ModeCount, 4, kMaxModalModes));
		m_Parameters.T60Seconds = ClampFinite(parameters.T60Seconds, 0.01f, 20.0f, 0.9f);
		m_Parameters.Damping = ClampFinite(parameters.Damping, 0.0f, 0.99f, 0.35f);
		m_Parameters.ExciteLengthMs = ClampFinite(parameters.ExciteLengthMs, 0.1f, 20.0f, 1.2f);
		m_Parameters.ExciteTiltDbPerOct = ClampFinite(parameters.ExciteTiltDbPerOct, 0.0f, 12.0f, 4.5f);
		m_Parameters.StrikePosition = ClampFinite(parameters.StrikePosition, 0.0f, 1.0f, 0.5f);
		m_Parameters.PitchDriftCents = ClampFinite(parameters.PitchDriftCents, 0.0f, 8.0f, 4.0f);
		m_Parameters.Amplitude = ClampFinite(parameters.Amplitude, 0.0f, 8.0f, 1.0f);
		m_ModeCount = m_Parameters.ModeCount;
		m_ExcitationFrames = MillisecondsToFrames(m_Parameters.ExciteLengthMs, m_SampleRate, 20.0f);
		m_ExcitationTilt = (std::pow(10.0f, m_Parameters.ExciteTiltDbPerOct / 20.0f) - 1.0f) /
		                   (std::pow(10.0f, m_Parameters.ExciteTiltDbPerOct / 20.0f) + 1.0f);

		const float fundamentalRate = kLn1000 / m_Parameters.T60Seconds;
		const float damping = m_Parameters.Damping;
		const float d0 = fundamentalRate * (1.0f - damping);
		const float k = fundamentalRate * damping /
		                (m_Parameters.F1Hz * m_Parameters.F1Hz);
		const ModalStrikeParameters defaults{};
		for (std::size_t index = 0; index < kMaxModalModes; ++index)
		{
			float ratio = parameters.Ratios[index];
			if (!(std::isfinite(ratio) && ratio > 0.0f))
				ratio = defaults.Ratios[index];
			ratio = ClampFinite(ratio, 0.1f, 1000.0f, defaults.Ratios[index]);
			m_Parameters.Ratios[index] = ratio;

			ModeState& mode = m_Modes[index];
			const float rawFrequency = m_Parameters.F1Hz * ratio;
			mode.FrequencyHz = (std::min)(rawFrequency, static_cast<float>(m_SampleRate) * 0.49f);
			const float decayRate = d0 + k * rawFrequency * rawFrequency;
			mode.T60Seconds = kLn1000 / (std::max)(decayRate, 1.0e-6f);
			mode.Decay = std::exp(-kLn1000 /
			                      (mode.T60Seconds * static_cast<float>(m_SampleRate)));
			const float modeNumber = static_cast<float>(index + 1);
			const float strikeGain = std::fabs(std::sin(kPi * modeNumber * m_Parameters.StrikePosition));
			mode.Gain = rawFrequency < static_cast<float>(m_SampleRate) * 0.49f
				? strikeGain / std::sqrt(ratio) * m_Parameters.Amplitude
				: 0.0f;
			const double t60Frames = static_cast<double>(mode.T60Seconds) * m_SampleRate;
			mode.T60Frames = static_cast<std::uint32_t>((std::clamp)(
				std::round(t60Frames), 1.0, static_cast<double>(m_SampleRate) * 20.0));
		}
	}

	void ModalStrike::Trigger(std::uint32_t seed) noexcept
	{
		m_ExcitationNoise.Reset(seed);
		m_ExcitationHighPass.Reset();
		m_PreviousExcitation = 0.0f;
		m_ExcitationFrame = 0;
		for (std::size_t index = 0; index < m_ModeCount; ++index)
		{
			ModeState& mode = m_Modes[index];
			mode.Amplitude = 0.0f;
			mode.AgeFrames = 0;
			mode.Phase = m_ExcitationNoise.ProcessSample() * kPi;
		}
		m_Active = true;
	}

	void ModalStrike::Reset() noexcept
	{
		m_ExcitationNoise.Reset(kDefaultSeed);
		m_ExcitationHighPass.Reset();
		m_PreviousExcitation = 0.0f;
		m_ExcitationFrame = 0;
		m_Active = false;
		for (ModeState& mode : m_Modes)
		{
			mode.Amplitude = 0.0f;
			mode.AgeFrames = 0;
			mode.Phase = 0.0f;
		}
	}

	float ModalStrike::ProcessSample() noexcept
	{
		if (!m_Active)
			return 0.0f;

		float excitation = 0.0f;
		if (m_ExcitationFrame < m_ExcitationFrames)
		{
			const float noise = m_ExcitationHighPass.ProcessSample(m_ExcitationNoise.ProcessSample());
			excitation = noise + m_ExcitationTilt * (noise - m_PreviousExcitation);
			m_PreviousExcitation = noise;
			++m_ExcitationFrame;
		}

		float output = 0.0f;
		bool anyModeAlive = false;
		const float excitationScale = 0.18f / std::sqrt(static_cast<float>(m_ExcitationFrames));
		for (std::size_t index = 0; index < m_ModeCount; ++index)
		{
			ModeState& mode = m_Modes[index];
			if (mode.AgeFrames >= mode.T60Frames)
			{
				mode.Amplitude = 0.0f;
				continue;
			}

			if (m_ExcitationFrame > 0 && m_ExcitationFrame <= m_ExcitationFrames &&
			    mode.Gain > 0.0f && m_ExcitationFrame > mode.AgeFrames)
				mode.Amplitude += excitation * excitationScale * mode.Gain;

			mode.Amplitude *= mode.Decay;
			const bool topMode = index >= (m_ModeCount * 3) / 5;
			float frequency = mode.FrequencyHz;
			if (topMode && m_Parameters.PitchDriftCents > 0.0f)
			{
				const float driftStart = 0.70f * static_cast<float>(mode.T60Frames);
				const float driftSpan = (std::max)(1.0f, 0.30f * static_cast<float>(mode.T60Frames));
				const float drift = std::clamp((static_cast<float>(mode.AgeFrames) - driftStart) / driftSpan,
				                               0.0f, 1.0f);
				frequency *= std::pow(2.0f, -m_Parameters.PitchDriftCents * drift / 1200.0f);
			}
			output += mode.Amplitude * std::sin(mode.Phase);
			mode.Phase += kTwoPi * frequency / static_cast<float>(m_SampleRate);
			if (mode.Phase >= kTwoPi)
				mode.Phase -= kTwoPi;
			if (++mode.AgeFrames < mode.T60Frames)
				anyModeAlive = true;
		}

		if (m_ExcitationFrame >= m_ExcitationFrames && !anyModeAlive)
			m_Active = false;
		return SafeAudio(output, 16.0f);
	}

	void ModalStrike::Process(float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
			output[frame] = ProcessSample();
	}

	float ModalStrike::ModeFrequencyHz(std::size_t mode) const noexcept
	{
		return mode < m_ModeCount ? m_Modes[mode].FrequencyHz : 0.0f;
	}

	float ModalStrike::ModeT60Seconds(std::size_t mode) const noexcept
	{
		return mode < m_ModeCount ? m_Modes[mode].T60Seconds : 0.0f;
	}

	float ModalStrike::ModeExcitationGain(std::size_t mode) const noexcept
	{
		return mode < m_ModeCount ? m_Modes[mode].Gain : 0.0f;
	}

	CombBank::CombBank(std::uint32_t sampleRate, float maxDelaySeconds)
		: m_SampleRate(ValidateRate(sampleRate)),
		  m_MaxDelaySeconds(ClampFinite(maxDelaySeconds, 0.001f, 10.0f, 1.0f)),
		  m_DelayLines(MakeCombLines(m_SampleRate, m_MaxDelaySeconds)),
		  m_LoopFilters(MakeCombFilters(m_SampleRate))
	{
		SetParameters(CombBankParameters{});
		Reset();
	}

	void CombBank::SetParameters(const CombBankParameters& parameters) noexcept
	{
		m_CombCount = static_cast<std::uint32_t>(std::clamp<std::size_t>(parameters.CombCount, 1, kMaxCombCount));
		m_Feedback = ClampFinite(parameters.Feedback, -0.99f, 0.99f, 0.93f);
		m_OutputGain = ClampFinite(parameters.OutputGain, -8.0f, 8.0f, 0.35f);
		const float fundamental = ClampFinite(parameters.FundamentalHz, 1.0f,
		                                       static_cast<float>(m_SampleRate) * 0.45f, 310.0f);
		const float cutoff = ClampFinite(parameters.LoopLpHz, 10.0f,
		                                 static_cast<float>(m_SampleRate) * 0.45f, 6500.0f);
		const float maximumDelay = static_cast<float>(m_DelayLines[0].size() - 2);
		for (std::size_t index = 0; index < kMaxCombCount; ++index)
		{
			float ratio = parameters.Ratios[index];
			if (!(std::isfinite(ratio) && ratio > 0.0f))
				ratio = 1.0f;
			ratio = ClampFinite(ratio, 0.1f, 100.0f, 1.0f);
			CombState& comb = m_Combs[index];
			comb.DelaySamples = std::clamp(static_cast<float>(m_SampleRate) / (fundamental * ratio),
			                               1.0f, maximumDelay);
			const float fractional = comb.DelaySamples - std::floor(comb.DelaySamples);
			comb.AllpassCoefficient = (1.0f - fractional) / (1.0f + fractional);
			m_LoopFilters[index].SetCutoff(cutoff);
		}
	}

	void CombBank::Reset() noexcept
	{
		for (auto& line : m_DelayLines)
			std::fill(line.begin(), line.end(), 0.0f);
		for (auto& filter : m_LoopFilters)
			filter.Reset();
		for (CombState& comb : m_Combs)
		{
			comb.PreviousAllpassInput = 0.0f;
			comb.PreviousAllpassOutput = 0.0f;
		}
		m_WriteIndex = 0;
	}

	float CombBank::ProcessSample(float input) noexcept
	{
		float stage = SafeAudio(input);
		for (std::size_t index = 0; index < m_CombCount; ++index)
		{
			auto& line = m_DelayLines[index];
			CombState& comb = m_Combs[index];
			const std::size_t integerDelay = static_cast<std::size_t>((std::max)(
				1.0f, std::floor(comb.DelaySamples)));
			const std::size_t readIndex = (m_WriteIndex + line.size() - integerDelay) % line.size();
			const float delayed = line[readIndex];
			const float allpass = comb.AllpassCoefficient * delayed + comb.PreviousAllpassInput -
			                      comb.AllpassCoefficient * comb.PreviousAllpassOutput;
			comb.PreviousAllpassInput = delayed;
			comb.PreviousAllpassOutput = SafeAudio(allpass, 100.0f);
			const float loop = m_LoopFilters[index].ProcessSample(comb.PreviousAllpassOutput);
			stage = SafeAudio(stage + m_Feedback * loop, 64.0f);
			line[m_WriteIndex] = stage;
		}
		m_WriteIndex = (m_WriteIndex + 1) % m_DelayLines[0].size();
		return SafeAudio(stage * m_OutputGain, 16.0f);
	}

	void CombBank::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
			output[frame] = ProcessSample(input ? input[frame] : 0.0f);
	}

	Resonator::Resonator(std::uint32_t sampleRate, float frequencyHz, float bandwidthHz, float stretch)
		: m_SampleRate(ValidateRate(sampleRate))
	{
		SetParameters(frequencyHz, bandwidthHz, stretch);
	}

	void Resonator::SetParameters(float frequencyHz, float bandwidthHz, float stretch) noexcept
	{
		m_FrequencyHz = ClampFinite(frequencyHz, 1.0f,
		                           static_cast<float>(m_SampleRate) * 0.45f, 440.0f);
		m_BandwidthHz = ClampFinite(bandwidthHz, 0.1f,
		                            static_cast<float>(m_SampleRate) * 0.45f, 30.0f);
		m_Stretch = ClampFinite(stretch, 1.0f, 1.15f, 1.0f);
		const float frequency = (std::min)(m_FrequencyHz * m_Stretch,
		                                   static_cast<float>(m_SampleRate) * 0.49f);
		m_Radius = std::exp(-kPi * m_BandwidthHz / static_cast<float>(m_SampleRate));
		m_Cosine = std::cos(kTwoPi * frequency / static_cast<float>(m_SampleRate));
	}

	void Resonator::Reset() noexcept
	{
		m_Previous1 = 0.0f;
		m_Previous2 = 0.0f;
	}

	float Resonator::ProcessSample(float input) noexcept
	{
		const float sample = SafeAudio(input);
		float output = sample + 2.0f * m_Radius * m_Cosine * m_Previous1 -
		               m_Radius * m_Radius * m_Previous2;
		if (!std::isfinite(output) || std::fabs(output) > 100000.0f)
		{
			Reset();
			return 0.0f;
		}
		m_Previous2 = m_Previous1;
		m_Previous1 = output;
		return output;
	}

	void Resonator::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
			output[frame] = ProcessSample(input ? input[frame] : 0.0f);
	}

	GrainDelay::GrainDelay(std::uint32_t sampleRate, float maxDelaySeconds, std::uint32_t seed)
		: m_SampleRate(ValidateRate(sampleRate)),
		  m_MaxDelaySeconds(ClampFinite(maxDelaySeconds, 0.001f, 10.0f, 2.0f)),
		  m_Buffer(DelayFrames(m_SampleRate, m_MaxDelaySeconds), 0.0f),
		  m_Random(m_SampleRate, seed)
	{
	}

	void GrainDelay::SetMaxGrainCount(std::uint32_t count) noexcept
	{
		m_MaxGrainCount = static_cast<std::uint32_t>(std::clamp<std::size_t>(count, 1, kMaxGrainCount));
		for (std::size_t index = m_MaxGrainCount; index < m_Grains.size(); ++index)
		{
			if (m_Grains[index].Active)
				--m_ActiveGrainCount;
			m_Grains[index].Active = false;
		}
	}

	void GrainDelay::SetFeedback(float feedback) noexcept
	{
		m_Feedback = ClampFinite(feedback, 0.0f, 0.95f, 0.0f);
	}

	void GrainDelay::Reset(std::uint32_t seed) noexcept
	{
		std::fill(m_Buffer.begin(), m_Buffer.end(), 0.0f);
		for (Grain& grain : m_Grains)
			grain = Grain{};
		m_Random.Reset(seed);
		m_WriteIndex = 0;
		m_ActiveGrainCount = 0;
		m_PreviousWet = 0.0f;
	}

	bool GrainDelay::SpawnGrain(float delayMs, float durationMs, float pitchShiftSemitones,
	                            GrainEnvelope envelope, float randomStartJitterMs) noexcept
	{
		if (m_ActiveGrainCount >= m_MaxGrainCount)
			return false;
		Grain* slot = nullptr;
		for (std::size_t index = 0; index < m_MaxGrainCount; ++index)
		{
			if (!m_Grains[index].Active)
			{
				slot = &m_Grains[index];
				break;
			}
		}
		if (!slot)
			return false;

		const float maximumMs = m_MaxDelaySeconds * 1000.0f;
		const float jitter = ClampFinite(randomStartJitterMs, 0.0f, maximumMs, 0.0f);
		float delay = ClampFinite(delayMs, 0.0f, maximumMs, 0.0f);
		if (jitter > 0.0f)
			delay += m_Random.ProcessSample() * jitter;
		delay = ClampFinite(delay, 0.0f, maximumMs, 0.0f);
		const float duration = ClampFinite(durationMs, 0.001f, 10000.0f, 1.0f);
		const float semitones = ClampFinite(pitchShiftSemitones, -48.0f, 48.0f, 0.0f);
		const float delayFrames = delay * 0.001f * static_cast<float>(m_SampleRate);
		slot->ReadPosition = WrapPosition(static_cast<float>(m_WriteIndex) - delayFrames,
		                                  static_cast<float>(m_Buffer.size()));
		slot->ReadIncrement = std::pow(2.0f, semitones / 12.0f);
		slot->Age = 0.0f;
		slot->DurationFrames = (std::max)(1.0f, duration * 0.001f * static_cast<float>(m_SampleRate));
		slot->Envelope = envelope;
		slot->Active = true;
		++m_ActiveGrainCount;
		return true;
	}

	float GrainDelay::ReadAt(float position) const noexcept
	{
		const float size = static_cast<float>(m_Buffer.size());
		position = WrapPosition(position, size);
		const std::size_t index0 = static_cast<std::size_t>(position);
		const std::size_t index1 = (index0 + 1) % m_Buffer.size();
		const float fraction = position - static_cast<float>(index0);
		return m_Buffer[index0] + (m_Buffer[index1] - m_Buffer[index0]) * fraction;
	}

	float GrainDelay::ProcessSample(float input) noexcept
	{
		const float dry = SafeAudio(input);
		m_Buffer[m_WriteIndex] = SafeAudio(dry + m_Feedback * m_PreviousWet, 64.0f);
		float wet = 0.0f;
		for (std::size_t index = 0; index < m_MaxGrainCount; ++index)
		{
			Grain& grain = m_Grains[index];
			if (!grain.Active)
				continue;
			const float phase = grain.Age / grain.DurationFrames;
			wet += ReadAt(grain.ReadPosition) * EnvelopeValue(grain.Envelope, phase);
			grain.ReadPosition = WrapPosition(grain.ReadPosition + grain.ReadIncrement,
			                                  static_cast<float>(m_Buffer.size()));
			grain.Age += 1.0f;
			if (grain.Age >= grain.DurationFrames)
			{
				grain.Active = false;
				--m_ActiveGrainCount;
			}
		}
		m_WriteIndex = (m_WriteIndex + 1) % m_Buffer.size();
		m_PreviousWet = SafeAudio(wet, 16.0f);
		return SafeAudio(dry + wet, 64.0f);
	}

	void GrainDelay::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
			output[frame] = ProcessSample(input ? input[frame] : 0.0f);
	}

	float GrainDelay::EnvelopeValue(GrainEnvelope envelope, float phase) noexcept
	{
		const float t = ClampFinite(phase, 0.0f, 1.0f, 0.0f);
		switch (envelope)
		{
		case GrainEnvelope::Gaussian:
		{
			const float raw = std::exp(-12.0f * (t - 0.5f) * (t - 0.5f));
			const float edge = std::exp(-3.0f);
			return std::clamp((raw - edge) / (1.0f - edge), 0.0f, 1.0f);
		}
		case GrainEnvelope::Triangle:
			return 1.0f - std::fabs(2.0f * t - 1.0f);
		case GrainEnvelope::Down:
			return 1.0f - t;
		case GrainEnvelope::Up:
			return t;
		case GrainEnvelope::ExpDecay:
			return (std::exp(-5.0f * t) - std::exp(-5.0f)) / (1.0f - std::exp(-5.0f));
		case GrainEnvelope::ExpAttack:
			return (1.0f - std::exp(-5.0f * t)) / (1.0f - std::exp(-5.0f));
		}
		return 0.0f;
	}

	WavePlayer::WavePlayer(std::uint32_t sampleRate) : m_SampleRate(ValidateRate(sampleRate))
	{
	}

	void WavePlayer::SetSource(const float* monoSamples, std::size_t frames) noexcept
	{
		SetSource(monoSamples, frames, m_SampleRate);
	}

	void WavePlayer::SetSource(const float* monoSamples, std::size_t frames,
	                           std::uint32_t sourceSampleRate) noexcept
	{
		m_Samples = monoSamples;
		m_FrameCount = monoSamples ? frames : 0;
		m_SourceSampleRate = sourceSampleRate > 0 ? sourceSampleRate : m_SampleRate;
		m_Playhead = 0.0;
		m_Playing = false;
		m_NearlySent = false;
		m_LoopStart = 0.0;
		m_LoopEnd = static_cast<double>(m_FrameCount);
		SetPitchSemitones(m_PitchSemitones);
	}

	void WavePlayer::SetLoop(bool enabled) noexcept
	{
		m_Loop = enabled && m_LoopEnd - m_LoopStart >= 1.0;
	}

	void WavePlayer::SetPitchSemitones(float semitones) noexcept
	{
		m_PitchSemitones = ClampFinite(semitones, -48.0f, 48.0f, 0.0f);
		const std::uint32_t sourceRate = m_SourceSampleRate > 0 ? m_SourceSampleRate : m_SampleRate;
		m_Step = static_cast<double>(sourceRate) / m_SampleRate *
		         std::pow(2.0, static_cast<double>(m_PitchSemitones) / 12.0);
	}

	void WavePlayer::SetLoopRegion(float startSeconds, float durationSeconds) noexcept
	{
		if (m_FrameCount == 0)
		{
			m_LoopStart = m_LoopEnd = 0.0;
			return;
		}
		const double total = static_cast<double>(m_FrameCount);
		const std::uint32_t sourceRate = m_SourceSampleRate > 0 ? m_SourceSampleRate : m_SampleRate;
		const double start = std::clamp(static_cast<double>(FiniteOr(startSeconds)) * sourceRate, 0.0, total);
		const double duration = FiniteOr(durationSeconds, -1.0f);
		const double end = duration < 0.0f
			? total
			: std::clamp(start + static_cast<double>(duration) * sourceRate, start, total);
		m_LoopStart = start;
		m_LoopEnd = end;
		if (m_LoopEnd - m_LoopStart < 1.0)
			m_Loop = false;
	}

	void WavePlayer::SetNearlyFinishedLeadSeconds(float seconds) noexcept
	{
		m_NearlyFinishedLeadSeconds = ClampFinite(seconds, 0.0f, 10.0f, 0.02f);
	}

	void WavePlayer::Stop() noexcept
	{
		m_Playing = false;
		m_NearlySent = false;
	}

	float WavePlayer::SampleAt(double position) const noexcept
	{
		if (!m_Samples || m_FrameCount == 0)
			return 0.0f;
		position = std::clamp(position, 0.0, static_cast<double>(m_FrameCount - 1));
		const std::size_t index0 = static_cast<std::size_t>(position);
		std::size_t index1 = (std::min)(index0 + 1, m_FrameCount - 1);
		if (m_Loop && position >= m_LoopStart && index1 >= static_cast<std::size_t>(m_LoopEnd))
			index1 = static_cast<std::size_t>(m_LoopStart);
		const float fraction = static_cast<float>(position - static_cast<double>(index0));
		const float first = SafeAudio(m_Samples[index0]);
		const float second = SafeAudio(m_Samples[index1]);
		return SafeAudio(first + (second - first) * fraction, 16.0f);
	}

	WavePlayerFrame WavePlayer::ProcessSample(bool playTrigger, bool stopTrigger,
	                                          float startTimeSeconds) noexcept
	{
		WavePlayerFrame frame{};
		if (stopTrigger)
			Stop();
		if (playTrigger)
		{
			const std::uint32_t sourceRate = m_SourceSampleRate > 0 ? m_SourceSampleRate : m_SampleRate;
			const double start = std::clamp(static_cast<double>(FiniteOr(startTimeSeconds)) * sourceRate,
			                                0.0, static_cast<double>(m_FrameCount));
			m_Playhead = m_Loop && m_LoopEnd > m_LoopStart && start >= m_LoopEnd ? m_LoopStart : start;
			m_Playing = m_Samples != nullptr && m_FrameCount > 0 && m_Playhead < m_FrameCount;
			m_NearlySent = false;
		}
		const std::uint32_t sourceRate = m_SourceSampleRate > 0 ? m_SourceSampleRate : m_SampleRate;
		frame.PlaybackLocation = static_cast<float>(m_Playhead / static_cast<double>(sourceRate));
		if (!m_Playing || !m_Samples || m_FrameCount == 0)
			return frame;

		const double end = m_Loop && m_LoopEnd > m_LoopStart
			? m_LoopEnd : static_cast<double>(m_FrameCount);
		const double remaining = (std::max)(0.0, end - m_Playhead);
		// Keep the seconds-to-frames conversion at the configured float precision:
		// N / sampleRate must not drift just below integer frame N in double.
		const double leadFrames = static_cast<double>(
			m_NearlyFinishedLeadSeconds * static_cast<float>(sourceRate));
		if (!m_NearlySent && remaining > 0.0 && remaining <= (std::max)(leadFrames, m_Step))
		{
			frame.OnNearlyFinished = true;
			m_NearlySent = true;
		}
		frame.Sample = SampleAt(m_Playhead);
		m_Playhead += m_Step;
		const double boundaryTolerance = std::numeric_limits<double>::epsilon() *
		                                  (std::max)(1.0, std::fabs(m_Playhead)) * 64.0;
		if (m_Loop && m_LoopEnd > m_LoopStart && m_Playhead >= m_LoopEnd - boundaryTolerance)
		{
			const double duration = m_LoopEnd - m_LoopStart;
			const double overshoot = (std::max)(0.0, m_Playhead - m_LoopEnd);
			m_Playhead = m_LoopStart + std::fmod(overshoot, duration);
			frame.OnLooped = true;
			m_NearlySent = false;
		}
		else if (!m_Loop && m_Playhead >= static_cast<double>(m_FrameCount) - boundaryTolerance)
		{
			m_Playhead = static_cast<double>(m_FrameCount);
			m_Playing = false;
			frame.OnFinished = true;
		}
		return frame;
	}
}
