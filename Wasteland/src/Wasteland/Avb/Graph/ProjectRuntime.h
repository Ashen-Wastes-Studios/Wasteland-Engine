#pragma once

#include "Runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Wasteland::Avb
{
	// Owns the graph, its compiled non-owning view, and the prepared runtime.
	// Not internally synchronized: Load/Unload/Reset must not overlap runtime
	// calls. Quiesce the render callback before replacing or destroying the host.
	// QueueTrigger and RenderBlock calls belong to one serialized render thread.
	class AvbProjectRuntime
	{
	public:
		AvbProjectRuntime();
		~AvbProjectRuntime();

		AvbProjectRuntime(const AvbProjectRuntime&) = delete;
		AvbProjectRuntime& operator=(const AvbProjectRuntime&) = delete;
		AvbProjectRuntime(AvbProjectRuntime&&) = delete;
		AvbProjectRuntime& operator=(AvbProjectRuntime&&) = delete;

		// Loads a project-relative .wlavb asset from the currently open Project,
		// compiles it, resolves its WavePlayer AssetRefs through that Project,
		// and prepares the runtime. A failed reload leaves the current graph live.
		bool Load(const std::string& graphAssetRef,
		          std::uint32_t sampleRate = 48000,
		          std::size_t framesPerBlock = 480,
		          std::size_t maxTriggersPerBlock = 256,
		          std::string* error = nullptr);
		void Unload() noexcept;
		void Reset() noexcept;

		bool QueueTrigger(const std::string& nodeId, const std::string& inputPin,
		                  int frame) noexcept;
		bool RenderBlock(float* monoOutput, std::size_t frames,
		                 AudioBusGraph* buses = nullptr,
		                 AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept;
		bool RenderBlockStereo(float* interleavedStereoOutput, std::size_t frames,
		                       AudioBusGraph* buses = nullptr,
		                       AvbBusBlockMode busMode = AvbBusBlockMode::Finalize) noexcept;

		const AvbTrigger* FindTriggerOutput(const std::string& nodeId,
		                                   const std::string& outputPin) const noexcept;
		bool IsPrepared() const noexcept;
		std::uint32_t SampleRate() const noexcept;
		std::size_t FramesPerBlock() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
