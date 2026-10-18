#include "wlpch.h"

#include "AudioDevice.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace Wasteland::Audio
{
	namespace
	{
		size_t BytesPerSample(AudioDevice::OutputFormat format)
		{
			switch (format)
			{
			case AudioDevice::OutputFormat::Float32:
			case AudioDevice::OutputFormat::Signed32:
				return 4;
			case AudioDevice::OutputFormat::Signed24:
				return 3;
			case AudioDevice::OutputFormat::Signed16:
				return 2;
			case AudioDevice::OutputFormat::Unsigned8:
				return 1;
			case AudioDevice::OutputFormat::Unknown:
			default:
				return 0;
			}
		}

		AudioDevice::OutputFormat ToOutputFormat(ma_format format)
		{
			switch (format)
			{
			case ma_format_f32: return AudioDevice::OutputFormat::Float32;
			case ma_format_s16: return AudioDevice::OutputFormat::Signed16;
			case ma_format_s24: return AudioDevice::OutputFormat::Signed24;
			case ma_format_s32: return AudioDevice::OutputFormat::Signed32;
			case ma_format_u8:  return AudioDevice::OutputFormat::Unsigned8;
			default:            return AudioDevice::OutputFormat::Unknown;
			}
		}
		bool ClearOutput(void *output, uint32_t frameCount,
			AudioDevice::OutputFormat format, uint32_t channels)
		{
			const size_t bytesPerSample = BytesPerSample(format);
			if (!output || bytesPerSample == 0 || channels == 0)
				return false;

			const size_t frames = static_cast<size_t>(frameCount);
			const size_t channelCount = static_cast<size_t>(channels);
			if (frames > std::numeric_limits<size_t>::max() / channelCount)
				return false;
			const size_t sampleCount = frames * channelCount;
			if (sampleCount > std::numeric_limits<size_t>::max() / bytesPerSample)
				return false;

			if (format == AudioDevice::OutputFormat::Unsigned8)
				std::memset(output, 0x80, sampleCount);
			else
				std::memset(output, 0, sampleCount * bytesPerSample);
			return true;
		}
	}

	struct AudioDevice::DeviceImpl
	{
		ma_device Device{};
		Mixer *pMixer = nullptr;
		bool Initialized = false;

		static void DataCallback(ma_device *device, void *output,
			const void *, ma_uint32 frameCount)
		{
			auto *impl = static_cast<DeviceImpl *>(device ? device->pUserData : nullptr);
			if (!impl || !impl->pMixer)
			{
				if (device)
					ClearOutput(output, frameCount, ToOutputFormat(device->playback.format),
						device->playback.channels);
				return;
			}

			AudioDevice::Render(*impl->pMixer, output, frameCount,
				ToOutputFormat(device->playback.format),
				device->playback.channels, device->sampleRate);
		}
	};

	AudioDevice::AudioDevice(Mixer &mixer)
		: m_Mixer(&mixer)
	{
	}

	AudioDevice::~AudioDevice()
	{
		Stop();
	}

	bool AudioDevice::Start(std::string *error)
	{
		if (error)
			error->clear();
		if (IsRunning())
			return true;

		Stop();
		auto impl = std::make_unique<DeviceImpl>();
		impl->pMixer = m_Mixer;

		ma_device_config config = ma_device_config_init(ma_device_type_playback);
		config.playback.format = ma_format_f32;
		config.playback.channels = 2;
		config.sampleRate = static_cast<ma_uint32>(Mixer::kEngineMixRate);
		config.periodSizeInMilliseconds = 10;
		config.noPreSilencedOutputBuffer = MA_TRUE;
		config.dataCallback = &DeviceImpl::DataCallback;
		config.pUserData = impl.get();
		config.resampling.algorithm = ma_resample_algorithm_linear;

		ma_result result = ma_device_init(nullptr, &config, &impl->Device);
		if (result != MA_SUCCESS)
		{
			if (error)
				*error = std::string("miniaudio playback device initialization failed: ") +
					ma_result_description(result);
			return false;
		}
		impl->Initialized = true;

		// Verify the callback-facing configuration, and also verify that the
		// backend reports a real native endpoint rate. The latter is allowed to
		// differ: miniaudio's device converter handles that rate boundary.
		if (impl->Device.sampleRate != static_cast<ma_uint32>(Mixer::kEngineMixRate) ||
			impl->Device.playback.format != ma_format_f32 ||
			impl->Device.playback.channels != 2 ||
			impl->Device.playback.internalSampleRate == 0 ||
			impl->Device.playback.internalFormat == ma_format_unknown ||
			impl->Device.playback.internalChannels == 0)
		{
			if (error)
				*error = "miniaudio returned an unexpected callback format or invalid native endpoint format";
			ma_device_uninit(&impl->Device);
			impl->Initialized = false;
			return false;
		}

		m_EndpointSampleRate.store(impl->Device.playback.internalSampleRate,
			std::memory_order_relaxed);
		m_Impl = std::move(impl);
		// Publish running before starting the callback thread. AudioSystem::Update
		// must stop wall-clock advancement before the first device buffer arrives.
		m_Running.store(true, std::memory_order_release);
		result = ma_device_start(&m_Impl->Device);
		if (result != MA_SUCCESS)
		{
			if (error)
				*error = std::string("miniaudio playback device start failed: ") +
					ma_result_description(result);
			ma_device_stop(&m_Impl->Device);
			ma_device_uninit(&m_Impl->Device);
			m_Impl->Initialized = false;
			m_Running.store(false, std::memory_order_release);
			m_EndpointSampleRate.store(0, std::memory_order_relaxed);
			m_Impl.reset();
			return false;
		}
		WL_CORE_INFO("Audio output started: engine mix {0} Hz, native endpoint {1} Hz",
			Mixer::kEngineMixRate, m_EndpointSampleRate.load(std::memory_order_relaxed));
		return true;
	}

	void AudioDevice::Stop()
	{
		if (!m_Impl)
		{
			m_Running.store(false, std::memory_order_release);
			m_EndpointSampleRate.store(0, std::memory_order_relaxed);
			return;
		}

		// Keep the running flag true until miniaudio has quiesced its callback
		// thread. AudioSystem::Update then cannot advance the same mixer voices
		// while the device is delivering its final buffer.
		if (m_Impl->Initialized)
		{
			ma_device_stop(&m_Impl->Device);
			ma_device_uninit(&m_Impl->Device);
			m_Impl->Initialized = false;
		}
		m_Running.store(false, std::memory_order_release);
		m_EndpointSampleRate.store(0, std::memory_order_relaxed);
		m_Impl.reset();
	}

	bool AudioDevice::Render(Mixer &mixer, void *output, uint32_t frameCount,
		OutputFormat format, uint32_t channels, uint32_t sampleRate)
	{
		if (!output)
			return false;
		if (frameCount == 0)
			return true;
		if (!ClearOutput(output, frameCount, format, channels))
			return false;

		if (format != OutputFormat::Float32 || channels != 2 ||
			sampleRate != static_cast<uint32_t>(Mixer::kEngineMixRate) ||
			frameCount > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			return false;

		mixer.MixRealtime(static_cast<float *>(output), static_cast<int>(frameCount));
		return true;
	}

}
