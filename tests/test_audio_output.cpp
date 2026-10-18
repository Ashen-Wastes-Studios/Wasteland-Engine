#include "wlpch.h"

#include "Wasteland/Audio/AudioDevice.h"
#include "Wasteland/Audio/AudioClip.h"
#include "Wasteland/Audio/Mixer.h"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace Wasteland::Audio;

namespace
{
	struct ToneMeasurement
	{
		double TargetAmplitude;
		double TargetCoherence;
		double Rms;
	};

	struct TargetToneAcceptanceThresholds
	{
		double Amplitude;
	};

	TargetToneAcceptanceThresholds GetTargetToneAcceptanceThresholds(const ToneMeasurement& baseline)
	{
		return {
			(std::max)(0.001, baseline.TargetAmplitude * 2.0 + 0.0002)
		};
	}

	bool AcceptTargetTone(const ToneMeasurement& measurement, const ToneMeasurement& baseline)
	{
		const TargetToneAcceptanceThresholds thresholds = GetTargetToneAcceptanceThresholds(baseline);
		return measurement.TargetAmplitude >= thresholds.Amplitude;
	}

	bool MeasureTargetTone(const std::vector<float>& interleavedStereo, uint32_t sampleRate,
		uint64_t frames, double targetHz, ToneMeasurement& out)
	{
		out = {};
		if (sampleRate == 0 || frames == 0 ||
			frames > static_cast<uint64_t>(interleavedStereo.size() / 2) ||
			!std::isfinite(targetHz) || targetHz <= 0.0 || targetHz >= sampleRate * 0.5)
			return false;

		constexpr uint32_t channelCount = 2;
		const double angularStep = 2.0 * 3.14159265358979323846 * targetHz / sampleRate;
		double totalSignalEnergy = 0.0;
		double totalTargetEnergy = 0.0;
		double totalAmplitudeSquared = 0.0;
		for (uint32_t channel = 0; channel < channelCount; ++channel)
		{
			double mean = 0.0;
			for (uint64_t frame = 0; frame < frames; ++frame)
			{
				const double sample = interleavedStereo[static_cast<size_t>(frame) * channelCount + channel];
				if (!std::isfinite(sample))
					return false;
				mean += sample;
			}
			mean /= static_cast<double>(frames);

			double signalEnergy = 0.0;
			double sumCos = 0.0;
			double sumSin = 0.0;
			double sumCosSquared = 0.0;
			double sumSinSquared = 0.0;
			double sumCosSin = 0.0;
			double sumSignalCos = 0.0;
			double sumSignalSin = 0.0;
			for (uint64_t frame = 0; frame < frames; ++frame)
			{
				const double sample = interleavedStereo[static_cast<size_t>(frame) * channelCount + channel];
				const double centeredSample = sample - mean;
				const double phase = angularStep * static_cast<double>(frame);
				const double cosine = std::cos(phase);
				const double sine = std::sin(phase);
				signalEnergy += centeredSample * centeredSample;
				sumCos += cosine;
				sumSin += sine;
				sumCosSquared += cosine * cosine;
				sumSinSquared += sine * sine;
				sumCosSin += cosine * sine;
				sumSignalCos += centeredSample * cosine;
				sumSignalSin += centeredSample * sine;
			}
			totalSignalEnergy += signalEnergy;

			// Center the quadrature basis as well as the samples, then solve the
			// two-parameter least-squares projection. This removes DC and avoids
			// phase- or window-alignment bias when the window is not an integer
			// number of target cycles.
			const double count = static_cast<double>(frames);
			const double cosineEnergy = sumCosSquared - sumCos * sumCos / count;
			const double sineEnergy = sumSinSquared - sumSin * sumSin / count;
			const double crossEnergy = sumCosSin - sumCos * sumSin / count;
			const double determinant = cosineEnergy * sineEnergy - crossEnergy * crossEnergy;
			const double determinantFloor = (std::max)(1.0e-18,
				(std::abs)(cosineEnergy * sineEnergy) * 1.0e-12);
			if (std::isfinite(determinant) && determinant > determinantFloor)
			{
				const double cosineCoefficient =
					(sumSignalCos * sineEnergy - sumSignalSin * crossEnergy) / determinant;
				const double sineCoefficient =
					(sumSignalSin * cosineEnergy - sumSignalCos * crossEnergy) / determinant;
				const double amplitudeSquared = cosineCoefficient * cosineCoefficient +
					sineCoefficient * sineCoefficient;
				const double fittedEnergy = (std::max)(0.0, (std::min)(signalEnergy,
					cosineCoefficient * sumSignalCos + sineCoefficient * sumSignalSin));
				totalAmplitudeSquared += amplitudeSquared;
				totalTargetEnergy += fittedEnergy;
			}
		}

		out.TargetAmplitude = std::sqrt(totalAmplitudeSquared / channelCount);
		out.TargetCoherence = totalSignalEnergy > 0.0
			? (std::max)(0.0, (std::min)(1.0, totalTargetEnergy / totalSignalEnergy)) : 0.0;
		out.Rms = std::sqrt(totalSignalEnergy / (static_cast<double>(frames) * channelCount));
		return true;
	}

