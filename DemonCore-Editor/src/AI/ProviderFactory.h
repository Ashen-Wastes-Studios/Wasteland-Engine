// ProviderFactory.h -- creates IAIProvider instances from ApiKeyStore settings.
//
// Knows the mapping from provider name to concrete class and default model.
// All providers are created on demand; the factory owns no state.
#pragma once

#include <string>

namespace Wasteland
{
	class IAIProvider;
	class ApiKeyStore;

	// Create a provider for the given provider name using settings from the
	// ApiKeyStore. Returns nullptr if the name is not recognised.
	IAIProvider *ProviderFactory_Create(const std::string &providerName);
}
