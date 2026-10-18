#include "Wasteland/Core/Core.h"
#include "Wasteland/Avb/AudioBus.h"
#include "Wasteland/Avb/AvbBusEffects.h"
#include "Wasteland/Audio/AudioClip.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
	std::atomic<bool> g_TrackAllocations{false};
	std::atomic<std::size_t> g_AllocationCount{0};
}

void* operator new(std::size_t size)
{
	if (g_TrackAllocations.load(std::memory_order_relaxed))
		g_AllocationCount.fetch_add(1, std::memory_order_relaxed);
	if (void* memory = std::malloc(size == 0 ? 1 : size))
		return memory;
	throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace Wasteland::Avb;

namespace
{
	int g_Checks = 0;
	int g_Failures = 0;

	void Check(bool condition, const char* message)
	{
		++g_Checks;
		if (!condition)
		{
			++g_Failures;
			std::printf("  FAIL: %s\n", message);
		}
	}

	void CheckNear(float actual, float expected, float tolerance, const char* message)
	{
		Check(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance, message);
	}

	float Rms(const std::vector<float>& samples, std::size_t firstFrame, std::size_t frameCount)
	{
		double sum = 0.0;
		for (std::size_t frame = firstFrame; frame < firstFrame + frameCount; ++frame)
		{
			const double left = samples[frame * 2];
			const double right = samples[frame * 2 + 1];
			sum += left * left + right * right;
		}
		return static_cast<float>(std::sqrt(sum / static_cast<double>(frameCount * 2)));
	}

	void TestNullAndEmptyImpulseResponsesPassThrough()
	{
		ConvolutionReverb noIr(nullptr);
		std::array<float, 10> input = {0.25f, -0.5f, 0.75f, -1.0f, 0.0f, 0.125f, -0.25f, 0.5f, 0.875f, -0.125f};
		const auto original = input;
		noIr.Process(input.data(), input.size() / 2);
		Check(input == original, "AVB.6: an absent IR is an exact in-place pass-through");

		Wasteland::Audio::AudioClip emptyClip;
		ConvolutionReverb emptyIr(&emptyClip);
		input = original;
		emptyIr.Process(input.data(), input.size() / 2);
		Check(input == original, "AVB.6: an invalid/empty AudioClip IR is an exact pass-through");
	}

	void TestDefaultBusReverbFallsBackToFdn()
	{
		BusReverb reverb;
		Check(!reverb.UsesConvolution(), "AVB.6: the default bus reverb selects FDN when no IR is supplied");
		std::vector<float> impulse(2400 * 2, 0.0f);
		impulse[0] = 1.0f;
		impulse[1] = 1.0f;
		reverb.Process(impulse.data(), impulse.size() / 2);
		const bool hasTail = std::any_of(impulse.begin() + 2000 * 2, impulse.end(),
			[](float sample) { return std::fabs(sample) > 1.0e-7f; });
		Check(hasTail, "AVB.6: default FDN fallback produces a reverberant tail without an IR asset");
	}

	void TestConvolutionUsesAudioClipImpulseResponse()
	{
		std::vector<float> impulseSamples = {1.0f};
		auto impulseClip = Wasteland::Audio::AudioClip::FromSamples(std::move(impulseSamples), 48000);
		ConvolutionReverb reverb(impulseClip.get());
		std::vector<float> buffer(600 * 2, 0.0f);
		buffer[0] = 0.75f;
		buffer[1] = -0.25f;
		reverb.Process(buffer.data(), 600);
		CheckNear(buffer[256 * 2], 0.75f, 1.0e-4f, "AVB.6: an AudioClip IR impulse produces the expected convolution response");
		CheckNear(buffer[256 * 2 + 1], -0.25f, 1.0e-4f, "AVB.6: convolution applies the mono IR independently to both channels");
	}

	void TestSubmixDelayAndDynamicsEffects()
	{
		SubmixDelay delay(2.0f / 48000.0f, 0.0f, 1.0f, 0.01f);
		std::array<float, 12> delayed = {1.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		delay.Process(delayed.data(), delayed.size() / 2);
		CheckNear(delayed[0], 0.0f, 1.0e-6f, "AVB.6: fully wet submix delay removes the direct tap");
		CheckNear(delayed[4], 1.0f, 1.0e-5f, "AVB.6: submix delay emits the left impulse at its configured delay");
		CheckNear(delayed[5], -0.5f, 1.0e-5f, "AVB.6: submix delay preserves the right channel");

		Compressor compressor;
		compressor.SetParameters(-24.0f, 8.0f, 0.0f, 0.001f, 0.05f);
		std::array<float, 64> compressed{};
		compressed.fill(0.9f);
		compressor.Process(compressed.data(), compressed.size() / 2);
		Check(std::isfinite(compressed.back()) && compressed.back() < 0.9f,
		      "AVB.6: bus compressor reduces sustained signal above threshold");

		Compressor lookahead;
		lookahead.SetParameters(0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 2.0f / 48000.0f);
		std::array<float, 12> delayedProgram{};
		delayedProgram[0] = 1.0f;
		lookahead.Process(delayedProgram.data(), delayedProgram.size() / 2);
		CheckNear(delayedProgram[0], 0.0f, 1.0e-6f, "AVB.6: compressor lookahead delays the left program channel");
		CheckNear(delayedProgram[1], 0.0f, 1.0e-6f, "AVB.6: compressor lookahead delays the linked right program channel");
		CheckNear(delayedProgram[4], 1.0f, 1.0e-5f, "AVB.6: compressor lookahead releases the left sample after two frames");
		CheckNear(delayedProgram[5], 0.0f, 1.0e-6f, "AVB.6: compressor lookahead keeps a silent right sample silent");

		Eq eq;
		eq.SetParameters(250.0f, 0.0f, 1000.0f, -12.0f, 7000.0f, 0.0f);
		std::array<float, 2048> tone{};
		for (std::size_t frame = 0; frame < tone.size() / 2; ++frame)
		{
			const float sample = std::sin(6.28318530718f * 1000.0f * static_cast<float>(frame) / 48000.0f);
			tone[frame * 2] = sample;
			tone[frame * 2 + 1] = sample;
		}
		const float before = Rms(std::vector<float>(tone.begin(), tone.end()), 0, tone.size() / 2);
		eq.Process(tone.data(), tone.size() / 2);
		const float after = Rms(std::vector<float>(tone.begin(), tone.end()), 0, tone.size() / 2);
		Check(after < before * 0.8f, "AVB.6: bus EQ applies the configured mid-band cut");

		MultibandCompressor multiband;
		multiband.SetCrossovers(300.0f, 3500.0f);
		multiband.SetBandParameters(MultibandBand::Low, -30.0f, 4.0f, 0.001f, 0.05f);
		std::array<float, 2048> broadband{};
		for (std::size_t frame = 0; frame < broadband.size() / 2; ++frame)
		{
			const float sample = 0.8f * std::sin(6.28318530718f * 90.0f * static_cast<float>(frame) / 48000.0f) +
			                     0.2f * std::sin(6.28318530718f * 6000.0f * static_cast<float>(frame) / 48000.0f);
			broadband[frame * 2] = sample;
			broadband[frame * 2 + 1] = sample;
		}
		multiband.Process(broadband.data(), broadband.size() / 2);
		Check(std::all_of(broadband.begin(), broadband.end(), [](float sample) { return std::isfinite(sample); }),
		      "AVB.6: three-band processing remains finite with configured band compression");
	}

	void TestMultibandNeutralResponseAtCrossovers()
	{
		constexpr float lowCrossoverHz = 300.0f;
		constexpr float highCrossoverHz = 3500.0f;
		constexpr std::size_t frameCount = 24000;
		constexpr std::size_t measuredFrames = 4096;
		MultibandCompressor multiband;
		multiband.SetCrossovers(lowCrossoverHz, highCrossoverHz);
		multiband.SetBandParameters(MultibandBand::Low, -60.0f, 1.0f);
		multiband.SetBandParameters(MultibandBand::Mid, -60.0f, 1.0f);
		multiband.SetBandParameters(MultibandBand::High, -60.0f, 1.0f);

		for (const float frequency : {lowCrossoverHz, highCrossoverHz})
		{
			std::vector<float> signal(frameCount * 2);
			for (std::size_t frame = 0; frame < frameCount; ++frame)
			{
				const float sample = 0.5f * std::sin(6.28318530718f * frequency *
					static_cast<float>(frame) / 48000.0f);
				signal[frame * 2] = sample;
				signal[frame * 2 + 1] = sample;
			}
			const float inputRms = Rms(signal, frameCount - measuredFrames, measuredFrames);
			multiband.Reset();
			const std::size_t allocationsBefore = g_AllocationCount.load(std::memory_order_relaxed);
			g_TrackAllocations.store(true, std::memory_order_relaxed);
			multiband.Process(signal.data(), frameCount);
			g_TrackAllocations.store(false, std::memory_order_relaxed);
			const std::size_t allocationsAfter = g_AllocationCount.load(std::memory_order_relaxed);
			const float outputRms = Rms(signal, frameCount - measuredFrames, measuredFrames);
			const float relativeError = std::fabs(outputRms - inputRms) / inputRms;
			Check(std::all_of(signal.begin(), signal.end(), [](float sample) { return std::isfinite(sample); }),
			      "AVB.6: unity-ratio multiband crossover output remains finite");
			Check(relativeError <= 0.02f,
			      "AVB.6: unity-ratio multiband output stays within 2% of input RMS at each crossover");
			Check(allocationsAfter == allocationsBefore,
			      "AVB.6: multiband crossover processing performs no heap allocations");
		}
	}



















































	void TestFdnDecayAndStability()
	{
		FdnReverb reverb;
		reverb.SetParameters(1.5f, 0.0f, 1.0f);
		constexpr std::size_t toneFrames = 4800;
		constexpr std::size_t tailFrames = 96000;
		std::vector<float> rendered((toneFrames + tailFrames) * 2, 0.0f);
		for (std::size_t frame = 0; frame < toneFrames; ++frame)
		{
			const float sample = 0.5f * std::sin(6.28318530718f * 1000.0f * static_cast<float>(frame) / 48000.0f);
			rendered[frame * 2] = sample;
			rendered[frame * 2 + 1] = sample;
		}
		reverb.Process(rendered.data(), rendered.size() / 2);
		const std::size_t windowFrames = 12000;
		float previousRms = Rms(rendered, toneFrames, windowFrames);
		bool monotonic = previousRms > 1.0e-7f;
		for (std::size_t window = 1; window < tailFrames / windowFrames; ++window)
		{
			const float currentRms = Rms(rendered, toneFrames + window * windowFrames, windowFrames);
			monotonic = monotonic && currentRms <= previousRms + 1.0e-6f;
			previousRms = currentRms;
		}
		Check(monotonic, "AVB.6: the FDN space produces a monotonically decaying 1 kHz tail over two seconds");
		Check(std::all_of(rendered.begin(), rendered.end(), [](float sample) { return std::isfinite(sample); }),
		      "AVB.6: FDN tail remains finite for the complete two-second render");

		std::array<float, 8> overload;
		overload.fill(1.0e20f);
		reverb.Process(overload.data(), overload.size() / 2);
		Check(std::all_of(overload.begin(), overload.end(), [](float sample) { return std::isfinite(sample); }),
		      "AVB.6: maximum finite drive cannot destabilize the FDN state");
	}

	void TestBusWithoutSendsIsUnity()
	{
		AudioBusGraph graph;
		const AudioBusId master = graph.AddBus();
		Check(graph.SetMasterBus(master), "AVB.6: no-send bus graph has a master");
		Check(graph.Prepare(3), "AVB.6: no-send bus graph prepares before rendering");
		const std::array<float, 6> input = {0.5f, -0.25f, 0.125f, 0.75f, -1.0f, 0.0f};
		std::array<float, 6> output{};
		const AudioBusInput source{master, input.data(), 1.0f};
		Check(graph.Process(&source, 1, output.data(), 3), "AVB.6: no-send bus renders normally");
		Check(output == input, "AVB.6: a bus with no sends and no effects is exactly unity");
	}

	void TestProcessingAllocatesNothing()
	{
		SubmixDelay delay;
		Compressor compressor;
		Eq eq;
		MultibandCompressor multiband;
		FdnReverb fdn;
		std::vector<float> ir(513, 0.0f);
		ir[0] = 1.0f;
		ConvolutionReverb convolution;
		convolution.SetImpulseResponse(ir.data(), ir.size(), 48000);
		std::array<float, 512> block{};
		block[0] = 0.25f;
		const std::size_t before = g_AllocationCount.load(std::memory_order_relaxed);
		g_TrackAllocations.store(true, std::memory_order_relaxed);
		delay.Process(block.data(), block.size() / 2);
		compressor.Process(block.data(), block.size() / 2);
		eq.Process(block.data(), block.size() / 2);
		multiband.Process(block.data(), block.size() / 2);
		fdn.Process(block.data(), block.size() / 2);
		convolution.Process(block.data(), block.size() / 2);
		g_TrackAllocations.store(false, std::memory_order_relaxed);
		const std::size_t after = g_AllocationCount.load(std::memory_order_relaxed);
		Check(after == before, "AVB.6: all prepared bus effects process without heap allocation");
		Check(std::all_of(block.begin(), block.end(), [](float sample) { return std::isfinite(sample); }),
		      "AVB.6: no-allocation processing also yields finite samples");
	}

	static_assert(noexcept(std::declval<IAudioBusEffect&>().Process(nullptr, 0)),
	              "AVB.6 effects must be callable from a noexcept audio callback");
}

int main()
{
	TestNullAndEmptyImpulseResponsesPassThrough();
	TestDefaultBusReverbFallsBackToFdn();
	TestConvolutionUsesAudioClipImpulseResponse();
	TestSubmixDelayAndDynamicsEffects();
	TestMultibandNeutralResponseAtCrossovers();
	TestFdnDecayAndStability();
	TestBusWithoutSendsIsUnity();
	TestProcessingAllocatesNothing();
	std::printf("test_avb_effects: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
