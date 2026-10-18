#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "Wasteland/Avb/Recorder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <winioctl.h>
#endif

using namespace Wasteland::Avb;
using Wasteland::Audio::AudioClip;

namespace
{
	int g_Checks = 0;
	int g_Failures = 0;
	constexpr float kPi = 3.14159265358979323846f;
	constexpr std::uint32_t kSampleRate = 48000;

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

	bool BitIdentical(const std::vector<float>& left, const std::vector<float>& right)
	{
		return left.size() == right.size() &&
			(left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(float)) == 0);
	}

	std::filesystem::path MakeTempRoot(const char* suffix)
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		const char* configuredScratch = std::getenv("TMPDIR");
		const std::filesystem::path scratch = configuredScratch && configuredScratch[0]
			? std::filesystem::path(configuredScratch)
			: std::filesystem::current_path() / ".tmp-avb-recording";
		std::filesystem::create_directories(scratch);
		const auto path = scratch /
			(std::string("WastelandAvbRecording-") + suffix + "-" + std::to_string(stamp));
		std::filesystem::remove_all(path);
		std::filesystem::create_directories(path);
		return path;
	}

#if defined(_WIN32)
	struct MountPointReparseBufferForTest
	{
		DWORD ReparseTag;
		WORD ReparseDataLength;
		WORD Reserved;
		WORD SubstituteNameOffset;
		WORD SubstituteNameLength;
		WORD PrintNameOffset;
		WORD PrintNameLength;
		WCHAR PathBuffer[1];
	};
#endif

	bool CreateDirectoryLink(const std::filesystem::path& target,
		const std::filesystem::path& link, std::error_code& error)
	{
#if defined(_WIN32)
		if (!std::filesystem::create_directory(link, error) || error)
			return false;

		const std::wstring printName = std::filesystem::absolute(target).lexically_normal().native();
		const std::wstring substituteName = L"\\??\\" + printName;
		const std::size_t substituteNameBytes = substituteName.size() * sizeof(WCHAR);
		const std::size_t printNameBytes = printName.size() * sizeof(WCHAR);
		const std::size_t pathBufferBytes = substituteNameBytes + sizeof(WCHAR) + printNameBytes + sizeof(WCHAR);
		const std::size_t reparseDataLength = 8 + pathBufferBytes;
		const std::size_t reparseBufferSize = 8 + reparseDataLength;
		std::vector<std::uint32_t> alignedBuffer(
			(reparseBufferSize + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t));
		auto* reparse = reinterpret_cast<MountPointReparseBufferForTest*>(alignedBuffer.data());
		reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
		reparse->ReparseDataLength = static_cast<WORD>(reparseDataLength);
		reparse->Reserved = 0;
		reparse->SubstituteNameOffset = 0;
		reparse->SubstituteNameLength = static_cast<WORD>(substituteNameBytes);
		reparse->PrintNameOffset = static_cast<WORD>(substituteNameBytes + sizeof(WCHAR));
		reparse->PrintNameLength = static_cast<WORD>(printNameBytes);
		WCHAR* pathBuffer = reparse->PathBuffer;
		std::memcpy(pathBuffer, substituteName.data(), substituteName.size() * sizeof(WCHAR));
		pathBuffer[substituteName.size()] = L'\0';
		std::memcpy(pathBuffer + substituteName.size() + 1, printName.data(), printName.size() * sizeof(WCHAR));
		pathBuffer[substituteName.size() + printName.size() + 1] = L'\0';

		HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (directory == INVALID_HANDLE_VALUE)
		{
			error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			std::fprintf(stderr, "CreateDirectoryLink CreateFileW failed: %d %s\n", error.value(), error.message().c_str());
			std::error_code cleanupError;
			std::filesystem::remove(link, cleanupError);
			return false;
		}

		DWORD bytesReturned = 0;
		const BOOL succeeded = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, reparse,
			static_cast<DWORD>(reparseBufferSize), nullptr, 0, &bytesReturned, nullptr);
		const DWORD win32Error = succeeded ? ERROR_SUCCESS : GetLastError();
		CloseHandle(directory);
		if (!succeeded)
		{
			error = std::error_code(static_cast<int>(win32Error), std::system_category());
			std::fprintf(stderr, "CreateDirectoryLink DeviceIoControl failed: %d %s\n", error.value(), error.message().c_str());
			std::error_code cleanupError;
			std::filesystem::remove(link, cleanupError);
			return false;
		}
		return true;
#else
		std::filesystem::create_directory_symlink(target, link, error);
		return !error;
