// Log.cpp -- one-time construction of the engine's two spdlog loggers.
//
// See Log.h for the accessors and the reasoning behind lazy initialization.
// This file is where the loggers are actually built.

#include "wlpch.h"
#include "Log.h"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"

namespace Wasteland {

	// The two logger instances, defined here rather than in the header so
	// there is exactly one of each per process.
	//
	// Default-constructed to null shared_ptrs. That null state is the whole
	// reason GetCoreLogger()/GetClientLogger() lazily call Init() -- see the
	// comment block on those accessors in Log.h.
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	// Builds the core and client loggers and attaches the file sink.
	//
	// Safe to call more than once; see the idempotence note below.
	void Log::Init()
	{
		// Idempotent. GetCoreLogger()/GetClientLogger() call this lazily on
		// first use, and Application startup also calls it explicitly, so
		// without this guard the second call would create a second logger
		// with the same name -- spdlog throws on a duplicate registration,
		// turning a benign "log before init" into a hard failure.
		//
		// The guard requires BOTH loggers to be present. If a previous call
		// had thrown partway through -- between creating the core logger and
		// the client one -- this condition would be false and Init would
		// re-enter, and the already-registered core logger's name would then
		// collide. The partial-failure window is narrow, but it is real.
		if (s_CoreLogger && s_ClientLogger)
			return;

		// Global format for every logger created from here on:
		//   %^ ... %$   colour start/end markers around the timestamp+name, so
		//                only that part is coloured and the message is not.
		//   %T           24-hour timestamp.
		//   %n           logger name (WASTELAND or APP).
		//   %v           the formatted message body.
		spdlog::set_pattern("%^[%T] %n: %v%$");

		// Console sink for engine-internal logging. Named "WASTELAND" so log
		// lines are attributable to the engine rather than the game.
		s_CoreLogger = spdlog::stdout_color_mt("WASTELAND");

		// Trace level, so WL_CORE_TRACE and WL_TRACE actually emit. Note this
		// is unconditional -- there is no build-config switch that raises it,
		// so a debug build pays the formatting cost for every trace call.
		s_CoreLogger->set_level(spdlog::level::trace);

		// Console sink for game/client logging, named "APP" to distinguish it
		// from engine lines in the same terminal.
		s_ClientLogger = spdlog::stdout_color_mt("APP");

		// Trace level, as above.
		s_ClientLogger->set_level(spdlog::level::trace);

		// Mirror all log output to Wasteland.log in the working directory so
		// crashes during renderer switches can be diagnosed after the fact.
		//
		// Wrapped in try/catch because the file is best-effort: a read-only
		// working directory, a missing permission, or a locked file must not
		// stop the engine from starting. Console logging is the primary
		// channel; the file is a convenience.
		try
		{
			// basic_file_sink_mt is the thread-safe sink variant. The second
			// argument (true) truncates rather than appending, so each run
			// starts a fresh log instead of accumulating across sessions.
			//
			// The sink is created once and shared by both loggers, so console
			// and file output stay in step and the file is opened only once.
			auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Wasteland.log", true);

			// File format drops the %^/%$ colour markers, which are meaningless
			// in a plain text file and would otherwise be written literally.
			fileSink->set_pattern("[%T] %n: %v");

			// push_back appends to each logger's existing sink list, so console
			// output is kept and the file becomes a second destination.
			s_CoreLogger->sinks().push_back(fileSink);
			s_ClientLogger->sinks().push_back(fileSink);

			// Flush at warn and above. Because this call sits inside the try
			// block, a failure to create the file sink also skips it -- in that
			// case logging is still buffered until the level is reached or the
			// loggers are destroyed, which can lose the final messages before a
			// crash. Set unconditionally (not per-sink) so it applies to the
			// console sinks too.
			spdlog::flush_on(spdlog::level::warn);
		}
		catch (...)
		{
			// Deliberately empty and deliberately silent: no logger exists yet
			// to report the failure through, and throwing here would turn a
			// missing log file into a failed startup. The cost is that a
			// permissions problem on Wasteland.log is completely invisible.
		}
	}

}