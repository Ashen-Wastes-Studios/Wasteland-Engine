#include "Wasteland/Core/Core.h"
#include "Wasteland/Audio/AudioClip.h"
#include "Wasteland/Avb/AudioBus.h"
#include "Wasteland/Avb/AvbDsp.h"
#include "Wasteland/Avb/Graph/Compiler.h"
#include "Wasteland/Avb/Graph/GraphSerializer.h"
#include "Wasteland/Avb/Graph/NodeRegistry.h"
#include "Wasteland/Avb/Graph/ProjectRuntime.h"
#include "Wasteland/Avb/Graph/Runtime.h"
#include "Wasteland/Project/Project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

using namespace Wasteland::Avb;

namespace
{
	constexpr std::uint32_t kSampleRate = 48000;
	constexpr std::size_t kBlockFrames = 480;
	int g_Failures = 0;

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			++g_Failures;
			std::fprintf(stderr, "FAIL: %s\n", message);
		}
	}

	AvbNode MakeNode(const char* type, const char* id)
	{
		return AvbNodeRegistry::Get().CreateNode(type, id);
	}

	void AddLink(AvbGraph& graph, const char* fromNode, const char* fromPin,
	             const char* toNode, const char* toPin, bool feedback = false)
	{
		graph.Links.push_back({ fromNode, fromPin, toNode, toPin, feedback });
	}

	bool CompileAndPrepare(AvbGraph& graph, AvbRuntime& runtime, std::string* error = nullptr,
	                      std::size_t maxTriggers = 256)
	{
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		if (!AvbCompiler::Compile(graph, compiled, validation))
		{
			if (error) *error = validation.Report();
			return false;
		}
		return runtime.Prepare(graph, compiled, kSampleRate, kBlockFrames, maxTriggers, error);
	}

	AvbGraph MakeSineGainOutputGraph(float frequency)
	{
		AvbGraph graph;
		graph.Name = "SineGainOutput";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode sine = MakeNode("Sine", "sine");
		sine.Literals.push_back({ "Frequency", AvbLiteral(frequency) });
		AvbNode gain = MakeNode("Gain", "gain");
		gain.Literals.push_back({ "Gain", AvbLiteral(0.75f) });
		graph.Nodes = { sine, gain, MakeNode("Output", "out") };
		AddLink(graph, "sine", "Out", "gain", "In");
		AddLink(graph, "gain", "Out", "out", "In");
		return graph;
	}

	bool TestGraphSerializerSaveRoundTripsAndReplacesExistingFile()
	{
		namespace fs = std::filesystem;
		const fs::path directory = fs::temp_directory_path() /
			("WastelandAvbGraphSaveTests-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::error_code ec;
		fs::create_directories(directory, ec);
		const fs::path target = directory / "roundtrip.wlavb";
		{
			std::ofstream existing(target, std::ios::binary);
			existing << "previous valid graph bytes";
			if (!existing)
			{
				Check(false, "the graph-save replacement fixture is created");
				fs::remove_all(directory, ec);
				return false;
			}
		}

		AvbGraphError error;
		AvbGraph firstGraph = MakeSineGainOutputGraph(440.0f);
		const bool firstSaved = AvbGraphSerializer::Save(target.string(), firstGraph, error);
		std::ifstream firstInput(target, std::ios::binary);
		const std::string firstBytes((std::istreambuf_iterator<char>(firstInput)), std::istreambuf_iterator<char>());
		firstInput.close();
		AvbGraph loadedGraph;
		const bool firstLoaded = AvbGraphSerializer::Load(target.string(), loadedGraph, error);
		const std::string firstExpected = AvbGraphSerializer::Emit(firstGraph);
		const bool firstRoundTrip = firstLoaded && AvbGraphSerializer::Emit(loadedGraph) == firstExpected;
		Check(firstSaved && firstBytes == firstExpected,
		      "Save publishes the complete serialized graph over an existing file");
		Check(firstRoundTrip, "a saved .wlavb graph loads back to the same serialization");

		AvbGraph replacementGraph = MakeSineGainOutputGraph(880.0f);
		replacementGraph.Name = "ReplacementGraph";
		error = {};
		const bool replacementSaved = AvbGraphSerializer::Save(target.string(), replacementGraph, error);
		std::ifstream replacementInput(target, std::ios::binary);
		const std::string replacementBytes((std::istreambuf_iterator<char>(replacementInput)),
		                                  std::istreambuf_iterator<char>());
		replacementInput.close();
		const std::string replacementExpected = AvbGraphSerializer::Emit(replacementGraph);
		const bool replacementCorrect = replacementSaved && replacementBytes == replacementExpected;
		Check(replacementCorrect, "Save normally replaces an existing .wlavb file with the new graph");
		fs::remove_all(directory, ec);
		return firstSaved && firstRoundTrip && replacementCorrect;
	}

#ifdef _WIN32
	bool TestGraphSerializerPreservesTargetWhenAtomicPublishFails()
	{
		namespace fs = std::filesystem;
		const fs::path directory = fs::temp_directory_path() /
			("WastelandAvbGraphPublishFailureTests-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::error_code ec;
		fs::create_directories(directory, ec);
		const fs::path target = directory / "locked.wlavb";
		const std::string previousBytes = "previous valid graph bytes that must survive";
		{
			std::ofstream existing(target, std::ios::binary);
			existing.write(previousBytes.data(), static_cast<std::streamsize>(previousBytes.size()));
			if (!existing)
			{
				Check(false, "the locked graph-save fixture is created");
				fs::remove_all(directory, ec);
				return false;
			}
		}

		HANDLE targetLock = CreateFileW(target.c_str(), GENERIC_READ,
		                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (targetLock == INVALID_HANDLE_VALUE)
		{
			Check(false, "the target is locked without delete sharing so atomic replacement must fail");
			fs::remove_all(directory, ec);
			return false;
		}

		AvbGraphError error;
		const AvbGraph graph = MakeSineGainOutputGraph(660.0f);
		const bool saved = AvbGraphSerializer::Save(target.string(), graph, error);
		std::ifstream targetInput(target, std::ios::binary);
		const std::string bytesAfterFailure((std::istreambuf_iterator<char>(targetInput)),
		                                    std::istreambuf_iterator<char>());
		targetInput.close();
		bool noTemporaryFiles = true;
		for (const fs::directory_entry& entry : fs::directory_iterator(directory))
			if (entry.path() != target) noTemporaryFiles = false;
		const bool publishFailure = error.Message.find("replace") != std::string::npos ||
		                            error.Message.find("publish") != std::string::npos;
		const bool rejectedAndPreserved = !saved && publishFailure && bytesAfterFailure == previousBytes;
		Check(!saved && publishFailure,
		      "Save reports failure when the fully written temporary graph cannot atomically replace the locked target");
		Check(bytesAfterFailure == previousBytes,
		      "a failed atomic publish leaves every previous target byte unchanged");
		Check(noTemporaryFiles, "Save removes its sibling temporary file after a publish failure");
		CloseHandle(targetLock);
		fs::remove_all(directory, ec);
		return rejectedAndPreserved && noTemporaryFiles;
	}
#endif

	AvbGraph MakeEnvelopeGraph(float attackSeconds)
	{
		AvbGraph graph;
		graph.Name = "SampleAccurateEnvelope";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode envelope = MakeNode("ADEnvelope", "envelope");
		envelope.Literals.push_back({ "Attack", AvbLiteral(attackSeconds) });
		envelope.Literals.push_back({ "Decay", AvbLiteral(0.01f) });
		graph.Nodes = { envelope, MakeNode("Output", "out") };
		AddLink(graph, "envelope", "Out", "out", "In");
		return graph;
	}

	bool TestWavePlayerReportsMissingAssetResolverAtClipPin()
	{
		AvbGraph graph;
		graph.Name = "WavePlayerMissingResolver";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode player = MakeNode("WavePlayer", "wave");
		player.Literals.push_back({ "Clip", AvbLiteral("audio/tone.wav") });
		graph.Nodes = { player, MakeNode("Output", "out") };
		AddLink(graph, "wave", "Out", "out", "In");
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		Check(AvbCompiler::Compile(graph, compiled, validation), validation.Report().c_str());
		AvbRuntime runtime;
		std::string error;
		const bool prepared = runtime.Prepare(graph, compiled, kSampleRate, kBlockFrames, 256, &error);
		const bool identifiesPin = error.find("wave") != std::string::npos &&
		                           error.find("Clip") != std::string::npos &&
		                           error.find("resolver") != std::string::npos;
		Check(!prepared && !runtime.IsPrepared(), "WavePlayer preparation without a resolver is rejected");
		Check(identifiesPin, "missing asset resolver diagnostic names the WavePlayer Clip pin");
		return !prepared && !runtime.IsPrepared() && identifiesPin;
	}

	bool TestWavePlayerRendersResolvedClipAndSampleAccurateTriggers()
	{
		AvbGraph graph;
		graph.Name = "ResolvedWavePlayer";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode player = MakeNode("WavePlayer", "wave");
		player.Literals = {
			{ "Clip", AvbLiteral("audio/tone.wav") },
			{ "NearlyFinishedLead", AvbLiteral(2.0 / kSampleRate) }
		};
		graph.Nodes = { player, MakeNode("Output", "out") };
		AddLink(graph, "wave", "Out", "out", "In");

		auto clip = Wasteland::Audio::AudioClip::FromSamples(std::vector<float>(8, 0.25f), kSampleRate);
		std::string resolvedAsset;
		std::size_t resolverCalls = 0;
		auto resolver = [&](const std::string& assetRef) -> std::shared_ptr<Wasteland::Audio::AudioClip>
		{
			++resolverCalls;
			resolvedAsset = assetRef;
			return assetRef == "audio/tone.wav" ? clip : nullptr;
		};
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		Check(AvbCompiler::Compile(graph, compiled, validation), validation.Report().c_str());
		AvbRuntime runtime;
		std::string error;
		Check(runtime.Prepare(graph, compiled, kSampleRate, kBlockFrames, 256, &error, resolver), error.c_str());
		Check(resolverCalls == 1 && resolvedAsset == "audio/tone.wav",
		      "Prepare resolves the original project-relative AssetRef once");
		if (!runtime.IsPrepared()) return false;
		clip.reset(); // The prepared runtime must retain the decoded samples through rendering.

		std::vector<float> first(kBlockFrames, -1.0f);
		Check(runtime.QueueTrigger("wave", "Play", 137), "WavePlayer accepts a sample-accurate Play trigger");
		Check(runtime.RenderBlock(first.data(), first.size()), "the resolved WavePlayer graph renders a real output buffer");
		bool samplesCorrect = true;
		for (std::size_t frame = 0; frame < first.size(); ++frame)
		{
			const float expected = frame >= 137 && frame <= 144 ? 0.25f : 0.0f;
			if (first[frame] != expected) samplesCorrect = false;
		}
		const AvbTrigger* nearly = runtime.FindTriggerOutput("wave", "OnNearlyFinished");
		const AvbTrigger* finished = runtime.FindTriggerOutput("wave", "OnFinished");
		const AvbTrigger* looped = runtime.FindTriggerOutput("wave", "OnLooped");
		const bool lifecycleFrames = nearly && nearly->Count() == 1 && nearly->FrameAt(0) == 143 &&
		                             finished && finished->Count() == 1 && finished->FrameAt(0) == 144 &&
		                             looped && looped->Count() == 0;
		Check(samplesCorrect, "the rendered output buffer contains the clip at precisely the Play frame");
		Check(lifecycleFrames, "WavePlayer lifecycle outputs preserve exact nearly-finished and finished frames");

		runtime.Reset();
		std::vector<float> repeated(kBlockFrames, -1.0f);
		Check(runtime.QueueTrigger("wave", "Play", 137), "Reset permits a deterministic replay trigger");
		Check(runtime.RenderBlock(repeated.data(), repeated.size()), "the reset WavePlayer graph renders again");
		const bool resetDeterministic = repeated == first;
		Check(resetDeterministic, "Reset reproduces identical clip samples and lifecycle trigger frames");

		runtime.Reset();
		std::vector<float> stopped(kBlockFrames, -1.0f);
		Check(runtime.QueueTrigger("wave", "Play", 137), "the stopped-playback test queues Play");
		Check(runtime.QueueTrigger("wave", "Stop", 140), "WavePlayer accepts a sample-accurate Stop trigger");
		Check(runtime.RenderBlock(stopped.data(), stopped.size()), "Play then Stop renders without a block-size change");
		bool stopFrameCorrect = true;
		for (std::size_t frame = 0; frame < stopped.size(); ++frame)
		{
			const float expected = frame >= 137 && frame < 140 ? 0.25f : 0.0f;
			if (stopped[frame] != expected) stopFrameCorrect = false;
		}
		nearly = runtime.FindTriggerOutput("wave", "OnNearlyFinished");
		finished = runtime.FindTriggerOutput("wave", "OnFinished");
		Check(stopFrameCorrect && nearly && nearly->Count() == 0 && finished && finished->Count() == 0,
		      "Stop silences output at its exact trigger frame without natural lifecycle events");
		return samplesCorrect && lifecycleFrames && resetDeterministic && stopFrameCorrect &&
		       resolverCalls == 1 && resolvedAsset == "audio/tone.wav";
	}

	bool TestRuntimeMaps44100HzClipTo48000HzMix()
	{
		AvbGraph graph;
		graph.Name = "WavePlayer44100To48000";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode player = MakeNode("WavePlayer", "wave_rate");
		player.Literals = {
			{ "Clip", AvbLiteral("audio/ten_ms.wav") },
			{ "NearlyFinishedLead", AvbLiteral(0.0) }
		};
		graph.Nodes = { player, MakeNode("Output", "out_rate") };
		AddLink(graph, "wave_rate", "Out", "out_rate", "In");
		auto clip = Wasteland::Audio::AudioClip::FromSamples(std::vector<float>(441, 0.25f), 44100);
		auto resolver = [clip](const std::string&) { return clip; };
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		Check(AvbCompiler::Compile(graph, compiled, validation), validation.Report().c_str());
		AvbRuntime runtime;
		std::string error;
		Check(runtime.Prepare(graph, compiled, kSampleRate, kBlockFrames, 256, &error, resolver), error.c_str());
		if (!runtime.IsPrepared()) return false;
		std::vector<float> rendered(kBlockFrames, -1.0f);
		Check(runtime.QueueTrigger("wave_rate", "Play", 0), "44.1 kHz clip playback starts at output frame zero");
		Check(runtime.RenderBlock(rendered.data(), rendered.size()), "the 44.1-to-48 kHz WavePlayer block renders");
		const bool fullDuration = std::all_of(rendered.begin(), rendered.end(),
		                                     [](float sample) { return sample == 0.25f; });
		const AvbTrigger* finished = runtime.FindTriggerOutput("wave_rate", "OnFinished");
		const bool finishedAtEnd = finished && finished->Count() == 1 && finished->FrameAt(0) == 479;
		Check(fullDuration, "a 10 ms native-rate clip fills 480 frames at the fixed 48 kHz mix rate");
		Check(finishedAtEnd, "source-rate conversion emits OnFinished on the last frame of the 10 ms duration");
		return fullDuration && finishedAtEnd;
	}

	bool TestWavePlayerResolutionAndNarrowingDiagnostics()
	{
		auto makeGraph = [](double pitchSemitones)
		{
			AvbGraph graph;
			graph.Name = "WavePlayerValidation";
			graph.Output = AvbChannelFormat::Mono;
			AvbNode player = MakeNode("WavePlayer", "wave_invalid");
			player.Literals = {
				{ "Clip", AvbLiteral("audio/missing.wav") },
				{ "PitchSemitones", AvbLiteral(pitchSemitones) }
			};
			graph.Nodes = { player, MakeNode("Output", "out_invalid") };
			AddLink(graph, "wave_invalid", "Out", "out_invalid", "In");
			return graph;
		};
		auto prepare = [&](AvbGraph graph, AvbAssetResolver resolver, std::string& error)
		{
			AvbCompiledGraph compiled;
			AvbValidationResult validation;
			if (!AvbCompiler::Compile(graph, compiled, validation))
			{
				error = validation.Report();
				return false;
			}
			AvbRuntime runtime;
			return runtime.Prepare(graph, compiled, kSampleRate, kBlockFrames, 256, &error, std::move(resolver));
		};

		std::string unresolvedError;
		const bool unresolvedPrepared = prepare(makeGraph(0.0),
			[](const std::string&) { return std::shared_ptr<Wasteland::Audio::AudioClip>{}; },
			unresolvedError);
		const bool unresolvedNamesClip = unresolvedError.find("wave_invalid") != std::string::npos &&
			                             unresolvedError.find("Clip") != std::string::npos;
		Check(!unresolvedPrepared && unresolvedNamesClip,
		      "unresolved project-relative audio references fail with node and Clip diagnostics");

		auto invalidClip = Wasteland::Audio::AudioClip::FromSamples({}, kSampleRate);
		std::string invalidError;
		const bool invalidPrepared = prepare(makeGraph(0.0),
			[invalidClip](const std::string&) { return invalidClip; }, invalidError);
		const bool invalidNamesClip = invalidError.find("wave_invalid") != std::string::npos &&
		                              invalidError.find("Clip") != std::string::npos;
		Check(!invalidPrepared && invalidNamesClip,
		      "resolved but invalid audio clips fail with node and Clip diagnostics");

		auto validClip = Wasteland::Audio::AudioClip::FromSamples(std::vector<float>(8, 0.25f), kSampleRate);
		std::string narrowingError;
		const bool narrowingPrepared = prepare(makeGraph(1.0e300),
			[validClip](const std::string&) { return validClip; }, narrowingError);
		const bool narrowingNamesPin = narrowingError.find("wave_invalid") != std::string::npos &&
		                               narrowingError.find("PitchSemitones") != std::string::npos;
		Check(!narrowingPrepared && narrowingNamesPin,
		      "non-finite or out-of-float-range WavePlayer controls identify their exact node and pin");
		return !unresolvedPrepared && unresolvedNamesClip && !invalidPrepared && invalidNamesClip &&
		       !narrowingPrepared && narrowingNamesPin;
	}

	bool TestSampleAccurateTriggerStartsEnvelopeAtFrame137()
	{
		AvbGraph graph = MakeEnvelopeGraph(0.001f);
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		std::vector<float> output(kBlockFrames, -1.0f);
		Check(runtime.QueueTrigger("envelope", "Trigger", 137),
		      "the frame-137 trigger is accepted for the envelope input");
		Check(runtime.RenderBlock(output.data(), output.size()),
		      "the 480-frame envelope block renders");

		std::size_t firstNonZero = output.size();
		for (std::size_t frame = 0; frame < output.size(); ++frame)
			if (output[frame] != 0.0f)
			{
				firstNonZero = frame;
				break;
			}
		Check(firstNonZero == 137,
		      "a trigger at frame 137 starts nonzero envelope output at exactly frame 137");
		return firstNonZero == 137;
	}

	bool TestTwoTriggersKeepTheirOwnFrames()
	{
		AvbGraph graph = MakeEnvelopeGraph(0.01f);
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		std::vector<float> output(kBlockFrames, 0.0f);
		Check(runtime.QueueTrigger("envelope", "Trigger", 137), "first same-block trigger is queued");
		Check(runtime.QueueTrigger("envelope", "Trigger", 300), "second same-block trigger is queued");
		Check(runtime.RenderBlock(output.data(), output.size()), "the block containing both triggers renders");
		Check(output[137] > 0.0f, "the first trigger begins the attack at frame 137");
		Check(output[300] > 0.0f && output[300] < output[299] * 0.05f,
		      "the second trigger retriggers the envelope at frame 300, not at block start");
		return output[137] > 0.0f && output[300] > 0.0f && output[300] < output[299] * 0.05f;
	}

	bool TestSineFrequencyAt48kHz()
	{
		constexpr float frequency = 997.0f;
		AvbGraph graph = MakeSineGainOutputGraph(frequency);
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;
		Check(runtime.FramesPerBlock() == kBlockFrames, "48 kHz at 100 Hz uses 480 frames per block");
		Check(std::fabs(runtime.GetActualBlockRate() - 100.0) < 1.0e-12,
		      "the runtime reports its actual 100 Hz block rate");

		std::vector<float> rendered(kBlockFrames * 100, 0.0f);
		for (std::size_t block = 0; block < 100; ++block)
			Check(runtime.RenderBlock(rendered.data() + block * kBlockFrames, kBlockFrames),
			      "sine/gain/output renders each 480-frame block");
		std::size_t positiveCrossings = 0;
		for (std::size_t frame = 1; frame < rendered.size(); ++frame)
			if (rendered[frame - 1] <= 0.0f && rendered[frame] > 0.0f)
				++positiveCrossings;
		const double measured = static_cast<double>(positiveCrossings) * kSampleRate /
		                        static_cast<double>(rendered.size() - 1);
		const double relativeError = std::fabs(measured - frequency) / frequency;
		Check(relativeError <= 0.001,
		      "the 48 kHz Sine -> Gain -> Output frequency is within 0.1 percent");
		return relativeError <= 0.001;
	}

	bool TestCompilerRejectsFloatNarrowingAndAcceptsFloatBoundary()
	{
		auto compileWithAmplitude = [](double amplitude, std::string* report = nullptr)
		{
			AvbGraph graph = MakeSineGainOutputGraph(440.0f);
			graph.FindNode("sine")->Literals.push_back({ "Amplitude", AvbLiteral(amplitude) });
			AvbCompiledGraph compiled;
			AvbValidationResult validation;
			const bool accepted = AvbCompiler::Compile(graph, compiled, validation);
			if (report) *report = validation.Report();
			return accepted;
		};

		const double maxFloat = static_cast<double>((std::numeric_limits<float>::max)());
		const bool acceptsBoundary = compileWithAmplitude(maxFloat);
		const double aboveMaxFloat = std::nextafter(maxFloat, std::numeric_limits<double>::infinity());
		std::string overflowReport;
		const bool acceptsNextValue = compileWithAmplitude(aboveMaxFloat, &overflowReport);
		std::string largeValueReport;
		const bool acceptsLargeValue = compileWithAmplitude(1.0e300, &largeValueReport);
		const bool identifiesPin = overflowReport.find("sine") != std::string::npos &&
		                           overflowReport.find("Amplitude") != std::string::npos &&
		                           largeValueReport.find("sine") != std::string::npos &&
		                           largeValueReport.find("Amplitude") != std::string::npos;
		Check(acceptsBoundary, "the largest finite float is an accepted Float literal");
		Check(!acceptsNextValue && !acceptsLargeValue,
		      "Float literals that narrow beyond finite float range are rejected by the compiler");
		Check(identifiesPin, "float narrowing diagnostics identify the exact node and pin");
		return acceptsBoundary && !acceptsNextValue && !acceptsLargeValue && identifiesPin;
	}

	bool TestCompilerRejectsTimeLiteralOutsideFloatRange()
	{
		auto compileWithAttack = [](double attack, std::string* report = nullptr)
		{
			AvbGraph graph;
			graph.Name = "TimeLiteralFloatRange";
			graph.Output = AvbChannelFormat::Mono;
			AvbNode envelope = MakeNode("ADEnvelope", "time_envelope");
			envelope.Literals.push_back({ "Attack", AvbLiteral(attack) });
			graph.Nodes = { envelope, MakeNode("Output", "time_out") };
			AddLink(graph, "time_envelope", "Out", "time_out", "In");
			AvbCompiledGraph compiled;
			AvbValidationResult validation;
			const bool accepted = AvbCompiler::Compile(graph, compiled, validation);
			if (report) *report = validation.Report();
			return accepted;
		};

		const double maxFloat = static_cast<double>((std::numeric_limits<float>::max)());
		const bool acceptsBoundary = compileWithAttack(maxFloat);
		const double aboveMaxFloat = std::nextafter(maxFloat, std::numeric_limits<double>::infinity());
		std::string overflowReport;
		const bool acceptsNextValue = compileWithAttack(aboveMaxFloat, &overflowReport);
		std::string largeValueReport;
		const bool acceptsLargeValue = compileWithAttack(1.0e300, &largeValueReport);
		const bool identifiesPin = overflowReport.find("time_envelope") != std::string::npos &&
		                           overflowReport.find("Attack") != std::string::npos &&
		                           largeValueReport.find("time_envelope") != std::string::npos &&
		                           largeValueReport.find("Attack") != std::string::npos;
		Check(acceptsBoundary, "the largest finite float is an accepted Time literal");
		Check(!acceptsNextValue && !acceptsLargeValue,
		      "Time literals that narrow beyond finite float range are rejected by the compiler");
		Check(identifiesPin, "Time narrowing diagnostics identify the exact node and pin");
		return acceptsBoundary && !acceptsNextValue && !acceptsLargeValue && identifiesPin;
	}

	bool TestCompilerRejectsFloatAndTimeOutOfRangeDefaults()
	{
		AvbNodeDef nodeDefinition;
		nodeDefinition.TypeId = "OutOfRangeDefaultTest";
		nodeDefinition.Category = "Tests";
		nodeDefinition.Pins.push_back({ "Value", AvbPinType::Float, AvbPinDir::Input,
		                                false, AvbPinRate::Block, AvbLiteral(1.0e300) });
		nodeDefinition.Factory = [](const std::string& id)
		{
			AvbNode node;
			node.Id = id;
			node.TypeId = "OutOfRangeDefaultTest";
			return node;
		};
		std::string registrationError;
		const bool registered = AvbNodeRegistry::Get().Register(std::move(nodeDefinition), &registrationError);
		Check(registered, registered ? "" : registrationError.c_str());
		if (!registered) return false;

		AvbGraph graph = MakeSineGainOutputGraph(440.0f);
		graph.Inputs.push_back({ "oversized_float_input", AvbPinType::Float,
		                         AvbPinDir::Input, AvbLiteral(1.0e300) });
		graph.Variables.push_back({ "oversized_time_variable", AvbPinType::Time,
		                            AvbLiteral(-1.0e300) });
		graph.Nodes.push_back(MakeNode("OutOfRangeDefaultTest", "bad_default"));
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		const bool accepted = AvbCompiler::Compile(graph, compiled, validation);
		const std::string report = validation.Report();
		const bool identifiesGraphInput = report.find("oversized_float_input") != std::string::npos;
		const bool identifiesVariable = report.find("oversized_time_variable") != std::string::npos;
		const bool identifiesNodePin = report.find("bad_default") != std::string::npos &&
		                               report.find("Value") != std::string::npos;
		Check(!accepted, "Float and Time defaults outside finite float range are rejected by the compiler");
		Check(identifiesGraphInput, "out-of-range Float graph-input defaults are reported");
		Check(identifiesVariable, "out-of-range Time variable defaults are reported");
		Check(identifiesNodePin, "out-of-range node defaults identify the exact node and pin");
		return !accepted && identifiesGraphInput && identifiesVariable && identifiesNodePin;
	}

	bool TestCompilerEnforcesAudioBusReaderTapChoices()
	{
		auto compileWithTap = [](const std::string& tap, std::string* report = nullptr)
		{
			AvbGraph graph;
			graph.Name = "AudioBusReaderTapChoices";
			graph.Output = AvbChannelFormat::Mono;
			AvbNode reader = MakeNode("AudioBusReader", "reader_choice");
			reader.Literals = { { "Bus", AvbLiteral("1") }, { "Tap", AvbLiteral(tap) } };
			graph.Nodes = { reader, MakeNode("Output", "out_choice") };
			AddLink(graph, "reader_choice", "Out", "out_choice", "In");
			AvbCompiledGraph compiled;
			AvbValidationResult validation;
			const bool accepted = AvbCompiler::Compile(graph, compiled, validation);
			if (report) *report = validation.Report();
			return accepted;
		};

		const std::vector<std::string> supportedTaps = {
			"PreFx", "PreFX", "PostFx", "PostFX", "PostFader", "PostFade"
		};
		bool acceptsAllRuntimeSpellings = true;
		for (const std::string& tap : supportedTaps)
			if (!compileWithTap(tap)) acceptsAllRuntimeSpellings = false;

		std::string report;
		const bool acceptsUnsupportedChoice = compileWithTap("UnsupportedTapChoice", &report);
		const bool identifiesNodePin = report.find("reader_choice") != std::string::npos &&
		                               report.find("Tap") != std::string::npos;
		Check(acceptsAllRuntimeSpellings,
		      "AudioBusReader Tap accepts every spelling already supported by runtime preparation");
		Check(!acceptsUnsupportedChoice,
		      "the compiler rejects AudioBusReader Tap strings outside its declared choices");
		Check(identifiesNodePin, "enum choice diagnostics identify the exact node and pin");
		return acceptsAllRuntimeSpellings && !acceptsUnsupportedChoice && identifiesNodePin;
	}

	bool TestRuntimeRejectsFiniteLiteralOutsideFloatRange()
	{
		AvbGraph graph = MakeSineGainOutputGraph(440.0f);
		graph.FindNode("sine")->Literals.push_back({ "Amplitude", AvbLiteral(1.0e300) });
		AvbRuntime runtime;
		std::string error;
		const bool accepted = CompileAndPrepare(graph, runtime, &error);
		const bool identifiesPin = error.find("sine") != std::string::npos &&
		                           error.find("Amplitude") != std::string::npos;
		Check(!accepted && !runtime.IsPrepared(),
		      "runtime preparation rejects a finite double that narrows to non-finite float");
		Check(identifiesPin, "the narrowing diagnostic identifies the node and pin");
		return !accepted && !runtime.IsPrepared() && identifiesPin;
	}


	bool TestNoiseSeedPreservesInt64LowBits()
	{
		constexpr std::int64_t seed = 9007199254740993LL;
		AvbGraph graph;
		graph.Name = "ExactInt64NoiseSeed";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode noise = MakeNode("Noise", "noise");
		noise.Literals.push_back({ "Seed", AvbLiteral(seed) });
		graph.Nodes = { noise, MakeNode("Output", "out") };
		AddLink(graph, "noise", "Out", "out", "In");

		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		std::vector<float> actual(kBlockFrames, 0.0f);
		Check(runtime.RenderBlock(actual.data(), actual.size()), "the exact-seed noise block renders");
		Dsp::Noise expected(kSampleRate, 1u);
		bool matches = true;
		for (float sample : actual)
			if (sample != expected.ProcessSample())
			{
				matches = false;
				break;
			}
		Check(matches, "Noise uses the exact int64 seed low bits without a lossy double round-trip");
		return matches;
	}

	bool TestFiniteExtremeGainDoesNotEmitOverflow()
	{
		AvbGraph graph = MakeSineGainOutputGraph(997.0f);
		graph.Nodes[0].Literals.push_back({ "Amplitude", AvbLiteral(2.0f) });
		graph.Nodes[1].Literals[0].Value = static_cast<double>((std::numeric_limits<float>::max)());
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		std::vector<float> output(kBlockFrames, 0.0f);
		Check(runtime.RenderBlock(output.data(), output.size()), "the extreme-but-finite gain block renders");
		const bool finite = std::all_of(output.begin(), output.end(),
		                                [](float sample) { return std::isfinite(sample); });
		Check(finite, "finite representable node parameters cannot leak overflow into graph output");
		return finite;
	}

	bool TestStereoGraphUsesInterleavedStereoOutputApi()
	{
		AvbGraph graph = MakeSineGainOutputGraph(440.0f);
		graph.Output = AvbChannelFormat::Stereo;
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		std::vector<float> mono(kBlockFrames, -1.0f);
		const bool monoAccepted = runtime.RenderBlock(mono.data(), mono.size());
		const bool monoCleared = std::all_of(mono.begin(), mono.end(),
		                                     [](float sample) { return sample == 0.0f; });
		Check(!monoAccepted && monoCleared,
		      "the mono render API rejects a Stereo graph and clears its output buffer");

		auto isDualMono = [](const std::vector<float>& stereo)
		{
			for (std::size_t frame = 0; frame < stereo.size() / 2; ++frame)
				if (stereo[frame * 2] != stereo[frame * 2 + 1])
					return false;
			return true;
		};
		std::vector<float> first(kBlockFrames * 2, -1.0f);
		std::vector<float> second(kBlockFrames * 2, -1.0f);
		const bool firstRendered = runtime.RenderBlockStereo(first.data(), kBlockFrames);
		const bool secondRendered = runtime.ExecuteBlockStereo(second.data(), kBlockFrames);
		const bool dualMono = isDualMono(first) && isDualMono(second);
		const bool audible = std::any_of(first.begin(), first.end(),
		                                 [](float sample) { return sample != 0.0f; });
		Check(firstRendered && secondRendered,
		      "RenderBlockStereo and ExecuteBlockStereo render the Stereo graph");
		Check(dualMono, "mono graph audio pins are upmixed to identical interleaved L/R samples");
		Check(audible, "stereo rendering preserves non-silent graph output");
		return !monoAccepted && monoCleared && firstRendered && secondRendered && dualMono && audible;
	}

	bool TestRuntimeRejectsNon48kEngineMixRate()
	{
		AvbGraph graph = MakeSineGainOutputGraph(440.0f);
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		Check(AvbCompiler::Compile(graph, compiled, validation), "the 44.1 kHz rejection graph compiles");
		AvbRuntime runtime;
		std::string error;
		const bool accepted = runtime.Prepare(graph, compiled, 44100, kBlockFrames, 256, &error);
		Check(!accepted && !runtime.IsPrepared(),
		      "the AVB runtime rejects a rate that disagrees with the fixed 48 kHz engine mix");
		return !accepted && !runtime.IsPrepared();
	}

	bool TestRuntimeRejectsCompiledViewFromAnotherGraph()
	{
		AvbGraph compiledSource = MakeSineGainOutputGraph(997.0f);
		AvbCompiledGraph compiled;
		AvbValidationResult validation;
		Check(AvbCompiler::Compile(compiledSource, compiled, validation),
		      "the compiled source graph is valid");
		AvbGraph differentGraph = MakeSineGainOutputGraph(1200.0f);
		AvbRuntime runtime;
		std::string error;
		const bool accepted = runtime.Prepare(differentGraph, compiled, kSampleRate, kBlockFrames, 256, &error);
		Check(!accepted && !runtime.IsPrepared(),
		      "runtime preparation rejects node pointers compiled from a different graph with matching IDs");
		return !accepted && !runtime.IsPrepared();
	}

	class CountingPassThroughEffect final : public IAudioBusEffect
	{
	public:
		explicit CountingPassThroughEffect(std::size_t& calls) noexcept : m_Calls(calls) {}
		void Process(float*, std::size_t) noexcept override { ++m_Calls; }
	private:
		std::size_t& m_Calls;
	};

	AvbGraph MakeBusWriterGraph(float frequency)
	{
		AvbGraph graph;
		graph.Name = "SharedBusWriter";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode sine = MakeNode("Sine", "sine");
		sine.Literals.push_back({ "Frequency", AvbLiteral(frequency) });
		AvbNode writer = MakeNode("AudioBusWriter", "writer");
		writer.Literals.push_back({ "Bus", AvbLiteral("1") });
		graph.Nodes = { sine, writer, MakeNode("Output", "out") };
		AddLink(graph, "sine", "Out", "writer", "In");
		AddLink(graph, "sine", "Out", "out", "In");
		return graph;
	}

	bool TestMultipleRuntimesAccumulateSharedBusOnce()
	{
		AudioBusGraph buses;
		const AudioBusId master = buses.AddBus();
		const AudioBusId sourceBus = buses.AddBus();
		Check(buses.SetMasterBus(master), "shared bus graph accepts its master bus");
		Check(buses.SetMainSend(sourceBus, false), "shared source bus keeps an isolated tap");
		std::size_t effectCalls = 0;
		Check(buses.AddEffect(sourceBus, std::make_unique<CountingPassThroughEffect>(effectCalls)),
		      "shared source bus accepts the counting passthrough effect");
		Check(buses.Prepare(kBlockFrames), "shared bus graph preallocates the transaction block");

		AvbGraph firstGraph = MakeBusWriterGraph(440.0f);
		AvbGraph secondGraph = MakeBusWriterGraph(997.0f);
		AvbRuntime firstRuntime;
		AvbRuntime secondRuntime;
		AvbRuntime readerRuntime;
		std::string error;
		Check(CompileAndPrepare(firstGraph, firstRuntime, &error), error.c_str());
		Check(CompileAndPrepare(secondGraph, secondRuntime, &error), error.c_str());
		AvbGraph readerGraph;
		readerGraph.Name = "SharedBusReader";
		readerGraph.Output = AvbChannelFormat::Mono;
		AvbNode reader = MakeNode("AudioBusReader", "reader");
		reader.Literals.push_back({ "Bus", AvbLiteral("1") });
		readerGraph.Nodes = { reader, MakeNode("Output", "out") };
		AddLink(readerGraph, "reader", "Out", "out", "In");
		Check(CompileAndPrepare(readerGraph, readerRuntime, &error), error.c_str());
		if (!firstRuntime.IsPrepared() || !secondRuntime.IsPrepared() || !readerRuntime.IsPrepared())
			return false;

		std::vector<float> previousBlock(kBlockFrames, 0.0f);
		std::vector<float> firstOutput(kBlockFrames, 0.0f);
		std::vector<float> secondOutput(kBlockFrames, 0.0f);
		std::vector<float> readDuringBlock(kBlockFrames, 0.0f);
		float busOutput[kBlockFrames * 2] = {};
		Check(firstRuntime.RenderBlock(previousBlock.data(), previousBlock.size(), &buses),
		      "the first runtime completes the prior shared-bus block by default");
		const float* priorTap = buses.GetTapBuffer(sourceBus, AudioBusTap::PostFader);
		Check(priorTap != nullptr && buses.LastRenderedFrames() == kBlockFrames,
		      "the prior shared-bus block is available to readers");
		if (!priorTap) return false;
		const float expectedPriorSample = 0.5f * (priorTap[20] + priorTap[21]);
		Check(effectCalls == 1, "the source-bus effect runs once for the prior block");

		Check(buses.BeginBlock(kBlockFrames), "the shared bus graph begins one block transaction");
		Check(buses.LastRenderedFrames() == kBlockFrames,
		      "beginning a transaction preserves the last completed block length");
		Check(readerRuntime.RenderBlock(readDuringBlock.data(), readDuringBlock.size(), &buses),
		      "a bus reader renders while the shared transaction is open");
		Check(std::fabs(readDuringBlock[10] - expectedPriorSample) < 1.0e-6f,
		      "readers observe the previous completed tap until transaction finalization");

		Check(firstRuntime.RenderBlock(firstOutput.data(), firstOutput.size(), &buses,
		                               AvbBusBlockMode::Defer),
		      "the first runtime accumulates its writer without finalizing the bus graph");
		Check(secondRuntime.RenderBlock(secondOutput.data(), secondOutput.size(), &buses,
		                                AvbBusBlockMode::Defer),
		      "the second runtime accumulates its writer into the same transaction");
		Check(readerRuntime.RenderBlock(readDuringBlock.data(), readDuringBlock.size(), &buses),
		      "a reader still renders after all writers stage their contributions");
		Check(std::fabs(readDuringBlock[10] - expectedPriorSample) < 1.0e-6f,
		      "staged writer inputs do not replace the prior completed reader tap");
		Check(effectCalls == 1, "deferred runtimes do not advance bus effects before finalization");

		Check(buses.EndBlock(busOutput, kBlockFrames), "the shared transaction finalizes successfully");
		const float* combinedTap = buses.GetTapBuffer(sourceBus, AudioBusTap::PostFader);
		Check(combinedTap != nullptr && buses.LastRenderedFrames() == kBlockFrames,
		      "finalization publishes the combined bus block");
		Check(effectCalls == 2, "the shared source-bus effect runs exactly once for both runtimes");
		if (!combinedTap) return false;
		bool summed = true;
		for (std::size_t frame = 0; frame < kBlockFrames; ++frame)
		{
			const float expected = firstOutput[frame] + secondOutput[frame];
			if (std::fabs(combinedTap[frame * 2] - expected) >= 1.0e-5f ||
			    std::fabs(combinedTap[frame * 2 + 1] - expected) >= 1.0e-5f)
				summed = false;
		}
		Check(summed, "both runtimes' stereo writer samples are summed on the shared source bus");
		return summed && effectCalls == 2 &&
		       std::fabs(readDuringBlock[10] - expectedPriorSample) < 1.0e-6f;
	}

	AvbGraph MakeNoiseGraph()
	{
		AvbGraph graph;
		graph.Name = "SeededNoise";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode noise = MakeNode("Noise", "noise");
		noise.Literals.push_back({ "Seed", AvbLiteral(1234567) });
		AvbNode gain = MakeNode("Gain", "gain");
		gain.Literals.push_back({ "Gain", AvbLiteral(0.2f) });
		graph.Nodes = { noise, gain, MakeNode("Output", "out") };
		AddLink(graph, "noise", "Out", "gain", "In");
		AddLink(graph, "gain", "Out", "out", "In");
		return graph;
	}

	bool TestThousandBlocksAreBitIdenticalAcrossRuns()
	{
		AvbGraph graph = MakeNoiseGraph();
		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;

		constexpr std::size_t totalSamples = 1000 * kBlockFrames;
		std::vector<float> first(totalSamples, 0.0f);
		std::vector<float> second(totalSamples, 0.0f);
		for (std::size_t block = 0; block < 1000; ++block)
			Check(runtime.RenderBlock(first.data() + block * kBlockFrames, kBlockFrames),
			      "first deterministic 1000-block render completes");
		runtime.Reset();
		for (std::size_t block = 0; block < 1000; ++block)
			Check(runtime.RenderBlock(second.data() + block * kBlockFrames, kBlockFrames),
			      "second deterministic 1000-block render completes");
		const bool identical = std::memcmp(first.data(), second.data(), totalSamples * sizeof(float)) == 0;
		Check(identical, "1000 blocks from the same graph and seed are bit-identical after reset");
		return identical;
	}

	bool TestFeedbackEdgeCarriesPreviousBlockAudio()
	{
		AvbGraph graph;
		graph.Name = "PersistentFeedbackEdge";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode sine = MakeNode("Sine", "sine");
		sine.Literals.push_back({ "Frequency", AvbLiteral(100.0f) });
		AvbNode mixer = MakeNode("StereoMixer", "mixer");
		mixer.Literals.push_back({ "GainA", AvbLiteral(1.0f) });
		mixer.Literals.push_back({ "GainB", AvbLiteral(0.5f) });
		graph.Nodes = { sine, mixer, MakeNode("Output", "out") };
		AddLink(graph, "sine", "Out", "mixer", "A");
		AddLink(graph, "mixer", "Out", "mixer", "B", true);
		AddLink(graph, "mixer", "Out", "out", "In");

		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;
		std::vector<float> first(kBlockFrames, 0.0f), second(kBlockFrames, 0.0f);
		Check(runtime.RenderBlock(first.data(), first.size()), "the first feedback block renders");
		Check(runtime.RenderBlock(second.data(), second.size()), "the second feedback block renders");
		const float expected = first[10] * 1.5f;
		const bool persisted = std::fabs(second[10] - expected) < 1.0e-4f;
		Check(persisted, "the declared feedback edge contributes its previous block sample");
		return persisted;
	}

	bool TestFeedbackTriggerIsDeliveredOnTheNextBlock()
	{
		AvbGraph graph;
		graph.Name = "PersistentFeedbackTrigger";
		graph.Output = AvbChannelFormat::Mono;
		AvbNode first = MakeNode("TriggerOnce", "onceA");
		AvbNode second = MakeNode("TriggerOnce", "onceB");
		AvbNode envelope = MakeNode("ADEnvelope", "envelope");
		envelope.Literals.push_back({ "Attack", AvbLiteral(0.001f) });
		graph.Nodes = { first, second, envelope, MakeNode("Output", "out") };
		AddLink(graph, "onceA", "Out", "onceB", "In", true);
		AddLink(graph, "onceB", "Out", "envelope", "Trigger");
		AddLink(graph, "envelope", "Out", "out", "In");

		AvbRuntime runtime;
		std::string error;
		Check(CompileAndPrepare(graph, runtime, &error), error.c_str());
		if (!runtime.IsPrepared()) return false;
		std::vector<float> firstBlock(kBlockFrames, -1.0f), secondBlock(kBlockFrames, 0.0f);
		Check(runtime.QueueTrigger("onceA", "In", 137), "feedback trigger is queued at frame 137");
		Check(runtime.RenderBlock(firstBlock.data(), firstBlock.size()), "source trigger block renders");
		Check(std::all_of(firstBlock.begin(), firstBlock.end(), [](float value) { return value == 0.0f; }),
		      "a feedback trigger is delayed rather than becoming a same-block combinational event");
		Check(runtime.RenderBlock(secondBlock.data(), secondBlock.size()), "delayed trigger block renders");
		std::size_t firstNonZero = secondBlock.size();
		for (std::size_t frame = 0; frame < secondBlock.size(); ++frame)
			if (secondBlock[frame] != 0.0f) { firstNonZero = frame; break; }
		Check(firstNonZero == 137, "the persistent trigger edge delivers its frame on the next block");
		return firstNonZero == 137;
	}

	bool TestAudioBusReaderWriterBridge()
	{
		AudioBusGraph buses;
		const AudioBusId master = buses.AddBus();
		const AudioBusId sourceBus = buses.AddBus();
		Check(buses.SetMasterBus(master), "bus graph accepts its master bus");
		Check(buses.SetMainSend(sourceBus, false), "the source bus can expose its own tap without master summing");
		Check(buses.Prepare(kBlockFrames), "bus graph preallocates the runtime block capacity");

		AvbGraph writerGraph;
		writerGraph.Name = "BusWriterAdapter";
		writerGraph.Output = AvbChannelFormat::Mono;
		AvbNode sine = MakeNode("Sine", "sine");
		sine.Literals.push_back({ "Frequency", AvbLiteral(1000.0f) });
		AvbNode writer = MakeNode("AudioBusWriter", "writer");
		writer.Literals.push_back({ "Bus", AvbLiteral("1") });
		writerGraph.Nodes = { sine, writer };
		AddLink(writerGraph, "sine", "Out", "writer", "In");

		AvbRuntime writerRuntime;
		std::string error;
		Check(CompileAndPrepare(writerGraph, writerRuntime, &error), error.c_str());
		if (!writerRuntime.IsPrepared()) return false;
		std::vector<float> ignored(kBlockFrames, 0.0f);
		Check(writerRuntime.RenderBlock(ignored.data(), ignored.size(), &buses),
		      "AudioBusWriter stages graph audio into the prepared bus graph");
		const float* busTap = buses.GetTapBuffer(sourceBus, AudioBusTap::PostFader);
		Check(busTap != nullptr && buses.LastRenderedFrames() == kBlockFrames,
		      "the bus graph exposes the writer's rendered post-fader tap");
		if (!busTap) return false;
		Check(std::fabs(busTap[20] - busTap[21]) < 1.0e-6f,
		      "AudioBusWriter adapts mono graph audio to duplicated stereo bus samples");
		Check(std::fabs(busTap[20]) > 0.01f, "the writer bridge sends non-silent audio to its selected bus");

		AvbGraph readerGraph;
		readerGraph.Name = "BusReaderAdapter";
		readerGraph.Output = AvbChannelFormat::Mono;
		AvbNode reader = MakeNode("AudioBusReader", "reader");
		reader.Literals.push_back({ "Bus", AvbLiteral("1") });
		readerGraph.Nodes = { reader, MakeNode("Output", "out") };
		AddLink(readerGraph, "reader", "Out", "out", "In");
		AvbRuntime readerRuntime;
		Check(CompileAndPrepare(readerGraph, readerRuntime, &error), error.c_str());
		if (!readerRuntime.IsPrepared()) return false;
		std::vector<float> readBack(kBlockFrames, 0.0f);
		Check(readerRuntime.RenderBlock(readBack.data(), readBack.size(), &buses),
		      "AudioBusReader consumes the selected AudioBusGraph tap");
		Check(std::fabs(readBack[10] - (busTap[20] + busTap[21]) * 0.5f) < 1.0e-6f,
		      "AudioBusReader downmixes the bus tap into the graph audio buffer");
		return std::fabs(readBack[10] - (busTap[20] + busTap[21]) * 0.5f) < 1.0e-6f;
	}

	bool TestProjectRuntimeHostLoadsProjectGraphAndRendersWav()
	{
		namespace fs = std::filesystem;
		const std::vector<std::string> previousRecentProjects = Wasteland::Project::GetRecentProjects();
		Wasteland::Project& project = Wasteland::Project::Get();
		project.Close();

		const fs::path projectRoot = fs::temp_directory_path() /
			("WastelandAvbProjectRuntimeTests-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		const fs::path assets = projectRoot / "assets";
		std::error_code ec;
		fs::remove_all(projectRoot, ec);
		fs::create_directories(assets / "graphs");
		fs::create_directories(assets / "audio");
		auto writeWavePlayerGraph = [](const fs::path& path, const std::string& clipAssetRef)
		{
			std::ofstream graph(path, std::ios::binary);
			graph << "schema: 1\n"
				"name: ProjectRuntimeWavePlayer\n"
				"output: Mono\n"
				"inputs: []\n"
				"variables: []\n"
				"nodes:\n"
				"  - id: wave\n"
				"    type: WavePlayer\n"
				"    title: Wave Player\n"
				"    literals:\n"
				"      - pin: Clip\n"
				"        value: \"" << clipAssetRef << "\"\n"
				"  - id: out\n"
				"    type: Output\n"
				"    title: Output\n"
				"links:\n"
				"  - from:\n"
				"      node: wave\n"
				"      pin: Out\n"
				"    to:\n"
				"      node: out\n"
				"      pin: In\n"
				"sends: []\n";
			return static_cast<bool>(graph);
		};

		{
			std::ofstream descriptor(projectRoot / "project.wasteland", std::ios::binary);
			descriptor << "Version: 1\nName: AVBProjectRuntimeTests\n";
			Check(static_cast<bool>(descriptor), "the temporary active Project descriptor is authored");
		}
		Check(writeWavePlayerGraph(assets / "graphs" / "ProjectWavePlayer.wlavb", "audio/tone.wav"),
		      "the authored .wlavb graph asset is written to the project");

		const std::vector<float> authoredSamples = { 0.25f, -0.5f, 0.75f, -0.25f, 0.5f, -0.75f, 1.0f, -1.0f };
		auto authoredClip = Wasteland::Audio::AudioClip::FromSamples(authoredSamples, static_cast<int>(kSampleRate));
		Check(authoredClip->SaveWav((assets / "audio" / "tone.wav").string()),
		      "the project WAV fixture is saved as a real RIFF/WAVE asset");
		const fs::path outsideWavPath = projectRoot / "outside.wav";
		auto outsideClip = Wasteland::Audio::AudioClip::FromSamples(authoredSamples, static_cast<int>(kSampleRate));
		Check(outsideClip->SaveWav(outsideWavPath.string()), "an outside-project WAV fixture is available for rejection checks");
		Check(writeWavePlayerGraph(assets / "graphs" / "TraversalClip.wlavb", "../outside.wav"),
		      "a traversal AssetRef graph fixture is authored");
		Check(writeWavePlayerGraph(assets / "graphs" / "AbsoluteClip.wlavb", outsideWavPath.generic_string()),
		      "an absolute AssetRef graph fixture is authored");
		Check(writeWavePlayerGraph(projectRoot / "outside.wlavb", "audio/tone.wav"),
		      "an outside-project .wlavb fixture is authored");
		const fs::path escapedWavLink = assets / "audio" / "outside-link.wav";
		std::error_code wavLinkError;
		fs::create_symlink(outsideWavPath, escapedWavLink, wavLinkError);
		const bool wavSymlinkSupported = !wavLinkError;
		if (wavSymlinkSupported)
			Check(writeWavePlayerGraph(assets / "graphs" / "SymlinkClip.wlavb", "audio/outside-link.wav"),
			      "a symlink-escape WavePlayer graph fixture is authored");
		else
			std::fprintf(stdout, "SKIP: WAV symlink escape fixture unavailable: %s\n", wavLinkError.message().c_str());
		const fs::path escapedGraphLink = assets / "graphs" / "EscapedGraph.wlavb";
		std::error_code graphLinkError;
		fs::create_symlink(projectRoot / "outside.wlavb", escapedGraphLink, graphLinkError);
		const bool graphSymlinkSupported = !graphLinkError;
		if (!graphSymlinkSupported)
			std::fprintf(stdout, "SKIP: graph symlink escape fixture unavailable: %s\n", graphLinkError.message().c_str());

		std::string error;
		const bool opened = project.Open(projectRoot, &error);
		Check(opened && project.IsOpen(), opened ? "the AVB test project becomes the active Project" : error.c_str());
		if (!opened)
		{
			project.Close();
			Wasteland::Project::ClearRecentProjects();
			for (auto it = previousRecentProjects.rbegin(); it != previousRecentProjects.rend(); ++it)
				Wasteland::Project::AddRecentProject(*it);
			fs::remove_all(projectRoot, ec);
			return false;
		}

		Wasteland::Audio::AudioClip::LoadResult loadResult = Wasteland::Audio::AudioClip::LoadResult::Success;
		auto expectedClip = Wasteland::Audio::AudioClip::LoadWav(
			project.GetAssetPath("audio/tone.wav").string(), &loadResult);
		Check(expectedClip && expectedClip->IsValid(), "Project::GetAssetPath resolves and decodes the authored WAV fixture");

		AvbProjectRuntime host;
		const bool traversalRejected = !host.Load("graphs/TraversalClip.wlavb", kSampleRate, kBlockFrames, 256, &error);
		Check(traversalRejected && error.find("asset") != std::string::npos,
		      "the project host rejects a WavePlayer AssetRef that traverses outside assets");
		const bool absoluteRejected = !host.Load("graphs/AbsoluteClip.wlavb", kSampleRate, kBlockFrames, 256, &error);
		Check(absoluteRejected && error.find("asset") != std::string::npos,
		      "the project host rejects an absolute WavePlayer AssetRef");
		if (wavSymlinkSupported)
		{
			const bool symlinkRejected = !host.Load("graphs/SymlinkClip.wlavb", kSampleRate, kBlockFrames, 256, &error);
			Check(symlinkRejected && error.find("asset") != std::string::npos,
			      "the project host rejects a WavePlayer AssetRef symlink that escapes assets");
		}
		if (graphSymlinkSupported)
		{
			const bool symlinkRejected = !host.Load("graphs/EscapedGraph.wlavb", kSampleRate, kBlockFrames, 256, &error);
			Check(symlinkRejected && error.find("asset") != std::string::npos,
			      "the project host rejects a graph symlink that escapes assets");
		}
		const bool graphTraversalRejected = !host.Load("../outside.wlavb", kSampleRate, kBlockFrames, 256, &error);
		Check(graphTraversalRejected && error.find("escape") != std::string::npos,
		      "the project host rejects a graph path that traverses outside assets");
		const bool loaded = host.Load("graphs/ProjectWavePlayer.wlavb", kSampleRate, kBlockFrames, 256, &error);
		Check(loaded && host.IsPrepared(), loaded ? "the project host compiles and prepares the authored graph" : error.c_str());
		project.Close(); // Rendering must use prepared memory, not the project's files.
		bool samplesCorrect = false;
		if (loaded && expectedClip)
		{
			std::vector<float> output(kBlockFrames, -1.0f);
			Check(host.QueueTrigger("wave", "Play", 137), "the project-owned runtime accepts a sample-accurate Play trigger");
			Check(host.RenderBlock(output.data(), output.size()), "the project-owned graph renders a real audio block");
			samplesCorrect = true;
			for (std::size_t frame = 0; frame < output.size(); ++frame)
			{
				const std::size_t clipFrame = frame >= 137 ? frame - 137 : expectedClip->GetFrameCount();
				const float expected = clipFrame < expectedClip->GetFrameCount()
					? expectedClip->GetSamples()[clipFrame] : 0.0f;
				if (output[frame] != expected)
					samplesCorrect = false;
			}
		}
		Check(samplesCorrect, "rendered samples come from the project WAV beginning exactly at Play frame 137");

		project.Close();
		Wasteland::Project::ClearRecentProjects();
		for (auto it = previousRecentProjects.rbegin(); it != previousRecentProjects.rend(); ++it)
			Wasteland::Project::AddRecentProject(*it);
		fs::remove_all(projectRoot, ec);
		return opened && loaded && samplesCorrect;
	}

	bool TestProjectHostClearsOutputWhenUnloaded()
	{
		AvbProjectRuntime host;
		float monoBeforeLoad[] = { 1.0f, -1.0f, 2.0f, -2.0f };
		float stereoBeforeLoad[] = { 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f };
		const bool monoBeforeResult = host.RenderBlock(monoBeforeLoad, 4);
		const bool stereoBeforeResult = host.RenderBlockStereo(stereoBeforeLoad, 4);
		const bool outputsClearedBeforeLoad =
			std::all_of(std::begin(monoBeforeLoad), std::end(monoBeforeLoad), [](float x) { return x == 0.0f; }) &&
			std::all_of(std::begin(stereoBeforeLoad), std::end(stereoBeforeLoad), [](float x) { return x == 0.0f; });
		host.Unload();
		float monoAfterUnload[] = { 1.0f, -1.0f, 2.0f, -2.0f };
		float stereoAfterUnload[] = { 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f };
		const bool monoAfterResult = host.RenderBlock(monoAfterUnload, 4);
		const bool stereoAfterResult = host.RenderBlockStereo(stereoAfterUnload, 4);
		const bool outputsClearedAfterUnload =
			std::all_of(std::begin(monoAfterUnload), std::end(monoAfterUnload), [](float x) { return x == 0.0f; }) &&
			std::all_of(std::begin(stereoAfterUnload), std::end(stereoAfterUnload), [](float x) { return x == 0.0f; });
		const bool outputsSilent = !monoBeforeResult && !stereoBeforeResult && outputsClearedBeforeLoad &&
			!monoAfterResult && !stereoAfterResult && outputsClearedAfterUnload;
		return outputsSilent;
	}
}

int main()
{
	Check(TestGraphSerializerSaveRoundTripsAndReplacesExistingFile(),
	      "graph serializer Save round-trip and normal replacement tests complete");
#ifdef _WIN32
	Check(TestGraphSerializerPreservesTargetWhenAtomicPublishFails(),
	      "failed atomic graph publish preserves the old file");
#endif
	Check(TestWavePlayerReportsMissingAssetResolverAtClipPin(),
	      "WavePlayer missing-resolver diagnostic proof test completes");
	Check(TestWavePlayerRendersResolvedClipAndSampleAccurateTriggers(),
	      "WavePlayer resolved-buffer and lifecycle-trigger proof test completes");
	Check(TestRuntimeMaps44100HzClipTo48000HzMix(),
	      "WavePlayer source-rate mapping proof test completes");
	Check(TestWavePlayerResolutionAndNarrowingDiagnostics(),
	      "WavePlayer asset and control validation proof test completes");
	Check(TestSampleAccurateTriggerStartsEnvelopeAtFrame137(),
	      "frame-accurate trigger proof test completes");
	Check(TestTwoTriggersKeepTheirOwnFrames(), "same-block trigger proof test completes");
	Check(TestSineFrequencyAt48kHz(), "48 kHz sine frequency proof test completes");
	Check(TestCompilerRejectsFloatNarrowingAndAcceptsFloatBoundary(),
	      "compiler rejects Float literals outside finite float range");
	Check(TestCompilerRejectsTimeLiteralOutsideFloatRange(),
	      "compiler rejects Time literals outside finite float range");
	Check(TestCompilerRejectsFloatAndTimeOutOfRangeDefaults(),
	      "compiler rejects Float and Time defaults outside finite float range");
	Check(TestCompilerEnforcesAudioBusReaderTapChoices(),
	      "compiler enforces AudioBusReader Tap enum choices");
	Check(TestRuntimeRejectsFiniteLiteralOutsideFloatRange(), "numeric narrowing rejection proof test completes");
	Check(TestNoiseSeedPreservesInt64LowBits(), "exact int64 Noise seed proof test completes");
	Check(TestFiniteExtremeGainDoesNotEmitOverflow(), "finite arithmetic overflow proof test completes");
	Check(TestStereoGraphUsesInterleavedStereoOutputApi(), "stereo runtime output API proof test completes");
	Check(TestRuntimeRejectsNon48kEngineMixRate(), "fixed 48 kHz runtime contract proof test completes");
	Check(TestRuntimeRejectsCompiledViewFromAnotherGraph(), "compiled/source graph identity proof test completes");
	Check(TestThousandBlocksAreBitIdenticalAcrossRuns(), "determinism proof test completes");
	Check(TestFeedbackEdgeCarriesPreviousBlockAudio(), "feedback edge state proof test completes");
	Check(TestFeedbackTriggerIsDeliveredOnTheNextBlock(), "feedback trigger state proof test completes");
	Check(TestAudioBusReaderWriterBridge(), "AudioBus node bridge proof test completes");
	Check(TestMultipleRuntimesAccumulateSharedBusOnce(),
	      "multiple AVB runtimes share one bus block transaction");
	Check(TestProjectRuntimeHostLoadsProjectGraphAndRendersWav(),
	      "Project-backed AVB host integration proof test completes");
	Check(TestProjectHostClearsOutputWhenUnloaded(),
	      "unprepared and unloaded AVB project host output is silent");
	if (g_Failures != 0)
	{
		std::fprintf(stderr, "%d AVB graph runtime checks failed.\n", g_Failures);
		return 1;
	}
	std::puts("AVB graph runtime tests passed.");
	return 0;
}
