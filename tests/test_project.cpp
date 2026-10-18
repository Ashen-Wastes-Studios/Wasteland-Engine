// Standalone functional test for Wasteland::Project.
//
// The engine's Project class is only meaningfully verified by exercising
// its filesystem behaviour: does Create() lay down the right tree, does
// Open() round-trip settings, does MakeProjectRelative() actually reject
// paths that escape the project. MSBuild proves it compiles; this proves
// it works.
//
// Built by scripts/test_project.sh against the real Project.cpp.

// <cassert> is DEFECT: included but never used. The file has zero assert()
// calls -- the CHECK macros below are the whole assertion mechanism, which
// is what lets it report every failure in a run instead of aborting on the
// first. A leftover from when this was written as a bare assert harness.
#include <cassert>
#include <algorithm>
// DEFECT (unused include): <cstdio> is included but no printf/sprintf/fputs
// call appears anywhere in the file. Output goes through std::cout only.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// The engine's log header, needed for Wasteland::Log::Init() in main().
// Project.h pulls in <filesystem>, which is why the remove_all/is_directory
// calls below compile without this file including it itself -- an implicit
// dependency worth knowing if the include list is ever trimmed.
#include "Wasteland/Core/Log.h"
#include "Wasteland/Project/Project.h"

// `using namespace Wasteland` so the tests read as Project:: rather than
// Wasteland::Project::. Scoped to this one file.
using namespace Wasteland;

// The recents file and every project-relative reference use forward slashes
// so they are portable across platforms. Mirrors the engine's own convention.
// Test-local helper. The recents file and every project-relative reference
// use forward slashes so they are portable across platforms, which mirrors
// the engine's own convention.
static std::string ToForwardSlashes(std::string s)
{
	// std::replace needs <algorithm> (line 12). In-place on a copy -- `s` is
	// taken BY VALUE, so the caller's string is untouched.
	std::replace(s.begin(), s.end(), '\\', '/');
	return s;
}

// File-scope counters rather than a struct or a fixture object: this is a
// plain main() with no test framework, so the CHECK macros need global
// state to reach. g_Checks counts assertions attempted, g_Failures counts
// those that failed -- keeping them separate lets the summary distinguish
// 'nothing ran' from 'everything passed'.
static int g_Failures = 0;
static int g_Checks = 0;

// CHECK(cond, what): increments the attempt count, then records and PRINTS a
// failure without aborting. Non-fatal by design -- a run that stops at the
// first failure hides every later one.
// Wrapped in do/while(0) rather than a bare `if` so the macro is a single
// statement and is safe at the tail of an unbraced if/else.
#define CHECK(cond, what)                                                        \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		if (!(cond))                                                             \
		{                                                                        \
			++g_Failures;                                                        \
			std::cout << "  FAIL: " << (what) << "\n";                            \
		}                                                                        \
	} while (0)

// CHECK_EQ: same accounting, plus it captures both sides into locals FIRST.
// That is what lets each argument be evaluated exactly once -- an argument
// like GetSettings().RendererAPI is a call, and evaluating it twice (once to
// compare, once to print) could observe a different value or cost twice.
// Both locals are also required to be streamable, which is why every call
// below passes .string() rather than a std::filesystem::path.
#define CHECK_EQ(actual, expected, what)                                         \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		const auto a_ = (actual);                                                \
		const auto e_ = (expected);                                              \
		if (!(a_ == e_))                                                         \
		{                                                                        \
			++g_Failures;                                                        \
			std::cout << "  FAIL: " << (what) << "\n    got:      " << a_        \
					  << "\n    expected: " << e_ << "\n";                         \
		}                                                                        \
	} while (0)

// Section prints a bracketed banner. Purely cosmetic -- it is what makes a
// failing run readable, since the FAIL lines carry no location information
// of their own.
static void Section(const char *name)
{
	// Leading newline so each banner separates from the previous output rather
	// than running into it.
	std::cout << "\n[" << name << "]\n";
}