	bool Check(bool condition, const char *message)
	{
		if (!condition)
			std::fprintf(stderr, "FAIL: %s\n", message);
		return condition;
	}

	std::vector<float> MakeTone(uint32_t sampleRate, float frequency, float durationSeconds)
	{
		const size_t frames = static_cast<size_t>(sampleRate * durationSeconds);
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
			samples[i] = 0.5f * static_cast<float>(
			std::sin(2.0 * 3.14159265358979323846 * frequency * i / sampleRate));
		return samples;
	}

	bool EstimateToneFrequency(const std::vector<float> &stereo, uint32_t sampleRate,
		uint64_t frames, double &frequencyHz)
	{
		frequencyHz = 0.0;
		if (sampleRate == 0 || frames < sampleRate / 20 || frames > stereo.size() / 2)
			return false;

		double channelEnergy[2] = { 0.0, 0.0 };
		for (uint64_t frame = 0; frame < frames; ++frame)
		{
			const float left = stereo[static_cast<size_t>(frame) * 2];
			const float right = stereo[static_cast<size_t>(frame) * 2 + 1];
			channelEnergy[0] += static_cast<double>(left) * left;
			channelEnergy[1] += static_cast<double>(right) * right;
		}
		const size_t channel = channelEnergy[1] > channelEnergy[0] ? 1 : 0;
		float peakRms = 0.0f;
		const size_t windowFrames = (std::max)(size_t(1), static_cast<size_t>(sampleRate / 100));
		for (uint64_t start = 0; start < frames; start += windowFrames)
		{
			const size_t count = static_cast<size_t>((std::min)(
				static_cast<uint64_t>(windowFrames), frames - start));
			double energy = 0.0;
			for (size_t i = 0; i < count; ++i)
			{
				const float sample = stereo[(static_cast<size_t>(start) + i) * 2 + channel];
				energy += static_cast<double>(sample) * sample;
			}
			peakRms = (std::max)(peakRms, static_cast<float>(std::sqrt(energy / count)));
		}
		if (peakRms < 1.0e-5f)
			return false;

		const float activityThreshold = (std::max)(1.0e-6f, peakRms * 0.15f);
		size_t activeStart = static_cast<size_t>(frames);
		size_t activeEnd = 0;
		for (uint64_t start = 0; start < frames; start += windowFrames)
		{
			const size_t count = static_cast<size_t>((std::min)(
				static_cast<uint64_t>(windowFrames), frames - start));
			double energy = 0.0;
			for (size_t i = 0; i < count; ++i)
			{
				const float sample = stereo[(static_cast<size_t>(start) + i) * 2 + channel];
				energy += static_cast<double>(sample) * sample;
			}
			if (std::sqrt(energy / count) >= activityThreshold)
			{
				activeStart = (std::min)(activeStart, static_cast<size_t>(start));
				activeEnd = static_cast<size_t>(start) + count;
			}
		}
		if (activeStart >= activeEnd)
			return false;

		const size_t activeFrames = activeEnd - activeStart;
		std::vector<double> active(activeFrames);
		double mean = 0.0;
		for (size_t i = 0; i < activeFrames; ++i)
		{
			active[i] = stereo[(activeStart + i) * 2 + channel];
			mean += active[i];
		}
		mean /= activeFrames;
		for (double &sample : active)
			sample -= mean;

		const size_t minLag = (std::max)(size_t(2), static_cast<size_t>(sampleRate / 5000));
		const size_t maxLag = (std::min)(static_cast<size_t>(sampleRate / 40), activeFrames / 4);
		if (maxLag <= minLag + 2)
			return false;
		std::vector<double> correlations(maxLag + 1, 0.0);
		for (size_t lag = minLag; lag <= maxLag; ++lag)
		{
			double product = 0.0;
			double leftEnergy = 0.0;
			double rightEnergy = 0.0;
			for (size_t i = 0; i + lag < activeFrames; ++i)
			{
				const double left = active[i];
				const double right = active[i + lag];
				product += left * right;
				leftEnergy += left * left;
				rightEnergy += right * right;
			}
			const double normalizer = std::sqrt(leftEnergy * rightEnergy);
			correlations[lag] = normalizer > 0.0 ? product / normalizer : 0.0;
		}

		// Pick the first strong autocorrelation peak, not a later integer multiple
		// of the period, which would report a subharmonic.
		size_t bestLag = 0;
		for (size_t lag = minLag + 1; lag < maxLag; ++lag)
		{
			if (correlations[lag] >= 0.5 && correlations[lag] >= correlations[lag - 1] &&
				correlations[lag] >= correlations[lag + 1])
			{
				bestLag = lag;
				break;
			}
		}
		if (bestLag == 0)
			return false;

		const double left = correlations[bestLag - 1];
		const double center = correlations[bestLag];
		const double right = correlations[bestLag + 1];
		const double curvature = left - 2.0 * center + right;
		const double offset = std::abs(curvature) > 1.0e-12
			? 0.5 * (left - right) / curvature : 0.0;
		const double period = static_cast<double>(bestLag) + (std::max)(-1.0, (std::min)(1.0, offset));
		frequencyHz = sampleRate / period;
		return true;
	}