#endif
	}

	std::vector<float> MakeStereoTone(std::size_t frames, float frequency, float amplitude)
	{
		std::vector<float> samples(frames * 2);
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			const float sample = amplitude * std::sin(2.0f * kPi * frequency *
				static_cast<float>(frame) / static_cast<float>(kSampleRate));
			samples[frame * 2] = sample;
			samples[frame * 2 + 1] = sample;
		}
		return samples;
	}

	void TestCaptureToneWritesWavAndCreatesZeroOffsetTake(const std::filesystem::path& root)
	{
		const std::filesystem::path wavPath = root / "takes" / "known-tone.wav";
		AvbRecorder recorder(kSampleRate * 2);
		std::string error;
		Check(recorder.StartRecording(wavPath.string(), &error),
			"recording accepts a destination before any callback samples arrive");

		const std::vector<float> tone = MakeStereoTone(kSampleRate, 440.0f, 0.625f);
		for (std::size_t frame = 0; frame < kSampleRate; frame += 480)
		{
			Check(recorder.IngestCaptureBlock(tone.data() + frame * 2, 480, 2),
				"the injectable capture seam accepts a stereo callback block");
			Check(recorder.DrainCapture() == 480,
				"the non-callback writer side drains each capture block");
		}

		AvbRecordedTake take;
		Check(recorder.StopRecording(take, &error), "stopping recording publishes the WAV and returns a take");
		Check(take.Clip && take.Clip->IsValid(), "the recorded take contains a valid captured AudioClip");
		Check(take.SourcePath == wavPath.string(), "the take retains the caller's original path spelling as metadata");
		Check(take.SourceOffset == 0.0, "a newly recorded take starts at source offset zero");
		Check(take.FrameCount == kSampleRate, "the take retains all one second of captured frames");
		Check(recorder.DroppedFrames() == 0, "draining callback blocks prevents capture-ring overflow");
		Check(std::filesystem::exists(wavPath), "recording creates the requested WAV file");

		AudioClip::LoadResult loadResult = AudioClip::LoadResult::InvalidFormat;
		auto reloaded = AudioClip::LoadWav(wavPath.string(), &loadResult);
		Check(reloaded && loadResult == AudioClip::LoadResult::Success,
			"AudioClip::LoadWav reads the recorded file successfully");
		if (reloaded)
		{
			CheckNear(reloaded->GetDuration(), 1.0f, 1.0f / kSampleRate,
				"the recorded WAV duration is one second at 48 kHz");
			CheckNear(reloaded->GetPeak(), 0.625f, 0.002f,
				"the recorded WAV peak preserves the known tone amplitude within PCM tolerance");
		}
	}

#if defined(_WIN32)
	struct TempFileReadProbe
	{
		bool Called = false;
		bool OpenSucceeded = false;
		DWORD Error = ERROR_SUCCESS;
	};

	void ProbeTempFileReadAccess(const std::string& utf8Path, void* context)
	{
		auto& probe = *static_cast<TempFileReadProbe*>(context);
		probe.Called = true;
		const std::filesystem::path path = std::filesystem::u8path(utf8Path);
		HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (reader == INVALID_HANDLE_VALUE)
			probe.Error = GetLastError();
		else
		{
			probe.OpenSucceeded = true;
			CloseHandle(reader);
		}
	}

	void TestTempRecordingWavCannotBeOpenedByAnotherReader(const std::filesystem::path& root)
	{
		const std::filesystem::path wavPath = root / "temp-read-probe" / "take.wav";
		AvbRecorder recorder(16);
		std::string error;
		Check(recorder.StartRecording(wavPath.string(), &error),
			"recording starts for temporary-file sharing probe");
		const float samples[] = { 0.125f, -0.25f, 0.375f, -0.5f };
		Check(recorder.IngestCaptureBlock(samples, 4, 1),
			"sharing-probe samples enter the capture ring");

		TempFileReadProbe probe;
		SetAvbStemTempFileOpenedTestHookForTesting(&ProbeTempFileReadAccess, &probe);
		AvbRecordedTake take;
		const bool stopped = recorder.StopRecording(take, &error);
		SetAvbStemTempFileOpenedTestHookForTesting(nullptr, nullptr);

		Check(stopped, "temporary-file sharing probe still publishes the take");
		Check(probe.Called, "the probe observes the temporary WAV before publication");
		Check(!probe.OpenSucceeded && probe.Error == ERROR_SHARING_VIOLATION,
			"another reader cannot open an in-progress temporary WAV");
	}

	struct TempPublishFailureProbe
	{
		std::filesystem::path FinalPath;
		bool Called = false;
		bool CreatedDirectory = false;
		DWORD Error = ERROR_SUCCESS;
	};

	void MakeTempPublicationFail(const std::string&, void* context)
	{
		auto& probe = *static_cast<TempPublishFailureProbe*>(context);
		probe.Called = true;
		probe.CreatedDirectory = CreateDirectoryW(probe.FinalPath.c_str(), nullptr) != FALSE;
		if (!probe.CreatedDirectory)
			probe.Error = GetLastError();
	}

	std::size_t CountAvbTemporaryFiles(const std::filesystem::path& directory)
	{
		std::size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; it != end && !error; it.increment(error))
		{
			const std::string name = it->path().filename().string();
			if (name.rfind(".avb-stem-", 0) == 0 && it->path().extension() == ".tmp")
				++count;
		}
		return error ? (std::numeric_limits<std::size_t>::max)() : count;
	}

	void TestFailedTempPublicationRetriesHandleBoundDeletion(const std::filesystem::path& root)
	{
		const std::filesystem::path wavPath = root / "cleanup-fallback" / "take.wav";
		AvbRecorder recorder(16);
		std::string error;
		Check(recorder.StartRecording(wavPath.string(), &error),
			"recording starts for temporary-file cleanup fallback probe");
		const float samples[] = { 0.125f, -0.25f };
		Check(recorder.IngestCaptureBlock(samples, 2, 1),
			"cleanup-probe samples enter the capture ring");

		TempPublishFailureProbe probe{ wavPath };
		SetAvbStemTempFileOpenedTestHookForTesting(&MakeTempPublicationFail, &probe);
		SetAvbForcePrimaryTempDispositionFailureForTesting(true);
		AvbRecordedTake take;
		const bool stopped = recorder.StopRecording(take, &error);
		SetAvbForcePrimaryTempDispositionFailureForTesting(false);
		SetAvbStemTempFileOpenedTestHookForTesting(nullptr, nullptr);

		Check(!stopped, "publication fails after a directory appears at the WAV leaf");
		Check(probe.Called && probe.CreatedDirectory && std::filesystem::is_directory(wavPath),
			"the test hook deterministically blocks replacement with a directory");
		Check(CountAvbTemporaryFiles(wavPath.parent_path()) == 0,
			"a fallback deletion removes the unpublished temporary WAV after primary cleanup fails");
	}