// --------------------------------------------------------------------
// SanitizeName is a pure function, so this test needs no project, no disk
// and no setup -- which is why it runs first and cannot fail for
// environmental reasons.
static void TestSanitizeName()
{
	Section("SanitizeName");

	// A name with nothing to strip must come back byte-identical. If it did
	// not, every existing project folder would silently change name on the next
	// Create().
	CHECK_EQ(Project::SanitizeName("My Game"), "My Game", "plain name passes through");
	// Leading AND trailing whitespace, with a doubled interior run. Pins all
	// three behaviours at once: trim both ends, collapse interior runs.
	CHECK_EQ(Project::SanitizeName("  Spaced  Out  "), "Spaced Out", "trims and collapses whitespace");
	// Path separators and a colon and a wildcard, all stripped rather than
	// substituted -- hence the output has no underscores, which is what makes
	// the next line's reserved-name SUFFIX check meaningful.
	CHECK_EQ(Project::SanitizeName("Bad/Name:Here*"), "BadNameHere", "strips characters illegal in a path component");
	// A backslash is a separator on Windows, so it must be stripped on every
	// platform for the result to be portable.
	CHECK_EQ(Project::SanitizeName("with\\backslash"), "withbackslash", "strips backslashes too");
	// The empty case: something must be returned, because an empty project name
	// would resolve to the projects directory itself.
	CHECK_EQ(Project::SanitizeName(""), "Untitled", "empty becomes Untitled");
	// A name made ENTIRELY of illegal characters also collapses to the default.
	// This is the case that proves the fallback runs after stripping, not
	// before -- otherwise it would return the empty string, not "Untitled".
	CHECK_EQ(Project::SanitizeName("///"), "Untitled", "all-illegal becomes Untitled");
	// CON is a reserved DOS device name. Legal as a string, unusable as a
	// directory, so it gains an underscore rather than being rejected -- the
	// user still gets a project, just not one that collides with the device.
	CHECK_EQ(Project::SanitizeName("CON"), "CON_", "Windows reserved device name gets a suffix");
	// com1, lower case. Paired with the line above this is the case-insensitivity
	// check: the engine lowercases before comparing to its reserved list.
	CHECK_EQ(Project::SanitizeName("com1"), "com1_", "reserved name check is case-insensitive");
	// LPT9 -- the top of the LPT range, so the check covers all nine LPT slots
	// rather than just the first.
	CHECK_EQ(Project::SanitizeName("LPT9"), "LPT9_", "all LPT slots covered");
	// The negative case, and the one a sloppy prefix check would break: 'CON'
	// is reserved but a name merely STARTING with those letters is not. Without
	// this line an implementation that matched on a prefix would rewrite
	// "Console" too.
	CHECK_EQ(Project::SanitizeName("Console"), "Console", "CON is reserved but Console is not");
	// Wildcards. Another category from the separator case above, confirming the
	// strip set is not just the path-illegal characters.
	CHECK_EQ(Project::SanitizeName("A*B?C"), "ABC", "wildcards stripped");
}

