// EntryPoint.h -- the client executable's main().
//
// This header contains main(). It is compiled into exactly one translation
// unit of the client (Sandbox, DemonCore-Editor), which is what lets the engine
// library stay ignorant of any concrete Application subclass. The client's
// CreateApplication() is declared extern here and defined in the client.
//
// Windows-only: the whole body sits inside a WL_PLATFORM_WINDOWS guard, so a
// Linux or macOS build of a client gets no main() from the engine and must
// supply its own.

#pragma once

#include <filesystem>
#include <string>

#include <Wasteland/Project/Project.h>

#ifdef WL_PLATFORM_WINDOWS

// Implemented by the client, not the engine. Defined in SandboxApp.cpp and
// DemonCoreEditorApp.cpp respectively.
//
// Declared at namespace scope with the qualified name in one go, which is legal
// C++ but unusual -- the equivalent would be to open `namespace Wasteland` and
// write the declaration unqualified.
//
// @returns A newly constructed Application. Ownership passes to main(), which
//          deletes it after Run() returns.
extern Wasteland::Application *Wasteland::CreateApplication();

namespace
{
	// Extracts the project path from the command line. Accepts both
	// "--project <path>" and "--project=<path>".
	//
	// argv/argc are passed in rather than read from globals so this stays
	// testable and does not shadow main()'s own parameters.
	//
	// Returns an empty string when no project argument is present, which
	// Project::Bootstrap treats as "choose a default" rather than as an error.
	//
	// @param argc  Argument count, from main.
	// @param argv  Argument vector, from main.
	// @returns The project path, or "" if none was given.
	inline std::string ParseProjectArgument(int argc, char **argv)
	{
		// The two accepted `--project=` spellings. A separate list rather than
		// two inline comparisons so adding a third alias is a one-line change.
		//
		// static const: constructed once, and const so the table cannot be
		// mutated at runtime.
		static const char *kEqualsFlags[] = {"--project=", "-project="};

		// Start at 1: argv[0] is the executable path, never a project argument.
		for (int i = 1; i < argc; ++i)
		{
			// Defensive read. argv[i] is expected non-null, but the test costs
			// nothing and the alternative is a crash in argument parsing -- which
			// would be a confusing way for a client to fail to start.
			// argv[i][0] additionally rejects an empty string, which would
			// otherwise match no prefix and fall through harmlessly anyway.
			const std::string arg = (argv[i] && argv[i][0]) ? argv[i] : "";
			// Nothing to match against.
			if (arg.empty())
				continue;

			// Spaced-free form: --project=<path> or -project=<path>.
			for (const char *flag : kEqualsFlags)
			{
				// Copied to a std::string so prefix.size() is well-defined; the
				// raw char* would need a strlen on every iteration.
				const std::string prefix = flag;
				// compare with n = prefix.size() compares only the first
				// prefix.size() characters, returning 0 only on a full prefix
				// match. Equivalent to starts_with, which C++17 does not have.
				if (arg.compare(0, prefix.size(), prefix) == 0)
					// Everything after the prefix is the value.
					return arg.substr(prefix.size());
			}

			// Spaced form: --project <path> or -project <path>.
			if (arg == "--project" || arg == "-project")
			{
				// The value is the next argument. The i + 1 < argc bound check
				// prevents reading past the end of argv, and the null test covers
				// an explicitly empty trailing argument. If either fails the
				// loop simply continues -- a trailing bare "--project" with no
				// value is ignored rather than treated as an error.
				if (i + 1 < argc && argv[i + 1])
					return argv[i + 1];
			}
		}

		// No project argument found: caller falls back to a default.
		return "";
	}
}

// The client's entry point.
//
// Ordering below is deliberate and load-bearing; see the comments at each step.
//
// @param argc  Argument count from the OS.
// @param argv  Argument vector from the OS, including argv[0] as the exe path.
// @returns 0 on normal exit. Implicit: main falls off the end after deleting
//          the application, and there is no non-zero exit path here.
int main(int argc, char **argv)
{
	// Logging comes up before the project so Project::Bootstrap can report
	// which project it selected -- and why, when it falls back.
	//
	// First statement in main for that reason. Anything that logs before this
	// would go through Log's lazy accessors, which works but skips the explicit
	// ordering this guarantees.
	Wasteland::Log::Init();

	// Absolute path of the running executable, used by Bootstrap to locate a
	// project relative to the binary.
	//
	// absolute() is what makes it absolute; the ternary guards a missing
	// argv[0], falling back to the working directory. Note this is the path to
	// the *executable*, not to the source or the project -- on Windows it ends
	// in .exe, which Project::Bootstrap must account for.
	const std::filesystem::path exePath =
		std::filesystem::absolute(argc > 0 && argv[0] ? argv[0] : ".");

	// Selects and initializes the project, from the --project argument if one
	// was given and otherwise by fallback search.
	//
	// Must precede CreateApplication: an Application subclass reads project
	// settings during construction, so the project has to exist first.
	Wasteland::Project::Bootstrap(ParseProjectArgument(argc, argv), exePath);

	// Profiling scope for construction only, written to its own file so the
	// startup cost can be read without the runtime trace in the way.
	WL_PROFILE_BEGIN_SESSION("Startup", "WastelandProfile-Startup.json");

	// Calls into client code to build the Application. Returns a raw pointer
	// because that is the contract CreateApplication is declared with.
	//
	// No null check: a client returning null would crash on the app->Run()
	// below, which is an acceptable and immediate failure for a contract
	// violation this basic.
	auto app = Wasteland::CreateApplication();

	// Ends the startup profile before the frame loop begins.
	WL_PROFILE_END_SESSION();

	// Separate profile for the frame loop, so a long run does not swamp the
	// startup trace.
	WL_PROFILE_BEGIN_SESSION("Runtime", "WastelandProfile-Runtime.json");

	// Runs the frame loop. Blocks here for the entire session, until the user
	// closes the window or something calls Application::Close().
	app->Run();

	WL_PROFILE_END_SESSION();

	// Profile teardown separately from runtime, because destruction is where
	// shutdown-order bugs surface -- a layer releasing a resource after its
	// context is gone shows up here and nowhere else.
	WL_PROFILE_BEGIN_SESSION("Shutdown", "WastelandProfile-Shutdown.json");

	// Destroys the Application, which tears down its layers and its window.
	//
	// Explicit delete rather than a smart pointer: the lifetime here is
	// deliberately bracketed by the two profile scopes, and uniqueness_ptr
	// would end it at the closing brace instead, outside the measured region.
	delete app;

	WL_PROFILE_END_SESSION();
}

#endif