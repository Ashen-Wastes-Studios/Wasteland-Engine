#pragma once

#include <filesystem>
#include <string>

namespace Wasteland
{

	// Stores AI API keys + endpoint settings outside the repo so they are
	// never committed. Location: %APPDATA%\Wasteland\ai_keys.cfg (Windows).
	// NOTE: keys are stored as plain text; the file lives in the user's
	// profile directory. Values are never written to the log.
	class ApiKeyStore
	{
	public:
		static ApiKeyStore &Instance();

		void Load();
		void Save() const;

		static std::filesystem::path FilePath();

		std::string OpenAIKey;
		std::string OpenAIBaseURL = "http://localhost:1234/v1";
		std::string OpenAIModel = "local-model";
		float Temperature = 0.7f;
		std::string SystemPrompt = "You are a helpful assistant inside the Wasteland game engine editor.";
		std::string HuggingFaceToken;

	private:
		ApiKeyStore() = default;
	};

}
