#include "wlpch.h"
#include "ApiKeyStore.h"

#include <cstdlib>
#include <fstream>

namespace Wasteland
{

	ApiKeyStore &ApiKeyStore::Instance()
	{
		static ApiKeyStore s_Instance;
		return s_Instance;
	}

	std::filesystem::path ApiKeyStore::FilePath()
	{
#if defined(_WIN32)
		const char *appData = std::getenv("APPDATA");
		std::filesystem::path dir = appData ? std::filesystem::path(appData) / "Wasteland" : std::filesystem::path("assets");
#else
		const char *home = std::getenv("HOME");
		std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "Wasteland" : std::filesystem::path("assets");
#endif
		return dir / "ai_keys.cfg";
	}

	static std::string Trim(const std::string &s)
	{
		size_t b = s.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			return "";
		size_t e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}

	void ApiKeyStore::Load()
	{
		std::ifstream file(FilePath());
		if (!file.is_open())
			return; // First run: keep defaults.
		std::string line;
		while (std::getline(file, line))
		{
			line = Trim(line);
			if (line.empty() || line[0] == '#')
				continue;
			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string key = Trim(line.substr(0, eq));
			std::string value = Trim(line.substr(eq + 1));
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
		}
	}

	void ApiKeyStore::Save() const
	{
		std::error_code ec;
		std::filesystem::create_directories(FilePath().parent_path(), ec);
		std::ofstream file(FilePath(), std::ios::trunc);
		if (!file.is_open())
		{
			WL_CORE_ERROR("ApiKeyStore: cannot write {0}", FilePath().string());
			return;
		}
		file << "# Wasteland editor AI keys. Keep this file private.\n";
		file << "openai_api_key=" << OpenAIKey << "\n";
		file << "openai_base_url=" << OpenAIBaseURL << "\n";
		file << "openai_model=" << OpenAIModel << "\n";
		file << "temperature=" << Temperature << "\n";
		file << "system_prompt=" << SystemPrompt << "\n";
		file << "hf_token=" << HuggingFaceToken << "\n";
	}

}
