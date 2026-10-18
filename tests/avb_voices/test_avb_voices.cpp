#include "Wasteland/Avb/AvbVoices.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <vector>

namespace
{
	bool g_TrackAllocations = false;
	std::size_t g_TrackedAllocations = 0;
}

void* operator new(std::size_t size)
{
	if (g_TrackAllocations)
		++g_TrackedAllocations;
	if (void* memory = std::malloc(size ? size : 1))
		return memory;
	throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory) noexcept
{
	::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
	::operator delete(memory);
}

using namespace Wasteland::Avb::Dsp;

namespace
{
	int g_Checks = 0;
	int g_Failures = 0;
	constexpr std::uint32_t kRate = 48000;
	constexpr float kPi = 3.14159265358979323846f;

	void Check(bool condition, const char* message)
	{
		++g_Checks;
		if (!condition)
		{
			++g_Failures;
			std::printf("  FAIL: %s\n", message);
		}
	}

	bool AllFinite(const float* samples, std::size_t count)
	{
		for (std::size_t index = 0; index < count; ++index)
			if (!std::isfinite(samples[index]))
				return false;
		return true;
	}

	float ProjectionMagnitude(const std::vector<float>& samples, std::size_t start,
	                         std::size_t count, float frequency)
	{
		double real = 0.0;
		double imaginary = 0.0;
		for (std::size_t index = 0; index < count; ++index)
		{
			const double phase = 2.0 * static_cast<double>(kPi) * frequency * index / kRate;
			const double sample = samples[start + index];
			real += sample * std::cos(phase);
			imaginary -= sample * std::sin(phase);
		}
		return static_cast<float>(2.0 * std::sqrt(real * real + imaginary * imaginary) / count);
	}

	float PeakFrequency(const std::vector<float>& samples, std::size_t start,
	                    std::size_t count, float low, float high, float step)
	{
		float bestFrequency = low;
		float bestMagnitude = -1.0f;
		for (float frequency = low; frequency <= high; frequency += step)
		{
			const float magnitude = ProjectionMagnitude(samples, start, count, frequency);
			if (magnitude > bestMagnitude)
			{
				bestMagnitude = magnitude;
				bestFrequency = frequency;
			}
		}
		return bestFrequency;
	}

	float NormalizedCorrelation(const std::vector<float>& samples, std::size_t start,
	                           std::size_t count, std::size_t lag)
	{
		double product = 0.0;
		double energyA = 0.0;
		double energyB = 0.0;
		for (std::size_t index = 0; index < count; ++index)
		{
			const double a = samples[start + index];
			const double b = samples[start + index + lag];
			product += a * b;
			energyA += a * a;
			energyB += b * b;
		}
		const double denominator = std::sqrt(energyA * energyB);
		return denominator > 0.0 ? static_cast<float>(product / denominator) : 0.0f;
	}

	std::vector<float> RenderModal(const ModalStrikeParameters& parameters,
	                               std::uint32_t seed, std::size_t frames)
	{
		ModalStrike voice(kRate);
		voice.SetParameters(parameters);
		voice.Trigger(seed);
		std::vector<float> output(frames);
		voice.Process(output.data(), output.size());
		return output;
	}

	std::vector<float> RenderComb(const CombBankParameters& parameters,
	                              const std::vector<float>& input)
	{
		CombBank bank(kRate, 0.5f);
		bank.SetParameters(parameters);
		std::vector<float> output(input.size());
		bank.Process(input.data(), output.data(), output.size());
		return output;
	}