	bool CheckFrequency(const std::vector<float> &stereo, uint32_t sampleRate, uint64_t frames)
	{
		double measured = 0.0;
		const bool estimated = EstimateToneFrequency(stereo, sampleRate, frames, measured);
		if (!estimated || std::abs(measured - 997.0) >= 2.0)
			std::fprintf(stderr, "MEASURED_TONE_HZ=%.3f frames=%llu sample_rate=%u\n",
				measured, static_cast<unsigned long long>(frames), sampleRate);
		return Check(estimated && std::abs(measured - 997.0) < 2.0,
			"a 997 Hz source tone must remain 997 Hz at every source/endpoint rate");
	}

	std::vector<float> MakePaddedNoisyTone(uint32_t sampleRate, float frequency)
	{
		const size_t frames = sampleRate;
		const size_t toneStart = sampleRate / 5;
		const size_t toneFrames = sampleRate / 2;
		std::vector<float> stereo(frames * 2, 0.0f);
		uint32_t noiseState = 0xC001D00Du;
		for (size_t frame = toneStart; frame < toneStart + toneFrames; ++frame)
		{
			noiseState = noiseState * 1664525u + 1013904223u;
			const float noise = static_cast<float>((noiseState >> 8) & 0x00FFFFFFu) /
				8388607.5f - 1.0f;
			const double phase = 2.0 * 3.14159265358979323846 * frequency *
				(frame - toneStart) / sampleRate;
			const float sample = 0.5f * static_cast<float>(std::sin(phase)) + 0.04f * noise;
			stereo[frame * 2] = sample;
			stereo[frame * 2 + 1] = sample;
		}
		return stereo;
	}

	std::vector<float> MakeStereo(const std::vector<float> &mono)
	{
		std::vector<float> stereo(mono.size() * 2);
		for (size_t frame = 0; frame < mono.size(); ++frame)
		{
			stereo[frame * 2] = mono[frame];
			stereo[frame * 2 + 1] = mono[frame];
		}
		return stereo;
	}

	std::vector<float> MakeStereoToneSignal(uint32_t sampleRate, uint64_t frames,
		double frequencyHz, double peakAmplitude, double phaseRadians)
	{
		std::vector<float> stereo(static_cast<size_t>(frames) * 2);
		for (uint64_t frame = 0; frame < frames; ++frame)
		{
			const float sample = static_cast<float>(peakAmplitude * std::sin(
				2.0 * 3.14159265358979323846 * frequencyHz * frame / sampleRate + phaseRadians));
			stereo[static_cast<size_t>(frame) * 2] = sample;
			stereo[static_cast<size_t>(frame) * 2 + 1] = sample;
		}
		return stereo;
	}

