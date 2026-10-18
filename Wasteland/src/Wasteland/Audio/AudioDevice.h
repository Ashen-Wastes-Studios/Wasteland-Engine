#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "Wasteland/Audio/Mixer.h"

namespace Wasteland::Audio
{

	// miniaudio playback device. The engine-facing callback is always float32
	// stereo at Mixer::kEngineMixRate; miniaudio converts that stream to the
	// native endpoint format/rate when necessary.
	class AudioDevice
	{
	public:
		enum class OutputFormat : uint8_t
		{
			Unknown,
			Float32,
			Signed16,
			Signed24,
			Signed32,
			Unsigned8
		};

		explicit AudioDevice(Mixer &mixer);
		~AudioDevice();

		AudioDevice(const AudioDevice &) = delete;
		AudioDevice &operator=(const AudioDevice &) = delete;

		// Opens and starts the default playback device. The callback rate is
		// fixed at 48 kHz even when the physical endpoint is 44.1 kHz.
		// Returns false with a readable error when initialization/start fails.
		bool Start(std::string *error = nullptr);
		void Stop();
		bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

		// Native endpoint rate selected by miniaudio (for diagnostics). Zero if
		// the device is not open. A different rate is valid: miniaudio resamples
		// between it and the fixed engine callback rate.
		uint32_t GetEndpointSampleRate() const
		{
			return m_EndpointSampleRate.load(std::memory_order_relaxed);
		}

		// Shared callback path, exposed so native tests can exercise callback
		// validation and silence behavior without opening physical hardware.
		// Invalid known formats are silenced and rejected; only float32 stereo
		// at the engine's fixed rate is mixed. This function performs no allocation.
		static bool Render(Mixer &mixer, void *output, uint32_t frameCount,
			OutputFormat format, uint32_t channels, uint32_t sampleRate);

	private:
		struct DeviceImpl;
		std::unique_ptr<DeviceImpl> m_Impl;
		Mixer *m_Mixer = nullptr;
		std::atomic<bool> m_Running{false};
		std::atomic<uint32_t> m_EndpointSampleRate{0};
	};

}