	void TestRateAndModalStrike()
	{
		for (const std::uint32_t rate : {44100u, 48000u, 96000u})
		{
			ModalStrike voice(rate);
			ModalStrikeParameters parameters;
			parameters.F1Hz = 420.0f;
			voice.SetParameters(parameters);
			voice.Trigger(0x1234ABCDu);
			float output[256] = {};
			voice.Process(output, 256);
			Check(AllFinite(output, 256), "ModalStrike remains finite at supported sample rates");
			for (float sample : output)
				Check(std::fabs(sample) <= 16.0f, "ModalStrike output stays inside its finite safety bound");
		}

		ModalStrikeParameters parameters;
		parameters.F1Hz = 420.0f;
		parameters.ModeCount = 14;
		parameters.T60Seconds = 0.9f;
		parameters.Damping = 0.35f;
		parameters.StrikePosition = 0.25f;
		const auto first = RenderModal(parameters, 0xDEADBEEFu, 12000);
		const auto repeated = RenderModal(parameters, 0xDEADBEEFu, 12000);
		Check(first == repeated, "same ModalStrike parameters and seed render bit-identical buffers");
		Check(AllFinite(first.data(), first.size()), "seeded modal strike has no NaN or infinite samples");
		Check(ProjectionMagnitude(first, 200, 4800, 420.0f) > 1.0e-4f,
		      "ModalStrike excitation reaches the fundamental in the real output buffer");

		ModalStrike inspection(kRate);
		inspection.SetParameters(parameters);
		Check(std::fabs(inspection.ModeFrequencyHz(1) / inspection.ModeFrequencyHz(0) - 2.756f) < 0.01f,
		      "free-free bar mode table preserves its inharmonic second-to-first ratio");
		Check(inspection.ModeT60Seconds(1) < inspection.ModeT60Seconds(0),
		      "Rayleigh damping makes the second mode decay faster than the fundamental");
		const float expectedSecondModeGain =
			std::fabs(std::sin(kPi * 2.0f * parameters.StrikePosition)) /
			std::sqrt(parameters.Ratios[1]) * parameters.Amplitude;
		Check(std::fabs(inspection.ModeExcitationGain(1) - expectedSecondModeGain) < 1.0e-6f,
		      "per-mode excitation gain follows strike-position weighting divided by sqrt(frequency ratio)");
		Check(inspection.ModeFrequencyHz(24) == 0.0f && inspection.ModeT60Seconds(24) == 0.0f,
		      "out-of-range modal inspection is safely empty");

		const float fundamentalPeak = PeakFrequency(first, 200, 4800, 390.0f, 450.0f, 2.0f);
		const float secondPeak = PeakFrequency(first, 200, 4800, 1080.0f, 1240.0f, 2.0f);
		Check(std::fabs(fundamentalPeak - 420.0f) < 18.0f,
		      "measured modal spectrum peaks at the authored fundamental frequency");
		Check(std::fabs(secondPeak / fundamentalPeak - 2.756f) < 0.05f,
		      "measured second spectral peak is inharmonic, not an integer harmonic");
		const float fundamentalMagnitude = ProjectionMagnitude(first, 200, 2400, 420.0f);
		const float upperMagnitude = ProjectionMagnitude(first, 200, 2400, 420.0f * 5.404f);
		Check(upperMagnitude < fundamentalMagnitude,
		      "modal amplitude weighting and decay reduce the upper inharmonic mode");

		ModalStrikeParameters centreParameters = parameters;
		centreParameters.StrikePosition = 0.5f;
		const auto centre = RenderModal(centreParameters, 0xDEADBEEFu, 12000);
		Check(centre != first,
		      "centre and off-centre strikes produce distinct spectra from the same mode table");
		Check(ProjectionMagnitude(centre, 200, 2400, 420.0f * 2.756f) <
		      ProjectionMagnitude(first, 200, 2400, 420.0f * 2.756f),
		      "the strike-position node pattern suppresses the second mode at centre strike");
	}

