// End-to-end check that a newly created project is immediately renderable:
// the shaders the renderers load by name must exist inside it.
//
// This is the whole point of seeding assets/shaders. A test that only
// checks "the directory exists" would pass while the engine still failed
// to find Renderer2D_Quad.glsl at runtime.

// DEFECT: dead include. Nothing in this file calls printf or any other
// <cstdio> facility -- the output below is all std::cout. Unlike
// test_policy.cpp, which genuinely needs it for its CHECK macro, this test
// has no macro that needs it.
#include <cstdio>
#include <filesystem>
// DEFECT: dead include. No std::ifstream or std::ofstream is used anywhere
// -- the test checks files by size with std::filesystem::file_size and never
// reads their contents, so there is nothing for <fstream> to provide.
#include <fstream>
// For std::cout, used throughout. This is the only output mechanism in the
// file.
#include <iostream>
// For the std::string holding the project name and Project's error text.
#include <string>
// DEFECT: probably dead. Nothing here declares a vector -- the shader list
// is a plain C array, and Project::Create is passed an empty brace-init
// list. std::vector is not named anywhere in the file.
#include <vector>

#include "Wasteland/Core/Log.h"
#include "Wasteland/Project/Project.h"

// So Project, Log and the settings API can be named unqualified. Broader
// than the module-level using-directive in test_policy.cpp, but this file
// touches only three names from the namespace.
using namespace Wasteland;

// Failure counter shared by the two phases below, so the final RESULT line
// can report both without either phase having a return path.
//
// No companion check counter, unlike test_policy.cpp -- the passes are
// counted locally in `copied` and printed per file instead.
static int g_Failures = 0;

// These are the exact paths Renderer2D.cpp, Renderer3D.cpp, DX12Nova.cpp
// and DX11Nova.h ask Project for. If any is missing, that backend draws
// nothing in a brand new project.
// The contract: exactly the shader paths the renderers ask Project for.
//
// This array IS the test. A missing entry is a project that builds and
// renders nothing, so the list has to be kept in step with the strings the
// renderers pass to GetAssetPath -- and nothing in the build enforces that.
// DEFECT: no compile-time count is checked against anything, so removing an
// entry silently weakens the test.
//
// Note it mixes GLSL and HLSL: the .glsl entries serve the OpenGL and legacy
// paths, the single .hlsl serves the NVRHI/DX11 line pass whose source in
// this tree is Renderer2D_Line.hlsl.
static const char *kRequiredShaders[] = {
	// Project-relative, not engine-relative -- these are resolved through
	// Project::Get().GetAssetPath below, which is the point: the test proves the
	// file landed inside the NEW project rather than merely existing in the
	// engine's own assets directory.
	"shaders/Renderer2D_Quad.glsl",
	"shaders/Renderer2D_Circle.glsl",
	"shaders/Renderer2D_Line.glsl",
	"shaders/Renderer3D_Basic.glsl",
	"shaders/Renderer3D_NovaRenderer.glsl",
	"shaders/Renderer3D_NovaRenderer.hlsl",
	"shaders/FlatColor.glsl",
};

