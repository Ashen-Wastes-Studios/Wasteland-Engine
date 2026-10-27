// Log.h -- the engine's logging facade and the macros that call it.
//
// Two loggers, split by origin rather than by severity:
//
//   Core   (WL_CORE_*)  engine-internal messages
//   Client (WL_*)       game and application messages
//
// Both go to the console and to Wasteland.log, so a session's output can be
// read as one interleaved stream while still being attributable to its source.

#pragma once

#include "wlpch.h"

#include "Core.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

namespace Wasteland
{

	// Static facade over the engine's two spdlog loggers.
	//
	// No instances: every member is static, so this is never constructed. It is
	// a namespace-like holder that happens to be a class so the members can be
	// private.
	//
	// Callers should use the WL_* macros at the bottom of this file rather than
	// touching the accessors directly.
	class WL_API Log
	{
	public:
		// Builds both loggers and attaches the file sink. Defined in Log.cpp.
		//
		// Optional to call: the accessors below invoke it lazily. It is called
		// explicitly from EntryPoint.h so that logging is up before anything
		// else, which guarantees the sinks exist before the first message
		// rather than on first use.
		//
		// Idempotent -- calling it twice is safe, and is exactly what the
		// explicit call plus a lazy first access amounts to.
		static void Init();

		// The accessors NEVER return a null logger. They lazily call Init()
		// when it has not run yet.
		//
		// The loggers are plain static shared_ptrs with no constructor-time
		// initialisation, so anything logging before Log::Init() -- a
		// headless Python process importing the module, a unit test, any
		// early static init -- dereferenced a null shared_ptr and took the
		// process down.
		//
		// This bit SceneSerializer::Deserialize, which calls
		// WL_CORE_TRACE("Deserializing scene '{0}'") on its second line. The
		// editor initialises logging during Application startup so it never
		// hit the null, and every headless scene load died with a segfault
		// that looked like a YAML-parsing bug. The lazy Init makes every
		// entry point safe instead of relying on "remember to call
		// Log::Init() before touching the engine".
		//
		// Returns a reference to the static member, so after the first call
		// this is just a null check.
		//
		// Returned by reference so the caller shares the logger rather than
		// bumping a refcount on every log line -- the whole point of which
		// would be defeated by a copy per message.
		static std::shared_ptr<spdlog::logger> &GetCoreLogger()
		{
			if (!s_CoreLogger)
				Init();
			return s_CoreLogger;
		}

		// The client/game logger, lazily initialized exactly as above.
		//
		// Note this checks s_ClientLogger rather than reusing GetCoreLogger's
		// check. The two are set together by Init(), so the distinction only
		// matters if Init() ever threw between constructing them -- in which
		// case this accessor would correctly retry where a shared check would
		// return null.
		static std::shared_ptr<spdlog::logger> &GetClientLogger()
		{
			if (!s_ClientLogger)
				Init();
			return s_ClientLogger;
		}

		// True once a usable logger exists, so a caller can skip building an
		// expensive message for a log call that would go nowhere.
		//
		// Necessary because the WL_* macros evaluate their arguments before
		// spdlog ever sees them -- a level check inside spdlog cannot stop
		// WL_CORE_TRACE("obj at {0}", obj.ToString()) from running ToString().
		// Guarding such a call with this is the only way to avoid the cost.
		//
		// Checks only s_CoreLogger. Since Init() constructs both together, that
		// is sufficient in practice; it would not be if the two could ever
		// diverge.
		static bool IsInitialized() { return s_CoreLogger != nullptr; }

	private:
		// The engine-internal logger, created by Init(). Null until then, which
		// is the state the lazy accessors above exist to handle.
		//
		// Defined in Log.cpp. shared_ptr rather than a bare pointer because
		// spdlog loggers are refcounted and the accessors hand the shared
		// ownership out by reference.
		static std::shared_ptr<spdlog::logger> s_CoreLogger;

		// The client/game logger. See s_CoreLogger.
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};

}

// Core log macros
//
// All ten macros forward straight to an spdlog level method on the appropriate
// logger. Three consequences worth knowing before using them:
//
//   * Arguments are ALWAYS evaluated. The macro expands to a function call, so
//     every argument expression runs before spdlog can discard the message on a
//     level check. Guard genuinely expensive arguments with Log::IsInitialized()
//     or compute them into a local first.
//
//   * Format strings use fmt's brace syntax, and the first string argument is
//     the format. Both spdlog and C-style positional forms appear in this
//     codebase -- "{0}" and "{}" are interchangeable -- but mixing them in one
//     call is not.
//
//   * At least one argument is required: there is no WL_ERROR() with no message,
//     because the format string is mandatory. `WL_ERROR("plain message")` is
//     correct and does not need a placeholder.
//
// Use these for engine-internal diagnostics.
#define WL_CORE_TRACE(...) ::Wasteland::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define WL_CORE_INFO(...) ::Wasteland::Log::GetCoreLogger()->info(__VA_ARGS__)
#define WL_CORE_WARN(...) ::Wasteland::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define WL_CORE_ERROR(...) ::Wasteland::Log::GetCoreLogger()->error(__VA_ARGS__)
#define WL_CORE_CRITICAL(...) ::Wasteland::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client log macros
//
// Identical in behaviour, routed to the client logger so the source of a line
// is visible in the output. Use these in game and application code; the
// WL_CORE_* forms above are for the engine itself.
//
// Note WL_ERROR is also used by WL_ASSERT in Core.h, which is why that macro
// works without Core.h including this header -- the expansion is resolved at
// the call site, where this header has been included.
#define WL_TRACE(...) ::Wasteland::Log::GetClientLogger()->trace(__VA_ARGS__)
#define WL_INFO(...) ::Wasteland::Log::GetClientLogger()->info(__VA_ARGS__)
#define WL_WARN(...) ::Wasteland::Log::GetClientLogger()->warn(__VA_ARGS__)
#define WL_ERROR(...) ::Wasteland::Log::GetClientLogger()->error(__VA_ARGS__)
#define WL_CRITICAL(...) ::Wasteland::Log::GetClientLogger()->critical(__VA_ARGS__)

// One-shot log macros
//
// Emit a message the FIRST time the tag is seen, then never again for the
// lifetime of the process. For diagnostics that sit on a per-frame call site --
// a UI panel's draw function, a render pass's setup -- where the useful thing
// is the fact that it happened at all, and repeating it would bury the log.
//
// Not an spdlog feature: spdlog has no "log once", and adding the guard
// outside the macro would mean either an if-statement per call site (which
// the macro's own braces would then silently attach to) or a per-file static
// bool.
//
// The tag is arbitrary and never printed; it only needs to be unique per
// distinct message. It is pasted into a variable name, so it must be a valid
// identifier and must not contain parentheses or commas.
//
// The guard is a function-local static, so the SAME tag used at two call sites
// inside one function shares one flag and the second message never fires. Use a
// distinct tag per message.
//
// Args are evaluated only when the message is actually emitted, so an
// expensive argument is cheap on the frames that skip.
#define WL_LOG_ONCE(tag, ...) \
    do { \
        static bool s_WlLoggedOnce_##tag = false; \
        if (!s_WlLoggedOnce_##tag) \
        { \
            s_WlLoggedOnce_##tag = true; \
            WL_INFO(__VA_ARGS__); \
        } \
    } while (0)