#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Wasteland::Audio
{
	class AudioClip;
}

namespace Wasteland::Avb
{
	// Resolve the exact project-relative AssetRef string during Prepare, never
	// during render. The runtime retains the returned shared owner. A project
	// host can return Wasteland::Audio::AudioClip::LoadWav(
	//     Wasteland::Project::Get().GetAssetPath(assetRef).string());
	using AvbAssetResolver = std::function<std::shared_ptr<Wasteland::Audio::AudioClip>(const std::string&)>;

	class AudioBusGraph;
	struct AvbGraph;
	struct AvbCompiledGraph;

	// A preallocated, ordered set of trigger frame indices for one block.
	// Prepare() allocates storage; AdvanceBlock() and TriggerFrame() do not.
	class AvbTrigger
	{
	public:
		AvbTrigger() = default;
		void Prepare(std::size_t maxFrames, std::size_t maxTriggersPerBlock);
		void AdvanceBlock() noexcept { m_Count = 0; }
		void AdvanceBlock(std::size_t frames) noexcept;
		bool TriggerFrame(int frame) noexcept;
		template<typename PreFn, typename TriggerFn>
		void ExecuteBlock(PreFn&& preTrigger, TriggerFn&& triggered) const
		{
			ExecuteBlock(static_cast<int>(m_BlockFrames), std::forward<PreFn>(preTrigger),
			             std::forward<TriggerFn>(triggered));
		}
		template<typename PreFn, typename TriggerFn>
		void ExecuteBlock(int frames, PreFn&& preTrigger, TriggerFn&& triggered) const
		{
			if (frames <= 0)
				return;
			int rangeStart = 0;
			for (std::size_t i = 0; i < m_Count; ++i)
			{
				const int frame = m_Frames[i];
				if (frame > rangeStart)
					preTrigger(rangeStart, frame);
				const int rangeEnd = i + 1 < m_Count ? m_Frames[i + 1] : frames;
				triggered(frame, rangeEnd);
				rangeStart = rangeEnd;
			}
			if (m_Count == 0)
				preTrigger(0, frames);
		}

		std::size_t Count() const noexcept { return m_Count; }
		int FrameAt(std::size_t index) const noexcept;
		const int* Frames() const noexcept { return m_Frames.get(); }
		std::size_t Capacity() const noexcept { return m_Capacity; }

	private:
		std::unique_ptr<int[]> m_Frames;
		std::size_t m_Capacity = 0;
		std::size_t m_Count = 0;
		std::size_t m_MaxFrames = 0;
		std::size_t m_BlockFrames = 0;
	};

	// Round the requested frames/block to a SIMD lane multiple, then report the
	// achieved block size separately from the requested rate. Invalid inputs
	// return zero. AVB's standard 48 kHz / 100 Hz / 8-lane setting yields 480.
	std::size_t AvbFramesPerBlock(std::uint32_t sampleRate,
	                              double targetBlockRate = 100.0,
	                              std::size_t simdLanes = 8) noexcept;

	enum class AvbBusBlockMode : std::uint8_t
	{
		Finalize,
		Defer
	};

	class AvbRuntime
	{
	public:
		AvbRuntime();
		~AvbRuntime();
		AvbRuntime(const AvbRuntime&) = delete;
		AvbRuntime& operator=(const AvbRuntime&) = delete;
		AvbRuntime(AvbRuntime&&) noexcept;
		AvbRuntime& operator=(AvbRuntime&&) noexcept;

		// Prepare compiles all node/link references and allocates audio, trigger,
		// feedback and bus-adapter storage. The source graph and its compiled view
		// must outlive the runtime. Rendering requires exactly framesPerBlock.
		bool Prepare(const AvbGraph& graph, const AvbCompiledGraph& compiled,
		             std::uint32_t sampleRate, std::size_t framesPerBlock,
		             std::size_t maxTriggersPerBlock = 256,
		             std::string* error = nullptr,
		             AvbAssetResolver assetResolver = {});
		void Reset() noexcept;

		// Queue a trigger on a node's Trigger input for the next block. Calls are
		// render-thread owned: no lock or allocation is used by this API.
		bool QueueTrigger(const std::string& nodeId, const std::string& inputPin,
		                  int frame) noexcept;

		// Graph audio pins are mono. RenderBlock is for Mono graph outputs;
		// RenderBlockStereo writes interleaved L/R samples for Stereo graph outputs
		// by duplicating each mono output sample to both channels. Bus writers also
		// duplicate mono samples to stereo, while bus readers average the selected
		// stereo tap. Reader nodes always observe the last completed bus block.
		// Finalize is the single-runtime default; for multiple runtimes, call
		// AudioBusGraph::BeginBlock once, render each with Defer, then call EndBlock
		// once after all writers.
		bool RenderBlock(float* monoOutput, std::size_t frames,
		                 AudioBusGraph* buses = nullptr,
		                 AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept;
		bool RenderBlockStereo(float* interleavedStereoOutput, std::size_t frames,
		                       AudioBusGraph* buses = nullptr,
		                       AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept;
		bool ExecuteBlock(float* monoOutput, std::size_t frames,
		                  AudioBusGraph* buses = nullptr,
		                  AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept
		{
			return RenderBlock(monoOutput, frames, buses, busMode);
		}
		bool ExecuteBlockStereo(float* interleavedStereoOutput, std::size_t frames,
		                        AudioBusGraph* buses = nullptr,
		                        AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept
		{
			return RenderBlockStereo(interleavedStereoOutput, frames, buses, busMode);
		}

		const AvbTrigger* FindTriggerOutput(const std::string& nodeId,
		                                   const std::string& outputPin) const noexcept;
		bool IsPrepared() const noexcept;
		std::uint32_t SampleRate() const noexcept;
		std::size_t FramesPerBlock() const noexcept;
		double GetActualBlockRate() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
