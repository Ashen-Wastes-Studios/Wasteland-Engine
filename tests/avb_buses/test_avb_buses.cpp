#include "Wasteland/Avb/AudioBus.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

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

	class MultiplyEffect final : public IAudioBusEffect
	{
	public:
		explicit MultiplyEffect(float gain) : m_Gain(gain) {}
		void Process(float* stereo, std::size_t frames) noexcept override
		{
			for (std::size_t sample = 0; sample < frames * 2; ++sample)
				stereo[sample] *= m_Gain;
		}
	private:
		float m_Gain;
	};

	void TestThreeSendTaps()
	{
		Check(AudioBusGraph::SampleRate() == 48000, "AVB.5: bus processing is fixed at the authored 48 kHz rate");
		AudioBusGraph graph;
		const AudioBusId master = graph.AddBus();
		const AudioBusId source = graph.AddBus();
		const AudioBusId preFxReturn = graph.AddBus();
		const AudioBusId postFxReturn = graph.AddBus();
		const AudioBusId postFaderReturn = graph.AddBus();
		Check(graph.SetMasterBus(master), "AVB.5: a valid bus can be designated as the graph master");
		Check(graph.AddEffect(source, std::make_unique<MultiplyEffect>(2.0f)),
		      "AVB.5: an effect chain can be installed before realtime preparation");
		Check(graph.AddSend(source, preFxReturn, AudioBusTap::PreFx, 1.0f),
		      "AVB.5: a pre-FX send is represented as a source/destination/tap tuple");
		Check(graph.AddSend(source, postFxReturn, AudioBusTap::PostFx, 1.0f),
		      "AVB.5: a post-FX send is represented as a source/destination/tap tuple");
		Check(graph.AddSend(source, postFaderReturn, AudioBusTap::PostFader, 1.0f),
		      "AVB.5: a post-fader send is represented as a source/destination/tap tuple");
		graph.GetBus(source)->SetGain(0.5f);
		graph.GetBus(source)->SetPan(0.25f);
		Check(graph.Prepare(4), "AVB.5: a valid tap-routing graph prepares its fixed 48 kHz buffers");

		const std::array<float, 8> input = {0.5f, -0.25f, -0.125f, 0.75f, 1.0f, -1.0f, 0.0f, 0.25f};
		std::array<float, 8> output{};
		const AudioBusInput inputBus{source, input.data(), 1.0f};
		Check(graph.Process(&inputBus, 1, output.data(), 4), "AVB.5: the send matrix renders one complete block");

		const float* pre = graph.GetTapBuffer(preFxReturn, AudioBusTap::PostFader);
		const float* post = graph.GetTapBuffer(postFxReturn, AudioBusTap::PostFader);
		const float* fader = graph.GetTapBuffer(postFaderReturn, AudioBusTap::PostFader);
		Check(pre && post && fader, "AVB.5: each destination exposes its rendered post-fader buffer");
		CheckNear(pre[0], 0.5f, 0.0f, "AVB.5: pre-FX tap receives the original left sample");
		CheckNear(pre[1], -0.25f, 0.0f, "AVB.5: pre-FX tap receives the original right sample");
		CheckNear(post[0], 1.0f, 0.0f, "AVB.5: post-FX tap receives the processed left sample");
		CheckNear(post[1], -0.5f, 0.0f, "AVB.5: post-FX tap receives the processed right sample");
		CheckNear(fader[0], 0.375f, 1.0e-7f, "AVB.5: post-fader tap includes both bus gain and pan");
		CheckNear(fader[1], -0.25f, 1.0e-7f, "AVB.5: post-fader tap carries the right-channel post-pan value");
		Check(pre[0] != post[0] && post[0] != fader[0] && pre[0] != fader[0],
		      "AVB.5: all three REAPER taps produce measurably distinct sends");

		Check(graph.GetSendCount(source) == 3, "AVB.5: the send matrix exposes all configured source sends");
		AudioBusSend firstSend;
		Check(graph.GetSend(source, 0, firstSend) && firstSend.Destination == preFxReturn &&
		      firstSend.Tap == AudioBusTap::PreFx && firstSend.Gain == 1.0f,
		      "AVB.5: send-matrix rows retain the authored destination, tap, and unity gain");
	}

	void TestNestedFolderSumsAndMute()
	{
		AudioBusGraph graph;
		const AudioBusId master = graph.AddBus();
		const AudioBusId folder = graph.AddBus();
		const AudioBusId nestedFolder = graph.AddBus();
		const AudioBusId child = graph.AddBus();
		Check(graph.SetMasterBus(master), "AVB.5: nested folder graph has a master destination");
		Check(graph.SetParentBus(nestedFolder, folder), "AVB.5: a folder can route to its parent bus");
		Check(graph.SetParentBus(child, nestedFolder), "AVB.5: folder routing supports arbitrary nesting");
		Check(graph.Prepare(2), "AVB.5: nested parent sends compile to a processing order");

		const std::array<float, 2> folderInput = {0.125f, -0.125f};
		const std::array<float, 2> nestedInput = {0.5f, -0.5f};
		const std::array<float, 2> childInput = {0.25f, -0.25f};
		const AudioBusInput inputs[] = {
			{folder, folderInput.data(), 1.0f},
			{nestedFolder, nestedInput.data(), 1.0f},
			{child, childInput.data(), 1.0f},
		};
		std::array<float, 2> output{};
		Check(graph.Process(inputs, 3, output.data(), 1), "AVB.5: folder subtree renders without flattening or loss");
		const float* folderMix = graph.GetTapBuffer(folder, AudioBusTap::PostFader);
		CheckNear(folderMix[0], 0.875f, 0.0f, "AVB.5: parent folder receives its own input plus the nested child sum");
		CheckNear(folderMix[1], -0.875f, 0.0f, "AVB.5: folder summing preserves the stereo right-channel polarity");
		CheckNear(output[0], 0.875f, 0.0f, "AVB.5: folder main send reaches the master bus");

		graph.GetBus(folder)->SetGain(0.5f);
		Check(graph.Process(inputs, 3, output.data(), 1), "AVB.5: folder controls can change between prepared blocks");
		CheckNear(output[0], 0.4375f, 0.0f, "AVB.5: parent fader applies to the complete child submix");

		graph.GetBus(folder)->SetMuted(true);
		Check(graph.Process(inputs, 3, output.data(), 1), "AVB.5: muting a folder remains realtime-safe");
		const float* childOutput = graph.GetTapBuffer(child, AudioBusTap::PostFader);
		const float* nestedOutput = graph.GetTapBuffer(nestedFolder, AudioBusTap::PostFader);
		CheckNear(output[0], 0.0f, 0.0f, "AVB.5: muting a folder silences the master output");
		CheckNear(childOutput[0], 0.0f, 0.0f, "AVB.5: muting a folder silences every descendant bus");
		CheckNear(nestedOutput[0], 0.0f, 0.0f, "AVB.5: mute propagates through multiple folder levels");
	}

	void TestUnitySendAndAllocationFreeProcess()
	{
		AudioBusGraph graph;
		const AudioBusId master = graph.AddBus();
		const AudioBusId source = graph.AddBus();
		const AudioBusId destination = graph.AddBus();
		Check(graph.SetMasterBus(master), "AVB.5: unity routing graph has a master bus");
		Check(graph.AddSend(source, destination, AudioBusTap::PostFader, 1.0f),
		      "AVB.5: unity send is configured before the render callback starts");
		Check(graph.Prepare(2), "AVB.5: runtime buffers are allocated during graph preparation");

		const std::array<float, 4> input = {0.25f, -0.5f, 0.75f, 0.125f};
		std::array<float, 4> output{};
		const AudioBusInput inputBus{source, input.data(), 1.0f};
		const std::size_t allocationsBefore = g_AllocationCount.load(std::memory_order_relaxed);
		g_TrackAllocations.store(true, std::memory_order_relaxed);
		const bool rendered = graph.Process(&inputBus, 1, output.data(), 2);
		g_TrackAllocations.store(false, std::memory_order_relaxed);
		const std::size_t allocationsAfter = g_AllocationCount.load(std::memory_order_relaxed);
		Check(rendered, "AVB.5: prepared graph renders in the audio callback");
		Check(allocationsAfter == allocationsBefore, "AVB.5: processing performs no heap allocations");
		const float* sent = graph.GetTapBuffer(destination, AudioBusTap::PostFader);
		for (std::size_t sample = 0; sample < input.size(); ++sample)
			CheckNear(sent[sample], input[sample], 0.0f, "AVB.5: unity send gain reproduces every input sample exactly");
	}

	void TestRoutingCycleRejected()
	{
		AudioBusGraph graph;
		const AudioBusId master = graph.AddBus();
		const AudioBusId child = graph.AddBus();
		Check(graph.SetMasterBus(master), "AVB.5: cycle-check graph has a designated master");
		Check(graph.SetParentBus(child, master), "AVB.5: child main send establishes one direction of the route");
		Check(graph.AddSend(master, child, AudioBusTap::PostFader, 0.5f),
		      "AVB.5: arbitrary sends are accepted for offline topology validation");
		Check(!graph.Prepare(8), "AVB.5: cyclic send graphs are rejected before realtime processing");
	}
}

int main()
{
	TestThreeSendTaps();
	TestNestedFolderSumsAndMute();
	TestUnitySendAndAllocationFreeProcess();
	TestRoutingCycleRejected();
	std::printf("test_avb_buses: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
