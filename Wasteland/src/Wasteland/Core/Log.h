#pragma once

#include "wlpch.h"

#include "Core.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

namespace Wasteland
{

	class WL_API Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger> &GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger> &GetClientLogger() { return s_ClientLogger; }

	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};

}

// Core log macros
#define WL_CORE_TRACE(...) ::Wasteland::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define WL_CORE_INFO(...) ::Wasteland::Log::GetCoreLogger()->info(__VA_ARGS__)
#define WL_CORE_WARN(...) ::Wasteland::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define WL_CORE_ERROR(...) ::Wasteland::Log::GetCoreLogger()->error(__VA_ARGS__)
#define WL_CORE_CRITICAL(...) ::Wasteland::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client log macros
#define WL_TRACE(...) ::Wasteland::Log::GetClientLogger()->trace(__VA_ARGS__)
#define WL_INFO(...) ::Wasteland::Log::GetClientLogger()->info(__VA_ARGS__)
#define WL_WARN(...) ::Wasteland::Log::GetClientLogger()->warn(__VA_ARGS__)
#define WL_ERROR(...) ::Wasteland::Log::GetClientLogger()->error(__VA_ARGS__)
#define WL_CRITICAL(...) ::Wasteland::Log::GetClientLogger()->critical(__VA_ARGS__)