	void TestCombBank()
	{
		for (const std::uint32_t rate : {44100u, 48000u})
		{
			CombBank bank(rate, 0.25f);
			CombBankParameters parameters;
			parameters.Feedback = 4.0f;
			bank.SetParameters(parameters);
			Check(bank.Feedback() <= 0.99f, "CombBank guards feedback at the stable upper bound");
			float output[512] = {};
			bank.Process(nullptr, output, 512);
			Check(AllFinite(output, 512), "CombBank remains finite at 44.1 and 48 kHz");
		}

		std::vector<float> input(28000);
		Noise noise(kRate, 0xCAFEBABEu);
		noise.Process(input.data(), input.size());
		CombBankParameters irrational;
		irrational.FundamentalHz = 310.0f;
		irrational.CombCount = 4;
		irrational.Feedback = 0.70f;
		irrational.LoopLpHz = 9000.0f;
		irrational.OutputGain = 0.3f;
		const auto inharmonic = RenderComb(irrational, input);
		CombBankParameters rational = irrational;
		rational.Ratios.fill(1.0f);
		const auto periodic = RenderComb(rational, input);
		Check(AllFinite(inharmonic.data(), inharmonic.size()), "irrational comb bank processes finite broadband audio");
		const float rationalLag = NormalizedCorrelation(periodic, 5000, 12000, 155);
		const float irrationalLag = NormalizedCorrelation(inharmonic, 5000, 12000, 155);
		Check(rationalLag > irrationalLag + 0.02f,
		      "irrational comb ratios weaken the shared periodic lag of a rational bank");
		float maximumCorrelation = 0.0f;
		for (std::size_t lag = 4; lag <= 9600; lag += 4)
			maximumCorrelation = (std::max)(maximumCorrelation,
				NormalizedCorrelation(inharmonic, 5000, 12000, lag));
		Check(maximumCorrelation < 0.99f,
		      "irrational comb output has no near-unity repeated waveform lag over a 200 ms window");

		CombBank hot(kRate, 0.5f);
		CombBankParameters hotParameters = irrational;
		hotParameters.Feedback = 2.0f;
		hot.SetParameters(hotParameters);
		std::vector<float> hotOutput(input.size());
		hot.Process(input.data(), hotOutput.data(), hotOutput.size());
		float hotPeak = 0.0f;
		for (float sample : hotOutput)
		{
			Check(std::isfinite(sample), "CombBank at guarded maximum feedback stays finite");
			hotPeak = (std::max)(hotPeak, std::fabs(sample));
		}
		Check(hotPeak <= 16.0f, "CombBank maximum feedback remains inside its output safety bound");
	}

	void TestResonator()
	{
		Resonator resonator(kRate, 1000.0f, 35.0f, 1.08f);
		std::vector<float> input(24000, 0.0f), output(input.size(), 0.0f);
		input[0] = 1.0f;
		resonator.Process(input.data(), output.data(), output.size());
		Check(AllFinite(output.data(), output.size()), "stretched two-pole Resonator has a finite impulse response");
		Check(resonator.FrequencyHz() == 1000.0f, "resonator reports its unclipped base frequency");
		const float peak = PeakFrequency(output, 0, 12000, 1000.0f, 1160.0f, 2.0f);
		Check(std::fabs(peak - 1080.0f) < 20.0f,
		      "resonator Stretch shifts its measured pole frequency upward");
		resonator.SetParameters(100000.0f, 0.0f, 9.0f);
		for (std::size_t index = 0; index < 2048; ++index)
			output[index] = resonator.ProcessSample(index == 0 ? 10000.0f : 0.0f);
		Check(AllFinite(output.data(), 2048), "extreme resonator controls are clamped to a stable finite region");
	}

