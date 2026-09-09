#include "wlpch.h"
#include "Log.h"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"

namespace Wasteland {

	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");
		s_CoreLogger = spdlog::stdout_color_mt("WASTELAND");
		s_CoreLogger->set_level(spdlog::level::trace);

		s_ClientLogger = spdlog::stdout_color_mt("APP");
		s_ClientLogger->set_level(spdlog::level::trace);

		// Mirror all log output to Wasteland.log in the working directory so
		// crashes during renderer switches can be diagnosed after the fact.
		try
		{
			auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Wasteland.log", true);
			fileSink->set_pattern("[%T] %n: %v");
			s_CoreLogger->sinks().push_back(fileSink);
			s_ClientLogger->sinks().push_back(fileSink);
			spdlog::flush_on(spdlog::level::warn);
		}
		catch (...)
		{
		}
	}

}