// --------------------------------------------------------------------
// This and every later test operate on the REAL user profile:
// Documents/Wasteland Projects. Nothing here is sandboxed or redirected,
// so the tests write to (and delete from) the developer's actual documents
// folder. The cleanup at the end of each test is load-bearing, not hygiene.
static void TestCreateAndOpen()
{
	Section("Create / Open / Save");

	// A name unique to this run so a stale directory cannot mask a failure.
	// A fixed name, not a generated one. The comment below says 'unique to this
	// run' -- DEFECT: it is a hard-coded constant, so uniqueness comes only from
	// the remove_all that follows. A second concurrent run would collide.
	const std::string name = "ProjectSystemTest";

	std::string error;
	// Start from no active project: Create() below operates on the singleton.
	Project::Get().Close();
	// Cleared so the recents assertions later start from a known-empty list
	// rather than from whatever the last session left behind.
	Project::ClearRecentProjects();

	// Start from a clean slate. A previous run that aborted before its
	// cleanup would otherwise leave a directory that makes Create() fail for
	// a reason unrelated to what is under test.
	// Best-effort removal of any previous run's directory. Wrapped in its own
	// scope with an error_code so a failure here (file locked, permission
	// denied) cannot throw -- std::filesystem's throwing overloads would
	// escape main and abort the run.
	{
		std::error_code ec;
		// The throwing-vs-error_code distinction matters throughout this file: the
		// ec-taking overloads return false and set ec instead of throwing, so a
		// locked file shows up as a CHECK failure rather than a crash.
		std::filesystem::remove_all(Project::GetUserProjectsDirectory() / name, ec);
	}

	// Empty seed directory: create the standard layout with nothing copied in.
	// The seed variants are exercised separately in TestSeedCopy below.
	if (!Project::Create(name, {}, &error))
	{
		// A failed Create is a hard error, not a CHECK: there is no project to make
		// further assertions about, so counting it as one failure and returning is
		// the only useful action.
		++g_Failures;
		std::cout << "  FAIL: Create() returned false: " << error << "\n";
		return;
	}

	// A reference to the singleton, held for the rest of the test. Valid
	// because Project::Get() returns a reference to a long-lived object.
	Project &project = Project::Get();
	CHECK(project.IsOpen(), "Create() leaves the project open");
	CHECK_EQ(project.GetName(), name, "name round-trips through Create");

	// The project must live under the user's Documents folder.
	// The expected root is computed from the SAME accessor the engine uses,
	// rather than hard-coded. That keeps the test meaningful on a machine where
	// Documents is redirected (OneDrive) and on Linux/macOS, where the answer
	// differs.
	const std::filesystem::path documents = Project::GetUserDocumentsDirectory();
	const std::filesystem::path expectedRoot = documents / "Wasteland Projects" / name;
	CHECK_EQ(project.GetRoot().string(), expectedRoot.string(), "root is Documents/Wasteland Projects/<name>");
	// Compared as .string() because CHECK_EQ streams both sides; std::filesystem
	// has no operator<<.
	CHECK_EQ(project.GetUserProjectsDirectory().string(), (documents / "Wasteland Projects").string(),
			 "GetUserProjectsDirectory sits under Documents");

	// Tree layout.
	// Tree layout. Each subdirectory is checked individually rather than as a
	// list, so a failure names the specific directory that is missing.
	CHECK(std::filesystem::is_directory(project.GetAssetRoot()), "assets/ exists");
	// The standard layout as a table, so the loop can name each one. 'ai' is
	// included, which is the newest addition and therefore the one most likely
	// to be missing from a stale Create().
	const char *subdirs[] = {"scenes", "scripts", "shaders", "textures", "models", "ai"};
	// Raw const char* array rather than std::string_view or a vector: it decays
	// to a pointer in the range-for with no allocation and no lifetime question.
	for (const char *sub : subdirs)
	{
		// is_directory, not exists: a FILE named 'scenes' would satisfy exists()
		// and still be broken. The recursive false argument is deliberately absent
		// so a symlinked directory still counts.
		CHECK(std::filesystem::is_directory(project.GetAssetRoot() / sub),
			  std::string("assets/") + sub + "/ exists");
	}
	// The descriptor file. This is the object the OS file dialog hands back, so
	// its absence would make the project un-openable by double-click.
	CHECK(std::filesystem::is_regular_file(project.GetProjectFile()), "project.wasteland exists");

	// Settings round-trip.
	// Settings are edited in place through a non-const accessor -- the reference
	// must outlive the Save() below, which is what the round-trip then verifies.
	Project::Settings &settings = project.GetSettings();
	// Three fields that a naive settings writer would plausibly drop: a scene
	// path, an enum-ish string, and an integer. Each exercises a different YAML
	// scalar type on the way back out.
	settings.StartScene = "assets/scenes/Main.wastescene";
	settings.QualityPreset = "High";
	settings.RendererAPI = 3;
	// Save BEFORE Close. Order matters -- closing discards the in-memory project,
	// so a Close-then-Save would write nothing and the round-trip below would
	// pass vacuously.
	CHECK(project.Save(), "Save() succeeds");

	std::string reopenError;
	// Close, so the Open() below is a genuine reload from disk rather than a
	// re-read of the in-memory object.
	Project::Get().Close();
	CHECK(!Project::Get().IsOpen(), "Close() clears the active project");

	// Open by DIRECTORY. The engine must accept a folder and find the
	// project.wasteland inside it.
	CHECK(Project::Get().Open(expectedRoot, &reopenError), "Open() on the directory succeeds");
	CHECK_EQ(Project::Get().GetSettings().StartScene, "assets/scenes/Main.wastescene", "StartScene round-trips");
	CHECK_EQ(Project::Get().GetSettings().QualityPreset, "High", "QualityPreset round-trips");
	CHECK_EQ(Project::Get().GetSettings().RendererAPI, 3, "RendererAPI round-trips");
	CHECK_EQ(Project::Get().GetName(), name, "Name round-trips");

	// Opening the descriptor file directly must work too -- that is what the
	// file dialog hands back.
	Project::Get().Close();
	// Open by the .wasteland FILE itself. This is the other entry point -- the
	// one a file dialog or a recents entry produces -- so both spellings of the
	// same project must work.
	CHECK(Project::Get().Open(expectedRoot / "project.wasteland", &reopenError),
		  "Open() on project.wasteland itself succeeds");

	// A second Create() with the same name must REFUSE rather than adopt or
	// clobber. Silently overwriting a real project is the worst failure this
	// class of code can have, so it is asserted explicitly.
	// A second Create() with the same name must fail rather than clobber.
	std::string dupError;
	// ...and it must explain itself. A bare false gives the user nothing to act
	// on; an empty error string means the failure path forgot to write one.
	CHECK(!Project::Create(name, {}, &dupError), "duplicate project name is rejected");
	CHECK(!dupError.empty(), "duplicate rejection explains itself");
}

