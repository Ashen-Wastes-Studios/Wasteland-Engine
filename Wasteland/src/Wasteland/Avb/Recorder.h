#pragma once

#include "Wasteland/Core/Core.h"
#include "Wasteland/Avb/AudioBus.h"
#include "Wasteland/Audio/AudioClip.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Wasteland::Avb
{
#if defined(AVB_RECORDER_TEST_HOOKS)
	// Test hooks must be installed/cleared only while no recorder or stem writer can invoke them.
	using AvbStemPublicationTestHook = void (*)(void* context);
	void SetAvbStemPublicationTestHookForTesting(AvbStemPublicationTestHook hook,
		void* context) noexcept;
	using AvbStemTempFileOpenedTestHook = void (*)(const std::string& utf8Path, void* context);
	void SetAvbStemTempFileOpenedTestHookForTesting(AvbStemTempFileOpenedTestHook hook,
		void* context) noexcept;
	void SetAvbForcePrimaryTempDispositionFailureForTesting(bool force) noexcept;
#if !defined(_WIN32)
	void SetAvbForceStageDirectoryOpenFailureForTesting(bool force) noexcept;
#endif
#endif

	// Materialized take produced by AvbRecorder::StopRecording(). The captured
	// clip is immediately usable by playback and is also published as a WAV.
	struct AvbRecordedTake
	{
		Ref<Audio::AudioClip> Clip;
		std::string SourcePath; // Original caller path spelling; metadata, not a stable file locator.
		double SourceOffset = 0.0; // Seconds into the source WAV.

		std::size_t FrameCount = 0;
	};

	// One entry in the stem render matrix. The graph's selected tap is written
	// to a stereo WAV and retained in the result for deterministic verification.
	struct AvbStemSpec
	{
		AudioBusId Bus = kInvalidAudioBusId;
		AudioBusTap Tap = AudioBusTap::PostFader;
		std::string TrackName;
	};

	struct AvbStemRenderOptions
	{
		std::string OutputRoot;
		std::string ProjectName;
		std::string RegionName;
		// Supports $project, $region, and $track. Both slash characters in the
		// template are directory separators; a missing extension becomes .wav.
		std::string FilenamePattern = "$project\\$region-$track";
	};

	struct AvbRenderedStem
	{
		std::string TrackName;
		std::string FilePath;
		std::vector<float> InterleavedStereo;
	};

	// Preallocated single-producer/single-consumer audio capture. The producer
	// callback only calls IngestCaptureBlock(); it performs no allocation,
	// locking, or file I/O. DrainCapture() and StopRecording() are control-thread
	// operations, and all WAV/file/vector work happens there.
	class AvbRecorder
	{
	public:
		explicit AvbRecorder(std::size_t ringCapacityFrames = kAvbAudioSampleRate * 10);
		~AvbRecorder();
		AvbRecorder(const AvbRecorder&) = delete;
		AvbRecorder& operator=(const AvbRecorder&) = delete;
		AvbRecorder(AvbRecorder&&) = delete;
		AvbRecorder& operator=(AvbRecorder&&) = delete;

		// Arms the ring and pins the output directory through a no-follow handle
		// walk. Symlink/reparse components are rejected; the caller must keep the
		// path components trusted until this call returns because this path-only
		// API binds to whichever ordinary directories the walk opens. Later path
		// renames/replacements cannot redirect publication away from the pinned
		// directory.
		// The ring is measured in mono frames; incoming mono or stereo float32
		// blocks are accepted at the fixed AVB 48 kHz rate.
		bool StartRecording(const std::string& wavPath, std::string* error = nullptr);

		// Deterministic input seam, also used directly by the miniaudio callback.
		// Supports one or two interleaved channels. Returns false for invalid
		// input or if the ring overflows; already accepted frames remain queued.
		bool IngestCaptureBlock(const float* interleavedSamples, std::size_t frames,
		                        std::size_t channels) noexcept;

		// Move queued capture frames into an off-callback accumulation buffer.
		// Call regularly during a long recording to avoid ring overflow.
		std::size_t DrainCapture();

		// Stops capture if active, drains the ring, securely publishes a mono
		// PCM16 WAV, and returns the captured clip with source offset 0.
		bool StopRecording(AvbRecordedTake& take, std::string* error = nullptr);

		// Optional physical capture backend. The device requests float32 stereo
		// at 48 kHz; miniaudio adapts native endpoint details. Tests can omit this
		// backend and inject capture blocks deterministically.
		bool StartCapture(std::string* error = nullptr);
		void StopCapture() noexcept;
		bool IsCapturing() const noexcept;
		std::size_t DroppedFrames() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};

	// Render the requested AudioBusGraph taps from an interleaved-stereo source
	// matrix. Input buffers contain `frames` stereo frames each. The graph is
	// reset first, then processed in prepared-capacity chunks; the same Process()
	// path is used by realtime playback. OutputRoot is traversed without following
	// symlink/reparse components, and output paths remain below its pinned directory.
	bool RenderStems(AudioBusGraph& graph, const AudioBusInput* inputs,
	                 std::size_t inputCount, std::size_t frames,
	                 const std::vector<AvbStemSpec>& stems,
	                 const AvbStemRenderOptions& options,
	                 std::vector<AvbRenderedStem>& rendered,
	                 std::string* error = nullptr);
}