	std::vector<float> MakeInterferedTargetToneSignal(uint32_t sampleRate, uint64_t frames,
		double interferingPeakAmplitude = 0.003)
	{
		std::vector<float> stereo(static_cast<size_t>(frames) * 2);
		uint32_t noiseState = 0x51A7E123u;
		for (uint64_t frame = 0; frame < frames; ++frame)
		{
			noiseState = noiseState * 1664525u + 1013904223u;
			const double noise = static_cast<double>((noiseState >> 8) & 0x00FFFFFFu) /
				8388607.5 - 1.0;
			const double time = static_cast<double>(frame) / sampleRate;
			const float sample = static_cast<float>(
				0.005 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * time + 1.2345) +
				interferingPeakAmplitude * std::sin(2.0 * 3.14159265358979323846 * 1035.0 * time - 0.731) +
				0.0005 * noise);
			stereo[static_cast<size_t>(frame) * 2] = sample;
			stereo[static_cast<size_t>(frame) * 2 + 1] = sample;
		}
		return stereo;
	}

	bool TestTargetToneMeasurementFindsLowGainArbitraryPhaseTone()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const auto stereo = MakeStereoToneSignal(sampleRate, frames, 997.0, 0.005, 1.2345);
		ToneMeasurement measurement{};
		const bool measured = MeasureTargetTone(stereo, sampleRate, frames, 997.0, measurement);
		return Check(measured && std::abs(measurement.TargetAmplitude - 0.005) < 0.0001 &&
			measurement.TargetCoherence > 0.98 && std::abs(measurement.Rms - 0.005 / std::sqrt(2.0)) < 0.00005,
			"target-tone measurement recovers a low-gain 997 Hz tone independent of phase");
	}

	bool TestTargetToneMeasurementReportsSilence()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const std::vector<float> silence(static_cast<size_t>(frames) * 2, 0.0f);
		ToneMeasurement measurement{};
		const bool measured = MeasureTargetTone(silence, sampleRate, frames, 997.0, measurement);
		return Check(measured && measurement.TargetAmplitude < 1.0e-7 &&
			measurement.TargetCoherence < 1.0e-7 && measurement.Rms < 1.0e-7,
			"target-tone measurement reports negligible amplitude and coherence for silence");
	}

	bool TestTargetToneMeasurementRejectsNearbyWrongFrequency()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const auto stereo = MakeStereoToneSignal(sampleRate, frames, 1035.0, 0.005, 0.417);
		ToneMeasurement measurement{};
		const bool measured = MeasureTargetTone(stereo, sampleRate, frames, 997.0, measurement);
		return Check(measured && measurement.TargetAmplitude < 0.0001 &&
			measurement.TargetCoherence < 0.01 && std::abs(measurement.Rms - 0.005 / std::sqrt(2.0)) < 0.00005,
			"a 1035 Hz-only signal has low coherent amplitude at the 997 Hz target");
	}

	bool TestTargetToneMeasurementSeparatesTargetFromNoiseAndInterference()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const auto stereo = MakeInterferedTargetToneSignal(sampleRate, frames);
		ToneMeasurement measurement{};
		const bool measured = MeasureTargetTone(stereo, sampleRate, frames, 997.0, measurement);
		return Check(measured && std::abs(measurement.TargetAmplitude - 0.005) < 0.0002 &&
			measurement.TargetCoherence > 0.6 && measurement.TargetCoherence < 0.95 &&
			measurement.Rms > 0.005 / std::sqrt(2.0),
			"target-tone measurement retains the 997 Hz component amid deterministic noise and interference");
	}

	bool TestCaptureAcceptanceRequiresTargetAbovePreToneBaseline()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const auto baselineSignal = MakeStereoToneSignal(sampleRate, frames, 1035.0, 0.003, 0.417);
		const auto targetSignal = MakeInterferedTargetToneSignal(sampleRate, frames);
		const auto contaminantSignal = MakeStereoToneSignal(sampleRate, frames, 1035.0, 0.005, 1.2345);
		ToneMeasurement baseline{};
		ToneMeasurement target{};
		ToneMeasurement contaminant{};
		const bool measured = MeasureTargetTone(baselineSignal, sampleRate, frames, 997.0, baseline) &&
			MeasureTargetTone(targetSignal, sampleRate, frames, 997.0, target) &&
			MeasureTargetTone(contaminantSignal, sampleRate, frames, 997.0, contaminant);
		return Check(measured && target.TargetAmplitude > baseline.TargetAmplitude &&
			AcceptTargetTone(target, baseline) && !AcceptTargetTone(contaminant, baseline),
			"capture acceptance requires the low-gain 997 Hz tone to emerge above baseline and rejects 1035 Hz-only interference");
	}

	bool TestCaptureAcceptanceSurvivesLouderOffTargetTone()
	{
		constexpr uint32_t sampleRate = 48000;
		constexpr uint64_t frames = 48000;
		const auto baselineSignal = MakeStereoToneSignal(sampleRate, frames, 1035.0, 0.003, 0.417);
		const auto targetSignal = MakeInterferedTargetToneSignal(sampleRate, frames, 0.05);
		ToneMeasurement baseline{};
		ToneMeasurement target{};
		const bool measured = MeasureTargetTone(baselineSignal, sampleRate, frames, 997.0, baseline) &&
			MeasureTargetTone(targetSignal, sampleRate, frames, 997.0, target);
		return Check(measured && target.TargetAmplitude > 0.004 &&
			AcceptTargetTone(target, baseline),
			"capture acceptance follows the coherent 997 Hz target even when a louder off-target tone is present");
	}

	bool TestToneMeasurementHandlesPaddingAndNoise()
	{
		const auto stereo = MakePaddedNoisyTone(48000, 997.0f);
		double measured = 0.0;
		const bool estimated = EstimateToneFrequency(stereo, 48000, 48000, measured);
		return Check(estimated && std::abs(measured - 997.0) < 2.0,
			"the tone estimator finds 997 Hz through leading/trailing silence and noise");
	}

	bool TestToneMeasurementRejectsSilence()
	{
		const std::vector<float> silence(48000 * 2, 0.0f);
		double measured = 0.0;
		return Check(!EstimateToneFrequency(silence, 48000, 48000, measured),
			"a silent capture must be rejected instead of reporting a tone");
	}

	bool TestToneMeasurementRejectsWrongFrequencyControl()
	{
		const auto stereo = MakeStereo(MakeTone(48000, 542.0f, 1.0f));
		double measured = 0.0;
		const bool estimated = EstimateToneFrequency(stereo, 48000, 48000, measured);
		return Check(estimated && std::abs(measured - 542.0) < 2.0 &&
			std::abs(measured - 997.0) >= 2.0,
			"the 542 Hz negative control must be estimated separately from 997 Hz");
	}

	bool TestRateMatrix()
	{
		bool passed = true;
		for (const uint32_t clipRate : {44100u, 48000u})
		{
			for (const uint32_t endpointRate : {44100u, 48000u})
			{
				Mixer mixer;
				auto clip = AudioClip::FromSamples(MakeTone(clipRate, 997.0f, 1.0f), static_cast<int>(clipRate));
				mixer.RegisterClip("tone", clip);
				PlayRequest request;
				request.ClipName = "tone";
				const int voiceId = mixer.Play(request);
				passed &= Check(voiceId >= 0, "a valid source clip must start playing");

				std::vector<float> engineMix(static_cast<size_t>(Mixer::kEngineMixRate) * 2, 0.0f);
				bool callbackAccepted = true;
				uint32_t callbackOffset = 0;
				uint32_t callbackIndex = 0;
				while (callbackOffset < static_cast<uint32_t>(Mixer::kEngineMixRate))
				{
					const uint32_t blockSize = (callbackIndex++ % 2 == 0) ? 480u : 512u;
					const uint32_t remaining = static_cast<uint32_t>(Mixer::kEngineMixRate) - callbackOffset;
					const uint32_t frames = remaining < blockSize ? remaining : blockSize;
					callbackAccepted = AudioDevice::Render(mixer,
						engineMix.data() + static_cast<size_t>(callbackOffset) * 2, frames,
						AudioDevice::OutputFormat::Float32, 2, Mixer::kEngineMixRate) && callbackAccepted;
					callbackOffset += frames;
				}
				passed &= Check(callbackAccepted,
					"variable callback blocks accept the fixed 48 kHz float stereo format");

				const auto &voices = mixer.GetVoices();
				passed &= Check(voices.size() == 1, "the rendered voice slot remains inspectable");
				if (!voices.empty())
				{
					passed &= Check(std::abs(voices[0].FramePosition - clipRate) < 1.0,
						"one second of 48 kHz output consumes exactly one second at source rate");
					if (mixer.IsPlaying(voiceId))
						std::fprintf(stderr, "live after one second: clip=%u endpoint=%u source-position=%.17g\n",
							clipRate, endpointRate, voices[0].FramePosition);
					passed &= Check(!mixer.IsPlaying(voiceId),
						"a one-second clip retires after one second of engine output");
				}

				ma_data_converter_config converterConfig = ma_data_converter_config_init(
					ma_format_f32, ma_format_f32, 2, 2,
					Mixer::kEngineMixRate, endpointRate);
				ma_data_converter converter;
				const bool converterReady = ma_data_converter_init(
					&converterConfig, nullptr, &converter) == MA_SUCCESS;
				passed &= Check(converterReady,
					"miniaudio's device converter initializes for the requested endpoint rate");
				if (converterReady)
				{
					ma_uint64 expectedFrames = 0;
					passed &= Check(ma_data_converter_get_expected_output_frame_count(
						&converter, Mixer::kEngineMixRate, &expectedFrames) == MA_SUCCESS,
						"miniaudio reports the endpoint frame count for one second of mix audio");
					std::vector<float> endpoint(static_cast<size_t>(expectedFrames + 1024) * 2, 0.0f);
					ma_uint64 inputFrames = Mixer::kEngineMixRate;
					ma_uint64 outputFrames = expectedFrames + 1024;
					const ma_result convertResult = ma_data_converter_process_pcm_frames(
						&converter, engineMix.data(), &inputFrames, endpoint.data(), &outputFrames);
					passed &= Check(convertResult == MA_SUCCESS, "miniaudio converts the mix to the endpoint sample rate");
					passed &= Check(inputFrames == Mixer::kEngineMixRate,
						"the endpoint converter consumes the complete one-second engine mix");
					passed &= Check(outputFrames + 64 >= endpointRate && outputFrames <= endpointRate + 64,
						"the endpoint converter preserves the one-second duration within resampler latency");
					if (outputFrames > 0)
						passed &= CheckFrequency(endpoint, endpointRate, outputFrames);
					ma_data_converter_uninit(&converter, nullptr);
				}
			}
		}
		return passed;
	}

	bool TestInvalidCallbackFormatsAreSilent()
	{
		bool passed = true;
		Mixer mixer;
		mixer.RegisterClip("tone", AudioClip::FromSamples(MakeTone(48000, 440.0f, 0.25f), 48000));
		PlayRequest request;
		request.ClipName = "tone";
		const int voiceId = mixer.Play(request);

		std::vector<float> wrongRate(32, 1.0f);
		passed &= Check(!AudioDevice::Render(mixer, wrongRate.data(), 16,
			AudioDevice::OutputFormat::Float32, 2, 44100),
			"a callback with a non-engine sample rate is rejected");
		for (float sample : wrongRate)
			passed &= Check(sample == 0.0f, "a rejected float callback writes exact silence");

		std::vector<int16_t> wrongFormat(32, 1234);
		passed &= Check(!AudioDevice::Render(mixer, wrongFormat.data(), 16,
			AudioDevice::OutputFormat::Signed16, 2, Mixer::kEngineMixRate),
			"a non-float callback format is rejected");
		for (int16_t sample : wrongFormat)
			passed &= Check(sample == 0, "a rejected signed PCM callback writes exact silence");

		passed &= Check(mixer.IsPlaying(voiceId), "a rejected callback does not consume voice time");
		const auto &voices = mixer.GetVoices();
		passed &= Check(!voices.empty() && voices[0].FramePosition == 0.0,
			"format validation occurs before touching mixer read positions");
		return passed;
	}

	struct LoopbackCapture
	{
		std::vector<float> Samples;
		std::atomic<size_t> WrittenSamples{ 0 };
		std::atomic<uint32_t> CallbackCount{ 0 };
		std::atomic<uint32_t> CallbackSampleRate{ 0 };
		std::atomic<uint32_t> CaptureChannels{ 0 };
		std::atomic<uint32_t> InternalSampleRate{ 0 };
	};

	void LoopbackDataCallback(ma_device* device, void*, const void* input, ma_uint32 frameCount)
	{
		auto* capture = static_cast<LoopbackCapture*>(device ? device->pUserData : nullptr);
		if (!capture || !input || !device || device->capture.channels == 0)
			return;
		capture->CallbackSampleRate.store(device->sampleRate, std::memory_order_relaxed);
		capture->CaptureChannels.store(device->capture.channels, std::memory_order_relaxed);
		capture->InternalSampleRate.store(device->capture.internalSampleRate, std::memory_order_relaxed);
		const size_t sampleCount = static_cast<size_t>(frameCount) * device->capture.channels;
		const size_t offset = capture->WrittenSamples.load(std::memory_order_relaxed);
		const size_t available = offset < capture->Samples.size() ? capture->Samples.size() - offset : 0;
		const size_t copyCount = (std::min)(sampleCount, available);
		if (copyCount > 0)
			std::copy_n(static_cast<const float*>(input), copyCount, capture->Samples.data() + offset);
		capture->WrittenSamples.store(offset + copyCount, std::memory_order_release);
		capture->CallbackCount.fetch_add(1, std::memory_order_relaxed);
	}

	bool TestDefaultDeviceLoopback()
	{
		Mixer mixer;
		mixer.RegisterClip("hardware_loopback_tone",
		AudioClip::FromSamples(MakeTone(Mixer::kEngineMixRate, 997.0f, 1.5f), Mixer::kEngineMixRate));
		PlayRequest request;
		request.ClipName = "hardware_loopback_tone";
		request.Volume = 0.01f;

		AudioDevice playback(mixer);
		std::string error;
		if (!playback.Start(&error))
		{
			std::fprintf(stderr, "NATIVE_AUDIO_UNAVAILABLE: %s\n", error.c_str());
			return false;
		}
		const uint32_t endpointRate = playback.GetEndpointSampleRate();
		if (!Check(playback.IsRunning() && endpointRate > 0,
			"AudioDevice opens a native endpoint and reports its negotiated sample rate"))
		{
			playback.Stop();
			return false;
		}

		constexpr ma_uint32 loopbackProcessID = 0;
		ma_context wasapiContext{};
		const ma_backend wasapiBackend = ma_backend_wasapi;
		ma_result result = ma_context_init(&wasapiBackend, 1, nullptr, &wasapiContext);
		if (result != MA_SUCCESS)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_UNAVAILABLE: WASAPI context initialization failed loopback_process_id=%u result=%s\n",
				loopbackProcessID, ma_result_description(result));
			playback.Stop();
			return false;
		}

		LoopbackCapture capture;
		ma_device_config config = ma_device_config_init(ma_device_type_loopback);
		config.capture.format = ma_format_f32;
		config.capture.channels = 2;
		config.capture.pDeviceID = nullptr;
		config.sampleRate = Mixer::kEngineMixRate;
		config.wasapi.loopbackProcessID = loopbackProcessID;
		config.wasapi.loopbackProcessExclude = MA_FALSE;
		config.dataCallback = &LoopbackDataCallback;
		config.pUserData = &capture;
		ma_device loopback{};
		result = ma_device_init(&wasapiContext, &config, &loopback);
		if (result != MA_SUCCESS)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_UNAVAILABLE: loopback_process_id=%u result=%s\n",
				loopbackProcessID, ma_result_description(result));
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		if (loopback.sampleRate == 0 || loopback.capture.channels != 2)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_UNSUPPORTED: callback_rate=%u capture_channels=%u internal_rate=%u loopback_process_id=%u\n",
				loopback.sampleRate, loopback.capture.channels, loopback.capture.internalSampleRate, loopbackProcessID);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		capture.Samples.resize(static_cast<size_t>(loopback.sampleRate) * 3 * loopback.capture.channels);
		result = ma_device_start(&loopback);
		if (result != MA_SUCCESS)
		{
			std::fprintf(stderr, "NATIVE_GLOBAL_LOOPBACK_START_FAILED: loopback_process_id=%u result=%s\n",
				loopbackProcessID, ma_result_description(result));
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		for (unsigned int attempt = 0; attempt < 100 &&
			capture.CallbackCount.load(std::memory_order_relaxed) < 20; ++attempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (!Check(capture.CallbackCount.load(std::memory_order_relaxed) >= 20,
			"WASAPI loopback callback is running before the reference tone is queued"))
		{
			ma_device_stop(&loopback);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		const size_t toneStartSample = capture.WrittenSamples.load(std::memory_order_acquire);
		const uint32_t callbackSampleRate = capture.CallbackSampleRate.load(std::memory_order_relaxed);
		const uint32_t captureChannels = capture.CaptureChannels.load(std::memory_order_relaxed);
		const uint32_t internalSampleRate = capture.InternalSampleRate.load(std::memory_order_relaxed);
		if (callbackSampleRate == 0 || captureChannels != 2)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_UNSUPPORTED: callback_rate=%u capture_channels=%u internal_rate=%u loopback_process_id=%u\n",
				callbackSampleRate, captureChannels, internalSampleRate, loopbackProcessID);
			ma_device_stop(&loopback);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		const size_t toneStartFrame = toneStartSample / captureChannels;
		const size_t baselineFrames = callbackSampleRate / 5;
		if (baselineFrames == 0 || toneStartSample % captureChannels != 0 ||
			toneStartFrame < baselineFrames)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_UNSUPPORTED: callback_rate=%u capture_channels=%u internal_rate=%u pre_tone_frames=%zu loopback_process_id=%u\n",
				callbackSampleRate, captureChannels, internalSampleRate, toneStartFrame, loopbackProcessID);
			ma_device_stop(&loopback);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		const size_t baselineStartFrame = toneStartFrame - baselineFrames;
		std::vector<float> baselineWindow(
			capture.Samples.begin() + baselineStartFrame * captureChannels,
			capture.Samples.begin() + toneStartSample);
		ToneMeasurement baseline{};
		if (!MeasureTargetTone(baselineWindow, callbackSampleRate, baselineFrames, 997.0, baseline))
		{
			std::fprintf(stderr, "NATIVE_GLOBAL_LOOPBACK_BASELINE_INVALID: callback_rate=%u loopback_process_id=%u\n",
				callbackSampleRate, loopbackProcessID);
			ma_device_stop(&loopback);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}
		if (!Check(mixer.Play(request) >= 0, "hardware loopback test queues its reference tone after capture starts"))
		{
			ma_device_stop(&loopback);
			ma_device_uninit(&loopback);
			ma_context_uninit(&wasapiContext);
			playback.Stop();
			return false;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		const ma_result stopResult = ma_device_stop(&loopback);
		ma_device_uninit(&loopback);
		ma_context_uninit(&wasapiContext);
		playback.Stop();
		if (!Check(stopResult == MA_SUCCESS, "WASAPI loopback capture stops cleanly"))
			return false;

		const size_t capturedSamples = capture.WrittenSamples.load(std::memory_order_acquire);
		const size_t capturedFrames = capturedSamples / captureChannels;
		if (!Check(capture.CallbackCount.load(std::memory_order_relaxed) > 0 &&
			capturedSamples % captureChannels == 0 && capturedFrames > toneStartFrame,
			"WASAPI default-device loopback callback captures post-queue audio"))
			return false;

		const size_t windowFrames = (std::max)(size_t(1), static_cast<size_t>(callbackSampleRate / 5));
		const size_t stepFrames = (std::max)(size_t(1), static_cast<size_t>(callbackSampleRate / 20));
		const TargetToneAcceptanceThresholds thresholds = GetTargetToneAcceptanceThresholds(baseline);
		bool targetToneFound = false;
		ToneMeasurement strongest{};
		ToneMeasurement strongestPassing{};
		for (size_t startFrame = toneStartFrame;
			startFrame <= capturedFrames && windowFrames <= capturedFrames - startFrame;
			startFrame += stepFrames)
		{
			const size_t firstSample = startFrame * captureChannels;
			const size_t sampleCount = windowFrames * captureChannels;
			std::vector<float> window(capture.Samples.begin() + firstSample,
				capture.Samples.begin() + firstSample + sampleCount);
			ToneMeasurement measurement{};
			if (!MeasureTargetTone(window, callbackSampleRate, windowFrames, 997.0, measurement))
				continue;
			if (measurement.TargetAmplitude > strongest.TargetAmplitude)
				strongest = measurement;
			if (AcceptTargetTone(measurement, baseline))
			{
				targetToneFound = true;
				if (measurement.TargetAmplitude > strongestPassing.TargetAmplitude)
					strongestPassing = measurement;
			}
		}
		if (!Check(!playback.IsRunning() && playback.GetEndpointSampleRate() == 0,
			"stopping the native endpoint clears its running state and negotiated rate"))
			return false;
		if (!targetToneFound)
		{
			std::fprintf(stderr,
				"NATIVE_GLOBAL_LOOPBACK_TONE_NOT_FOUND: loopback_process_id=%u endpoint_rate=%u callback_rate=%u capture_channels=%u internal_rate=%u captured_frames=%zu baseline_amplitude=%.8f baseline_coherence=%.6f strongest_amplitude=%.8f strongest_coherence=%.6f required_amplitude=%.8f\n",
				loopbackProcessID, endpointRate, callbackSampleRate, captureChannels,
				internalSampleRate, capturedFrames, baseline.TargetAmplitude, baseline.TargetCoherence,
				strongest.TargetAmplitude, strongest.TargetCoherence, thresholds.Amplitude);
			return false;
		}
		std::printf("NATIVE_ENDPOINT_LOOPBACK_OK loopback_process_id=%u negotiated_rate=%u callback_rate=%u capture_channels=%u internal_rate=%u captured_frames=%zu baseline_amplitude=%.8f baseline_coherence=%.6f target_amplitude=%.8f target_coherence=%.6f\n",
			loopbackProcessID, endpointRate, callbackSampleRate, captureChannels,
			internalSampleRate, capturedFrames, baseline.TargetAmplitude, baseline.TargetCoherence,
			strongestPassing.TargetAmplitude, strongestPassing.TargetCoherence);
		return true;
	}
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--hardware-loopback") == 0)
		return TestDefaultDeviceLoopback() ? 0 : 1;
	bool passed = Check(Mixer::kEngineMixRate == 48000,
		"the engine-facing audio mix rate is fixed at 48 kHz");
	passed &= TestTargetToneMeasurementFindsLowGainArbitraryPhaseTone();
	passed &= TestTargetToneMeasurementReportsSilence();
	passed &= TestTargetToneMeasurementRejectsNearbyWrongFrequency();
	passed &= TestTargetToneMeasurementSeparatesTargetFromNoiseAndInterference();
	passed &= TestCaptureAcceptanceRequiresTargetAbovePreToneBaseline();
	passed &= TestCaptureAcceptanceSurvivesLouderOffTargetTone();
	passed &= TestToneMeasurementHandlesPaddingAndNoise();
	passed &= TestToneMeasurementRejectsSilence();
	passed &= TestToneMeasurementRejectsWrongFrequencyControl();
	passed &= TestRateMatrix();
	passed &= TestInvalidCallbackFormatsAreSilent();
	if (!passed)
		return 1;
	std::puts("Audio output rate-conversion tests passed.");
	return 0;
}