	void TestGrainDelay()
	{
		for (GrainEnvelope envelope : {GrainEnvelope::Gaussian, GrainEnvelope::Triangle,
		                              GrainEnvelope::Down, GrainEnvelope::Up,
		                              GrainEnvelope::ExpDecay, GrainEnvelope::ExpAttack})
		{
			const float start = GrainDelay::EnvelopeValue(envelope, 0.0f);
			const float middle = GrainDelay::EnvelopeValue(envelope, 0.5f);
			const float end = GrainDelay::EnvelopeValue(envelope, 1.0f);
			Check(start >= 0.0f && start <= 1.0f && middle >= 0.0f && middle <= 1.0f &&
			      end >= 0.0f && end <= 1.0f,
			      "every grain envelope remains in its documented unit range");
		}

		const auto render = [](std::uint32_t seed)
		{
			GrainDelay grains(kRate, 0.25f, seed);
			grains.SetMaxGrainCount(12);
			grains.SetFeedback(0.95f);
			std::vector<float> output(12000, 0.0f);
			for (std::size_t frame = 0; frame < output.size(); ++frame)
			{
				if (frame == 2048 || frame == 3072 || frame == 4096 || frame == 5120)
					grains.SpawnGrain(35.0f, 45.0f, 5.0f, GrainEnvelope::Gaussian, 3.0f);
				const float input = 0.5f * std::sin(2.0f * kPi * 330.0f * frame / kRate);
				output[frame] = grains.ProcessSample(input);
			}
			return output;
		};
		const auto first = render(0x12345678u);
		const auto sameSeed = render(0x12345678u);
		const auto otherSeed = render(0x87654321u);
		Check(first == sameSeed, "granular jitter is bit-deterministic for the same seed");
		Check(first != otherSeed, "granular start jitter changes with the per-instance seed");
		Check(AllFinite(first.data(), first.size()), "seeded granular voice remains finite with feedback at its maximum");
		float peak = 0.0f;
		for (float sample : first)
			peak = (std::max)(peak, std::fabs(sample));
		Check(peak <= 64.0f, "granular feedback and 100-grain safety bounds keep output finite and bounded");

		GrainDelay full(kRate, 0.1f, 7u);
		full.SetMaxGrainCount(100);
		full.SetFeedback(2.0f);
		Check(full.Feedback() <= 0.95f, "granular feedback is clamped to its stable maximum");
		for (std::uint32_t index = 0; index < 100; ++index)
			Check(full.SpawnGrain(20.0f, 50.0f, 0.0f), "MaxGrainCount=100 accepts each available grain slot");
		Check(full.ActiveGrainCount() == 100, "all one hundred fixed grain slots become active");
		Check(!full.SpawnGrain(20.0f, 500.0f, 0.0f), "the 101st grain is rejected rather than allocating or overrunning");
		float fullPeak = 0.0f;
		bool fullFinite = true;
		for (int frame = 0; frame < 4096; ++frame)
		{
			const float sample = full.ProcessSample(frame == 0 ? 1.0e8f : 0.0f);
			fullFinite = fullFinite && std::isfinite(sample);
			fullPeak = (std::max)(fullPeak, std::fabs(sample));
		}
		Check(fullFinite && fullPeak <= 64.0f,
		      "100 simultaneous grains and maximum feedback stay finite and bounded");
		Check(full.ActiveGrainCount() == 0, "grain slots retire after their authored duration");
	}

