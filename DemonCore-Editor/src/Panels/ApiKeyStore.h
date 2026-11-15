// ApiKeyStore.h -- persistent settings for the editor's AI features: API keys,
// endpoint, model, temperature and system prompt.
//
// A singleton holding public data members, deliberately not a class with
// accessors. The editor panels write ApiKeyStore::Instance().OpenAIKey = ... 
// directly, so the fields are the interface.
//
// Persisted to a plain-text file outside the repository so credentials are
// never committed. Load() and Save() are the only serialisation points.
#pragma once

// For the std::filesystem::path return type of FilePath below.
//
// <string> is pulled in separately even though <filesystem> tends to drag it
// in, because the data members below are std::string and relying on a transitive
// include would be fragile.
#include <filesystem>
#include <string>

namespace Wasteland
{

	// The location is computed by FilePath() in the .cpp: %APPDATA%\Wasteland\ai_keys.cfg
	// on Windows, $HOME/.config/Wasteland/ on other platforms, falling
	// back to a relative assets/ directory when the environment variable is unset.
	//
	// Outside the repo is the point -- an in-tree file would be one `git add -A`
	// away from being committed.
	// Stores AI API keys + endpoint settings outside the repo so they are
	// never committed. Location: %APPDATA%\Wasteland\ai_keys.cfg (Windows).
	// NOTE: keys are stored as plain text; the file lives in the user's
	// profile directory. Values are never written to the log.
	// A singleton in the Meyers form (a function-local static), reached through
	// Instance(). Constructed on first use, so there is no initialisation-order
	// problem between this and the editor panels that read it.
	//
	// No virtual destructor and no inheritance: nothing derives from it and it is
	// never deleted (the static's destructor runs at exit).
	//
	// On the security note in the comment block above, which is the honest state of
	// the implementation: the keys are NOT encrypted, obfuscated, or held in any
	// credential store (no DPAPI, no libsecret, no keychain). What protects them
	// is only the file's location and the filesystem permissions -- any process
	// running as the same user can read it, and any profile backup or sync will
	// include it.
	//
	// The mitigating half of that contract IS honoured: Save() logs the PATH on
	// failure and never the contents, so a key does not reach a log file.
	class ApiKeyStore
	{
	public:
		// The accessor. Returns a REFERENCE, so callers mutate the singleton in place:
		// `ApiKeyStore::Instance().Temperature = 0.9f;` is the intended spelling.
		//
		// static on the method, not a static member: the instance is a function-local
		// static inside the definition, so it cannot be touched before this runs.
		//
		// Not thread-safe against concurrent first use beyond what C++11 guarantees --
		// the initialisation itself is thread-safe, but a read of OpenAIKey concurrent
		// with a write from another thread is not. In practice the editor is
		// single-threaded at this level.
		static ApiKeyStore &Instance();

		// Reads the file and overwrites the members it recognises.
		//
		// NOT reset-then-load: a key absent from the file LEAVES the current member
		// untouched rather than clearing it. For OpenAIKey that is invisible (it starts
		// empty), but it means a hand-edited file that removes a line does not remove
		// the in-memory value.
		//
		// Deliberately quiet on a missing file -- that is the first-run case, and the
		// defaults in this header stand. Unparseable lines are skipped silently too.
		// Defined in the .cpp.
		void Load();
		// Writes every member back, truncating the file.
		//
		// `const` because saving does not change the settings -- an accurate signature
		// that also documents the intent.
		//
		// Called by the editor when a settings field is committed. It rewrites the WHOLE
		// file each time, including the header comment.
		//
		// DEFECT (data loss on a partial write): there is no temporary-file-then-rename,
		// so an interrupted write or a full disk truncates the file and destroys every
		// stored key. The atomic-replace idiom is the standard fix.
		//
		// It also logs the PATH on failure (WL_CORE_ERROR in the .cpp), never the
		// contents -- which is what keeps the note above true.
		void Save() const;

		// Where the file lives, computed fresh on each call from the environment.
		//
		// static and not caching the result: APPDATA or HOME can change during a
		// session (a test harness setting it, a user profile switch), and the cost of
		// two getenv calls is irrelevant next to the file I/O that follows.
		//
		// Exposed publicly so a diagnostics panel can show the user which file to back
		// up or delete. The fallback is a relative `assets/ai_keys.cfg` when the
		// environment variable is unset -- which puts the credentials INSIDE the
		// working directory, defeating the entire purpose of the class on such a
		// system. On Windows APPDATA is effectively always set; on a stripped Linux
		// container HOME may not be.
		//
		// The parent directory is created on demand by Save(), so nothing needs to
		// exist before the first save.
		static std::filesystem::path FilePath();

		// No initialiser, so it defaults to an empty string -- which is the correct
		// "not configured" value and needs no explicit `= ""`.
		std::string OpenAIKey;
		std::string OpenAIBaseURL = "https://api.openai.com/v1";
		std::string OpenAIModel = "gpt-6.1-sol";
		float Temperature = 0.7f;
		std::string SystemPrompt = "You are a helpful assistant inside the Wasteland game engine editor.";
		std::string HuggingFaceToken;

		// Anthropic (Claude) credentials.
		std::string AnthropicKey;
		std::string AnthropicBaseURL = "https://api.anthropic.com";
		std::string AnthropicModel = "opus-5.5";

		// Google (Gemini) credentials.
		std::string GoogleKey;
		std::string GoogleBaseURL = "https://generativelanguage.googleapis.com/v1beta";
		std::string GoogleModel = "gemini-3.8-flash";

		// Hermes / Nous credentials.
		std::string HermesKey;
		std::string HermesBaseURL = "https://inference-api.nousresearch.com/v1";
		std::string HermesModel = "step-3.7-flash";

		// Effort + model presets per provider (for the dropdown UI).
		std::string EffortPreset = "medium";
		std::string ModelPreset = "";

		// Last selected provider in the AI Chat panel.
		std::string Provider = "openai";

		// Subagent settings.
		bool AutoDelegate = false;
		int MaxSubagents = 4;

	// Private with a single defaulted constructor, so the only way to obtain one is
	// Instance().
	private:
		// Defaulted, relying on the NSDMIs above for every field. Private, which is what
		// makes the singleton enforced rather than conventional.
		//
		// No copy/move deletion, so the class is still copyable in principle -- nothing
		// prevents `auto copy = ApiKeyStore::Instance();` from duplicating every key
		// into a second object that is never persisted. The private constructor stops
		// construction, not copying. A deleted copy ctor would close that.
		//
		// The instance itself is never destroyed until process exit, so no member needs
		// custom teardown.
		ApiKeyStore() = default;
	};

}
