// OpenAICompatibleProvider.h -- provider for OpenAI-compatible chat APIs.
//
// Covers: OpenAI (api.openai.com), OpenRouter (openrouter.ai), Hermes/Nous,
// and any local server (LM Studio, Ollama, vLLM, llama.cpp) that exposes
// /chat/completions in OpenAI format.
#pragma once

#include "IAIProvider.h"
#include <string>
#include <vector>

namespace Wasteland
{
	class OpenAICompatibleProvider final : public IAIProvider
	{
	public:
		OpenAICompatibleProvider(std::string name, std::string baseUrl, std::string apiKey, std::string defaultModel);
		~OpenAICompatibleProvider() override = default;

		ProviderResult Chat(const ProviderRequest &request, ProviderStreamCallback onChunk) override;
		std::string GetName() const override;
		std::string GetDefaultModel() const override;
		bool IsAuthenticated() const override;
		bool RefreshAuth() override;
		std::vector<std::string> GetModels() const override;

		void SetApiKey(std::string key);
		void SetBaseUrl(std::string url);
		void SetDefaultModel(std::string model);
		const std::string &GetApiKey() const;
		const std::string &GetBaseUrl() const;

	private:
		std::string m_Name;
		std::string m_BaseUrl;
		std::string m_ApiKey;
		std::string m_DefaultModel;
	};
}