#endif

#if !defined(_WIN32) && defined(AVB_RECORDER_TEST_HOOKS)
	std::size_t CountAvbStagingDirectories(const std::filesystem::path& directory)
	{
		std::size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; it != end && !error; it.increment(error))
		{
			const std::string name = it->path().filename().string();
			if (name.rfind(".avb-stem-stage-", 0) == 0 && it->is_directory())
				++count;
		}
		return error ? (std::numeric_limits<std::size_t>::max)() : count;
	}

	void TestFailedStagingDirectoryOpenRemovesCreatedDirectory(const std::filesystem::path& root)
	{
		const std::filesystem::path wavPath = root / "stage-open-failure" / "take.wav";
		AvbRecorder recorder(16);
		std::string error;
		Check(recorder.StartRecording(wavPath.string(), &error),
			"recording starts for staging-directory cleanup probe");
		const float samples[] = { 0.25f, -0.5f };
		Check(recorder.IngestCaptureBlock(samples, 2, 1),
			"staging-cleanup samples enter the capture ring");

		SetAvbForceStageDirectoryOpenFailureForTesting(true);
		AvbRecordedTake take;
		const bool stopped = recorder.StopRecording(take, &error);
		SetAvbForceStageDirectoryOpenFailureForTesting(false);

		Check(!stopped, "forced staging-directory open failure prevents WAV publication");
		Check(CountAvbStagingDirectories(wavPath.parent_path()) == 0,
			"failed staging-directory open removes only the directory created for this temporary WAV");
		Check(!std::filesystem::exists(wavPath), "failed staging setup leaves no published WAV");
	}
