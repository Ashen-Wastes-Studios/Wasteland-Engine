#include "wlpch.h"

#include "NodeRegistry.h"

#include <set>

namespace Wasteland::Avb
{
	namespace
	{
		AvbPinDef Pin(const char* id, AvbPinType type, AvbPinDir dir,
		              bool required = false, AvbPinRate rate = AvbPinRate::Block,
		              std::optional<AvbLiteral> defaultValue = std::nullopt,
		              std::vector<std::string> allowedValues = {})
		{
			return { id, type, dir, required, rate, std::move(defaultValue), std::move(allowedValues) };
		}

		std::function<AvbNode(const std::string&)> Factory(const std::string& typeId)
		{
			return [typeId](const std::string& id)
			{
				AvbNode node;
				node.Id = id;
				node.TypeId = typeId;
				return node;
			};
		}

		AvbNodeDef Definition(const char* typeId, const char* category,
		                      std::vector<AvbPinDef> pins)
		{
			AvbNodeDef def;
			def.TypeId = typeId;
			def.Category = category;
			def.Version = 1;
			def.Pins = std::move(pins);
			def.Factory = Factory(def.TypeId);
			return def;
		}
	}

	const AvbPinDef* AvbNodeDef::FindPin(const std::string& pinId) const noexcept
	{
		for (const AvbPinDef& pin : Pins)
			if (pin.Id == pinId)
				return &pin;
		return nullptr;
	}

	AvbNodeRegistry& AvbNodeRegistry::Get()
	{
		static AvbNodeRegistry registry;
		return registry;
	}

	AvbNodeRegistry::AvbNodeRegistry()
	{
		RegisterBuiltins();
	}

	const AvbNodeDef* AvbNodeRegistry::FindExact(const std::string& typeId) const noexcept
	{
		for (const AvbNodeDef& def : m_Defs)
			if (def.TypeId == typeId)
				return &def;
		return nullptr;
	}

	const AvbNodeDef* AvbNodeRegistry::Find(const std::string& typeId) const noexcept
	{
		const AvbNodeDef* def = FindExact(typeId);
		for (std::size_t depth = 0; def && !def->AliasOf.empty() && depth < m_Defs.size(); ++depth)
			def = FindExact(def->AliasOf);
		return def && def->AliasOf.empty() ? def : nullptr;
	}

	bool AvbNodeRegistry::IsRegistered(const std::string& typeId) const noexcept
	{
		return Find(typeId) != nullptr;
	}

	bool AvbNodeRegistry::Register(AvbNodeDef def, std::string* error)
	{
		const auto reject = [error](const std::string& message)
		{
			if (error)
				*error = message;
			return false;
		};
		if (def.TypeId.empty())
			return reject("node type id must not be empty");
		if (FindExact(def.TypeId))
			return reject("node type '" + def.TypeId + "' is already registered");
		if (def.Version == 0)
			return reject("node type '" + def.TypeId + "' has invalid version 0");

		std::set<std::string> pinIds;
		for (const AvbPinDef& pin : def.Pins)
		{
			if (pin.Id.empty() || !pinIds.insert(pin.Id).second)
				return reject("node type '" + def.TypeId + "' has an empty or duplicate pin id");
			AvbPinType checkedType;
			AvbPinDir checkedDirection;
			AvbPinRate checkedRate;
			if (!AvbPinTypeFromName(AvbPinTypeName(pin.Type), checkedType) ||
			    !AvbPinDirFromName(AvbPinDirName(pin.Direction), checkedDirection) ||
			    !AvbPinRateFromName(AvbPinRateName(pin.Rate), checkedRate))
				return reject("node type '" + def.TypeId + "' has an invalid type, direction or rate for pin '" + pin.Id + "'");
			if (pin.Direction == AvbPinDir::Output && pin.Required && pin.Type != AvbPinType::Trigger)
				return reject("only Trigger outputs may require a receiver on node type '" + def.TypeId + "'");
			if (pin.DefaultValue && (pin.Direction != AvbPinDir::Input ||
			                         !AvbLiteralMatchesType(*pin.DefaultValue, pin.Type)))
				return reject("node type '" + def.TypeId + "' has a default of the wrong type for pin '" + pin.Id + "'");
		}

		if (!def.AliasOf.empty())
		{
			if (!def.Pins.empty() || def.Factory)
				return reject("alias '" + def.TypeId + "' must inherit pins and factory from AliasOf");
			const AvbNodeDef* target = Find(def.AliasOf);
			if (!target)
				return reject("alias '" + def.TypeId + "' points to unknown node type '" + def.AliasOf + "'");
		}
		else if (!def.Factory)
			return reject("node type '" + def.TypeId + "' requires a factory");

		m_Defs.push_back(std::move(def));
		return true;
	}

