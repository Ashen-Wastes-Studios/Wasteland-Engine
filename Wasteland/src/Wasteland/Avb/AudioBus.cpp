#include "wlpch.h"

#include "Wasteland/Avb/AudioBus.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Wasteland::Avb
{
	namespace
	{
		constexpr float kMaxBusValue = 1000000.0f;
		constexpr float kMaxBusGain = 1000.0f;

		float ClampGain(float gain, float fallback) noexcept
		{
			if (!std::isfinite(gain))
				return fallback;
			return std::clamp(gain, -kMaxBusGain, kMaxBusGain);
		}

		float SafeSample(float sample) noexcept
		{
			if (!std::isfinite(sample))
				return 0.0f;
			return std::clamp(sample, -kMaxBusValue, kMaxBusValue);
		}

		bool IsValidTap(AudioBusTap tap) noexcept
		{
			switch (tap)
			{
			case AudioBusTap::PreFx:
			case AudioBusTap::PostFx:
			case AudioBusTap::PostFader:
				return true;
			}
			return false;
		}
	}

	void AudioBus::SetGain(float gain) noexcept
	{
		m_Gain.store(ClampGain(gain, 1.0f), std::memory_order_relaxed);
	}

	void AudioBus::SetPan(float pan) noexcept
	{
		if (!std::isfinite(pan))
			pan = 0.0f;
		m_Pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
	}

	AudioBusId AudioBusGraph::AddBus()
	{
		Invalidate();
		if (m_Buses.size() >= static_cast<std::size_t>(kInvalidAudioBusId))
			return kInvalidAudioBusId;
		const AudioBusId id = static_cast<AudioBusId>(m_Buses.size());
		m_Buses.emplace_back(new AudioBus(id));
		return id;
	}

	bool AudioBusGraph::SetMasterBus(AudioBusId bus) noexcept
	{
		if (bus >= m_Buses.size())
			return false;
		m_MasterBus = bus;
		Invalidate();
		return true;
	}

	bool AudioBusGraph::SetParentBus(AudioBusId child, AudioBusId parent) noexcept
	{
		if (child >= m_Buses.size() || (parent != kInvalidAudioBusId && parent >= m_Buses.size()) ||
		    child == parent || child == m_MasterBus)
			return false;
		m_Buses[child]->m_Parent = parent;
		Invalidate();
		return true;
	}

	bool AudioBusGraph::SetMainSend(AudioBusId bus, bool enabled) noexcept
	{
		if (bus >= m_Buses.size() || bus == m_MasterBus)
			return false;
		m_Buses[bus]->m_MainSend = enabled;
		Invalidate();
		return true;
	}

	bool AudioBusGraph::AddSend(AudioBusId source, AudioBusId destination, AudioBusTap tap, float gain)
	{
		if (source >= m_Buses.size() || destination >= m_Buses.size() || source == destination ||
		    !IsValidTap(tap) || !std::isfinite(gain))
			return false;
		Invalidate();
		m_Buses[source]->m_Sends.push_back({destination, tap, ClampGain(gain, 0.0f)});
		return true;
	}

	bool AudioBusGraph::AddEffect(AudioBusId bus, std::unique_ptr<IAudioBusEffect> effect)
	{
		if (bus >= m_Buses.size() || !effect)
			return false;
		Invalidate();
		m_Buses[bus]->m_Effects.emplace_back(std::move(effect));
		return true;
	}

	AudioBus* AudioBusGraph::GetBus(AudioBusId bus) noexcept
	{
		return bus < m_Buses.size() ? m_Buses[bus].get() : nullptr;
	}

	const AudioBus* AudioBusGraph::GetBus(AudioBusId bus) const noexcept
	{
		return bus < m_Buses.size() ? m_Buses[bus].get() : nullptr;
	}

	std::size_t AudioBusGraph::GetSendCount(AudioBusId source) const noexcept
	{
		return source < m_Buses.size() ? m_Buses[source]->m_Sends.size() : 0;
	}

	bool AudioBusGraph::GetSend(AudioBusId source, std::size_t index, AudioBusSend& send) const noexcept
	{
		if (source >= m_Buses.size() || index >= m_Buses[source]->m_Sends.size())
			return false;
		send = m_Buses[source]->m_Sends[index];
		return true;
	}

	void AudioBusGraph::Invalidate() noexcept
	{
		m_Prepared = false;
		m_MaxFrames = 0;
		m_LastRenderedFrames = 0;
		m_BlockFrames = 0;
		m_BlockOpen = false;
		m_ProcessOrder.clear();
	}

	bool AudioBusGraph::Prepare(std::size_t maxFrames)
	{
		Invalidate();
		if (m_Buses.empty() || m_MasterBus >= m_Buses.size() || maxFrames == 0 ||
		    maxFrames > (std::numeric_limits<std::size_t>::max)() / 2)
			return false;
		if (m_Buses[m_MasterBus]->m_Parent != kInvalidAudioBusId)
			return false;

		const std::size_t busCount = m_Buses.size();
		// Parent links are also the mute-inheritance tree, so reject folder cycles
		// even when main-send has been disabled on one of the participating buses.
		for (std::size_t start = 0; start < busCount; ++start)
		{
			AudioBusId cursor = static_cast<AudioBusId>(start);
			for (std::size_t depth = 0; depth <= busCount; ++depth)
			{
				const AudioBusId parent = m_Buses[cursor]->m_Parent;
				if (parent == kInvalidAudioBusId)
					break;
				if (parent >= busCount || depth == busCount)
					return false;
				cursor = parent;
			}
		}

		std::vector<std::vector<AudioBusId>> outgoing(busCount);
		std::vector<std::size_t> incoming(busCount, 0);
		auto addEdge = [&](AudioBusId source, AudioBusId destination) -> bool
		{
			if (source >= busCount || destination >= busCount || source == destination)
				return false;
			outgoing[source].push_back(destination);
			++incoming[destination];
			return true;
		};

		for (std::size_t sourceIndex = 0; sourceIndex < busCount; ++sourceIndex)
		{
			const AudioBus& source = *m_Buses[sourceIndex];
			for (const AudioBusSend& send : source.m_Sends)
				if (!addEdge(static_cast<AudioBusId>(sourceIndex), send.Destination))
					return false;

			if (source.m_Id != m_MasterBus && source.m_MainSend)
			{
				const AudioBusId destination = source.m_Parent == kInvalidAudioBusId
					? m_MasterBus : source.m_Parent;
				if (!addEdge(source.m_Id, destination))
					return false;
			}
		}

		std::vector<AudioBusId> order;
		order.reserve(busCount);
		std::vector<bool> processed(busCount, false);
		while (order.size() < busCount)
		{
			AudioBusId next = kInvalidAudioBusId;
			for (std::size_t candidate = 0; candidate < busCount; ++candidate)
			{
				if (!processed[candidate] && incoming[candidate] == 0)
				{
					next = static_cast<AudioBusId>(candidate);
					break;
				}
			}
			if (next == kInvalidAudioBusId)
				return false;

			processed[next] = true;
			order.push_back(next);
			for (AudioBusId destination : outgoing[next])
				--incoming[destination];
		}

		const std::size_t sampleCapacity = maxFrames * 2;
		for (const auto& bus : m_Buses)
		{
			bus->m_PreFx.assign(sampleCapacity, 0.0f);
			bus->m_PostFx.assign(sampleCapacity, 0.0f);
			bus->m_PostFader.assign(sampleCapacity, 0.0f);
			bus->m_PendingInputs.assign(sampleCapacity, 0.0f);
		}
		m_ProcessOrder = std::move(order);
		m_MaxFrames = maxFrames;
		m_LastRenderedFrames = 0;
		m_Prepared = true;
		return true;
	}

	void AudioBusGraph::Reset() noexcept
	{
		m_LastRenderedFrames = 0;
		m_BlockFrames = 0;
		m_BlockOpen = false;
		for (const auto& bus : m_Buses)
		{
			std::fill(bus->m_PreFx.begin(), bus->m_PreFx.end(), 0.0f);
			std::fill(bus->m_PostFx.begin(), bus->m_PostFx.end(), 0.0f);
			std::fill(bus->m_PostFader.begin(), bus->m_PostFader.end(), 0.0f);
			std::fill(bus->m_PendingInputs.begin(), bus->m_PendingInputs.end(), 0.0f);
			for (const auto& effect : bus->m_Effects)
				effect->Reset();
		}
	}

	bool AudioBusGraph::IsEffectivelyMuted(const AudioBus& bus) const noexcept
	{
		if (bus.m_Muted.load(std::memory_order_relaxed))
			return true;
		AudioBusId ancestor = bus.m_Parent;
		for (std::size_t depth = 0; ancestor != kInvalidAudioBusId && depth < m_Buses.size(); ++depth)
		{
			if (ancestor >= m_Buses.size())
				return true;
			const AudioBus& parent = *m_Buses[ancestor];
			if (parent.m_Muted.load(std::memory_order_relaxed))
				return true;
			ancestor = parent.m_Parent;
		}
		return ancestor != kInvalidAudioBusId;
	}

	const float* AudioBusGraph::TapBuffer(const AudioBus& bus, AudioBusTap tap) const noexcept
	{
		switch (tap)
		{
		case AudioBusTap::PreFx: return bus.m_PreFx.data();
		case AudioBusTap::PostFx: return bus.m_PostFx.data();
		case AudioBusTap::PostFader: return bus.m_PostFader.data();
		}
		return nullptr;
	}

	const float* AudioBusGraph::GetTapBuffer(AudioBusId bus, AudioBusTap tap) const noexcept
	{
		if (!m_Prepared || bus >= m_Buses.size() || !IsValidTap(tap))
			return nullptr;
		return TapBuffer(*m_Buses[bus], tap);
	}

	void AudioBusGraph::Accumulate(float* destination, const float* source, float gain,
	                               std::size_t sampleCount) noexcept
	{
		if (source == nullptr)
			return;
		gain = ClampGain(gain, 0.0f);
		for (std::size_t sample = 0; sample < sampleCount; ++sample)
			destination[sample] = SafeSample(destination[sample] + SafeSample(source[sample]) * gain);
	}

	bool AudioBusGraph::BeginBlock(std::size_t frames) noexcept
	{
		if (!m_Prepared || m_BlockOpen || frames > m_MaxFrames)
			return false;
		m_BlockFrames = frames;
		m_BlockOpen = true;
		const std::size_t sampleCount = frames * 2;
		for (const auto& bus : m_Buses)
			std::fill_n(bus->m_PendingInputs.data(), sampleCount, 0.0f);
		return true;
	}

	bool AudioBusGraph::AccumulateInputs(const AudioBusInput* inputs, std::size_t inputCount,
	                                    std::size_t frames) noexcept
	{
		if (!m_Prepared || !m_BlockOpen || frames != m_BlockFrames ||
		    (inputCount > 0 && inputs == nullptr))
			return false;
		for (std::size_t i = 0; i < inputCount; ++i)
			if (inputs[i].Bus >= m_Buses.size())
				return false;

		const std::size_t sampleCount = frames * 2;
		for (std::size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex)
		{
			const AudioBusInput& input = inputs[inputIndex];
			const float gain = ClampGain(input.Gain, 0.0f);
			Accumulate(m_Buses[input.Bus]->m_PendingInputs.data(), input.InterleavedStereo,
			           gain, sampleCount);
		}
		return true;
	}

	bool AudioBusGraph::EndBlock(float* interleavedStereoOutput, std::size_t frames) noexcept
	{
		if (!m_Prepared || !m_BlockOpen || frames != m_BlockFrames || frames > m_MaxFrames ||
		    (frames > 0 && interleavedStereoOutput == nullptr))
			return false;
		if (frames == 0)
		{
			m_BlockFrames = 0;
			m_BlockOpen = false;
			m_LastRenderedFrames = 0;
			return true;
		}

		const std::size_t sampleCount = frames * 2;
		for (const auto& bus : m_Buses)
		{
			std::fill_n(bus->m_PreFx.data(), sampleCount, 0.0f);
			std::fill_n(bus->m_PostFx.data(), sampleCount, 0.0f);
			std::fill_n(bus->m_PostFader.data(), sampleCount, 0.0f);
			std::copy_n(bus->m_PendingInputs.data(), sampleCount, bus->m_PreFx.data());
		}

		for (AudioBusId busId : m_ProcessOrder)
		{
			AudioBus& bus = *m_Buses[busId];
			if (IsEffectivelyMuted(bus))
			{
				std::fill_n(bus.m_PreFx.data(), sampleCount, 0.0f);
				std::fill_n(bus.m_PostFx.data(), sampleCount, 0.0f);
				std::fill_n(bus.m_PostFader.data(), sampleCount, 0.0f);
				continue;
			}

			std::copy_n(bus.m_PreFx.data(), sampleCount, bus.m_PostFx.data());
			for (const auto& effect : bus.m_Effects)
			{
				effect->Process(bus.m_PostFx.data(), frames);
				for (std::size_t sample = 0; sample < sampleCount; ++sample)
					bus.m_PostFx[sample] = SafeSample(bus.m_PostFx[sample]);
			}

			const float gain = bus.m_Gain.load(std::memory_order_relaxed);
			const float pan = bus.m_Pan.load(std::memory_order_relaxed);
			const float leftPan = pan > 0.0f ? 1.0f - pan : 1.0f;
			const float rightPan = pan < 0.0f ? 1.0f + pan : 1.0f;
			for (std::size_t frame = 0; frame < frames; ++frame)
			{
				const std::size_t offset = frame * 2;
				bus.m_PostFader[offset] = SafeSample(bus.m_PostFx[offset] * gain * leftPan);
				bus.m_PostFader[offset + 1] = SafeSample(bus.m_PostFx[offset + 1] * gain * rightPan);
			}

			for (const AudioBusSend& send : bus.m_Sends)
			{
				const float* source = TapBuffer(bus, send.Tap);
				Accumulate(m_Buses[send.Destination]->m_PreFx.data(), source, send.Gain, sampleCount);
			}

			if (busId != m_MasterBus && bus.m_MainSend)
			{
				const AudioBusId destination = bus.m_Parent == kInvalidAudioBusId
					? m_MasterBus : bus.m_Parent;
				Accumulate(m_Buses[destination]->m_PreFx.data(), bus.m_PostFader.data(), 1.0f, sampleCount);
			}
		}

		const AudioBus& master = *m_Buses[m_MasterBus];
		std::copy_n(master.m_PostFader.data(), sampleCount, interleavedStereoOutput);
		m_BlockFrames = 0;
		m_BlockOpen = false;
		m_LastRenderedFrames = frames;
		return true;
	}

	bool AudioBusGraph::Process(const AudioBusInput* inputs, std::size_t inputCount,
	                           float* interleavedStereoOutput, std::size_t frames) noexcept
	{
		auto failClosed = [&]() noexcept
		{
			if (interleavedStereoOutput && frames <= (std::numeric_limits<std::size_t>::max)() / 2)
				std::fill_n(interleavedStereoOutput, frames * 2, 0.0f);
			if (!m_BlockOpen)
				m_LastRenderedFrames = 0;
			return false;
		};

		if (!BeginBlock(frames))
			return failClosed();
		if (!AccumulateInputs(inputs, inputCount, frames))
		{
			m_BlockFrames = 0;
			m_BlockOpen = false;
			return failClosed();
		}
		if (!EndBlock(interleavedStereoOutput, frames))
		{
			m_BlockFrames = 0;
			m_BlockOpen = false;
			return failClosed();
		}
		return true;
	}
}