#endif

	void TestRecorderBindsOutputDirectoryAtStart(const std::filesystem::path& root)
	{
		const std::filesystem::path outputDirectory = root / "recording";
		const std::filesystem::path movedDirectory = root / "recording-original";
		const std::filesystem::path outputPath = outputDirectory / "capture.wav";
		const std::filesystem::path sentinelPath = root / "directory-binding-sentinel.wav";
		const std::string initialSentinelContents = "sentinel written after recording starts";
		const std::string sentinelContents = "replacement directory hard-link sentinel";
		std::filesystem::create_directories(outputDirectory);

		AvbRecorder recorder(16);
		std::string error;
		const bool startSucceeded = recorder.StartRecording(outputPath.string(), &error);
		Check(startSucceeded, "recording starts before its caller-owned destination directory is renamed");
		if (!startSucceeded)
			return;

		{
			std::ofstream sentinelFile(outputPath, std::ios::binary | std::ios::trunc);
			sentinelFile.write(initialSentinelContents.data(),
				static_cast<std::streamsize>(initialSentinelContents.size()));
			Check(sentinelFile.good(), "directory-binding setup writes a sentinel after recording starts");
			if (!sentinelFile.good())
				return;
		}

		std::error_code filesystemError;
		std::filesystem::rename(outputDirectory, movedDirectory, filesystemError);
		Check(!filesystemError, "directory-binding setup renames the original destination directory");
		if (filesystemError)
			return;
		std::filesystem::create_directories(outputDirectory, filesystemError);
		Check(!filesystemError, "directory-binding setup creates a replacement at the original directory path");
		if (filesystemError)
			return;

		{
			std::ofstream sentinelFile(sentinelPath, std::ios::binary | std::ios::trunc);
			sentinelFile.write(sentinelContents.data(), static_cast<std::streamsize>(sentinelContents.size()));
			Check(sentinelFile.good(), "directory-binding setup writes a separate sentinel file");
			if (!sentinelFile.good())
				return;
		}
		std::error_code hardLinkError;
		std::filesystem::create_hard_link(sentinelPath, outputPath, hardLinkError);
		Check(!hardLinkError, "replacement directory contains the output leaf as a hard link to the sentinel");
		if (hardLinkError)
			return;

		const std::array<float, 4> samples = { 0.125f, -0.25f, 0.375f, -0.5f };
		Check(recorder.IngestCaptureBlock(samples.data(), samples.size(), 1),
			"the recorder accepts mono samples after its destination directory is replaced");
		Check(recorder.DrainCapture() == samples.size(),
			"the recorder drains all mono samples after its destination directory is replaced");

		AvbRecordedTake take;
		const bool stopSucceeded = recorder.StopRecording(take, &error);
		Check(stopSucceeded && take.Clip && take.Clip->IsValid(),
			"stopping succeeds with a valid take clip after its destination directory is renamed");

		const std::filesystem::path movedOutputPath = movedDirectory / outputPath.filename();
		Check(std::filesystem::exists(movedOutputPath),
			"the recording WAV is created in the original directory at its renamed path");
		AudioClip::LoadResult loadResult = AudioClip::LoadResult::InvalidFormat;
		auto movedRecording = AudioClip::LoadWav(movedOutputPath.string(), &loadResult);
		Check(movedRecording && loadResult == AudioClip::LoadResult::Success &&
			movedRecording->GetFrameCount() == samples.size(),
			"the moved-directory output is a valid WAV containing the captured mono frames");

		std::error_code equivalentError;
		const bool replacementStillAliasesSentinel =
			std::filesystem::equivalent(outputPath, sentinelPath, equivalentError);
		Check(!equivalentError && replacementStillAliasesSentinel,
			"the output leaf in the replacement directory still aliases the sentinel file");
		std::ifstream sentinelFile(sentinelPath, std::ios::binary);
		const std::string actualSentinelContents((std::istreambuf_iterator<char>(sentinelFile)),
			std::istreambuf_iterator<char>());
		Check(actualSentinelContents == sentinelContents,
			"recording leaves the separate sentinel file contents unchanged");
	}

	void TestRecorderRejectsLinkedOutputParent(const std::filesystem::path& root)
	{
		const std::filesystem::path targetParent = root / "recording-parent-target";
		const std::filesystem::path linkedParent = root / "recording-parent-link";
		std::error_code setupError;
		std::filesystem::create_directories(targetParent, setupError);
		Check(!setupError, "recording link setup creates the real parent directory");
		if (setupError)
			return;

		const bool linkCreated = CreateDirectoryLink(targetParent, linkedParent, setupError);
		Check(linkCreated, "recording link setup creates a directory symlink or junction");
		if (!linkCreated)
			return;

		AvbRecorder recorder(16);
		std::string error;
		const bool started = recorder.StartRecording((linkedParent / "capture.wav").string(), &error);
		Check(!started, "StartRecording rejects an output parent path containing a directory link");
		if (started)
		{
			AvbRecordedTake take;
			recorder.StopRecording(take, &error);
		}
	}

	void TestAudioClipWavRoundTripPreservesSamples(const std::filesystem::path& root)
	{
		std::filesystem::create_directories(root);
		std::vector<float> samples(8192);
		for (std::size_t i = 0; i < samples.size(); ++i)
			samples[i] = 0.8f * std::sin(2.0f * kPi * 997.0f * static_cast<float>(i) / kSampleRate);

		auto source = AudioClip::FromSamples(samples, static_cast<int>(kSampleRate));
		const auto firstPath = root / "round-trip-a.wav";
		const auto secondPath = root / "round-trip-b.wav";
		Check(source && source->SaveWav(firstPath.string()), "AudioClip::SaveWav writes source samples as PCM WAV");
		auto first = AudioClip::LoadWav(firstPath.string());
		Check(first && first->IsValid(), "AudioClip::LoadWav decodes the first saved file");
		Check(first && first->SaveWav(secondPath.string()), "a loaded clip can be written back through SaveWav");
		auto second = AudioClip::LoadWav(secondPath.string());
		Check(second && second->IsValid(), "the second saved file loads as a valid clip");
		if (first && second)
		{
			Check(first->GetFrameCount() == samples.size() && second->GetFrameCount() == samples.size(),
				"the WAV round trip preserves every frame");
			Check(first->GetSampleRate() == static_cast<int>(kSampleRate) &&
				second->GetSampleRate() == static_cast<int>(kSampleRate),
				"the WAV round trip preserves the 48 kHz sample rate");
			CheckNear(second->GetDuration(), first->GetDuration(), 1.0f / kSampleRate,
				"the WAV round trip preserves duration");
			for (std::size_t i = 0; i < samples.size(); ++i)
				CheckNear(second->GetSamples()[i], first->GetSamples()[i], 1.0f / 32768.0f,
					"the LoadWav/SaveWav round trip preserves samples to 16-bit PCM tolerance");
		}
	}

	void TestCaptureOverflowDoesNotPublishTruncatedTake(const std::filesystem::path& root)
	{
		const std::filesystem::path wavPath = root / "overflow.wav";
		AvbRecorder recorder(2);
		std::string error;
		Check(recorder.StartRecording(wavPath.string(), &error),
			"overflow test recording starts with a two-frame ring");
		const std::array<float, 3> samples = { 0.25f, -0.25f, 0.5f };
		Check(!recorder.IngestCaptureBlock(samples.data(), samples.size(), 1),
			"capture reports a block that exceeded ring capacity");
		Check(recorder.DroppedFrames() == 1, "capture reports the exact number of dropped frames");
		AvbRecordedTake take;
		Check(!recorder.StopRecording(take, &error),
			"a recording with a capture overrun is not published as a successful take");
		Check(!take.Clip && !std::filesystem::exists(wavPath),
			"an overrun does not create a silently truncated WAV take");
		Check(error.find("overrun") != std::string::npos,
			"the rejected take reports capture overrun in its error");
	}

	void TestRecorderOutputHardLinkCannotModifyOutsideFile(const std::filesystem::path& root)
	{
		const std::filesystem::path outputPath = root / "recording" / "capture.wav";
		const std::filesystem::path outsideSentinel = root / "outside-sentinel.wav";
		const std::string sentinelContents = "AVB recorder hard-link sentinel must remain intact";
		std::filesystem::create_directories(outputPath.parent_path());
		{
			std::ofstream sentinelFile(outsideSentinel, std::ios::binary | std::ios::trunc);
			sentinelFile.write(sentinelContents.data(), static_cast<std::streamsize>(sentinelContents.size()));
			Check(sentinelFile.good(), "recorder hard-link test setup writes the outside sentinel file");
			if (!sentinelFile.good())
				return;
		}

		AvbRecorder recorder(16);
		std::string error;
		Check(recorder.StartRecording(outputPath.string(), &error),
			"recording starts with the requested raw output path");
		const std::array<float, 4> samples = { 0.125f, -0.25f, 0.375f, -0.5f };
		Check(recorder.IngestCaptureBlock(samples.data(), samples.size(), 1),
			"the recorder accepts a short mono capture block");
		Check(recorder.DrainCapture() == samples.size(),
			"the recorder drains every frame from the short mono capture block");

		std::error_code hardLinkError;
		std::filesystem::create_hard_link(outsideSentinel, outputPath, hardLinkError);
		Check(!hardLinkError,
			"recorder hard-link setup creates a native same-volume link at the exact output path");
		if (hardLinkError)
			return;

		AvbRecordedTake take;
		const bool stopSucceeded = recorder.StopRecording(take, &error);
		std::ifstream sentinelFile(outsideSentinel, std::ios::binary);
		const std::string actualContents((std::istreambuf_iterator<char>(sentinelFile)),
			std::istreambuf_iterator<char>());
		Check(actualContents == sentinelContents,
			"stopping a recording cannot truncate or modify an outside file hard-linked at its output path");
		if (stopSucceeded)
		{
			std::error_code equivalentError;
			const bool outputAliasesSentinel = std::filesystem::equivalent(outputPath, outsideSentinel, equivalentError);
			Check(!equivalentError && !outputAliasesSentinel,
				"a successful recording replaces the hard link instead of retaining the outside-file alias");
		}
	}

	struct TestBusSetup
	{
		AudioBusId Master = kInvalidAudioBusId;
		AudioBusId Drums = kInvalidAudioBusId;
		AudioBusId Lead = kInvalidAudioBusId;
	};

	TestBusSetup ConfigureTestGraph(AudioBusGraph& graph, std::size_t maxFrames)
	{
		TestBusSetup ids;
		ids.Master = graph.AddBus();
		ids.Drums = graph.AddBus();
		ids.Lead = graph.AddBus();
		graph.SetMasterBus(ids.Master);
		graph.SetParentBus(ids.Drums, ids.Master);
		graph.SetParentBus(ids.Lead, ids.Master);
		graph.GetBus(ids.Drums)->SetGain(0.75f);
		graph.GetBus(ids.Drums)->SetPan(-0.25f);
		graph.GetBus(ids.Lead)->SetGain(0.5f);
		graph.GetBus(ids.Lead)->SetPan(0.5f);
		graph.Prepare(maxFrames);
		return ids;
	}

	std::vector<AvbRenderedStem> RenderRealtimeTaps(AudioBusGraph& graph,
		const std::vector<AudioBusInput>& sources, std::size_t frames,
		const std::vector<AvbStemSpec>& stems)
	{
		std::vector<AvbRenderedStem> rendered(stems.size());
		for (std::size_t stem = 0; stem < stems.size(); ++stem)
		{
			rendered[stem].TrackName = stems[stem].TrackName;
			rendered[stem].InterleavedStereo.resize(frames * 2);
		}
		std::vector<AudioBusInput> blockInputs(sources.size());
		std::vector<float> masterOutput(graph.MaxFrames() * 2);
		graph.Reset();
		for (std::size_t offset = 0; offset < frames;)
		{
			const std::size_t count = std::min(graph.MaxFrames(), frames - offset);
			for (std::size_t i = 0; i < sources.size(); ++i)
			{
				blockInputs[i] = sources[i];
				if (blockInputs[i].InterleavedStereo)
					blockInputs[i].InterleavedStereo += offset * 2;
			}
			if (!graph.Process(blockInputs.data(), blockInputs.size(), masterOutput.data(), count))
				return {};
			for (std::size_t stem = 0; stem < stems.size(); ++stem)
			{
				const float* tap = graph.GetTapBuffer(stems[stem].Bus, stems[stem].Tap);
				if (!tap)
					return {};
				std::copy_n(tap, count * 2, rendered[stem].InterleavedStereo.data() + offset * 2);
			}
			offset += count;
		}
		return rendered;
	}

	void TestStemWildcardTreeAndBitIdenticalRenders(const std::filesystem::path& root)
	{
		constexpr std::size_t totalFrames = 511;
		constexpr std::size_t blockCapacity = 128;
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, blockCapacity);
		std::vector<float> drums(totalFrames * 2);
		std::vector<float> lead(totalFrames * 2);
		for (std::size_t frame = 0; frame < totalFrames; ++frame)
		{
			drums[frame * 2] = static_cast<float>(static_cast<int>(frame % 19) - 9) / 20.0f;
			drums[frame * 2 + 1] = static_cast<float>(static_cast<int>(frame % 13) - 6) / 17.0f;
			lead[frame * 2] = static_cast<float>(static_cast<int>(frame % 23) - 11) / 25.0f;
			lead[frame * 2 + 1] = static_cast<float>(static_cast<int>(frame % 17) - 8) / 21.0f;
		}
		const std::vector<AudioBusInput> inputs = {
			{ ids.Drums, drums.data(), 1.0f },
			{ ids.Lead, lead.data(), 1.0f },
		};
		const std::vector<AvbStemSpec> stems = {
			{ ids.Drums, AudioBusTap::PostFader, "Drums" },
			{ ids.Lead, AudioBusTap::PostFader, "Lead" },
		};
		const std::vector<AvbRenderedStem> realtime = RenderRealtimeTaps(graph, inputs, totalFrames, stems);
		Check(realtime.size() == stems.size(), "the realtime reference captures each selected bus tap");

		AvbStemRenderOptions options;
		options.OutputRoot = (root / "export-a").string();
		options.ProjectName = "Doom Session";
		options.RegionName = "Verse 01";
		options.FilenamePattern = "$project\\$region-$track";
		std::vector<AvbRenderedStem> offline;
		std::string error;
		Check(RenderStems(graph, inputs.data(), inputs.size(), totalFrames, stems, options, offline, &error),
			"offline stem export renders and writes the requested bus taps");
		Check(offline.size() == stems.size(), "the stem export returns one result per render-matrix entry");
		if (offline.size() == stems.size() && realtime.size() == stems.size())
		{
			for (std::size_t i = 0; i < stems.size(); ++i)
			{
				Check(BitIdentical(offline[i].InterleavedStereo, realtime[i].InterleavedStereo),
					"offline stem samples are bit-identical to realtime AudioBusGraph taps");
				const std::filesystem::path expected = std::filesystem::path(options.OutputRoot) /
					"Doom Session" / (std::string("Verse 01-") + stems[i].TrackName + ".wav");
				Check(std::filesystem::exists(expected),
					"$project\\$region-$track creates the project folder and named stereo stem");
				Check(offline[i].FilePath == expected.string(),
					"the stem result reports the expanded path below its output root");
				std::ifstream wav(expected, std::ios::binary);
				std::array<std::uint8_t, 44> header{};
				wav.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
				Check(wav.good() && header[22] == 2 && header[23] == 0,
					"stem WAV files retain stereo channel layout");
				Check(header[24] == 0x80 && header[25] == 0xBB && header[26] == 0 && header[27] == 0,
					"stem WAV files use the fixed 48 kHz AVB rate");
			}
		}

		options.OutputRoot = (root / "export-b").string();
		std::vector<AvbRenderedStem> repeated;
		Check(RenderStems(graph, inputs.data(), inputs.size(), totalFrames, stems, options, repeated, &error),
			"the same offline render can be repeated after graph state reset");
		Check(repeated.size() == offline.size(), "the repeated export has the same stem count");
		if (repeated.size() == offline.size())
			for (std::size_t i = 0; i < repeated.size(); ++i)
				Check(BitIdentical(repeated[i].InterleavedStereo, offline[i].InterleavedStereo),
					"repeated offline renders are bit-identical sample for sample");
	}

	void TestStemPathCannotEscapeOutputRoot(const std::filesystem::path& root)
	{
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, 4);
		const std::array<float, 8> source = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		const AudioBusInput input{ ids.Drums, source.data(), 1.0f };
		const AvbStemSpec stem{ ids.Drums, AudioBusTap::PostFader, "Drums" };
		AvbStemRenderOptions options;
		options.OutputRoot = (root / "confined").string();
		options.ProjectName = "Project";
		options.RegionName = "Region";
		options.FilenamePattern = "$project\\..\\escape-$track";
		std::vector<AvbRenderedStem> rendered;
		std::string error;
		Check(!RenderStems(graph, &input, 1, 4, { stem }, options, rendered, &error),
			"a wildcard template containing a parent traversal is rejected");
		Check(!std::filesystem::exists(root / "confined" / "escape-Drums.wav") &&
			!std::filesystem::exists(root / "escape-Drums.wav"),
			"a rejected stem path does not create a file outside its output root");
	}

	void TestRenderStemsRejectsLinkedOutputRoot(const std::filesystem::path& root)
	{
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, 4);
		const std::array<float, 8> source = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		const AudioBusInput input{ ids.Drums, source.data(), 1.0f };
		const AvbStemSpec stem{ ids.Drums, AudioBusTap::PostFader, "Drums" };
		const std::filesystem::path targetRoot = root / "stem-output-target";
		const std::filesystem::path linkedRoot = root / "stem-output-link";
		std::error_code setupError;
		std::filesystem::create_directories(targetRoot, setupError);
		Check(!setupError, "stem link setup creates the real output-root directory");
		if (setupError)
			return;

		const bool linkCreated = CreateDirectoryLink(targetRoot, linkedRoot, setupError);
		Check(linkCreated, "stem link setup creates a directory symlink or junction");
		if (!linkCreated)
			return;

		AvbStemRenderOptions options;
		options.OutputRoot = linkedRoot.string();
		options.ProjectName = "Project";
		options.RegionName = "Region";
		options.FilenamePattern = "$project\\$region-$track";
		std::vector<AvbRenderedStem> rendered;
		std::string error;
		Check(!RenderStems(graph, &input, 1, 4, { stem }, options, rendered, &error),
			"RenderStems rejects an OutputRoot that is itself a directory link");
	}

	void TestStemPathHardLinkCannotModifyOutsideFile(const std::filesystem::path& root)
	{
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, 4);
		const std::array<float, 8> source = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		const AudioBusInput input{ ids.Drums, source.data(), 1.0f };
		const AvbStemSpec stem{ ids.Drums, AudioBusTap::PostFader, "Drums" };
		const std::string sentinelContents = "AVB.11 hard-link sentinel must remain intact";
		const std::filesystem::path outsideSentinel = root / "outside-sentinel.wav";
		AvbStemRenderOptions options;
		options.OutputRoot = (root / "hard-link-confined").string();
		options.ProjectName = "Project";
		options.RegionName = "Region";
		options.FilenamePattern = "$project\\$region-$track";
		const std::filesystem::path outputPath = std::filesystem::path(options.OutputRoot) /
			"Project" / "Region-Drums.wav";
		std::filesystem::create_directories(outputPath.parent_path());
		{
			std::ofstream sentinelFile(outsideSentinel, std::ios::binary | std::ios::trunc);
			sentinelFile.write(sentinelContents.data(), static_cast<std::streamsize>(sentinelContents.size()));
			Check(sentinelFile.good(), "hard-link test setup writes the outside sentinel file");
			if (!sentinelFile.good())
				return;
		}

		std::error_code hardLinkError;
		std::filesystem::create_hard_link(outsideSentinel, outputPath, hardLinkError);
		Check(!hardLinkError,
			"hard-link test setup must create a native same-volume hard link; failure is a test failure");
		if (hardLinkError)
			return;

		std::vector<AvbRenderedStem> rendered;
		std::string error;
		const bool exportSucceeded = RenderStems(graph, &input, 1, 4, { stem }, options, rendered, &error);
		std::ifstream sentinelFile(outsideSentinel, std::ios::binary);
		const std::string actualContents((std::istreambuf_iterator<char>(sentinelFile)),
			std::istreambuf_iterator<char>());
		Check(actualContents == sentinelContents,
			"rendering a stem cannot truncate or modify an outside file hard-linked at its output path");
		if (exportSucceeded)
		{
			std::error_code equivalentError;
			const bool outputAliasesSentinel = std::filesystem::equivalent(outputPath, outsideSentinel, equivalentError);
			Check(!equivalentError && !outputAliasesSentinel,
				"a successful stem export replaces the hard link instead of retaining the outside-file alias");
		}
	}

	struct StemHardLinkSwap
	{
		std::filesystem::path OutsideSentinel;
		std::filesystem::path Output;
		bool Succeeded = false;
	};

	void SwapStemOutputForHardLink(void* context)
	{
		auto& swap = *static_cast<StemHardLinkSwap*>(context);
		std::error_code error;
		std::filesystem::create_hard_link(swap.OutsideSentinel, swap.Output, error);
		swap.Succeeded = !error;
	}

	void TestStemPublicationCannotFollowPostValidationHardLink(const std::filesystem::path& root)
	{
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, 4);
		const std::array<float, 8> source = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		const AudioBusInput input{ ids.Drums, source.data(), 1.0f };
		const AvbStemSpec stem{ ids.Drums, AudioBusTap::PostFader, "Drums" };
		const std::string sentinelContents = "AVB.11 post-validation hard-link sentinel";
		const std::filesystem::path outsideSentinel = root / "race-outside-sentinel.wav";
		AvbStemRenderOptions options;
		options.OutputRoot = (root / "race-confined").string();
		options.ProjectName = "Project";
		options.RegionName = "Region";
		options.FilenamePattern = "$project\\$region-$track";
		const std::filesystem::path outputPath = std::filesystem::path(options.OutputRoot) /
			"Project" / "Region-Drums.wav";
		{
			std::ofstream sentinelFile(outsideSentinel, std::ios::binary | std::ios::trunc);
			sentinelFile.write(sentinelContents.data(), static_cast<std::streamsize>(sentinelContents.size()));
			Check(sentinelFile.good(), "TOCTOU test setup writes its outside sentinel");
			if (!sentinelFile.good())
				return;
		}

		StemHardLinkSwap swap{ outsideSentinel, outputPath };
		SetAvbStemPublicationTestHookForTesting(&SwapStemOutputForHardLink, &swap);
		std::vector<AvbRenderedStem> rendered;
		std::string error;
		const bool exportSucceeded = RenderStems(graph, &input, 1, 4, { stem }, options, rendered, &error);
		SetAvbStemPublicationTestHookForTesting(nullptr, nullptr);
		Check(exportSucceeded, "stem rendering succeeds when a hard link is substituted after validation");
		Check(swap.Succeeded,
			"the deterministic test hook installs an outside hard link after output validation");
		std::ifstream sentinelFile(outsideSentinel, std::ios::binary);
		const std::string actualContents((std::istreambuf_iterator<char>(sentinelFile)),
			std::istreambuf_iterator<char>());
		Check(actualContents == sentinelContents,
			"publication cannot modify an outside hard link installed after validation");
		if (exportSucceeded)
		{
			std::error_code equivalentError;
			const bool outputAliasesSentinel = std::filesystem::equivalent(outputPath, outsideSentinel, equivalentError);
			Check(!equivalentError && !outputAliasesSentinel,
				"post-validation hard-link substitution is atomically replaced by stem output");
		}
	}

	void TestStemPathCaseCollisionsMatchPlatformFilesystem(const std::filesystem::path& root)
	{
		AudioBusGraph graph;
		const TestBusSetup ids = ConfigureTestGraph(graph, 4);
		const std::array<float, 8> source = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		const AudioBusInput input{ ids.Drums, source.data(), 1.0f };
		const std::vector<AvbStemSpec> stems = {
			{ ids.Drums, AudioBusTap::PostFader, "Bass" },
			{ ids.Drums, AudioBusTap::PostFader, "bass" },
		};
		AvbStemRenderOptions options;
		options.OutputRoot = (root / "case-collision").string();
		options.ProjectName = "Project";
		options.RegionName = "Region";
		options.FilenamePattern = "$project\\$region-$track";
		std::vector<AvbRenderedStem> rendered;
		std::string error;
#if defined(_WIN32)
		Check(!RenderStems(graph, &input, 1, 4, stems, options, rendered, &error),
			"Windows stem export rejects filenames that differ only by ASCII case");
		Check(!std::filesystem::exists(root / "case-collision" / "Project" / "Region-Bass.wav"),
			"case-colliding stems are rejected before either output file is written");
#else
		Check(RenderStems(graph, &input, 1, 4, stems, options, rendered, &error),
			"case-distinct filenames remain valid on case-sensitive filesystems");
		Check(std::filesystem::exists(root / "case-collision" / "Project" / "Region-Bass.wav") &&
			std::filesystem::exists(root / "case-collision" / "Project" / "Region-bass.wav"),
			"case-sensitive filesystems receive both distinct stem files");
#endif
	}
}

