// PCH first, by house rule: std::string and std::filesystem both arrive with it,
// which is why the header includes below are limited to what this file adds.
#include "wlpch.h"
// The declaration: public data members for each setting, plus Load/Save and a
// static FilePath. The header documents that keys are stored as PLAIN TEXT in the
// user profile, so every comment here has to assume the file is readable by
// anything running as that user.
#include "ApiKeyStore.h"

// getenv for APPDATA/HOME, and <fstream> for the read/write below.
// <cstdlib> is included unconditionally although getenv is POSIX-and-Windows
// rather than standard C++ -- which is fine here because the whole file is
// unconditional, unlike LinuxPlatformUtils.cpp which gates itself.
#include <cstdlib>
#include <fstream>

// Wasteland, matching the header. The class has a private defaulted constructor,
// so this is the only way to obtain one -- a deliberate singleton, since the keys
// are process-wide configuration rather than per-panel state.
namespace Wasteland
{

	// The Meyers singleton, thread-safe since C++11: the local static is constructed
	// exactly once even under a race. Contrast Instrumentor::Get, which reaches the
	// same pattern by a different route.
	//
	// No destruction-order concern here in practice -- nothing else holds a reference
	// past main -- but the AiChatPanel and ModelDownloaderPanel call sites all reach
	// for Instance(), so this is the single object they share.
	ApiKeyStore &ApiKeyStore::Instance()
	{
		static ApiKeyStore s_Instance;
		return s_Instance;
	}

	// Called from Load and Save alike, and it re-reads the environment every time
	// rather than caching. Cheap enough for an editor that saves on demand, and it
	// means a changed APPDATA is picked up without a restart.
	//
	// Both branches fall back to a RELATIVE "assets" directory when the environment
	// variable is absent. That is the sharp edge: a relative path resolves against the
	// process working directory, so on a machine with no APPDATA the keys file lands
	// beside the executable -- or inside the repo -- which is exactly the thing this
	// file exists to avoid. Not fixed here; comments only.
	std::filesystem::path ApiKeyStore::FilePath()
	{
// Compiled-time branch rather than a runtime getenv: only one path is even
// compiled, so the other platform's conventions cost nothing at runtime.
#if defined(_WIN32)
		// getenv returns a pointer into the environment, which is why it is assigned to
		// a `const char*` and immediately copied into a std::filesystem::path rather than
		// stored. The environment block is not guaranteed stable across setenv.
		const char *appData = std::getenv("APPDATA");
		// The Roaming AppData subdirectory, so the file follows a user across machines
		// on a domain profile -- the correct location for a credential that is meant to
		// persist. The ternary also handles the null case, hence the relative fallback
		// noted above.
		std::filesystem::path dir = appData ? std::filesystem::path(appData) / "Wasteland" : std::filesystem::path("assets");
#else
		const char *home = std::getenv("HOME");
		std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "Wasteland" : std::filesystem::path("assets");
#endif
		// A fixed filename inside the directory. ".cfg" rather than ".ini" because the
		// format is a bare key=value list with a # comment convention -- which is exactly
		// what Trim/Load below implement, so nothing here needs a parser library.
		return dir / "ai_keys.cfg";
	}

	// File-local rather than namespace-scope, so Trim has internal linkage and
	// cannot collide with another Trim in a unity build.
	static std::string Trim(const std::string &s)
	{
		// find_first_not_of skips leading whitespace including CR and LF, so a file
		// written with CRLF endings (Save writes "\n" explicitly, which becomes CRLF on
		// Windows) still parses. Without CR in the set, every value would keep a trailing
		// carriage return and the last field of each line would be corrupted.
		size_t b = s.find_first_not_of(" \t\r\n");
		// An all-whitespace line is indistinguishable from an empty one here, and both
		// become "". The caller treats "" as "skip", so a line of spaces is silently
		// ignored rather than producing an empty key.
		if (b == std::string::npos)
			return "";
		// find_last_not_of for the same reason, with the same character set. substr(b,
		// e - b + 1) is the inclusive-end form: for a single-character string e == b and
		// the length is 1, not 0.
		size_t e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}