// --------------------------------------------------------------------
// Path resolution. Depends on the project left open by the previous test --
// hence the IsOpen() guard inside rather than a setup of its own.
static void TestPathResolution()
{
	Section("Path resolution");

	Project &project = Project::Get();
	// Skips rather than fails. The increment is deliberate: an unrunnable test
	// is still a problem worth reporting, but its checks are not counted, so the
	// final check total stays honest.
	if (!project.IsOpen())
	{
		++g_Failures;
		std::cout << "  FAIL: no project open; skipping path tests\n";
		return;
	}

	// Asset-relative and project-relative forms must land in the same place.
	// An ASSET-relative path resolves under assets/. Paired with the two below,
	// which cover the project-relative spelling and assert the two are equal --
	// that equality is the actual contract callers depend on.
	CHECK_EQ(project.GetAssetPath("textures/x.png").string(),
			 (project.GetAssetRoot() / "textures" / "x.png").string(),
			 "asset-relative resolves under assets/");

	// The 'assets/'-prefixed spelling. The engine accepts both, which is what
	// makes scene files written by older versions still load.
	CHECK_EQ(project.GetAssetPath("assets/textures/x.png").string(),
			 (project.GetRoot() / "assets" / "textures" / "x.png").string(),
			 "project-relative 'assets/...' resolves to the same file");

	CHECK_EQ(project.GetAssetPath("assets/textures/x.png").string(),
			 project.GetAssetPath("textures/x.png").string(),
			 "both accepted forms resolve identically");

	CHECK_EQ(project.GetAssetPath("assets/textures/x.png").string(),
			 (project.GetRoot() / "assets" / "textures" / "x.png").string(),
			 "legacy 'assets/'-prefixed scene data still resolves");

	// Forward slashes in, native separators out.
	// Separator normalisation: a backslash input must reach the same file as the
	// forward-slash one. Compared with == rather than CHECK_EQ because CHECK_EQ
	// needs streamable operands and paths are not streamable.
	CHECK(project.GetAssetPath("textures\\x.png") == project.GetAssetPath("textures/x.png"),
		  "backslash input is normalized");

	// An ABSOLUTE path is honoured as-is rather than being joined onto the
	// project root, which is what makes a path from a file dialog usable.
	// Absolute input is honoured as-is.
	const std::filesystem::path absolute = project.GetAssetRoot() / "scenes" / "A.wastescene";
	CHECK_EQ(project.GetAssetPath(absolute.string()).string(), absolute.string(),
			 "absolute path passes through unchanged");

	// GetProjectPath is the sibling that does NOT imply the assets/ prefix, so
	// 'assets' here must not become 'assets/assets'.
	CHECK_EQ(project.GetProjectPath("assets").string(), (project.GetRoot() / "assets").string(),
			 "GetProjectPath resolves from the project root");
	// The empty-string edge case. Yields the asset root itself rather than an
	// empty path, which would otherwise propagate as a relative path and resolve
	// against the process CWD.
	CHECK_EQ(project.GetAssetPath("").string(), project.GetAssetRoot().string(),
			 "empty relative path yields the asset root");
}

// --------------------------------------------------------------------
// The containment guarantee. This is the test that matters most:
// MakeProjectRelative returning a correct-looking path for a file OUTSIDE
// the project is how a save ends up holding a reference that breaks the
// moment the project moves.
static void TestMakeProjectRelative()
{
	Section("MakeProjectRelative (the containment guarantee)");

	Project &project = Project::Get();
	if (!project.IsOpen())
	{
		++g_Failures;
		std::cout << "  FAIL: no project open; skipping relative-path tests\n";
		return;
	}

	CHECK_EQ(project.MakeProjectRelative(project.GetRoot() / "assets" / "textures" / "x.png"),
			 "assets/textures/x.png", "converts an in-project absolute path");

	CHECK_EQ(project.MakeProjectRelative(project.GetAssetRoot() / "scripts" / "Player.py"),
			 "assets/scripts/Player.py", "converts an asset-root path");

	// The property the whole design rests on, and the reason the function
	// returns an empty string on failure instead of throwing or returning a
	// best-effort path: the caller must be able to DETECT a foreign path.
	// This is the property the whole design rests on: anything outside the
	// project yields empty so callers detect a foreign path instead of
	// writing a reference that breaks the moment the project moves.
	// A well-formed absolute path that is definitively outside the project. The
	// hard-coded Windows path makes this Windows-specific, though the check is
	// platform-independent in the engine.
	CHECK_EQ(project.MakeProjectRelative(std::filesystem::path("C:/Windows/System32/config")),
			 "", "a foreign absolute path is rejected");

	// The harder case: a path that shares a PREFIX with the project root's
	// parent. A naive string-prefix containment test would wrongly accept this,
	// so it is the case that distinguishes real path logic from string
	// matching.
	const std::filesystem::path parent = project.GetRoot().parent_path() / "OtherProject" / "assets" / "x.png";
	CHECK_EQ(project.MakeProjectRelative(parent), "", "a sibling project's file is rejected");

	// A lexical '..' escape. The engine must normalise before comparing, not
	// just reject the string '..'.
	const std::filesystem::path escape = project.GetRoot() / ".." / ".." / ".." / "etc" / "passwd";
	CHECK_EQ(project.MakeProjectRelative(escape), "", "a ../.. escape is rejected");

	// Output uses forward slashes so it is stable across platforms.
	// Checked as a raw string rather than against an expected value: the point
	// is the absence of backslashes, which a hard-coded expected string would
	// only demonstrate on Windows.
	const std::string rel = project.MakeProjectRelative(project.GetAssetRoot() / "a" / "b" / "c.txt");
	CHECK(rel.find('\\') == std::string::npos, "output contains no backslashes");
}

