#pragma once

#include <xhash>
#include <spdlog/fmt/fmt.h>

namespace Wasteland
{
	class UUID
	{
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID &) = default;

		operator uint64_t() const { return m_UUID; }
		bool operator==(const UUID &other) const { return m_UUID == other.m_UUID; }

	private:
		uint64_t m_UUID;
	};
}

namespace fmt
{
	template <>
	struct formatter<Wasteland::UUID> : formatter<std::string_view>
	{
		auto format(const Wasteland::UUID &uuid, format_context &ctx) const
		{
			return formatter<std::string_view>::format(std::to_string((uint64_t)uuid), ctx);
		}
	};
}

namespace std
{
	template <>
	struct hash<Wasteland::UUID>
	{
		std::size_t operator()(const Wasteland::UUID &uuid) const
		{
			return hash<uint64_t>()((uint64_t)uuid);
		}
	};
}