	void TestWavePlayer()
	{
		float source[16] = {};
		for (std::size_t index = 0; index < 16; ++index)
			source[index] = static_cast<float>(index) / 16.0f;
		WavePlayer player(kRate);
		player.SetSource(source, 16);
		player.SetPitchSemitones(12.0f);
		player.SetNearlyFinishedLeadSeconds(4.0f / kRate);
		const auto start = player.ProcessSample(true, false, 0.0f);
		const auto next = player.ProcessSample();
		Check(std::fabs(start.Sample - source[0]) < 1.0e-6f &&
		      std::fabs(next.Sample - source[2]) < 1.0e-6f,
		      "WavePlayer applies semitone pitch as sample-rate-correct resampling");
		WavePlayerFrame frame{};
		int nearlyFrame = -1;
		int finishedFrame = -1;
		for (int index = 2; index < 10 && finishedFrame < 0; ++index)
		{
			frame = player.ProcessSample();
			if (frame.OnNearlyFinished)
				nearlyFrame = index;
			if (frame.OnFinished)
				finishedFrame = index;
		}
		Check(nearlyFrame == 6 && finishedFrame == 7,
		      "OnNearlyFinished fires exactly four source frames before natural finish");
		Check(!player.IsPlaying() && frame.OnFinished,
		      "WavePlayer emits OnFinished exactly when a non-looped sample reaches its end");

		player.SetPitchSemitones(0.0f);
		player.SetLoopRegion(4.0f / kRate, 4.0f / kRate);
		player.SetLoop(true);
		player.SetNearlyFinishedLeadSeconds(0.0f);
		player.ProcessSample(true, false, 4.0f / kRate);
		WavePlayerFrame loopFrame{};
		for (int index = 0; index < 3; ++index)
			loopFrame = player.ProcessSample();
		Check(loopFrame.OnLooped && player.IsPlaying(),
		      "loop range wraps at its exact end and emits OnLooped without finishing");
		player.SetLoop(false);
		bool finishedAfterToggle = false;
		for (int index = 0; index < 20; ++index)
			finishedAfterToggle = player.ProcessSample().OnFinished || finishedAfterToggle;
		Check(finishedAfterToggle && !player.IsPlaying(),
		      "Loop can be disabled mid-playback and the take then finishes naturally");
	}

	void TestWavePlayerNative44100SourceAt48000MixRate()
	{
		std::vector<float> source(441, 0.25f);
		WavePlayer player(48000);
		player.SetSource(source.data(), source.size(), 44100);
		WavePlayerFrame middle{};
		bool finishedBeforeExpectedDuration = false;
		for (int frame = 0; frame < 480; ++frame)
		{
			const WavePlayerFrame rendered = player.ProcessSample(frame == 0);
			if (frame == 240)
				middle = rendered;
			if (rendered.OnFinished && frame != 479)
				finishedBeforeExpectedDuration = true;
			if (frame < 479)
				Check(!rendered.OnFinished, "44.1 kHz source stays active through 479 frames at a 48 kHz mix rate");
			else
				Check(rendered.OnFinished, "44.1 kHz source finishes after exactly 480 output frames");
		}
		Check(std::fabs(middle.PlaybackLocation - 0.005f) < 1.0e-6f,
		      "source-frame position reports 5 ms at the middle of a 10 ms 44.1 kHz clip");
		Check(!finishedBeforeExpectedDuration,
		      "44.1/48 kHz conversion avoids shortening source-rate playback duration");
	}

	void TestNoProcessAllocations()
	{
		ModalStrike modal(kRate);
		modal.Trigger(99u);
		CombBank comb(kRate, 0.1f);
		GrainDelay grains(kRate, 0.1f, 99u);
		grains.SpawnGrain(5.0f, 10.0f, 2.0f, GrainEnvelope::Triangle, 1.0f);
		float source[64] = {};
		WavePlayer player(kRate);
		player.SetSource(source, 64);
		player.ProcessSample(true);
		float sink = 0.0f;

		const std::size_t before = g_TrackedAllocations;
		g_TrackAllocations = true;
		for (int frame = 0; frame < 4096; ++frame)
		{
			const float input = frame == 0 ? 0.5f : 0.0f;
			sink += modal.ProcessSample();
			sink += comb.ProcessSample(input);
			sink += grains.ProcessSample(input);
			sink += player.ProcessSample().Sample;
		}
		g_TrackAllocations = false;
		Check(g_TrackedAllocations == before,
		      "ModalStrike, CombBank, GrainDelay and WavePlayer processing allocate no memory");
		Check(std::isfinite(sink), "allocation-free voice rendering still produces finite output");
	}
}

int main()
{
	TestRateAndModalStrike();
	TestCombBank();
	TestResonator();
	TestGrainDelay();
	TestWavePlayer();
	TestWavePlayerNative44100SourceAt48000MixRate();
	TestNoProcessAllocations();
	std::printf("test_avb_voices: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