// --------------------------------------------------------------------
// The recents list. Recents are PER-USER state that outlives the process,
// which is why this test both writes to and restores it.
static void TestRecentProjects()
{
	Section("Recent projects");

	// Clearing first makes the size assertion below a real assertion rather than
	// a check against leftovers from a previous run.
	Project::ClearRecentProjects();
	CHECK(Project::GetRecentProjects().empty(), "cleared list is empty");

	// The project file of whatever is currently open -- TestCreateAndOpen left
	// ProjectSystemTest open, and later assertions depend on that identity.
	const std::filesystem::path file = Project::Get().GetProjectFile();

	// Add takes the project FILE, not the directory: recents entries are
	// descriptor paths, which is what a recents menu needs to reopen directly.
	Project::AddRecentProject(file);
	CHECK_EQ(Project::GetRecentProjects().size(), size_t(1), "one entry after first add");

	std::error_code ec;
	// A conditional rather than indexing [0], so this cannot crash if the list
	// is unexpectedly empty -- it degrades to a sentinel that then fails the
	// comparison below with a readable message.
	const std::string stored =
		Project::GetRecentProjects().empty()
			? std::string("<none>")
			: std::filesystem::path(Project::GetRecentProjects().front()).lexically_normal().string();
	CHECK_EQ(stored, file.lexically_normal().string(), "the entry points at this project");

	// Re-adding the SAME path must move it to the front rather than append a
	// second entry. A recents list that grows without bound on every launch is
	// the bug this catches.
	// Re-adding must move to the front, not duplicate.
	Project::AddRecentProject(file);
	CHECK_EQ(Project::GetRecentProjects().size(), size_t(1), "re-adding the same project does not duplicate");

	// A genuinely different project, so 'front of the list' is distinguishable
	// from 'the only entry'. Without this the ordering claim below would be
	// vacuous.
	// A second, genuinely different project must land at the front and push
	// the first one down -- this is the ordering the recents menu relies on.
	const std::string otherName = "ProjectSystemTestOther";
	std::string otherError;
	// Same best-effort cleanup as earlier, with its own error_code so it cannot
	// throw.
	std::filesystem::remove_all(Project::GetUserProjectsDirectory() / otherName, ec);

	if (!Project::Create(otherName, {}, &otherError))
	{
		++g_Failures;
		std::cout << "  FAIL: could not create the second project: " << otherError << "\n";
		return;
	}

	const std::filesystem::path otherFile = Project::Get().GetProjectFile();
	Project::Get().Close();

	// Two entries, so adding the second did not replace the first. Combined with
	// the size-1 assertion above this pins both halves of the dedup contract.
	CHECK_EQ(Project::GetRecentProjects().size(), size_t(2), "a second project adds a second entry");
	// The recents file stores forward-slash paths deliberately, so
	// compare in that same normalized form rather than as a native path.
	// Compared in the forward-slash form the recents file actually stores.
	// Comparing a native path here would fail on Windows, where generic_string()
	// and string() differ in separator -- hence the ToForwardSlashes helper at
	// the top of this file.
	CHECK_EQ(Project::GetRecentProjects().front(),
			 ToForwardSlashes(otherFile.generic_string()),
			 "most recent project is first");

	// Bootstrap must select the front entry, which is now otherName.
	// Bootstrap with an empty explicit path, so it must fall through to the
	// recents list. The exe path is a non-existent file on purpose: it removes
	// the 'directory near the executable' fallback from consideration, so this
	// tests the recents branch specifically.
	Project::Bootstrap("", std::filesystem::absolute("nonexistent.exe"));
	CHECK(Project::Get().IsOpen(), "Bootstrap() reopens the most recent project");
	// Compared with lexically_normal() on both sides so a trailing separator or
	// a '.' component cannot make identical paths compare unequal.
	CHECK_EQ(Project::Get().GetProjectFile().lexically_normal().generic_string(),
			 otherFile.lexically_normal().generic_string(),
			 "Bootstrap() picks the most recent project");

	// Close before deleting the project directory: Bootstrap() is about to
	// reopen a project, and the currently-open one is the one being removed.
	Project::Get().Close();
	{
		// The root is recomputed rather than captured earlier, because the project
		// is closed and its paths are no longer authoritative.
		const std::filesystem::path otherRoot = Project::GetUserProjectsDirectory() / otherName;
		std::error_code removeEc;
		std::filesystem::remove_all(otherRoot, removeEc);
		// Precondition checks, and they are ordered deliberately: remove_all's own
		// error code is inspected BEFORE any other filesystem call can overwrite
		// it. Checking exists() first would silently clear the evidence.
		// Precondition: Bootstrap can only skip a stale entry if the entry
		// really is gone. Check remove_all's own error code *before* any
		// other filesystem call can overwrite it.
		CHECK(!removeEc, "precondition: remove_all reported no error");
		if (removeEc)
			std::cout << "    remove_all: " << removeEc.message() << "\n";
		CHECK(!std::filesystem::exists(otherRoot, removeEc),
			  "precondition: the second project was deleted from disk");
	}

	// And with the second project gone, the first is selected again.
	// With the front entry now deleted from disk, Bootstrap must SKIP it and
	// take the next one rather than failing outright or opening whatever
	// happens to occupy that path.
	CHECK(Project::Bootstrap("", std::filesystem::absolute("nonexistent.exe")),
		  "Bootstrap() falls back to the next recent entry");
	CHECK_EQ(Project::Get().GetProjectFile().lexically_normal().generic_string(),
			 file.lexically_normal().generic_string(),
			 "fallback selected the original project");
}