int main()
{
	const std::filesystem::path root = MakeTempRoot("suite");
	TestCaptureToneWritesWavAndCreatesZeroOffsetTake(root / "record");
#if defined(_WIN32)
	TestTempRecordingWavCannotBeOpenedByAnotherReader(root);
	TestFailedTempPublicationRetriesHandleBoundDeletion(root);
#endif
#if !defined(_WIN32) && defined(AVB_RECORDER_TEST_HOOKS)
	TestFailedStagingDirectoryOpenRemovesCreatedDirectory(root);
#endif
	TestCaptureOverflowDoesNotPublishTruncatedTake(root / "overflow");
	TestRecorderOutputHardLinkCannotModifyOutsideFile(root / "recorder-hard-link");
	TestRecorderBindsOutputDirectoryAtStart(root / "recorder-directory-binding");
	TestRecorderRejectsLinkedOutputParent(root / "recorder-linked-parent");
	TestAudioClipWavRoundTripPreservesSamples(root / "roundtrip");
	TestStemWildcardTreeAndBitIdenticalRenders(root);
	TestStemPathCannotEscapeOutputRoot(root);
	TestRenderStemsRejectsLinkedOutputRoot(root / "linked-stem-root");
	TestStemPathHardLinkCannotModifyOutsideFile(root);
	TestStemPublicationCannotFollowPostValidationHardLink(root);
	TestStemPathCaseCollisionsMatchPlatformFilesystem(root);
	std::filesystem::remove_all(root);
	std::printf("test_avb_recording: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
