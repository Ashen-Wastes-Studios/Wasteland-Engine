#include "Wasteland/Avb/Graph/Compiler.h"
#include "Wasteland/Avb/Graph/GraphSerializer.h"
#include "Wasteland/Avb/Graph/NodeRegistry.h"
#include "Wasteland/Avb/Graph/Validator.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

	bool HasFinding(const AvbValidationResult& result, const std::string& node,
	                const std::string& pin, const std::string& phrase)
	{
		for (const AvbDiagnostic& finding : result.Findings)
			if (finding.NodeId == node && finding.PinId == pin && finding.Message.find(phrase) != std::string::npos)
				return true;
		return false;
	}

	AvbGraph MakeValidGraph(bool reverseInsertion = false)
	{
		AvbGraph graph;
		graph.Name = "Graph round trip";
		graph.Output = AvbChannelFormat::Stereo;
		graph.Inputs.push_back({ "Trigger", AvbPinType::Trigger, AvbPinDir::Input, AvbLiteral{} });
		graph.Inputs.push_back({ "Frequency", AvbPinType::Float, AvbPinDir::Input, AvbLiteral(55.0) });
		graph.Inputs.push_back({ "OnFinished", AvbPinType::Trigger, AvbPinDir::Output, AvbLiteral{} });
		graph.Variables.push_back({ "phase", AvbPinType::Float, AvbLiteral(0.25) });
		graph.Variables.push_back({ "enabled", AvbPinType::Bool, AvbLiteral(true) });

		std::vector<AvbNode> nodes = {
			AvbNodeRegistry::Get().CreateNode("Sine", "n_osc"),
			AvbNodeRegistry::Get().CreateNode("WaveShaper", "n_shape"),
			AvbNodeRegistry::Get().CreateNode("Output", "n_out")
		};
		if (reverseInsertion) std::reverse(nodes.begin(), nodes.end());
		for (AvbNode& node : nodes) graph.AddNode(std::move(node));
		graph.FindNode("n_osc")->Title = "Base oscillator";
		graph.FindNode("n_osc")->X = 10.5f;
		graph.FindNode("n_osc")->Y = -7.25f;
		graph.FindNode("n_osc")->Literals = { { "Frequency", AvbLiteral(55.0) }, { "Amplitude", AvbLiteral(0.75) } };
		graph.FindNode("n_shape")->Title = "Asymmetric drive";
		graph.FindNode("n_shape")->X = 200.0f;
		graph.FindNode("n_shape")->Y = 40.0f;
		graph.FindNode("n_shape")->Literals = {
			{ "Amount", AvbLiteral(0.65) }, { "Bias", AvbLiteral(-0.1) }, { "OutputGain", AvbLiteral(0.8) }
		};
		graph.Links = {
			{ "n_osc", "Out", "n_shape", "In", false },
			{ "n_shape", "Out", "n_out", "In", false }
		};
		graph.Sends.push_back({ "n_shape", "bus_Space", AvbSendMode::PostFade, 0.25f });
		return graph;
	}

	void TestStableUniqueNodeIdsAndAliasResolution()
	{
		AvbGraph graph;
		graph.Name = "IDs";
		AvbNode first = AvbNodeRegistry::Get().CreateNode("Sine");
		AvbNode second = AvbNodeRegistry::Get().CreateNode("Sine");
		const std::string firstId = first.Id;
		Check(!firstId.empty() && firstId.rfind("avb-", 0) == 0, "generated node IDs have a stable persisted prefix");
		Check(firstId != second.Id, "generated node IDs are unique");
		Check(graph.AddNode(std::move(first)), "a generated node can be inserted");
		Check(graph.AddNode(std::move(second)), "a second generated node can be inserted");
		Check(graph.FindNode(firstId) != nullptr, "a node retains its ID after insertion");
		std::string error;
		AvbNode duplicate = AvbNodeRegistry::Get().CreateNode("Sine", firstId);
		Check(!graph.AddNode(std::move(duplicate), &error) && error.find(firstId) != std::string::npos,
		      "duplicate stable IDs are rejected with the colliding ID named");

		const AvbNodeDef* alias = AvbNodeRegistry::Get().Find("Waveshaper");
		AvbNode aliasedNode = AvbNodeRegistry::Get().CreateNode("Waveshaper", "legacy-shaper");
		Check(alias && alias->TypeId == "WaveShaper" && alias->FindPin("Amount"),
		      "registry aliases resolve to the canonical typed pin table");
		Check(aliasedNode.TypeId == "Waveshaper", "factory creation preserves the saved alias ID");
	}

	void TestTypedPinsAndRateValidation()
	{
		const AvbNodeDef* sine = AvbNodeRegistry::Get().Find("Sine");
		const AvbPinDef* sync = sine ? sine->FindPin("Sync") : nullptr;
		const AvbPinDef* audioOut = sine ? sine->FindPin("Out") : nullptr;
		Check(sync && sync->Type == AvbPinType::Trigger && sync->Direction == AvbPinDir::Input && sync->Rate == AvbPinRate::Sample,
		      "registered pins expose type, direction and sample rate");
		Check(audioOut && audioOut->Type == AvbPinType::Audio && audioOut->Direction == AvbPinDir::Output,
		      "audio outputs use explicit typed output pins");

		AvbGraph badTrigger;
		badTrigger.Name = "Trigger mismatch";
		badTrigger.AddNode(AvbNodeRegistry::Get().CreateNode("TriggerOnce", "trigger"));
		badTrigger.AddNode(AvbNodeRegistry::Get().CreateNode("Gain", "gain"));
		badTrigger.Links.push_back({ "trigger", "Out", "gain", "In", false });
		AvbValidationResult validation;
		Check(!AvbValidator::Validate(badTrigger, validation), "Trigger-to-Audio connection is rejected");
		Check(HasFinding(validation, "gain", "In", "Trigger wire"), "trigger type diagnostic names target node and pin");

		std::string registryError;
		AvbNodeDef sampleTrigger;
		sampleTrigger.TypeId = "AvbGraphTestSampleTrigger";
		sampleTrigger.Category = "Tests";
		sampleTrigger.Pins.push_back({ "Out", AvbPinType::Trigger, AvbPinDir::Output,
			false, AvbPinRate::Sample, std::nullopt });
		sampleTrigger.Factory = [](const std::string& id) { AvbNode node; node.Id = id; node.TypeId = "AvbGraphTestSampleTrigger"; return node; };
		Check(AvbNodeRegistry::Get().Register(std::move(sampleTrigger), &registryError),
			"test-only sample-rate trigger can be registered");
		AvbNodeDef blockTrigger;
		blockTrigger.TypeId = "AvbGraphTestBlockTrigger";
		blockTrigger.Category = "Tests";
		blockTrigger.Pins.push_back({ "In", AvbPinType::Trigger, AvbPinDir::Input,
			false, AvbPinRate::Block, std::nullopt });
		blockTrigger.Factory = [](const std::string& id) { AvbNode node; node.Id = id; node.TypeId = "AvbGraphTestBlockTrigger"; return node; };
		Check(AvbNodeRegistry::Get().Register(std::move(blockTrigger), &registryError),
			"test-only block-rate trigger can be registered");

		AvbGraph badRate;
		badRate.Name = "Rate mismatch";
		badRate.AddNode(AvbNodeRegistry::Get().CreateNode("AvbGraphTestSampleTrigger", "sample_trigger"));
		badRate.AddNode(AvbNodeRegistry::Get().CreateNode("AvbGraphTestBlockTrigger", "block_trigger"));
		badRate.Links.push_back({ "sample_trigger", "Out", "block_trigger", "In", false });
		Check(!AvbValidator::Validate(badRate, validation), "block-rate and sample-rate pins cannot be linked implicitly");
		Check(HasFinding(validation, "block_trigger", "In", "rate mismatch"), "rate diagnostic identifies the target pin");

		AvbNodeDef floatSource;
		floatSource.TypeId = "AvbGraphTestFloatSource";
		floatSource.Category = "Tests";
		floatSource.Pins.push_back({ "Value", AvbPinType::Float, AvbPinDir::Output, false, AvbPinRate::Block, std::nullopt });
		floatSource.Factory = [](const std::string& id) { AvbNode node; node.Id = id; node.TypeId = "AvbGraphTestFloatSource"; return node; };
		Check(AvbNodeRegistry::Get().Register(std::move(floatSource), &registryError), "test-only float source can be registered");
		AvbNodeDef intTarget;
		intTarget.TypeId = "AvbGraphTestIntTarget";
		intTarget.Category = "Tests";
		intTarget.Pins.push_back({ "Value", AvbPinType::Int, AvbPinDir::Input, false, AvbPinRate::Block, std::nullopt });
		intTarget.Factory = [](const std::string& id) { AvbNode node; node.Id = id; node.TypeId = "AvbGraphTestIntTarget"; return node; };
		Check(AvbNodeRegistry::Get().Register(std::move(intTarget), &registryError), "test-only integer target can be registered");
		AvbGraph incompatible;
		incompatible.Name = "Different data types";
		incompatible.AddNode(AvbNodeRegistry::Get().CreateNode("AvbGraphTestFloatSource", "float"));
		incompatible.AddNode(AvbNodeRegistry::Get().CreateNode("AvbGraphTestIntTarget", "integer"));
		incompatible.Links.push_back({ "float", "Value", "integer", "Value", false });
		Check(!AvbValidator::Validate(incompatible, validation), "incompatible data types are rejected");
		Check(HasFinding(validation, "integer", "Value", "incompatible types"), "data type diagnostic identifies the target pin");

		AvbGraph reverseMismatch;
		reverseMismatch.Name = "Data into trigger";
		reverseMismatch.AddNode(AvbNodeRegistry::Get().CreateNode("Gain", "audio"));
		reverseMismatch.AddNode(AvbNodeRegistry::Get().CreateNode("TriggerOnce", "trigger"));
		reverseMismatch.Links.push_back({ "audio", "Out", "trigger", "In", false });
		Check(!AvbValidator::Validate(reverseMismatch, validation), "data wire into a Trigger pin is rejected");
		Check(HasFinding(validation, "trigger", "In", "data wire cannot connect"), "reverse Trigger mismatch names its target");
	}

	void TestWavePlayerPinsAndAssetRefSerializerRoundTrip()
	{
		const AvbNodeDef* definition = AvbNodeRegistry::Get().Find("WavePlayer");
		if (!definition)
		{
			Check(false, "WavePlayer is registered for graph editing and serialization");
			return;
		}
		Check(definition->TypeId == "WavePlayer", "WavePlayer keeps its stable type ID");
		auto expectPin = [&](const char* id, AvbPinType type, AvbPinDir direction,
		                     AvbPinRate rate, bool required)
		{
			const AvbPinDef* pin = definition->FindPin(id);
			Check(pin && pin->Type == type && pin->Direction == direction &&
			      pin->Rate == rate && pin->Required == required,
			      (std::string("WavePlayer pin contract: ") + id).c_str());
		};
		expectPin("Clip", AvbPinType::AssetRef, AvbPinDir::Input, AvbPinRate::Block, true);
		expectPin("Play", AvbPinType::Trigger, AvbPinDir::Input, AvbPinRate::Sample, false);
		expectPin("Stop", AvbPinType::Trigger, AvbPinDir::Input, AvbPinRate::Sample, false);
		expectPin("StartTime", AvbPinType::Time, AvbPinDir::Input, AvbPinRate::Block, false);
		expectPin("PitchSemitones", AvbPinType::Float, AvbPinDir::Input, AvbPinRate::Block, false);
		expectPin("Loop", AvbPinType::Bool, AvbPinDir::Input, AvbPinRate::Block, false);
		expectPin("NearlyFinishedLead", AvbPinType::Time, AvbPinDir::Input, AvbPinRate::Block, false);
		expectPin("Out", AvbPinType::Audio, AvbPinDir::Output, AvbPinRate::Sample, false);
		expectPin("OnFinished", AvbPinType::Trigger, AvbPinDir::Output, AvbPinRate::Sample, false);
		expectPin("OnNearlyFinished", AvbPinType::Trigger, AvbPinDir::Output, AvbPinRate::Sample, false);
		expectPin("OnLooped", AvbPinType::Trigger, AvbPinDir::Output, AvbPinRate::Sample, false);

		AvbGraph source;
		source.Name = "WavePlayerAssetRef";
		AvbNode player = AvbNodeRegistry::Get().CreateNode("WavePlayer", "wave_asset");
		player.Literals = {
			{ "Clip", AvbLiteral("audio/ambience.wav") },
			{ "StartTime", AvbLiteral(0.125) },
			{ "PitchSemitones", AvbLiteral(-7.0) },
			{ "Loop", AvbLiteral(true) },
			{ "NearlyFinishedLead", AvbLiteral(0.4) }
		};
		source.Nodes.push_back(std::move(player));
		const std::string yaml = AvbGraphSerializer::Emit(source);
		AvbGraph restored;
		AvbGraphError error;
		Check(AvbGraphSerializer::Parse(yaml, restored, error),
		      "WavePlayer AssetRef and controls parse from serialized graph YAML");
		Check(AvbGraphSerializer::Emit(restored) == yaml,
		      "WavePlayer AssetRef round-trips with byte-identical YAML");
		const AvbNode* restoredPlayer = restored.FindNode("wave_asset");
		const AvbLiteral* restoredClip = restoredPlayer ? restoredPlayer->FindLiteral("Clip") : nullptr;
		Check(restoredClip && std::get<std::string>(restoredClip->Value) == "audio/ambience.wav",
		      "serializer preserves the project-relative AssetRef under its stable Clip pin ID");
	}

	void TestValidatorRulesAndPreciseDiagnostics()
	{
		AvbValidationResult validation;
		AvbGraph missingInput;
		missingInput.Name = "Missing required input";
		missingInput.AddNode(AvbNodeRegistry::Get().CreateNode("Gain", "gain"));
		Check(!AvbValidator::Validate(missingInput, validation), "an unbound required input fails validation");
		Check(HasFinding(validation, "gain", "In", "neither a link nor a literal"), "required-input message names node and pin");

		AvbGraph danglingTrigger;
		danglingTrigger.Name = "Dangling trigger";
		danglingTrigger.AddNode(AvbNodeRegistry::Get().CreateNode("TriggerOnce", "once"));
		Check(!AvbValidator::Validate(danglingTrigger, validation), "a required Trigger fan-out with no receiver is rejected");
		Check(HasFinding(validation, "once", "Out", "required trigger"), "dangling trigger finding names its output pin");

		AvbGraph deletedNode;
		deletedNode.Name = "Deleted endpoint";
		deletedNode.AddNode(AvbNodeRegistry::Get().CreateNode("Output", "sink"));
		deletedNode.Links.push_back({ "removed", "Out", "sink", "In", false });
		Check(!AvbValidator::Validate(deletedNode, validation), "a link to a deleted node is rejected");
		Check(HasFinding(validation, "removed", "Out", "deleted node"), "deleted endpoint diagnostic names missing node and pin");

		AvbGraph unknownNode;
		unknownNode.Name = "Unknown node";
		unknownNode.AddNode({ "bad_node", "NotRegistered", "", 0, 0, {} });
		Check(!AvbValidator::Validate(unknownNode, validation), "unknown node types fail validation");
		Check(HasFinding(validation, "bad_node", "", "unknown node type 'NotRegistered'"), "unknown type diagnostic names the instance");

		AvbGraph unknownPin;
		unknownPin.Name = "Unknown pin";
		AvbNode badPin = AvbNodeRegistry::Get().CreateNode("Sine", "bad_pin");
		badPin.Literals.push_back({ "Ghost", AvbLiteral(1.0) });
		unknownPin.AddNode(std::move(badPin));
		Check(!AvbValidator::Validate(unknownPin, validation), "unknown node pins fail validation");
		Check(HasFinding(validation, "bad_pin", "Ghost", "unknown pin"), "unknown pin validator diagnostic names node and pin");

		AvbGraph cycle;
		cycle.Name = "Data cycle";
		cycle.AddNode(AvbNodeRegistry::Get().CreateNode("Gain", "a"));
		cycle.AddNode(AvbNodeRegistry::Get().CreateNode("Gain", "b"));
		cycle.Links = { { "a", "Out", "b", "In", false }, { "b", "Out", "a", "In", false } };
		Check(!AvbValidator::Validate(cycle, validation), "an undeclared data cycle is rejected");
		bool namesCyclePin = false;
		for (const AvbDiagnostic& finding : validation.Findings)
			if (finding.Message.find("data cycle") != std::string::npos && finding.HasNode && !finding.PinId.empty())
				namesCyclePin = true;
		Check(namesCyclePin, "cycle diagnostics identify a concrete node and input pin");
		cycle.Links[0].Feedback = true;
		Check(AvbValidator::Validate(cycle, validation), "an explicitly declared delayed-state edge breaks a data cycle");
		AvbCompiledGraph compiledFeedback;
		Check(AvbCompiler::Compile(cycle, compiledFeedback, validation), "a valid declared feedback graph compiles");
		Check(compiledFeedback.Output.find("\"feedback\":true") != std::string::npos,
		      "compiled graph preserves the delayed-state edge for the runtime");
	}

	void TestStrictYamlRoundTripAndUnknownReferences()
	{
		const AvbGraph source = MakeValidGraph();
		const std::string first = AvbGraphSerializer::Emit(source);
		AvbGraph restored;
		AvbGraphError error;
		Check(AvbGraphSerializer::Parse(first, restored, error), "all AVB graph fields parse back from YAML");
		Check(AvbGraphSerializer::Emit(restored) == first, "round-trip serialization is byte-identical");
		Check(restored.Nodes.size() == 3 && restored.Inputs.size() == 3 && restored.Variables.size() == 2 &&
		      restored.Links.size() == 2 && restored.Sends.size() == 1, "round trip preserves graph collections");
		Check(restored.FindNode("n_osc") && restored.FindNode("n_osc")->FindLiteral("Frequency") &&
		      std::get<double>(restored.FindNode("n_osc")->FindLiteral("Frequency")->Value) == 55.0,
		      "round trip preserves typed literals keyed by stable pin ID");
		Check(restored.FindNode("n_osc") && restored.FindNode("n_osc")->Id == "n_osc" &&
		      restored.FindNode("n_osc")->X == 10.5f && restored.FindNode("n_osc")->Y == -7.25f,
		      "round trip preserves stable node IDs and canvas metadata");

		const std::string unknownType = "schema: 1\nname: bad\nnodes:\n  - id: missing\n    type: DoesNotExist\n";
		Check(!AvbGraphSerializer::Parse(unknownType, restored, error), "unknown node types fail during load");
		const std::string typeDiagnostic = error.Format("assets/test.wlavb");
		Check(typeDiagnostic.find("assets/test.wlavb") != std::string::npos &&
		      typeDiagnostic.find("DoesNotExist") != std::string::npos,
		      "unknown node load error names both node type and file");
		Check(restored.Nodes.size() == 3, "failed parsing leaves the prior output graph untouched");

		const std::string unknownPin = "schema: 1\nname: bad\nnodes:\n  - id: oscillator\n    type: Sine\n    literals:\n      - pin: Ghost\n        value: 2.0\n";
		Check(!AvbGraphSerializer::Parse(unknownPin, restored, error), "unknown pins fail during load rather than being dropped");
		Check(error.Format("bad.wlavb").find("node 'oscillator' pin 'Ghost'") != std::string::npos,
		      "unknown pin load error includes exact node and pin IDs");

		const std::string unknownSend = "schema: 1\nname: bad\nsends:\n  - from: n1\n    to: bus\n    mode: PreAmp\n";
		Check(!AvbGraphSerializer::Parse(unknownSend, restored, error), "unknown send modes fail during load");
		Check(error.Format("bad.wlavb").find("unknown send mode 'PreAmp'") != std::string::npos,
		      "unknown send diagnostic identifies the unsupported mode and file");
	}

	void TestDeterministicCompilerOutput()
	{
		AvbGraph first = MakeValidGraph(false);
		AvbGraph second = MakeValidGraph(true);
		AvbCompiledGraph compiledA, compiledB;
		AvbValidationResult validationA, validationB;
		Check(AvbCompiler::Compile(first, compiledA, validationA), "valid graph compiles");
		Check(AvbCompiler::Compile(second, compiledB, validationB), "same graph compiles independent of insertion order");
		Check(compiledA.Output == compiledB.Output, "compiler output is byte-identical across node insertion orders");
		Check(compiledA.Nodes.size() == 3 && compiledA.Nodes[0]->Id == "n_osc" &&
		      compiledA.Nodes[1]->Id == "n_shape" && compiledA.Nodes[2]->Id == "n_out",
		      "compiled nodes use deterministic topological order");
		Check(compiledA.Output.find("n_osc") != std::string::npos &&
		      compiledA.Output.find("bus_Space") != std::string::npos,
		      "compiled JSON contains graph identity and send data, without a source path");
	}
}

int main()
{
	TestStableUniqueNodeIdsAndAliasResolution();
	TestTypedPinsAndRateValidation();
	TestWavePlayerPinsAndAssetRefSerializerRoundTrip();
	TestValidatorRulesAndPreciseDiagnostics();
	TestStrictYamlRoundTripAndUnknownReferences();
	TestDeterministicCompilerOutput();
	std::printf("AVB graph: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures == 0 ? 0 : 1;
}
