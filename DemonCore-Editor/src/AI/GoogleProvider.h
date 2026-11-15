// GoogleProvider.h -- provider for Google's Gemini generateContent API.
//
// Wire format: https://ai.google.dev/api/rest/v1beta/models/generateContent
#pragma once

#include "IAIProvider.h"
#include <string>
#include <vector>

namespace Wasteland
{
	class GoogleProvider final : public IAIProvider
	{
	public:
		GoogleProvider(std::string baseUrl, std::string apiKey, std::string defaultModel);
		~GoogleProvider() override = default;

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
		std::string m_BaseUrl;
		std::string m_ApiKey;
		std::string m_DefaultModel;
	};
}