// Takes the engine assets root rather than hardcoding it, so the test can
// run against a real install instead of a fixture.
//
// @param argc  Checked for the argument below.
// @param argv  argv[1] is the engine assets directory containing "shaders".
int main(int argc, char **argv)
{
	// Initialises logging even though this test writes nothing to the engine's
	// log -- Project::Create almost certainly does, and an uninitialised logger
	// would either crash or silently drop those messages.
	Wasteland::Log::Init();

	// argv[1] is the engine assets root to seed from, i.e. the directory
	// that contains "shaders". Passed in so the test can run against the
	// real editor install rather than a fixture.
	// Usage guard. Returns 2 rather than 0 or 1, the conventional distinct code
	// for 'invoked wrongly' -- so a harness can tell a misconfigured run from a
	// real failure.
	if (argc < 2)
	{
		std::cout << "usage: test_project_seeding <engine-assets-dir>\n";
		// Distinct from the failure code below, so a CI misconfiguration is not
		// reported as the test failing.
		return 2;
	}

	// Constructed from the raw argument. No validation of the string itself;
	// the directory check two lines below is the validation.
	const std::filesystem::path engineAssets = argv[1];
	// One error_code reused by every filesystem call below, which is the
	// standard non-throwing idiom.
	//
	// DEFECT: because it is shared and only ever assigned, a call that fails
	// mid-sequence leaves ec set, and the next successful call clears it -- so
	// the value read later does not necessarily describe the call being checked.
	// The `if (ec || bytes == 0)` test at line 86 is the one place this could
	// bite: it reads ec after file_size, which is correct, but only by accident
	// of ordering. A local error_code per call would make each check
	// self-contained.
	std::error_code ec;

	std::cout << "Engine assets: " << engineAssets.string() << "\n";
	// Treats a missing shaders directory as a SKIP rather than a FAIL.
	//
	// Defensible -- the test is about project SEEDING, and without an engine
	// assets tree there is nothing to seed from. But it is also a silent pass:
	// a mistyped path in CI would report SKIP and exit 0, indistinguishable from
	// success unless someone reads the text. The RESULT line does say SKIP, so
	// the information is there for a human; a harness that only checks the exit
	// code would not notice.
	if (!std::filesystem::is_directory(engineAssets / "shaders", ec))
	{
		std::cout << "RESULT: SKIP (no shaders directory at that path)\n";
		return 0;
	}

	// Points the engine at the real assets tree. Set BEFORE Create, because
	// seeding reads from it.
	//
	// Global process state, which is why the reset at line 119 is mandatory:
	// this test mutates a singleton and leaves the process in a valid state only
	// because it undoes that on the way out.
	Project::SetEngineAssetsRoot(engineAssets);

	// A fixed project name in the user's real projects directory, so repeated
	// runs collide with themselves -- which is why remove_all appears on both
	// sides of the test.
	const std::string name = "SeedSmokeTest";
	// The on-disk location, kept so the same directory can be deleted before and
	// after. Derived from the same helper Create() will use.
	const std::filesystem::path root = Project::GetUserProjectsDirectory() / name;
	// Pre-clean. Without it, a previous run's leftovers would satisfy the
	// existence checks below even if seeding copied nothing at all -- the test
	// would pass on a completely broken Create.
	//
	// ec is not checked: a failure here means the directory did not exist, which
	// is the expected case on a first run. A genuine permission problem would
	// be swallowed, and then the later Create would fail and report properly.
	std::filesystem::remove_all(root, ec);

	std::string error;
	// Creates the project, seeding assets from the engine root set above.
	//
	// @param name   Project name.
	// @param {}     Second parameter, passed empty -- most likely the list of
	//                extra content templates. Not named here, so this comment
	//                can only say what is visible: it is deliberately empty.
	// @param error  Receives a human-readable reason on failure.
	if (!Project::Create(name, {}, &error))
	{
		std::cout << "RESULT: FAIL -- Create(): " << error << "\n";
		return 1;
	}

	// The check that matters: can the renderers find their shaders?
	// The existence pass. Every entry is checked and every failure recorded --
	// it does not stop at the first, so one run reports every missing shader
	// rather than making the developer fix them one run at a time.
	for (const char *rel : kRequiredShaders)
	{
		// Resolution through the live Project object, which is the load-bearing
		// line of the whole test: it proves the path the RENDERERS compute reaches a
		// real file. Checking the filesystem directly against a hand-built path
		// would test the filesystem, not the engine's lookup.
		const std::filesystem::path path = Project::Get().GetAssetPath(rel);
		if (!std::filesystem::is_regular_file(path, ec))
		{
			++g_Failures;
			// Prints the fully resolved path, not just the relative one. That detail is
			// what makes a failure diagnosable -- the expected path shows immediately
			// whether Project resolved it somewhere unexpected.
			std::cout << "  MISSING: " << rel << "  ->  " << path.string() << "\n";
		}
		else
		{
			// Size check, and the reason <fstream> is not needed: the test never opens
			// the files, it only measures them.
			const auto bytes = std::filesystem::file_size(path, ec);
			std::cout << "  ok: " << rel << " (" << bytes << " bytes)\n";
			// A zero-byte shader compiles to nothing and fails at pipeline
			// creation with a far more confusing error, so catch it here.
			// Both conditions matter. `ec` catches a size query that failed, which would
			// otherwise leave `bytes` at 0 and be indistinguishable from an empty file.
			//
			// DEFECT: `ec` at this point was last assigned by the is_regular_file check
			// on line 75, then by file_size here -- but the shared ec means a failure in
			// either call is invisible to this test's verdict beyond the byte count. A
			// fresh error_code per call would attribute the error precisely.
			if (ec || bytes == 0)
			{
				++g_Failures;
				std::cout << "  EMPTY: " << rel << "\n";
			}
		}
	}

	// Count what actually landed, so a partial copy is visible.
	int copied = 0;
	// Counts what actually landed, walking the seeded shaders tree.
	//
	// Informational only -- `copied` is printed and never compared against an
	// expected count or against the length of kRequiredShaders. So a partial
	// copy is visible in the log, as the comment says, but a partial copy that
	// still contains every required file is NOT a failure. That is arguably the
	// right call (extra files are fine), but it does mean the count cannot fail
	// the test.
	for (auto it = std::filesystem::recursive_directory_iterator(Project::Get().GetAssetPath("shaders"), ec);
		 // DEFECT: it.increment(ec) does not check ec. If an increment fails -- an I/O
		 // error, a directory removed while iterating -- the standard leaves the
		 // iterator unchanged and the loop spins forever. Every other filesystem
		 // call in this file is error-checked; this one, in the loop condition, is
		 // not. Checking ec in the condition and breaking would close it.
		 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		if (it->is_regular_file(ec))
			++copied;
	std::cout << "Total shader files in project: " << copied << "\n";

	// Save must still work, and must not have disturbed the shaders.
	// Mutates a real setting before Save(), so the save is not a no-op. If
	// Save() short-circuits on an unchanged settings object, this line is what
	// keeps the save path exercised.
	Project::Get().GetSettings().QualityPreset = "High";
	// Save must still work after seeding -- the check that a seeded project is
	// not read-only or half-initialised.
	if (!Project::Get().Save())
	{
		++g_Failures;
		std::cout << "  Save() failed after creating a seeded project\n";
	}
	// The same list re-checked after Save().
	//
	// This second pass is the one that catches a save that clobbers the assets
	// tree -- for instance a settings serialiser that writes the project as a
	// fresh directory, or a save that truncates the project file. Checking only
	// before the save would miss that entirely.
	for (const char *rel : kRequiredShaders)
	{
		if (!std::filesystem::is_regular_file(Project::Get().GetAssetPath(rel), ec))
		{
			++g_Failures;
			std::cout << "  LOST AFTER SAVE: " << rel << "\n";
		}
	}

	// Closes the project, flushing the singleton. Must precede the
	// SetEngineAssetsRoot({}) below.
	//
	// DEFECT: neither this line nor the three after it run if the test returns
	// early at line 68. A failed Create leaves the engine assets root pointing
	// at the test's directory and the project open -- harmless in a standalone
	// process that exits immediately, but it would matter in a harness running
	// several tests in one process. An RAII guard would fix it.
	Project::Get().Close();
	Project::SetEngineAssetsRoot({});
	// Post-clean, so the test does not leave a project behind in the user's real
	// projects directory. Same unchecked ec as the pre-clean.
	std::filesystem::remove_all(root, ec);

	// The single verdict line. Note it cannot report SKIP -- the SKIP path
	// returns at line 55 before reaching here, so a skip is distinguishable by
	// the absence of this line as well as by its own text.
	std::cout << "\nRESULT: " << (g_Failures == 0 ? "PASS" : "FAIL") << "\n";
	// 0 or 1 only; 2 was reserved for the usage error, so the three outcomes
	// stay distinguishable by exit code alone -- except the SKIP, which shares
	// 0 with a genuine pass.
	return g_Failures == 0 ? 0 : 1;
}
