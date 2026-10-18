#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Wasteland::Avb
{
	enum class AvbPinType : std::uint8_t
	{
		Trigger, Audio, Time, Float, Int, Bool, Enum, String, AssetRef
	};
	enum class AvbPinDir : std::uint8_t { Input, Output };
	enum class AvbPinRate : std::uint8_t { Block, Sample };
	enum class AvbChannelFormat : std::uint8_t { Mono, Stereo };
	enum class AvbSendMode : std::uint8_t { PreFX, PostFX, PostFade };

	// A typed, lossless graph literal. Audio buffers and triggers are not literals.
	struct AvbLiteral
	{
		using Storage = std::variant<std::monostate, double, std::int64_t, bool, std::string>;
		Storage Value;

		AvbLiteral() = default;
		AvbLiteral(double value) : Value(value) {}
		AvbLiteral(float value) : Value(static_cast<double>(value)) {}
		AvbLiteral(std::int64_t value) : Value(value) {}
		AvbLiteral(int value) : Value(static_cast<std::int64_t>(value)) {}
		AvbLiteral(bool value) : Value(value) {}
		AvbLiteral(std::string value) : Value(std::move(value)) {}
		AvbLiteral(const char* value) : Value(std::string(value ? value : "")) {}
		bool IsSet() const noexcept { return !std::holds_alternative<std::monostate>(Value); }
	};

	struct AvbGraphInput
	{
		std::string Name;
		AvbPinType Type = AvbPinType::Float;
		AvbPinDir Direction = AvbPinDir::Input;
		std::optional<AvbLiteral> DefaultValue;
	};

	struct AvbVariable
	{
		std::string Name;
		AvbPinType Type = AvbPinType::Float;
		AvbLiteral DefaultValue;
	};

	struct AvbPinLiteral
	{
		std::string PinId;
		AvbLiteral Value;
	};

	struct AvbNode
	{
		std::string Id;
		std::string TypeId;
		std::string Title;
		float X = 0.0f;
		float Y = 0.0f;
		std::vector<AvbPinLiteral> Literals;

		const AvbLiteral* FindLiteral(const std::string& pinId) const noexcept;
	};

	struct AvbLink
	{
		std::string FromNode;
		std::string FromPin;
		std::string ToNode;
		std::string ToPin;
		// A declared delayed-state edge is the only legal data-cycle break.
		bool Feedback = false;
	};

	struct AvbSend
	{
		std::string FromNode;
		std::string ToBus;
		AvbSendMode Mode = AvbSendMode::PostFX;
		float Gain = 1.0f;
	};

	struct AvbGraph
	{
		std::uint32_t Schema = 1;
		std::string Name;
		AvbChannelFormat Output = AvbChannelFormat::Stereo;
		std::vector<AvbGraphInput> Inputs;
		std::vector<AvbVariable> Variables;
		std::vector<AvbNode> Nodes;
		std::vector<AvbLink> Links;
		std::vector<AvbSend> Sends;

		// IDs are generated once and persisted with the graph. AddNode rejects
		// collisions instead of silently renaming links that may already refer to it.
		bool AddNode(AvbNode node, std::string* error = nullptr);
		AvbNode* FindNode(const std::string& id) noexcept;
		const AvbNode* FindNode(const std::string& id) const noexcept;
	};

	std::string GenerateStableId();
	const char* AvbPinTypeName(AvbPinType type) noexcept;
	bool AvbPinTypeFromName(const std::string& name, AvbPinType& out) noexcept;
	const char* AvbPinDirName(AvbPinDir dir) noexcept;
	bool AvbPinDirFromName(const std::string& name, AvbPinDir& out) noexcept;
	const char* AvbPinRateName(AvbPinRate rate) noexcept;
	bool AvbPinRateFromName(const std::string& name, AvbPinRate& out) noexcept;
	const char* AvbChannelFormatName(AvbChannelFormat format) noexcept;
	bool AvbChannelFormatFromName(const std::string& name, AvbChannelFormat& out) noexcept;
	const char* AvbSendModeName(AvbSendMode mode) noexcept;
	bool AvbSendModeFromName(const std::string& name, AvbSendMode& out) noexcept;
	bool AvbLiteralMatchesType(const AvbLiteral& literal, AvbPinType type) noexcept;
}