// --------------------------------------------------------------------
// Bootstrap fallbacks: the explicit-path branch and the total-failure branch.
static void TestBootstrapFallback()
{
	Section("Bootstrap fallbacks");

	Project::Get().Close();

	// An explicit path wins over everything else.
	std::string error;
	// Recomputed from the well-known directory rather than reused from the
	// earlier test, so this test does not depend on that project's state beyond
	// existing on disk.
	const std::filesystem::path explicitRoot = Project::GetUserProjectsDirectory() / "ProjectSystemTest";
	Project::Get().Close();
	Project::ClearRecentProjects();
	// Explicit path wins over everything, including an empty recents list.
	CHECK(Project::Bootstrap(explicitRoot.string(), std::filesystem::absolute("nonexistent.exe")),
		  "Bootstrap() honours an explicit path with an empty recent list");
	CHECK_EQ(Project::Get().GetRoot().string(), explicitRoot.string(), "explicit path was used");

	// A bogus explicit path must not silently open something else.
	Project::Get().Close();
	Project::ClearRecentProjects();
	// A bogus explicit path with an empty recents list: every fallback is
	// exhausted, so Bootstrap must return false.
	CHECK(!Project::Bootstrap("C:/definitely/not/here", std::filesystem::absolute("nonexistent.exe")),
		  "Bootstrap() reports failure when nothing can be opened");
	// ...and must leave nothing half-open. A failed bootstrap that still left a
	// project open would give the user an editor pointed at a project they
	// never chose.
	CHECK(!Project::Get().IsOpen(), "no project is left half-open after a failed bootstrap");
}

// --------------------------------------------------------------------
// Create() with a seed assets directory. Seed copying is what lets a project
// be created from a template rather than from nothing.
static void TestSeedCopy()
{
	Section("Create() with seed assets");

	// A project seeded from another project's assets should receive a copy
	// of the built-in shaders -- without them the renderer cannot find them.
	// 'A project seeded from another project's assets should receive a copy of
	// the built-in shaders -- without them the renderer cannot find them.' This
	// is the test that proves that copy actually happens.
	const std::string name = "ProjectSystemSeedTest";
	std::string error;

	// Clean slate, for the same reason as in TestCreateAndOpen.
	{
		std::error_code ec;
		std::filesystem::remove_all(Project::GetUserProjectsDirectory() / name, ec);
	}

	// The seed source is ProjectSystemTest's assets/ -- the project the earlier
	// test created. Ordering dependency between tests: this one is meaningless
	// if TestCreateAndOpen did not run first.
	const std::filesystem::path source = Project::GetUserProjectsDirectory() / "ProjectSystemTest" / "assets";
	// create_directories with a TEMPORARY error_code{} -- the error is
	// discarded entirely, so a failure here is invisible and the subsequent
	// ofstream silently creates nothing.
	std::filesystem::create_directories(source / "shaders", std::error_code{});
	{
		// A minimal file. Contents do not matter; only that a file exists at this
		// path to be copied.
		std::ofstream f(source / "shaders" / "Test.glsl", std::ios::trunc);
		f << "// seed shader\n";
	}

	// Create WITH a seed. The empty-map seed path and the populated-seed path
	// are different code inside Create(), so both need coverage.
	CHECK(Project::Create(name, source, &error), "Create() succeeds with a seed directory");

	// The expected destination is computed independently of the project's own
	// GetAssetPath(), so the test does not validate the code against itself.
	const std::filesystem::path copied =
		Project::GetUserProjectsDirectory() / name / "assets" / "shaders" / "Test.glsl";
	// is_regular_file, not is_directory: a copy that produced an empty folder
	// would satisfy the weaker check.
	CHECK(std::filesystem::is_regular_file(copied), "seed assets are copied into the new project");

	// The source must still be there. Copy, not move -- a move implementation
	// would satisfy the previous assertion and break every subsequent run.
	// And the seed source is untouched.
	CHECK(std::filesystem::is_regular_file(source / "shaders" / "Test.glsl"),
		  "seed source still exists (copy, not move)");

	// Clean up so repeated runs start clean.
	std::error_code ec;
	// Cleanup so the next run starts from the same state this one did.
	std::filesystem::remove_all(Project::GetUserProjectsDirectory() / name, ec);
	Project::Get().Close();
}

