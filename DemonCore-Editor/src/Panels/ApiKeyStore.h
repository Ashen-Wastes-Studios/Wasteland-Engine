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

	// The location is computed by FilePath() in the .cpp: %APPDATA%\Wasteland\
	// ai_keys.cfg on Windows, $HOME/.config/Wasteland/ on other platforms, falling
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
		//
		// The only member WITHOUT an initialiser in this block, and the difference is
		// invisible.
		//
		// Serialised as `openai_api_key=`. Written to disk in plain text; see the
		// security note at the top of this header.
		std::string OpenAIKey;
		// An OpenAI-COMPATIBLE endpoint, defaulting to LM Studio's port -- so with no
		// configuration at all the editor talks to a locally running model server
		// rather than to the public OpenAI API. That is the privacy-preserving default:
		// no key is needed and no prompt leaves the machine.
		//
		// http, not https, because it is localhost.
		//
		// A user pointing this at api.openai.com must remember to change the scheme
		// to https; nothing validates or warns about a plaintext URL carrying an API
		// key to a remote host.
		std::string OpenAIBaseURL = "http://localhost:1234/v1";
		// The model identifier sent in the request. The default is a placeholder that
		// matches no real model on any server, so an unconfigured setup fails at the
		// first request rather than silently picking something -- arguably the right
		// default, since silently choosing a model would be worse.
		//
		// Serialised as `openai_model=`.
		std::string OpenAIModel = "local-model";
		// Sampling temperature, passed straight through to the chat-completions
		// request. 0.7 is a middling default: high enough to vary, low enough to stay
		// on task for an in-editor assistant.
		//
		// The only non-string scalar, and the only member parsed rather than copied on
		// load: Load() runs std::stof inside a try/catch, so a malformed value leaves
		// the default in place instead of setting 0.
		//
		// No range validation. A stored 5.0 or a negative value is forwarded to the
		// server unchanged, which will either clamp or error.
		float Temperature = 0.7f;
		// The instruction prepended to every AI request. Names the product so the
		// model has context without the caller having to restate it.
		//
		// DEFECT (serialisation, and it is not hypothetical): this is written to a
		// line-oriented `key=value` file, and a multi-line prompt -- which is exactly
		// what a system prompt tends to become -- is written across several lines.
		// Load() then reads only the FIRST line and discards the rest, so a multi-line
		// prompt is silently truncated on the next load.
		//
		// The same defect applies to any value containing a newline, and to any value
		// containing `=` -- Load() splits on the FIRST `=`, so a base URL with a query
		// string would lose everything after it.
		std::string SystemPrompt = "You are a helpful assistant inside the Wasteland game engine editor.";
		// The Hugging Face access token, for model downloads through the editor's
		// downloader panel.
		//
		// Same plain-text treatment as OpenAIKey, and the same exposure: any process as
		// this user can read it. The class comment's "values are never written to the
		// log" applies to this field too.
		//
		// Serialised as `hf_token=`, NOT as `huggingface_token=` -- the short form is
		// the wire name and Save/Load must agree on it. They do.
		std::string HuggingFaceToken;

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
