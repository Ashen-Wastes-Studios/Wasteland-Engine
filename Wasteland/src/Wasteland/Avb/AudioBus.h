#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Wasteland::Avb
{
	// AVB runs at a fixed rate. AudioBus buffers are interleaved stereo (L,R),
	// and every frame count in this API is a count of stereo frames, not floats.
	inline constexpr std::uint32_t kAvbAudioSampleRate = 48000;
	static_assert(std::atomic<float>::is_always_lock_free, "AVB live float controls must be lock-free");
	static_assert(std::atomic<bool>::is_always_lock_free, "AVB live mute control must be lock-free");
	using AudioBusId = std::uint32_t;
	inline constexpr AudioBusId kInvalidAudioBusId = (std::numeric_limits<AudioBusId>::max)();

	enum class AudioBusTap : std::uint8_t
	{
		PreFx,
		PostFx,
		PostFader
	};

	struct AudioBusSend
	{
		AudioBusId Destination = kInvalidAudioBusId;
		AudioBusTap Tap = AudioBusTap::PostFader;
		float Gain = 1.0f;
	};

	// Effects are installed before Prepare(), then called in order in-place on
	// the audio thread. Implementations must not allocate, lock, or throw from
	// Process(). AVB.6 supplies the concrete bus effects.
	class IAudioBusEffect
	{
	public:
		virtual ~IAudioBusEffect() = default;
		virtual void Process(float* interleavedStereo, std::size_t frames) noexcept = 0;
		virtual void Reset() noexcept {}
	};

	struct AudioBusInput
	{
		AudioBusId Bus = kInvalidAudioBusId;
		const float* InterleavedStereo = nullptr;
		float Gain = 1.0f;
	};

	class AudioBusGraph;

	// Per-bus live controls. Gain, pan and mute use atomics so the control
	// thread may update them while a bus block is processing; topology, sends
	// and effects are configured only while rendering is stopped.
	class AudioBus
	{
	public:
		AudioBusId Id() const noexcept { return m_Id; }
		void SetGain(float gain) noexcept;
		float Gain() const noexcept { return m_Gain.load(std::memory_order_relaxed); }
		void SetPan(float pan) noexcept;
		float Pan() const noexcept { return m_Pan.load(std::memory_order_relaxed); }
		void SetMuted(bool muted) noexcept { m_Muted.store(muted, std::memory_order_relaxed); }
		bool IsMuted() const noexcept { return m_Muted.load(std::memory_order_relaxed); }

	private:
		friend class AudioBusGraph;
		explicit AudioBus(AudioBusId id) noexcept : m_Id(id) {}

		AudioBusId m_Id;
		AudioBusId m_Parent = kInvalidAudioBusId;
		bool m_MainSend = true;
		std::atomic<float> m_Gain{1.0f};
		std::atomic<float> m_Pan{0.0f};
		std::atomic<bool> m_Muted{false};
		std::vector<AudioBusSend> m_Sends;
		std::vector<std::unique_ptr<IAudioBusEffect>> m_Effects;
		std::vector<float> m_PreFx;
		std::vector<float> m_PostFx;
		std::vector<float> m_PostFader;
		std::vector<float> m_PendingInputs;
	};

	// Owns bus semantics, the source->destination send matrix, folder routing,
	// and all scratch buffers. Create/configure the graph, then Prepare() once
	// before the audio callback starts. Process() and the block transaction
	// methods perform no allocation or locking. Topology/effects may only be
	// edited with processing stopped; bus gain, pan and mute are safe to change live.
	class AudioBusGraph
	{
	public:
		AudioBusGraph() = default;
		AudioBusGraph(const AudioBusGraph&) = delete;
		AudioBusGraph& operator=(const AudioBusGraph&) = delete;
		AudioBusGraph(AudioBusGraph&&) = delete;
		AudioBusGraph& operator=(AudioBusGraph&&) = delete;

		AudioBusId AddBus();
		bool SetMasterBus(AudioBusId bus) noexcept;
		bool SetParentBus(AudioBusId child, AudioBusId parent) noexcept;
		bool SetMainSend(AudioBusId bus, bool enabled) noexcept;
		bool AddSend(AudioBusId source, AudioBusId destination, AudioBusTap tap,
		             float gain = 1.0f);
		bool AddEffect(AudioBusId bus, std::unique_ptr<IAudioBusEffect> effect);

		AudioBus* GetBus(AudioBusId bus) noexcept;
		const AudioBus* GetBus(AudioBusId bus) const noexcept;
		std::size_t BusCount() const noexcept { return m_Buses.size(); }
		std::size_t GetSendCount(AudioBusId source) const noexcept;
		bool GetSend(AudioBusId source, std::size_t index, AudioBusSend& send) const noexcept;

		// Allocates all per-bus buffers and compiles a deterministic routing
		// order off the audio thread. Returns false for an invalid master,
		// folder cycle, send cycle, or zero/overflowing block capacity.
		bool Prepare(std::size_t maxFrames);
		void Reset() noexcept;

		// A block transaction preserves completed taps while runtimes render.
		// BeginBlock clears only preallocated pending-input buffers; accumulate
		// each runtime's writer inputs, then EndBlock routes/processes the graph
		// exactly once and publishes the new taps. These calls allocate and lock
		// nothing. Input samples may be null (silence) but must not alias taps.
		bool BeginBlock(std::size_t frames) noexcept;
		bool AccumulateInputs(const AudioBusInput* inputs, std::size_t inputCount,
		                      std::size_t frames) noexcept;
		bool EndBlock(float* interleavedStereoOutput, std::size_t frames) noexcept;

		// Single-call compatibility path: begin, accumulate all inputs, and
		// finalize once. Invalid input bus IDs fail closed and clear output.
		bool Process(const AudioBusInput* inputs, std::size_t inputCount,
		             float* interleavedStereoOutput, std::size_t frames) noexcept;

		// Tap pointers reference graph-owned storage and stay valid until the
		// next Prepare() or graph destruction. Read/render them on the audio
		// thread (or after rendering has stopped); only LastRenderedFrames()
		// samples are valid after a completed EndBlock()/Process().
		const float* GetTapBuffer(AudioBusId bus, AudioBusTap tap) const noexcept;
		std::size_t MaxFrames() const noexcept { return m_MaxFrames; }
		std::size_t LastRenderedFrames() const noexcept { return m_LastRenderedFrames; }
		static constexpr std::uint32_t SampleRate() noexcept { return kAvbAudioSampleRate; }

	private:
		void Invalidate() noexcept;
		bool IsEffectivelyMuted(const AudioBus& bus) const noexcept;
		const float* TapBuffer(const AudioBus& bus, AudioBusTap tap) const noexcept;
		static void Accumulate(float* destination, const float* source, float gain,
		                       std::size_t sampleCount) noexcept;

		std::vector<std::unique_ptr<AudioBus>> m_Buses;
		std::vector<AudioBusId> m_ProcessOrder;
		AudioBusId m_MasterBus = kInvalidAudioBusId;
		std::size_t m_MaxFrames = 0;
		std::size_t m_LastRenderedFrames = 0;
		std::size_t m_BlockFrames = 0;
		bool m_BlockOpen = false;
		bool m_Prepared = false;
	};
}