// --------------------------------------------------------------------
// Built-in shader seeding.
//
// This is the part that keeps a new project from being useless: the
// renderers look for their GLSL in the active project's assets/shaders,
// so an empty one means nothing draws.
// Built-in shader seeding -- the part that keeps a new project from being
// useless. The renderers look for their GLSL in the ACTIVE project's
// assets/shaders, so an empty one means nothing draws.
static void TestBuiltInShaderSeeding()
{
	Section("Built-in shader seeding");

	std::error_code ec;
	const std::filesystem::path projects = Project::GetUserProjectsDirectory();

	// Stand in for the engine install: assets/shaders with two files.
	// A stand-in for the engine install: assets/shaders with two files. Faking it
	// means the test does not depend on whatever the real build happens to ship,
	// and it can add a file mid-test to simulate an engine upgrade.
	const std::filesystem::path fakeEngine = projects / "ProjectSystemFakeEngine";
	std::filesystem::remove_all(fakeEngine, ec);
	std::filesystem::create_directories(fakeEngine / "assets" / "shaders", ec);
	{
		std::ofstream quad(fakeEngine / "assets" / "shaders" / "Renderer2D_Quad.glsl", std::ios::trunc);
		quad << "// engine quad\n";
		std::ofstream nova(fakeEngine / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl", std::ios::trunc);
		nova << "// engine nova\n";
		// A nested subdirectory must be recreated too.
		// A NESTED subdirectory, so the copy is tested for recursion rather than a
		// single flat directory.
		std::filesystem::create_directories(fakeEngine / "assets" / "shaders" / "nested", ec);
		std::ofstream nested(fakeEngine / "assets" / "shaders" / "nested" / "Deep.glsl", std::ios::trunc);
		nested << "// engine deep\n";
	}

	// From here on every Create() seeds from the fake engine.
	// From here on every Create() seeds from the fake engine. This is a global
	// static, not a parameter -- which is why it must be reset to {} before this
	// function returns, or every later test inherits the fake.
	Project::SetEngineAssetsRoot(fakeEngine / "assets");

	const std::string name = "ProjectSystemShaderTest";
	std::filesystem::remove_all(projects / name, ec);

	std::string error;
	// Create with no caller seed, so the ONLY source of the built-ins is the
	// engine root configured above.
	CHECK(Project::Create(name, {}, &error), "Create() succeeds with engine assets configured");

	const std::filesystem::path shaders = projects / name / "assets" / "shaders";
	CHECK(std::filesystem::is_regular_file(shaders / "Renderer2D_Quad.glsl"),
		  "built-in quad shader was copied into the new project");
	CHECK(std::filesystem::is_regular_file(shaders / "Renderer3D_NovaRenderer.glsl"),
		  "built-in nova shader was copied into the new project");
	CHECK(std::filesystem::is_regular_file(shaders / "nested" / "Deep.glsl"),
		  "nested built-in shader subdirectory was recreated");

	// The engine-update case: a shader that did not exist when the project was
	// created must arrive on Save(). Without this a project keeps running an
	// engine's old shader set forever.
	// A shader the project does not have should arrive on Save() -- this
	// is the engine-update case.
	{
		// Added to the FAKE ENGINE after the project was created, so it can only
		// appear in the project if Save() re-scans the engine directory.
		std::ofstream added(fakeEngine / "assets" / "shaders" / "NewlyAdded.glsl", std::ios::trunc);
		added << "// added in a later engine version\n";
	}
	CHECK(Project::Get().Save(), "Save() succeeds");
	CHECK(std::filesystem::is_regular_file(shaders / "NewlyAdded.glsl"),
		  "Save() copies a shader added to the engine since the project was created");

	// The critical property: Save() must never destroy a user's edits.
	// Overwriting here would silently revert someone's shader every time they
	// hit save, which is unrecoverable.
	// The critical property: Save() must never destroy a user's edits.
	// Overwriting here would silently revert someone's shader every time
	// they hit save, which is unrecoverable.
	{
		std::ofstream edited(shaders / "Renderer2D_Quad.glsl", std::ios::trunc);
		edited << "// USER EDIT -- must survive Save()\n";
	}
	// A deleted built-in should be restored, since the project cannot
	// render without it.
	// DEFECT: this removes 'Renderer3D_NovaRenderer.hlsl', but the fake engine
	// only ever creates '.glsl' files, and only Renderer2D_Quad.glsl was edited
	// above. The remove() therefore targets a file that does not exist and is a
	// NO-OP -- the comment above it claims to be testing restoration of a
	// deleted built-in, and it tests nothing. The real deletion test is the
	// '.glsl' removal further down, which does work.
	// `ec` is also set by this failed remove and is not inspected, so the next
	// CHECK sees no evidence of it.
	std::filesystem::remove(shaders / "Renderer3D_NovaRenderer.hlsl", ec);

	// Save after the user edit. The assertion that the edit SURVIVED is below --
	// this only confirms the save itself succeeded.
	CHECK(Project::Get().Save(), "Save() succeeds after the user edits a shader");

	{
		// Read the edited file back and compare its first line exactly. Comparing
		// the content rather than the timestamp is what makes this a real check: a
		// save that rewrote the file with the engine's copy would fail here.
		std::ifstream edited(shaders / "Renderer2D_Quad.glsl");
		std::string content;
		std::getline(edited, content);
		CHECK(content == "// USER EDIT -- must survive Save()",
			  "Save() does not overwrite a shader the user edited");
	}

	// Removing a built-in entirely and saving brings it back -- the counterpart
	// to the edit-survival check above. Both are needed: one proves Save is not
	// destructive, the other that it is not merely additive.
	// Removing a built-in entirely and saving brings it back.
	// '.glsl' this time, matching what the fake engine actually created. The
	// existence check on the next line makes this a verified precondition rather
	// than an assumption.
	std::filesystem::remove(shaders / "Renderer3D_NovaRenderer.glsl", ec);
	CHECK(!std::filesystem::exists(shaders / "Renderer3D_NovaRenderer.glsl"), "precondition: shader was deleted");
	CHECK(Project::Get().Save(), "Save() succeeds after a built-in was deleted");
	CHECK(std::filesystem::is_regular_file(shaders / "Renderer3D_NovaRenderer.glsl"),
		  "Save() restores a deleted built-in shader");

	// A caller seed still wins over the built-in of the same name -- that is
	// how a project deliberately overrides one shader without forking the whole
	// shader set.
	// A caller seed still wins over the built-in of the same name, which
	// is how a project deliberately overrides one.
	// The override seed directory is created WITHOUT a preceding remove_all,
	// unlike every other setup in this file -- so a leftover directory from a
	// previous run would be silently reused.
	const std::filesystem::path overrideDir = projects / "ProjectSystemOverride";
	std::filesystem::create_directories(overrideDir / "shaders", ec);
	{
		std::ofstream o(overrideDir / "shaders" / "Renderer2D_Quad.glsl", std::ios::trunc);
		o << "// deliberately overridden\n";
	}
	const std::string overrideName = "ProjectSystemOverrideTest";
	std::filesystem::remove_all(projects / overrideName, ec);
	// Create with the override seed. Wrapped in an if rather than a CHECK
	// because the verification below needs the project to exist first.
	if (Project::Create(overrideName, overrideDir, &error))
	{
		// Read the override project, not the one above -- this is a DIFFERENT
		// directory with its own copy. Both projects have a shader of this name, so
		// reading the wrong one would make this test pass for the wrong reason.
		// Read the override project, not the one above -- this is a
		// different directory with its own copy.
		std::ifstream seed(projects / overrideName / "assets" / "shaders" / "Renderer2D_Quad.glsl");
		std::string content;
		std::getline(seed, content);
		CHECK(content == "// deliberately overridden",
			  "a caller seed overrides the built-in shader of the same name");
	}
	else
	{
		// The else branch counts the failure directly, matching the early-return
		// style used in TestCreateAndOpen above.
		++g_Failures;
		std::cout << "  FAIL: Create() with an override seed: " << error << "\n";
	}

	// With no engine assets configured, Create() must still work. A project that
	// cannot be created without a located engine install is un-creatable on a
	// developer machine -- so this is a real robustness case, not an edge.
	// With no engine assets configured, Create() must still work.
	Project::SetEngineAssetsRoot({});
	std::filesystem::remove_all(projects / "ProjectSystemNoEngine", ec);
	CHECK(Project::Create("ProjectSystemNoEngine", {}, &error),
		  "Create() succeeds when no engine assets root is set");
	CHECK(std::filesystem::is_directory(projects / "ProjectSystemNoEngine" / "assets" / "shaders"),
		  "an unseeded project still has a shaders directory");

	// Restore, and clean up.
	Project::SetEngineAssetsRoot({});
	Project::Get().Close();
	// Range-for over an initialiser list of names, removing each. One statement
	// without braces -- correct here, and a trap if a second statement is ever
	// added to the body.
	for (const char *n : {"ProjectSystemShaderTest", "ProjectSystemOverrideTest",
						  "ProjectSystemNoEngine"})
		std::filesystem::remove_all(projects / n, ec);
	std::filesystem::remove_all(fakeEngine, ec);
	std::filesystem::remove_all(overrideDir, ec);
}

// Entry point. The test order below is a DEPENDENCY chain, not a
// preference: each test consumes the project state the previous one left
// behind. Reordering these silently invalidates later tests.
int main()
{
	// Project.cpp reports failures through the engine logger, so it must be
	// initialized first -- otherwise the first WL_CORE_ERROR dereferences a
	// null logger and takes the process down before the message is seen.
	// Log::Init() must come first. Project.cpp reports failures through the
	// engine logger, so the first WL_CORE_ERROR before Init dereferences a null
	// logger and takes the process down before the message is ever seen.
	Wasteland::Log::Init();

	// The two directories are printed up front so a failure message quoting a
	// path is diagnosable without re-deriving where the test was looking.
	std::cout << "Wasteland::Project functional tests\n";
	std::cout << "Documents: " << Project::GetUserDocumentsDirectory().string() << "\n";
	std::cout << "Config:    " << Project::GetUserConfigDirectory().string() << "\n";

	// TestCreateAndOpen runs before the path tests because it creates and opens
	// the project they all operate on.
	TestSanitizeName();
	TestCreateAndOpen();
	TestPathResolution();
	TestMakeProjectRelative();
	// TestRecentProjects runs before TestBootstrapFallback because it builds the
	// multi-entry recents list that Bootstrap's ordering behaviour depends on.
	TestRecentProjects();
	TestBootstrapFallback();
	TestSeedCopy();
	TestBuiltInShaderSeeding();

	std::cout << "\n----------------------------------------\n";
	std::cout << g_Checks << " checks, " << g_Failures << " failure(s)\n";

	// Leave the machine as we found it.
	std::error_code ec;
	// Final cleanup of the one project that is intentionally left on disk
	// between tests. Everything else is cleaned up in its own function.
	std::filesystem::remove_all(Project::GetUserProjectsDirectory() / "ProjectSystemTest", ec);
	// The recents list is per-user persistent state, so leaving it populated
	// would change what the NEXT run's Bootstrap selects.
	Project::ClearRecentProjects();

	if (g_Failures == 0)
		std::cout << "RESULT: PASS\n";
	else
		std::cout << "RESULT: FAIL\n";

	return g_Failures == 0 ? 0 : 1;
}