	AvbNode AvbNodeRegistry::CreateNode(const std::string& typeId, const std::string& id) const
	{
		const AvbNodeDef* def = Find(typeId);
		if (!def)
			return {};
		AvbNode node = def->Factory(id.empty() ? GenerateStableId() : id);
		node.TypeId = typeId;
		return node;
	}

	void AvbNodeRegistry::RegisterBuiltins()
	{
		const AvbLiteral one(1.0);
		Register(Definition("Sine", "Generators", {
			Pin("Frequency", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(440.0)),
			Pin("Amplitude", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Sync", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample),
			Pin("OnFinished", AvbPinType::Trigger, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("Noise", "Generators", {
			Pin("Seed", AvbPinType::Int, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0)),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("WavePlayer", "Generators", {
			Pin("Clip", AvbPinType::AssetRef, AvbPinDir::Input, true),
			Pin("Play", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("Stop", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("StartTime", AvbPinType::Time, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.0)),
			Pin("PitchSemitones", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.0)),
			Pin("Loop", AvbPinType::Bool, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(false)),
			Pin("NearlyFinishedLead", AvbPinType::Time, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.02)),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample),
			Pin("OnFinished", AvbPinType::Trigger, AvbPinDir::Output, false, AvbPinRate::Sample),
			Pin("OnNearlyFinished", AvbPinType::Trigger, AvbPinDir::Output, false, AvbPinRate::Sample),
			Pin("OnLooped", AvbPinType::Trigger, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("Gain", "Mixing", {
			Pin("In", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample),
			Pin("Gain", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("WaveShaper", "Shaping", {
			Pin("In", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample),
			Pin("Amount", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Bias", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.0)),
			Pin("OutputGain", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("StereoMixer", "Mixing", {
			Pin("A", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample),
			Pin("B", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample),
			Pin("GainA", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("GainB", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("AudioBusReader", "IO", {
			Pin("Bus", AvbPinType::AssetRef, AvbPinDir::Input, true),
			Pin("Tap", AvbPinType::Enum, AvbPinDir::Input, false, AvbPinRate::Block,
			    AvbLiteral("PostFader"), { "PreFx", "PreFX", "PostFx", "PostFX", "PostFader", "PostFade" }),
			Pin("Gain", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("AudioBusWriter", "IO", {
			Pin("In", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample),
			Pin("Bus", AvbPinType::AssetRef, AvbPinDir::Input, true),
			Pin("Gain", AvbPinType::Float, AvbPinDir::Input, false, AvbPinRate::Block, one)
		}));
		Register(Definition("ADEnvelope", "Envelopes", {
			Pin("Trigger", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("Attack", AvbPinType::Time, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.01)),
			Pin("Decay", AvbPinType::Time, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.1)),
			Pin("Out", AvbPinType::Audio, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("TriggerDelay", "Triggers", {
			Pin("In", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("Delay", AvbPinType::Time, AvbPinDir::Input, false, AvbPinRate::Block, AvbLiteral(0.0)),
			Pin("Out", AvbPinType::Trigger, AvbPinDir::Output, false, AvbPinRate::Sample)
		}));
		Register(Definition("TriggerOnce", "Triggers", {
			Pin("In", AvbPinType::Trigger, AvbPinDir::Input, false, AvbPinRate::Sample),
			Pin("Out", AvbPinType::Trigger, AvbPinDir::Output, true, AvbPinRate::Sample)
		}));
		Register(Definition("Output", "IO", {
			Pin("In", AvbPinType::Audio, AvbPinDir::Input, true, AvbPinRate::Sample)
		}));
		AvbNodeDef alias;
		alias.TypeId = "Waveshaper";
		alias.Category = "Shaping";
		alias.Version = 1;
		alias.AliasOf = "WaveShaper";
		Register(std::move(alias));
	}
}
