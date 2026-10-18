#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Graph.h"

namespace Wasteland::Avb
{
	struct AvbPinDef
	{
		std::string Id;
		AvbPinType Type = AvbPinType::Float;
		AvbPinDir Direction = AvbPinDir::Input;
		bool Required = false;
		AvbPinRate Rate = AvbPinRate::Block;
		std::optional<AvbLiteral> DefaultValue;
		std::vector<std::string> AllowedValues;
	};

	struct AvbNodeDef
	{
		std::string TypeId;
		std::string Category;
		std::uint32_t Version = 1;
		std::vector<AvbPinDef> Pins;
		std::string AliasOf;
		std::function<AvbNode(const std::string&)> Factory;

		const AvbPinDef* FindPin(const std::string& pinId) const noexcept;
	};

	// The AVB registry is deliberately independent of VisualBlocks. Aliases
	// resolve for signatures and factories while their original file ID survives.
	class AvbNodeRegistry
	{
	public:
		static AvbNodeRegistry& Get();
		const AvbNodeDef* Find(const std::string& typeId) const noexcept;
		const AvbNodeDef* FindExact(const std::string& typeId) const noexcept;
		const std::vector<AvbNodeDef>& All() const noexcept { return m_Defs; }
		bool IsRegistered(const std::string& typeId) const noexcept;
		bool Register(AvbNodeDef def, std::string* error = nullptr);
		AvbNode CreateNode(const std::string& typeId, const std::string& id = {}) const;

	private:
		AvbNodeRegistry();
		void RegisterBuiltins();
		std::vector<AvbNodeDef> m_Defs;
	};
}
