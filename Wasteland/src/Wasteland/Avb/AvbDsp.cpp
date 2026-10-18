#include "wlpch.h"

#include "Wasteland/Avb/AvbDsp.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace Wasteland::Avb::Dsp
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kTwoPi = 2.0f * kPi;
		constexpr float kMaxInternal = 1000000.0f;
		constexpr std::uint32_t kMaxEnvelopeSeconds = 60;
		constexpr float kMaxLookaheadSeconds = 0.1f;

		void ValidateSampleRate(std::uint32_t sampleRate)
		{
			if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate)
				throw std::invalid_argument("AVB DSP sample rate must be in [8000, 384000] Hz");
		}

		float FiniteOr(float value, float fallback = 0.0f) noexcept
		{
			return std::isfinite(value) ? value : fallback;
		}

		float SafeSample(float value) noexcept
		{
			return std::clamp(FiniteOr(value), -kMaxInternal, kMaxInternal);
		}

		float ClampFinite(float value, float low, float high, float fallback) noexcept
		{
			return std::clamp(FiniteOr(value, fallback), low, high);
		}

		float ClampFrequency(float frequencyHz, std::uint32_t sampleRate) noexcept
		{
			return ClampFinite(frequencyHz, 1.0f, static_cast<float>(sampleRate) * 0.499f, 1000.0f);
		}

		float TimeCoefficient(float seconds, std::uint32_t sampleRate) noexcept
		{
			if (!(seconds > 0.0f))
				return 0.0f;
			return std::exp(-1.0f / (seconds * static_cast<float>(sampleRate)));
		}

		std::uint32_t TimeToFrames(float seconds, std::uint32_t sampleRate, float maxSeconds = static_cast<float>(kMaxEnvelopeSeconds)) noexcept
		{
			const float safeSeconds = ClampFinite(seconds, 0.0f, maxSeconds, 0.0f);
			if (safeSeconds == 0.0f)
				return 0;
			const double frames = static_cast<double>(safeSeconds) * sampleRate;
			return static_cast<std::uint32_t>((std::max)(1.0, std::round(frames)));
		}

		float Curve(float position, float exponent) noexcept
		{
			position = ClampFinite(position, 0.0f, 1.0f, 0.0f);
			exponent = ClampFinite(exponent, 0.1f, 10.0f, 1.0f);
			return std::pow(position, exponent);
		}

		float WrapPhase(float phase) noexcept
		{
			if (!std::isfinite(phase))
				return 0.0f;
			phase = std::fmod(phase, kTwoPi);
			return phase < 0.0f ? phase + kTwoPi : phase;
		}

		std::size_t DelayBufferFrames(std::uint32_t sampleRate, float seconds)
		{
			const float safeSeconds = ClampFinite(seconds, 0.0f, kMaxDelaySeconds, 0.0f);
			const double requested = std::ceil(static_cast<double>(sampleRate) * safeSeconds) + 2.0;
			const double bounded = (std::min)(requested, static_cast<double>(kMaxSampleRate) * kMaxDelaySeconds + 2.0);
			return static_cast<std::size_t>((std::max)(2.0, bounded));
		}

		float ReadRing(const std::vector<float>& buffer, std::size_t writeIndex, float delaySamples) noexcept
		{
			if (buffer.empty())
				return 0.0f;
			const float maxDelay = static_cast<float>(buffer.size() - 2);
			delaySamples = ClampFinite(delaySamples, 1.0f, (std::max)(1.0f, maxDelay), 1.0f);
			const float size = static_cast<float>(buffer.size());
			float position = static_cast<float>(writeIndex) - delaySamples;
			while (position < 0.0f)
				position += size;
			while (position >= size)
				position -= size;
			const auto index0 = static_cast<std::size_t>(position);
			const auto index1 = (index0 + 1) % buffer.size();
			const float fraction = position - static_cast<float>(index0);
			return buffer[index0] + (buffer[index1] - buffer[index0]) * fraction;
		}

		float CompressorTargetGain(float detector, float thresholdDb, float ratio, float kneeDb) noexcept
		{
			const float levelDb = 20.0f * std::log10((std::max)(detector, 1.0e-9f));
			const float overDb = levelDb - thresholdDb;
			float reductionDb = 0.0f;
			if (kneeDb > 0.0f && overDb > -0.5f * kneeDb && overDb < 0.5f * kneeDb)
			{
				const float x = overDb + 0.5f * kneeDb;
				reductionDb = (1.0f - 1.0f / ratio) * x * x / (2.0f * kneeDb);
			}
			else if (overDb >= 0.5f * kneeDb)
			{
				reductionDb = overDb * (1.0f - 1.0f / ratio);
			}
			return std::clamp(std::pow(10.0f, -reductionDb / 20.0f), 0.0f, 1.0f);
		}

		float OscillatorValue(OscillatorWaveform waveform, float phase) noexcept
		{
			switch (waveform)
			{
			case OscillatorWaveform::Sine:
				return std::sin(phase);
			case OscillatorWaveform::Triangle:
				return (2.0f / kPi) * std::asin(std::sin(phase));
			case OscillatorWaveform::Saw:
				return phase / kPi - 1.0f;
			case OscillatorWaveform::Square:
				return phase < kPi ? 1.0f : -1.0f;
			}
			return std::sin(phase);
		}
	}

	Biquad::Biquad(std::uint32_t sampleRate, BiquadType type, float cutoffHz, float q)
		: m_SampleRate(sampleRate), m_Type(type), m_CutoffHz(cutoffHz), m_Q(q)
	{
		ValidateSampleRate(sampleRate);
		Configure(type, cutoffHz, q);
	}

	void Biquad::Configure(BiquadType type, float cutoffHz, float q, float gainDb) noexcept
	{
		m_Type = type;
		m_CutoffHz = ClampFrequency(cutoffHz, m_SampleRate);
		m_Q = ClampFinite(q, 0.05f, 20.0f, 0.70710678f);
		m_GainDb = ClampFinite(gainDb, -60.0f, 24.0f, 0.0f);

		const float omega = kTwoPi * m_CutoffHz / static_cast<float>(m_SampleRate);
		const float cosine = std::cos(omega);
		const float sine = std::sin(omega);
		const float alpha = sine / (2.0f * m_Q);
		float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

		switch (m_Type)
		{
		case BiquadType::LowPass:
			b0 = (1.0f - cosine) * 0.5f; b1 = 1.0f - cosine; b2 = b0;
			a0 = 1.0f + alpha; a1 = -2.0f * cosine; a2 = 1.0f - alpha;
			break;
		case BiquadType::HighPass:
			b0 = (1.0f + cosine) * 0.5f; b1 = -(1.0f + cosine); b2 = b0;
			a0 = 1.0f + alpha; a1 = -2.0f * cosine; a2 = 1.0f - alpha;
			break;
		case BiquadType::BandPass:
			b0 = alpha; b1 = 0.0f; b2 = -alpha;
			a0 = 1.0f + alpha; a1 = -2.0f * cosine; a2 = 1.0f - alpha;
			break;
		case BiquadType::Notch:
			b0 = 1.0f; b1 = -2.0f * cosine; b2 = 1.0f;
			a0 = 1.0f + alpha; a1 = -2.0f * cosine; a2 = 1.0f - alpha;
			break;
		case BiquadType::Peaking:
		{
			const float A = std::pow(10.0f, m_GainDb / 40.0f);
			b0 = 1.0f + alpha * A; b1 = -2.0f * cosine; b2 = 1.0f - alpha * A;
			a0 = 1.0f + alpha / A; a1 = -2.0f * cosine; a2 = 1.0f - alpha / A;
			break;
		}
		case BiquadType::LowShelf:
		case BiquadType::HighShelf:
		{
			const float A = std::pow(10.0f, m_GainDb / 40.0f);
			const float rootA = std::sqrt(A);
			// RBJ shelf slope S=1: alpha = sin(w0)/2 * sqrt(2).
			const float shelfAlpha = (sine * 0.5f) * std::sqrt(2.0f);
			const float beta = 2.0f * rootA * shelfAlpha;
			if (m_Type == BiquadType::LowShelf)
			{
				b0 = A * ((A + 1.0f) - (A - 1.0f) * cosine + beta);
				b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosine);
				b2 = A * ((A + 1.0f) - (A - 1.0f) * cosine - beta);
				a0 = (A + 1.0f) + (A - 1.0f) * cosine + beta;
				a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosine);
				a2 = (A + 1.0f) + (A - 1.0f) * cosine - beta;
			}
			else
			{
				b0 = A * ((A + 1.0f) + (A - 1.0f) * cosine + beta);
				b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosine);
				b2 = A * ((A + 1.0f) + (A - 1.0f) * cosine - beta);
				a0 = (A + 1.0f) - (A - 1.0f) * cosine + beta;
				a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosine);
				a2 = (A + 1.0f) - (A - 1.0f) * cosine - beta;
			}
			break;
		}
		}

		if (!std::isfinite(a0) || std::fabs(a0) < 1.0e-12f)
		{
			m_B0 = 1.0f; m_B1 = m_B2 = m_A1 = m_A2 = 0.0f;
			return;
		}
		const float inverseA0 = 1.0f / a0;
		m_B0 = b0 * inverseA0;
		m_B1 = b1 * inverseA0;
		m_B2 = b2 * inverseA0;
		m_A1 = a1 * inverseA0;
		m_A2 = a2 * inverseA0;
	}

	void Biquad::Reset() noexcept
	{
		m_Z1 = m_Z2 = 0.0f;
	}

	float Biquad::ProcessSample(float input) noexcept
	{
		const float x = SafeSample(input);
		float output = m_B0 * x + m_Z1;
		if (!std::isfinite(output))
		{
			Reset();
			return 0.0f;
		}
		output = std::clamp(output, -kMaxInternal, kMaxInternal);
		m_Z1 = m_B1 * x - m_A1 * output + m_Z2;
		m_Z2 = m_B2 * x - m_A2 * output;
		if (!std::isfinite(m_Z1) || !std::isfinite(m_Z2) || std::fabs(m_Z1) > kMaxInternal || std::fabs(m_Z2) > kMaxInternal)
			Reset();
		return output;
	}

	void Biquad::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(input ? input[i] : 0.0f);
	}

	StateVariableFilter::StateVariableFilter(std::uint32_t sampleRate, float cutoffHz, float q)
		: m_SampleRate(sampleRate), m_CutoffHz(cutoffHz), m_Q(q)
	{
		ValidateSampleRate(sampleRate);
		SetParameters(cutoffHz, q);
	}

	void StateVariableFilter::SetParameters(float cutoffHz, float q) noexcept
	{
		m_CutoffHz = ClampFrequency(cutoffHz, m_SampleRate);
		m_Q = ClampFinite(q, 0.5f, 20.0f, 0.70710678f);
		UpdateCoefficients();
	}

	void StateVariableFilter::UpdateCoefficients() noexcept
	{
		const float g = std::tan(kPi * m_CutoffHz / static_cast<float>(m_SampleRate));
		m_K = 1.0f / m_Q;
		m_G = std::isfinite(g) ? std::clamp(g, 0.0f, 100.0f) : 0.0f;
	}

	void StateVariableFilter::Reset() noexcept
	{
		m_Integrator1 = m_Integrator2 = 0.0f;
	}

	StateVariableOutput StateVariableFilter::ProcessSample(float input) noexcept
	{
		const float x = SafeSample(input);
		const float a1 = 1.0f / (1.0f + m_G * (m_G + m_K));
		const float a2 = m_G * a1;
		const float a3 = m_G * a2;
		const float v3 = x - m_Integrator2;
		const float band = a1 * m_Integrator1 + a2 * v3;
		const float low = m_Integrator2 + a2 * m_Integrator1 + a3 * v3;
		const float high = x - m_K * band - low;
		m_Integrator1 = 2.0f * band - m_Integrator1;
		m_Integrator2 = 2.0f * low - m_Integrator2;
		if (!std::isfinite(m_Integrator1) || !std::isfinite(m_Integrator2) ||
		    std::fabs(m_Integrator1) > kMaxInternal || std::fabs(m_Integrator2) > kMaxInternal)
			Reset();
		return {low, band, high, low + high};
	}

	void StateVariableFilter::Process(const float* input, StateVariableOutput* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(input ? input[i] : 0.0f);
	}

	OnePole::OnePole(std::uint32_t sampleRate, OnePoleType type, float cutoffHz)
		: m_SampleRate(sampleRate), m_Type(type), m_CutoffHz(cutoffHz)
	{
		ValidateSampleRate(sampleRate);
		SetCutoff(cutoffHz);
	}

	void OnePole::SetCutoff(float cutoffHz) noexcept
	{
		m_CutoffHz = ClampFrequency(cutoffHz, m_SampleRate);
		UpdateCoefficient();
	}

	void OnePole::UpdateCoefficient() noexcept
	{
		m_Coefficient = 1.0f - std::exp(-kTwoPi * m_CutoffHz / static_cast<float>(m_SampleRate));
		m_Coefficient = ClampFinite(m_Coefficient, 0.0f, 1.0f, 0.0f);
	}

	void OnePole::Reset() noexcept
	{
		m_PreviousLow = 0.0f;
	}

	float OnePole::ProcessSample(float input) noexcept
	{
		const float x = SafeSample(input);
		m_PreviousLow += m_Coefficient * (x - m_PreviousLow);
		if (!std::isfinite(m_PreviousLow) || std::fabs(m_PreviousLow) > kMaxInternal)
			m_PreviousLow = 0.0f;
		const float output = m_Type == OnePoleType::LowPass ? m_PreviousLow : x - m_PreviousLow;
		return SafeSample(output);
	}

	void OnePole::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(input ? input[i] : 0.0f);
	}

	LadderFilter::LadderFilter(std::uint32_t sampleRate, float cutoffHz, float resonance)
		: m_SampleRate(sampleRate), m_CutoffHz(cutoffHz), m_Resonance(resonance)
	{
		ValidateSampleRate(sampleRate);
		SetParameters(cutoffHz, resonance);
	}

	void LadderFilter::SetParameters(float cutoffHz, float resonance) noexcept
	{
		m_CutoffHz = ClampFrequency(cutoffHz, m_SampleRate);
		m_Resonance = ClampFinite(resonance, 0.0f, 3.8f, 0.0f);
		UpdateCoefficient();
	}

	void LadderFilter::UpdateCoefficient() noexcept
	{
		m_Coefficient = 1.0f - std::exp(-kTwoPi * m_CutoffHz / static_cast<float>(m_SampleRate));
		m_Coefficient = ClampFinite(m_Coefficient, 0.0f, 1.0f, 0.0f);
	}

	void LadderFilter::Reset() noexcept
	{
		std::fill(std::begin(m_Stages), std::end(m_Stages), 0.0f);
	}

	float LadderFilter::ProcessSample(float input) noexcept
	{
		const float x = SafeSample(input);
		const float feedback = m_Stages[3] * (m_Resonance * 0.25f);
		float stageInput = std::tanh(std::clamp(x - feedback, -20.0f, 20.0f));
		for (float& stage : m_Stages)
		{
			stage += m_Coefficient * (stageInput - stage);
			stage = ClampFinite(stage, -1.0f, 1.0f, 0.0f);
			stageInput = std::tanh(stage);
		}
		return m_Stages[3];
	}

	void LadderFilter::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(input ? input[i] : 0.0f);
	}

	void WaveShaper::SetParameters(WaveShape shape, float amount, float bias, float outputGain) noexcept
	{
		m_Shape = shape;
		m_Amount = ClampFinite(amount, 0.0f, 1.0f, 1.0f);
		m_Bias = ClampFinite(bias, -4.0f, 4.0f, 0.0f);
		m_OutputGain = ClampFinite(outputGain, -16.0f, 16.0f, 1.0f);
	}

	float WaveShaper::ProcessSample(float input) const noexcept
	{
		const float x = SafeSample(input) + m_Bias;
		float shaped = x;
		if (m_Amount > 1.0e-6f)
		{
			switch (m_Shape)
			{
			case WaveShape::Sine:
			{
				const float denominator = std::sin(0.5f * kPi * m_Amount);
				shaped = std::sin(0.5f * kPi * m_Amount * x) / (std::max)(denominator, 1.0e-6f);
				break;
			}
			case WaveShape::InverseTangent:
				shaped = std::atan(m_Amount * x) / std::atan(m_Amount);
				break;
			case WaveShape::HyperbolicTangent:
				shaped = std::tanh(m_Amount * x) / std::tanh(m_Amount);
				break;
			case WaveShape::Cubic:
			{
				const float x2 = x * x;
				const float normalizer = 1.0f - 0.25f * m_Amount * m_Amount;
				shaped = (x * (1.0f - 0.25f * m_Amount * m_Amount * x2)) / normalizer;
				break;
			}
			case WaveShape::HardClip:
				shaped = std::clamp(m_Amount * x, -1.0f, 1.0f) / m_Amount;
				break;
			}
		}
		return SafeSample(shaped * m_OutputGain);
	}

	void WaveShaper::Process(const float* input, float* output, std::size_t frames) const noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(input ? input[i] : 0.0f);
	}

	AdEnvelope::AdEnvelope(std::uint32_t sampleRate, float attackSeconds, float decaySeconds)
		: m_SampleRate(sampleRate)
	{
		ValidateSampleRate(sampleRate);
		SetParameters(attackSeconds, decaySeconds);
	}

	void AdEnvelope::SetParameters(float attackSeconds, float decaySeconds, float attackCurve, float decayCurve) noexcept
	{
		m_AttackFrames = TimeToFrames(attackSeconds, m_SampleRate);
		m_DecayFrames = TimeToFrames(decaySeconds, m_SampleRate);
		m_AttackCurve = ClampFinite(attackCurve, 0.1f, 10.0f, 1.0f);
		m_DecayCurve = ClampFinite(decayCurve, 0.1f, 10.0f, 1.0f);
	}

	void AdEnvelope::Trigger() noexcept
	{
		m_PhaseFrame = 0;
		m_Value = 0.0f;
		if (m_AttackFrames == 0 && m_DecayFrames == 0)
		{
			// Both phases are instantaneous; produce exact silence rather than
			// dividing by a zero frame count or spinning a zero-time loop.
			m_Stage = EnvelopeStage::Idle;
			return;
		}
		m_Stage = m_AttackFrames == 0 ? EnvelopeStage::Decay : EnvelopeStage::Attack;
		if (m_AttackFrames == 0)
			m_Value = 1.0f;
	}

	void AdEnvelope::Reset() noexcept
	{
		m_PhaseFrame = 0;
		m_Value = 0.0f;
		m_Stage = EnvelopeStage::Idle;
	}

	float AdEnvelope::ProcessSample() noexcept
	{
		switch (m_Stage)
		{
		case EnvelopeStage::Attack:
		{
			if (m_AttackFrames == 0)
			{
				m_Value = 1.0f;
				m_PhaseFrame = 0;
				m_Stage = m_DecayFrames == 0 ? EnvelopeStage::Idle : EnvelopeStage::Decay;
				if (m_DecayFrames == 0)
					m_Value = 0.0f;
				break;
			}
			const float t = static_cast<float>(m_PhaseFrame + 1) / static_cast<float>(m_AttackFrames);
			m_Value = Curve(t, m_AttackCurve);
			if (++m_PhaseFrame >= m_AttackFrames)
			{
				m_PhaseFrame = 0;
				if (m_DecayFrames == 0)
				{
					m_Value = 0.0f;
					m_Stage = m_Looping ? EnvelopeStage::Attack : EnvelopeStage::Idle;
				}
				else
				{
					m_Value = 1.0f;
					m_Stage = EnvelopeStage::Decay;
				}
			}
			break;
		}
		case EnvelopeStage::Decay:
		{
			if (m_DecayFrames == 0)
			{
				m_Value = 0.0f;
				m_PhaseFrame = 0;
				m_Stage = m_Looping && m_AttackFrames > 0 ? EnvelopeStage::Attack : EnvelopeStage::Idle;
				break;
			}
			const float t = static_cast<float>(m_PhaseFrame + 1) / static_cast<float>(m_DecayFrames);
			m_Value = 1.0f - Curve(t, m_DecayCurve);
			if (++m_PhaseFrame >= m_DecayFrames)
			{
				m_Value = 0.0f;
				m_PhaseFrame = 0;
				if (m_Looping)
				{
					m_Stage = m_AttackFrames == 0 ? EnvelopeStage::Decay : EnvelopeStage::Attack;
					if (m_AttackFrames == 0)
						m_Value = 1.0f;
				}
				else
					m_Stage = EnvelopeStage::Idle;
			}
			break;
		}
		case EnvelopeStage::Idle:
		case EnvelopeStage::Sustain:
		case EnvelopeStage::Release:
			break;
		}
		return m_Value;
	}

	void AdEnvelope::Process(float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample();
	}

	AdsrEnvelope::AdsrEnvelope(std::uint32_t sampleRate) : m_SampleRate(sampleRate)
	{
		ValidateSampleRate(sampleRate);
	}

	void AdsrEnvelope::SetParameters(float attackSeconds, float decaySeconds, float sustain,
	                                 float releaseSeconds, float attackCurve, float decayCurve,
	                                 float releaseCurve) noexcept
	{
		m_AttackFrames = TimeToFrames(attackSeconds, m_SampleRate);
		m_DecayFrames = TimeToFrames(decaySeconds, m_SampleRate);
		m_ReleaseFrames = TimeToFrames(releaseSeconds, m_SampleRate);
		m_Sustain = ClampFinite(sustain, 0.0f, 1.0f, 0.7f);
		m_AttackCurve = ClampFinite(attackCurve, 0.1f, 10.0f, 1.0f);
		m_DecayCurve = ClampFinite(decayCurve, 0.1f, 10.0f, 1.0f);
		m_ReleaseCurve = ClampFinite(releaseCurve, 0.1f, 10.0f, 1.0f);
	}

	void AdsrEnvelope::TriggerAttack() noexcept
	{
		m_Value = 0.0f;
		m_PhaseFrame = 0;
		m_ReleaseStart = 0.0f;
		m_Stage = m_AttackFrames == 0 ? EnvelopeStage::Decay : EnvelopeStage::Attack;
		if (m_AttackFrames == 0)
		{
			m_Value = 1.0f;
			if (m_DecayFrames == 0)
			{
				m_Value = m_Sustain;
				m_Stage = EnvelopeStage::Sustain;
			}
		}
	}

	void AdsrEnvelope::TriggerRelease() noexcept
	{
		if (m_Stage == EnvelopeStage::Idle || m_Value <= 0.0f || m_ReleaseFrames == 0)
		{
			m_Value = 0.0f;
			m_Stage = EnvelopeStage::Idle;
			m_PhaseFrame = 0;
			return;
		}
		m_ReleaseStart = m_Value;
		m_PhaseFrame = 0;
		m_Stage = EnvelopeStage::Release;
	}

	void AdsrEnvelope::Reset() noexcept
	{
		m_Value = m_ReleaseStart = 0.0f;
		m_PhaseFrame = 0;
		m_Stage = EnvelopeStage::Idle;
	}

	float AdsrEnvelope::ProcessSample() noexcept
	{
		switch (m_Stage)
		{
		case EnvelopeStage::Attack:
		{
			if (m_AttackFrames == 0)
			{
				m_Value = 1.0f;
				m_PhaseFrame = 0;
				m_Stage = m_DecayFrames == 0 ? EnvelopeStage::Sustain : EnvelopeStage::Decay;
				if (m_DecayFrames == 0)
					m_Value = m_Sustain;
				break;
			}
			m_Value = Curve(static_cast<float>(m_PhaseFrame + 1) / m_AttackFrames, m_AttackCurve);
			if (++m_PhaseFrame >= m_AttackFrames)
			{
				m_Value = 1.0f;
				m_PhaseFrame = 0;
				m_Stage = m_DecayFrames == 0 ? EnvelopeStage::Sustain : EnvelopeStage::Decay;
				if (m_DecayFrames == 0)
					m_Value = m_Sustain;
			}
			break;
		}
		case EnvelopeStage::Decay:
		{
			if (m_DecayFrames == 0)
			{
				m_Value = m_Sustain;
				m_PhaseFrame = 0;
				m_Stage = EnvelopeStage::Sustain;
				break;
			}
			const float t = Curve(static_cast<float>(m_PhaseFrame + 1) / m_DecayFrames, m_DecayCurve);
			m_Value = 1.0f + (m_Sustain - 1.0f) * t;
			if (++m_PhaseFrame >= m_DecayFrames)
			{
				m_Value = m_Sustain;
				m_PhaseFrame = 0;
				m_Stage = EnvelopeStage::Sustain;
			}
			break;
		}
		case EnvelopeStage::Release:
		{
			if (m_ReleaseFrames == 0)
			{
				m_Value = 0.0f;
				m_PhaseFrame = 0;
				m_Stage = EnvelopeStage::Idle;
				break;
			}
			const float t = Curve(static_cast<float>(m_PhaseFrame + 1) / m_ReleaseFrames, m_ReleaseCurve);
			m_Value = m_ReleaseStart * (1.0f - t);
			if (++m_PhaseFrame >= m_ReleaseFrames)
			{
				m_Value = 0.0f;
				m_PhaseFrame = 0;
				m_Stage = EnvelopeStage::Idle;
			}
			break;
		}
		case EnvelopeStage::Idle:
			m_Value = 0.0f;
			break;
		case EnvelopeStage::Sustain:
			m_Value = m_Sustain;
			break;
		}
		return ClampFinite(m_Value, 0.0f, 1.0f, 0.0f);
	}

	void AdsrEnvelope::Process(float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample();
	}

	Compressor::Compressor(std::uint32_t sampleRate) : m_SampleRate(sampleRate)
	{
		ValidateSampleRate(sampleRate);
		const auto capacity = static_cast<std::size_t>(std::ceil(sampleRate * kMaxLookaheadSeconds));
		m_Lookahead.assign(std::max<std::size_t>(capacity, 1), 0.0f);
		SetParameters(m_ThresholdDb, m_Ratio, m_KneeDb, m_AttackSeconds, m_ReleaseSeconds);
	}

	void Compressor::SetParameters(float thresholdDb, float ratio, float kneeDb, float attackSeconds,
	                               float releaseSeconds, float lookaheadSeconds, DetectorMode mode) noexcept
	{
		m_ThresholdDb = ClampFinite(thresholdDb, -96.0f, 0.0f, -18.0f);
		m_Ratio = ClampFinite(ratio, 1.0f, 100.0f, 4.0f);
		m_KneeDb = ClampFinite(kneeDb, 0.0f, 24.0f, 0.0f);
		m_AttackSeconds = ClampFinite(attackSeconds, 0.0f, 10.0f, 0.01f);
		m_ReleaseSeconds = ClampFinite(releaseSeconds, 0.0f, 10.0f, 0.1f);
		m_LookaheadSeconds = ClampFinite(lookaheadSeconds, 0.0f, kMaxLookaheadSeconds, 0.0f);
		m_LookaheadFrames = std::min<std::size_t>(TimeToFrames(m_LookaheadSeconds, m_SampleRate, kMaxLookaheadSeconds), m_Lookahead.size());
		m_Mode = mode;
		m_LookaheadIndex = 0;
		std::fill(m_Lookahead.begin(), m_Lookahead.end(), 0.0f);
	}

	void Compressor::Reset() noexcept
	{
		m_Detector = 0.0f;
		m_Gain = 1.0f;
		m_LookaheadIndex = 0;
		std::fill(m_Lookahead.begin(), m_Lookahead.end(), 0.0f);
	}

	void Compressor::Process(const float* input, const float* sidechain, float* output,
	                        float* gainEnvelope, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		const float attackCoefficient = TimeCoefficient(m_AttackSeconds, m_SampleRate);
		const float releaseCoefficient = TimeCoefficient(m_ReleaseSeconds, m_SampleRate);
		for (std::size_t i = 0; i < frames; ++i)
		{
			const float program = SafeSample(input ? input[i] : 0.0f);
			const float detectorInput = std::fabs(SafeSample(sidechain ? sidechain[i] : program));
			const float targetDetector = m_Mode == DetectorMode::RMS ? detectorInput * detectorInput : detectorInput;
			const float currentDetector = m_Mode == DetectorMode::RMS ? m_Detector * m_Detector : m_Detector;
			const float detectorCoefficient = targetDetector > currentDetector ? attackCoefficient : releaseCoefficient;
			const float nextDetector = detectorCoefficient * currentDetector + (1.0f - detectorCoefficient) * targetDetector;
			m_Detector = m_Mode == DetectorMode::RMS ? std::sqrt((std::max)(0.0f, nextDetector)) : nextDetector;
			const float targetGain = CompressorTargetGain(m_Detector, m_ThresholdDb, m_Ratio, m_KneeDb);
			const float gainCoefficient = targetGain < m_Gain ? attackCoefficient : releaseCoefficient;
			m_Gain = gainCoefficient * m_Gain + (1.0f - gainCoefficient) * targetGain;
			m_Gain = ClampFinite(m_Gain, 0.0f, 1.0f, 1.0f);

			float delayedProgram = program;
			if (m_LookaheadFrames > 0)
			{
				delayedProgram = m_Lookahead[m_LookaheadIndex];
				m_Lookahead[m_LookaheadIndex] = program;
				m_LookaheadIndex = (m_LookaheadIndex + 1) % m_LookaheadFrames;
			}
			output[i] = SafeSample(delayedProgram * m_Gain);
			if (gainEnvelope)
				gainEnvelope[i] = m_Gain;
		}
	}

	Limiter::Limiter(std::uint32_t sampleRate) : m_SampleRate(sampleRate)
	{
		ValidateSampleRate(sampleRate);
		const auto capacity = static_cast<std::size_t>(std::ceil(sampleRate * kMaxLookaheadSeconds));
		m_Lookahead.assign(std::max<std::size_t>(capacity, 1), 0.0f);
	}

	void Limiter::SetParameters(float ceiling, float releaseSeconds, float lookaheadSeconds) noexcept
	{
		m_Ceiling = ClampFinite(ceiling, 1.0e-4f, 1.0f, 0.98f);
		m_ReleaseSeconds = ClampFinite(releaseSeconds, 0.0f, 10.0f, 0.05f);
		const float safeLookahead = ClampFinite(lookaheadSeconds, 0.0f, kMaxLookaheadSeconds, 0.0f);
		m_LookaheadFrames = std::min<std::size_t>(TimeToFrames(safeLookahead, m_SampleRate, kMaxLookaheadSeconds), m_Lookahead.size());
		m_LookaheadIndex = 0;
		std::fill(m_Lookahead.begin(), m_Lookahead.end(), 0.0f);
	}

	void Limiter::Reset() noexcept
	{
		m_Gain = 1.0f;
		m_LookaheadIndex = 0;
		std::fill(m_Lookahead.begin(), m_Lookahead.end(), 0.0f);
	}

	void Limiter::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		const float releaseCoefficient = TimeCoefficient(m_ReleaseSeconds, m_SampleRate);
		for (std::size_t i = 0; i < frames; ++i)
		{
			const float current = SafeSample(input ? input[i] : 0.0f);
			float delayed = current;
			if (m_LookaheadFrames > 0)
			{
				delayed = m_Lookahead[m_LookaheadIndex];
				m_Lookahead[m_LookaheadIndex] = current;
				m_LookaheadIndex = (m_LookaheadIndex + 1) % m_LookaheadFrames;
			}
			const float magnitude = std::fabs(current);
			const float desiredGain = magnitude > m_Ceiling ? m_Ceiling / magnitude : 1.0f;
			if (desiredGain < m_Gain)
				m_Gain = desiredGain; // Instant attack is the hard safety guarantee.
			else
				m_Gain = releaseCoefficient * m_Gain + (1.0f - releaseCoefficient) * desiredGain;
			m_Gain = ClampFinite(m_Gain, 0.0f, 1.0f, 1.0f);
			output[i] = std::clamp(SafeSample(delayed * m_Gain), -m_Ceiling, m_Ceiling);
		}
	}

	Delay::Delay(std::uint32_t sampleRate, float maxDelaySeconds)
		: m_SampleRate(sampleRate), m_MaxDelaySeconds(ClampFinite(maxDelaySeconds, 0.0f, kMaxDelaySeconds, 2.0f))
	{
		ValidateSampleRate(sampleRate);
		m_Buffer.assign(DelayBufferFrames(sampleRate, m_MaxDelaySeconds), 0.0f);
		SetDelaySeconds((std::min)(0.25f, m_MaxDelaySeconds));
	}

	void Delay::SetDelaySeconds(float seconds) noexcept
	{
		const float safeSeconds = ClampFinite(seconds, 0.0f, m_MaxDelaySeconds, 0.0f);
		m_DelaySamples = safeSeconds * static_cast<float>(m_SampleRate);
	}

	void Delay::SetFeedback(float feedback) noexcept
	{
		m_Feedback = ClampFinite(feedback, -0.99f, 0.99f, 0.0f);
	}

	void Delay::SetMix(float dry, float wet) noexcept
	{
		m_Dry = ClampFinite(dry, -4.0f, 4.0f, 1.0f);
		m_Wet = ClampFinite(wet, -4.0f, 4.0f, 0.5f);
	}

	void Delay::Reset() noexcept
	{
		std::fill(m_Buffer.begin(), m_Buffer.end(), 0.0f);
		m_WriteIndex = 0;
	}

	float Delay::ReadDelay() const noexcept
	{
		return ReadRing(m_Buffer, m_WriteIndex, m_DelaySamples);
	}

	void Delay::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
		{
			const float sample = SafeSample(input ? input[i] : 0.0f);
			const float delayed = m_DelaySamples < 1.0f ? sample : ReadDelay();
			output[i] = SafeSample(sample * m_Dry + delayed * m_Wet);
			m_Buffer[m_WriteIndex] = SafeSample(sample + m_Feedback * delayed);
			m_WriteIndex = (m_WriteIndex + 1) % m_Buffer.size();
		}
	}

	StereoDelay::StereoDelay(std::uint32_t sampleRate, float maxDelaySeconds)
		: m_SampleRate(sampleRate), m_MaxDelaySeconds(ClampFinite(maxDelaySeconds, 0.0f, kMaxDelaySeconds, 2.0f))
	{
		ValidateSampleRate(sampleRate);
		const std::size_t frames = DelayBufferFrames(sampleRate, m_MaxDelaySeconds);
		m_LeftBuffer.assign(frames, 0.0f);
		m_RightBuffer.assign(frames, 0.0f);
		const float initial = (std::min)(0.25f, m_MaxDelaySeconds);
		SetDelaySeconds(initial, initial);
	}

	void StereoDelay::SetDelaySeconds(float leftSeconds, float rightSeconds) noexcept
	{
		leftSeconds = ClampFinite(leftSeconds, 0.0f, m_MaxDelaySeconds, 0.0f);
		rightSeconds = ClampFinite(rightSeconds, 0.0f, m_MaxDelaySeconds, 0.0f);
		m_DelaySamples[0] = leftSeconds * static_cast<float>(m_SampleRate);
		m_DelaySamples[1] = rightSeconds * static_cast<float>(m_SampleRate);
	}

	void StereoDelay::SetFeedback(float feedback) noexcept
	{
		m_Feedback = ClampFinite(feedback, -0.99f, 0.99f, 0.0f);
	}

	void StereoDelay::SetMix(float dry, float wet) noexcept
	{
		m_Dry = ClampFinite(dry, -4.0f, 4.0f, 1.0f);
		m_Wet = ClampFinite(wet, -4.0f, 4.0f, 0.5f);
	}

	void StereoDelay::Reset() noexcept
	{
		std::fill(m_LeftBuffer.begin(), m_LeftBuffer.end(), 0.0f);
		std::fill(m_RightBuffer.begin(), m_RightBuffer.end(), 0.0f);
		m_WriteIndex = 0;
	}

	float StereoDelay::ReadDelay(const std::vector<float>& buffer, float delaySamples) const noexcept
	{
		return ReadRing(buffer, m_WriteIndex, delaySamples);
	}

	void StereoDelay::Process(const float* interleavedInput, float* interleavedOutput, std::size_t frames) noexcept
	{
		if (frames == 0 || interleavedOutput == nullptr)
			return;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			const std::size_t offset = frame * 2;
			const float left = SafeSample(interleavedInput ? interleavedInput[offset] : 0.0f);
			const float right = SafeSample(interleavedInput ? interleavedInput[offset + 1] : 0.0f);
			const float delayedLeft = m_DelaySamples[0] < 1.0f ? left : ReadDelay(m_LeftBuffer, m_DelaySamples[0]);
			const float delayedRight = m_DelaySamples[1] < 1.0f ? right : ReadDelay(m_RightBuffer, m_DelaySamples[1]);
			float wetLeft = delayedLeft, wetRight = delayedRight;
			float feedbackLeft = delayedLeft, feedbackRight = delayedRight;
			if (m_Mode == StereoDelayMode::Cross)
			{
				wetLeft = delayedRight;
				wetRight = delayedLeft;
			}
			else if (m_Mode == StereoDelayMode::PingPong)
			{
				feedbackLeft = delayedRight;
				feedbackRight = delayedLeft;
			}
			interleavedOutput[offset] = SafeSample(left * m_Dry + wetLeft * m_Wet);
			interleavedOutput[offset + 1] = SafeSample(right * m_Dry + wetRight * m_Wet);
			m_LeftBuffer[m_WriteIndex] = SafeSample(left + m_Feedback * feedbackLeft);
			m_RightBuffer[m_WriteIndex] = SafeSample(right + m_Feedback * feedbackRight);
			m_WriteIndex = (m_WriteIndex + 1) % m_LeftBuffer.size();
		}
	}

	void StereoPanner::Gains(float pan, PanLaw law, float& left, float& right) noexcept
	{
		pan = ClampFinite(pan, -1.0f, 1.0f, 0.0f);
		if (law == PanLaw::Linear)
		{
			left = 0.5f * (1.0f - pan);
			right = 0.5f * (1.0f + pan);
			return;
		}
		const float angle = (pan + 1.0f) * (kPi * 0.25f);
		left = std::cos(angle);
		right = std::sin(angle);
	}

	void StereoPanner::Process(const float* monoInput, float* interleavedStereoOutput, std::size_t frames,
	                           float pan, PanLaw law) const noexcept
	{
		if (frames == 0 || interleavedStereoOutput == nullptr)
			return;
		float left = 0.0f, right = 0.0f;
		Gains(pan, law, left, right);
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			const float sample = SafeSample(monoInput ? monoInput[frame] : 0.0f);
			interleavedStereoOutput[frame * 2] = SafeSample(sample * left);
			interleavedStereoOutput[frame * 2 + 1] = SafeSample(sample * right);
		}
	}

	void StereoMixer::Process(const float* stereoA, const float* stereoB, float* stereoOutput,
	                         std::size_t frames, float gainA, float gainB) const noexcept
	{
		if (frames == 0 || stereoOutput == nullptr)
			return;
		// Gains are intentionally not normalized or restricted to [0, 1]: negative
		// and over-unity values are valid for inversion and headroom workflows.
		// Only the extreme finite range is bounded to protect arithmetic state.
		gainA = ClampFinite(gainA, -kMaxInternal, kMaxInternal, 1.0f);
		gainB = ClampFinite(gainB, -kMaxInternal, kMaxInternal, 1.0f);
		for (std::size_t i = 0; i < frames * 2; ++i)
		{
			const float a = SafeSample(stereoA ? stereoA[i] : 0.0f);
			const float b = SafeSample(stereoB ? stereoB[i] : 0.0f);
			stereoOutput[i] = SafeSample(a * gainA + b * gainB);
		}
	}

	Lfo::Lfo(std::uint32_t sampleRate, float frequencyHz, OscillatorWaveform waveform, float phaseRadians)
		: m_SampleRate(sampleRate), m_Waveform(waveform)
	{
		ValidateSampleRate(sampleRate);
		SetFrequency(frequencyHz);
		SetPhase(phaseRadians);
	}

	void Lfo::SetFrequency(float frequencyHz) noexcept
	{
		m_FrequencyHz = ClampFinite(frequencyHz, 0.0f, static_cast<float>(m_SampleRate) * 0.5f, 0.0f);
	}

	void Lfo::SetPhase(float phaseRadians) noexcept
	{
		m_Phase = WrapPhase(phaseRadians);
	}

	void Lfo::Reset(float phaseRadians) noexcept
	{
		SetPhase(phaseRadians);
	}

	float Lfo::ProcessSample() noexcept
	{
		const float value = OscillatorValue(m_Waveform, m_Phase);
		m_Phase = WrapPhase(m_Phase + kTwoPi * m_FrequencyHz / static_cast<float>(m_SampleRate));
		return ClampFinite(value, -1.0f, 1.0f, 0.0f);
	}

	void Lfo::Process(float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample();
	}

	Sine::Sine(std::uint32_t sampleRate, float frequencyHz, float amplitude)
		: m_SampleRate(sampleRate), m_CurrentFrequency(440.0f), m_TargetFrequency(440.0f), m_Amplitude(1.0f)
	{
		ValidateSampleRate(sampleRate);
		SetFrequency(frequencyHz);
		SetAmplitude(amplitude);
	}

	void Sine::SetFrequency(float frequencyHz, float glideSeconds) noexcept
	{
		m_TargetFrequency = ClampFinite(frequencyHz, 0.0f, static_cast<float>(m_SampleRate) * 0.5f, 0.0f);
		m_GlideFramesRemaining = TimeToFrames(glideSeconds, m_SampleRate, kMaxEnvelopeSeconds);
		if (m_GlideFramesRemaining == 0)
		{
			m_CurrentFrequency = m_TargetFrequency;
			m_GlideStep = 0.0f;
		}
		else
			m_GlideStep = (m_TargetFrequency - m_CurrentFrequency) / static_cast<float>(m_GlideFramesRemaining);
	}

	void Sine::SetAmplitude(float amplitude) noexcept
	{
		m_Amplitude = ClampFinite(amplitude, -16.0f, 16.0f, 1.0f);
	}

	void Sine::Reset() noexcept
	{
		m_Phase = 0.0f;
	}

	float Sine::ProcessSample(bool sync) noexcept
	{
		if (sync)
			m_Phase = 0.0f;
		if (m_GlideFramesRemaining > 0)
		{
			m_CurrentFrequency += m_GlideStep;
			if (--m_GlideFramesRemaining == 0)
				m_CurrentFrequency = m_TargetFrequency;
		}
		const float output = m_Amplitude * std::sin(m_Phase);
		m_Phase = WrapPhase(m_Phase + kTwoPi * m_CurrentFrequency / static_cast<float>(m_SampleRate));
		return SafeSample(output);
	}

	void Sine::Process(float* output, std::size_t frames, const std::uint8_t* sync) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample(sync && sync[i] != 0);
	}

	Noise::Noise(std::uint32_t sampleRate, std::uint32_t seed) : m_SampleRate(sampleRate), m_State(seed)
	{
		ValidateSampleRate(sampleRate);
		Reset(seed);
	}

	void Noise::Reset(std::uint32_t seed) noexcept
	{
		m_State = seed ? seed : 0xA341316Cu;
	}

	float Noise::ProcessSample() noexcept
	{
		// xorshift32: fixed state, no global RNG, bounded work and bit-repeatable output.
		m_State ^= m_State << 13;
		m_State ^= m_State >> 17;
		m_State ^= m_State << 5;
		const float unit = static_cast<float>(m_State >> 8) * (1.0f / 16777216.0f);
		return unit * 2.0f - 1.0f;
	}

	void Noise::Process(float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
			output[i] = ProcessSample();
	}

	BiquadEq::BiquadEq(std::uint32_t sampleRate)
		: m_SampleRate(sampleRate),
		  m_LowShelf(sampleRate, BiquadType::LowShelf, 250.0f, 0.70710678f),
		  m_Mid(sampleRate, BiquadType::Peaking, 1200.0f, 0.70710678f),
		  m_HighShelf(sampleRate, BiquadType::HighShelf, 7000.0f, 0.70710678f)
	{
		ValidateSampleRate(sampleRate);
	}

	void BiquadEq::SetParameters(float lowFrequencyHz, float lowGainDb, float midFrequencyHz,
	                             float midGainDb, float highFrequencyHz, float highGainDb,
	                             float midQ) noexcept
	{
		m_LowShelf.Configure(BiquadType::LowShelf, lowFrequencyHz, 0.70710678f, lowGainDb);
		m_Mid.Configure(BiquadType::Peaking, midFrequencyHz, midQ, midGainDb);
		m_HighShelf.Configure(BiquadType::HighShelf, highFrequencyHz, 0.70710678f, highGainDb);
	}

	void BiquadEq::Reset() noexcept
	{
		m_LowShelf.Reset();
		m_Mid.Reset();
		m_HighShelf.Reset();
	}

	void BiquadEq::Process(const float* input, float* output, std::size_t frames) noexcept
	{
		if (frames == 0 || output == nullptr)
			return;
		for (std::size_t i = 0; i < frames; ++i)
		{
			float sample = m_LowShelf.ProcessSample(input ? input[i] : 0.0f);
			sample = m_Mid.ProcessSample(sample);
			output[i] = m_HighShelf.ProcessSample(sample);
		}
	}
}
