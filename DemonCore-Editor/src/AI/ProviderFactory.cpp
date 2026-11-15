// ProviderFactory.cpp -- creates IAIProvider instances from ApiKeyStore settings.
//
// Knows the mapping from provider name to concrete class and default model.
// All providers are created on demand; the factory owns no state.
#include "wlpch.h"
#include "IAIProvider.h"
#include "OpenAICompatibleProvider.h"
#include "AnthropicProvider.h"
#include "GoogleProvider.h"
#include "../Panels/ApiKeyStore.h"

namespace Wasteland
{
	// Create a provider for the given provider name using settings from the
	// ApiKeyStore. Returns nullptr if the name is not recognised.
	//
	// Thread-safety: ApiKeyStore::Instance() is read-only here, and the
	// returned provider is immutable after construction, so this is safe
	// to call from any thread.
	IAIProvider *ProviderFactory_Create(const std::string &providerName)
	{
		ApiKeyStore &store = ApiKeyStore::Instance();

		// Normalise to lowercase for comparison.
		std::string lower = providerName;
		for (char &c : lower)
			c = static_cast<char>(tolower(c));

		if (lower == "openai" || lower == "openrouter" || lower == "custom" || lower == "local")
		{
			// OpenAI-compatible family.
			std::string name = lower;
			if (name == "custom")
				name = "Custom";
			else if (name == "local")
				name = "Local";
			else if (name == "openrouter")
				name = "OpenRouter";
			else
				name = "OpenAI";

			std::string baseUrl = store.OpenAIBaseURL.empty() ? "https://api.openai.com/v1" : store.OpenAIBaseURL;
			std::string model = store.OpenAIModel.empty() ? "gpt-6.1-sol" : store.OpenAIModel;
			std::string key = store.OpenAIKey;

			return new OpenAICompatibleProvider(name, baseUrl, key, model);
		}
		else if (lower == "hermes")
		{
			std::string baseUrl = store.HermesBaseURL.empty() ? "https://inference-api.nousresearch.com/v1" : store.HermesBaseURL;
			std::string model = store.HermesModel.empty() ? "step-3.7-flash" : store.HermesModel;
			std::string key = store.HermesKey;

			return new OpenAICompatibleProvider("Hermes", baseUrl, key, model);
		}
		else if (lower == "anthropic" || lower == "claude")
		{
			std::string baseUrl = "https://api.anthropic.com";
			std::string model = "opus-5.5";
			std::string key = store.AnthropicKey;

			return new AnthropicProvider(baseUrl, key, model);
		}
		else if (lower == "google" || lower == "gemini")
		{
			std::string baseUrl = "https://generativelanguage.googleapis.com/v1beta";
			std::string model = "gemini-3.8-flash";
			std::string key = store.GoogleKey;

			return new GoogleProvider(baseUrl, key, model);
		}

		return nullptr;
	}
}