	// Load is not const and does not clear the members first. DEFECT (partial load):
	// a key present in the file overwrites the default; a key ABSENT from the file
	// leaves whatever the member already held. So calling Load() twice -- or calling
	// Load() after the user cleared OpenAIKey in the UI -- merges rather than resets.
	// EditorLayer.cpp calls it once at startup, where the merge is invisible. Not
	// fixed here.
	void ApiKeyStore::Load()
	{
		// No error_code overload, so a filesystem error becomes an exception on some
		// paths -- the ifstream constructor itself does not throw, but resolving a path
		// can. Save() below uses the error_code form; Load() does not. Asymmetric.
		std::ifstream file(FilePath());
		// First run: keep the header defaults. Returning silently rather than logging is
		// right -- a missing file is the normal state before the user has ever entered a
		// key, and the header defaults (localhost:1234/v1, local-model, 0.7) are
		// deliberate. An existing-but-unreadable file is silently treated the same way,
		// which is the one case where a message would help.
		if (!file.is_open())
			return; // First run: keep defaults.
		std::string line;
		// getline rather than a >> extraction, so a value containing spaces survives --
		// SystemPrompt is a whole sentence and would otherwise stop at the first space.
		while (std::getline(file, line))
		{
			// Trim BEFORE the comment test, so "  # a comment" is recognised as a comment.
			// Reversing the order would leak indented comments into the key stream.
			line = Trim(line);
			// Blank lines and # comments skipped. The # convention is written by Save()'s
			// own header line, so the file is self-describing to anyone who opens it -- which
			// matters because the file also holds a bearer token in plain text.
			if (line.empty() || line[0] == '#')
				continue;
			// find('=') rather than splitting on whitespace, so a value may itself contain
			// an equals sign -- a system prompt with "x = y" in it survives. The FIRST equals
			// wins, so a key cannot contain one; the key set below has no such need.
			size_t eq = line.find('=');
			// A line with no equals is skipped rather than treated as an error. Lenient on
			// purpose: the only writer is Save(), so a malformed line means a hand edit, and
			// refusing to load the whole file over one bad line would be worse than ignoring
			// it.
			if (eq == std::string::npos)
				continue;
			std::string key = Trim(line.substr(0, eq));
			std::string value = Trim(line.substr(eq + 1));
			// Assigned unconditionally, including when the value is empty -- so an explicitly
			// blank key in the file clears the stored one. Contrast the two branches below,
			// which guard on !value.empty() so a blank value cannot wipe a default.
			if (key == "openai_api_key")
				OpenAIKey = value;
			else if (key == "openai_base_url" && !value.empty())
				OpenAIBaseURL = value;
			else if (key == "openai_model" && !value.empty())
				OpenAIModel = value;
			else if (key == "temperature")
			{
				try
				{
					Temperature = std::stof(value);
				}
				catch (...)
				{
				}
			}
			else if (key == "system_prompt" && !value.empty())
				SystemPrompt = value;
			else if (key == "hf_token")
				HuggingFaceToken = value;
			else if (key == "anthropic_api_key")
				AnthropicKey = value;
			else if (key == "anthropic_base_url" && !value.empty())
				AnthropicBaseURL = value;
			else if (key == "anthropic_model" && !value.empty())
				AnthropicModel = value;
			else if (key == "google_api_key")
				GoogleKey = value;
			else if (key == "google_base_url" && !value.empty())
				GoogleBaseURL = value;
			else if (key == "google_model" && !value.empty())
				GoogleModel = value;
			else if (key == "hermes_api_key")
				HermesKey = value;
			else if (key == "hermes_base_url" && !value.empty())
				HermesBaseURL = value;
			else if (key == "hermes_model" && !value.empty())
				HermesModel = value;
			else if (key == "effort_preset" && !value.empty())
				EffortPreset = value;
			else if (key == "model_preset" && !value.empty())
				ModelPreset = value;
			else if (key == "provider" && !value.empty())
				Provider = value;
			else if (key == "auto_delegate")
				AutoDelegate = (value == "1" || value == "true" || value == "yes");
			else if (key == "max_subagents")
			{
				try
				{
					int v = std::stoi(value);
					if (v >= 1 && v <= 16)
						MaxSubagents = v;
				}
				catch (...)
				{
				}
			}
		}
	}

	// Save is const and rewrites the WHOLE file every time rather than editing in
	// place, which is what makes it safe against a partially-written previous state.
	void ApiKeyStore::Save() const
	{
		// The error_code form: create_directories reports failure through `ec` instead
		// of throwing. Deliberate -- a save that cannot create its directory should leave
		// the existing file alone, not propagate an exception out of a UI callback.
		std::error_code ec;
		// Failures are logged and swallowed. The error code itself is not inspected, so
		// "permission denied" and "path too long" produce the same message, which does at
		// least name the path that failed.
		std::filesystem::create_directories(FilePath().parent_path(), ec);
		// TRUNC mode, so the previous content is replaced rather than appended to. Any
		// keys the user had removed since the last Load are therefore really gone from
		// disk -- the correct behaviour for a file that is the source of truth.
		std::ofstream file(FilePath(), std::ios::trunc);
		if (!file.is_open())
		{
			WL_CORE_ERROR("ApiKeyStore: cannot write {0}", FilePath().string());
			return;
		}
		// The self-describing header comment. Written on every save, so it is a constant
		// cost of four bytes and the reason a user opening the file in Notepad sees a
		// warning before the token.
		//
		// Note the security posture overall: keys are plain text in the user profile, as
		// the header admits. On a shared machine every other process running as that user
		// can read them. That is a deliberate trade for an editor tool, not an oversight,
		// but it is the thing to revisit first if this store is ever used from a shipped
		// game rather than only from the editor.
		file << "# Wasteland editor AI keys. Keep this file private.\n";
		file << "openai_api_key=" << OpenAIKey << "\n";
		file << "openai_base_url=" << OpenAIBaseURL << "\n";
		file << "openai_model=" << OpenAIModel << "\n";
		file << "temperature=" << Temperature << "\n";
		file << "system_prompt=" << SystemPrompt << "\n";
		file << "hf_token=" << HuggingFaceToken << "\n";
		file << "anthropic_api_key=" << AnthropicKey << "\n";
		file << "anthropic_base_url=" << AnthropicBaseURL << "\n";
		file << "anthropic_model=" << AnthropicModel << "\n";
		file << "google_api_key=" << GoogleKey << "\n";
		file << "google_base_url=" << GoogleBaseURL << "\n";
		file << "google_model=" << GoogleModel << "\n";
		file << "hermes_api_key=" << HermesKey << "\n";
		file << "hermes_base_url=" << HermesBaseURL << "\n";
		file << "hermes_model=" << HermesModel << "\n";
		file << "effort_preset=" << EffortPreset << "\n";
		file << "model_preset=" << ModelPreset << "\n";
		file << "provider=" << Provider << "\n";
		file << "auto_delegate=" << (AutoDelegate ? "1" : "0") << "\n";
		file << "max_subagents=" << MaxSubagents << "\n";
	}

}
