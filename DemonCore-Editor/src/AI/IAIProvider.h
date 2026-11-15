// IAIProvider.h -- abstraction for any LLM provider the panel can talk to.
//
// A provider is anything that accepts an OpenAI-compatible or provider-specific
// chat request and returns streamed deltas. The panel never talks to a provider
// directly; it goes through IAIProvider so the rest of the code does not care
// whether the wire format is OpenAI, Anthropic, or Google.
//
// Each provider is responsible for:
//   - its own endpoint URL and headers
//   - translating the panel's canonical request into the wire format
//   - translating wire-format deltas back into the panel's canonical delta
//
// The panel supplies:
//   - model name
//   - temperature
//   - system prompt
//   - conversation history
//   - tool definitions (JSON schema)
//   - streaming callback
//
// A provider that does not support streaming should emit a single delta and
// close the stream; the panel's SSE parser handles both cases identically.
#pragma once

// <string> for every name and payload field.
#include <string>
// <vector> for the conversation history and tool definitions.
#include <vector>
// <functional> for the streaming callback, so the panel can capture state.
#include <functional>

// Forward declaration rather than include: IAIProvider has no reason to
// depend on the panel's ChatMessage, and breaking the cycle keeps the header
// free of panel details.
namespace Wasteland { struct ChatMessage; }

namespace Wasteland
{
	// The streaming callback signature. onChunk is called with each delta's
	// content string as it arrives. Returning false aborts the stream.
	using ProviderStreamCallback = std::function<bool(const std::string &delta)>;

	// A chat completion request in provider-neutral form. Every field the
	// panel needs to send is here; each provider maps these into its own
	// wire format.
	struct ProviderRequest
	{
		std::string Model;
		float Temperature = 0.7f;
		std::string SystemPrompt;
		std::vector<ChatMessage> History;
		size_t MaxHistory = 30;
		// Tool definitions in OpenAI function-calling JSON schema shape.
		// Providers that do not support tool calls ignore this field.
		std::string ToolDefinitionsJson;
		// When true, the provider should stream deltas via onChunk.
		bool Stream = true;
	};

	// The result of a provider call. StatusCode is the HTTP status (0 for
	// transport failure). Error is empty on success.
	struct ProviderResult
	{
		bool Succeeded = false;
		long StatusCode = 0;
		std::string Error;
	};

	// The abstraction boundary between the panel and any LLM provider.
	//
	// Implementations must be thread-safe: the panel calls Chat() from a
	// worker thread while the UI thread may read provider state (current
	// model name, status) concurrently.
	class IAIProvider
	{
	public:
		virtual ~IAIProvider() = default;

		// Send a chat request and stream deltas through onChunk. Returns
		// ProviderResult describing the overall outcome. onChunk may be
		// called zero or more times; returning false from onChunk aborts
		// the request early.
		virtual ProviderResult Chat(const ProviderRequest &request, ProviderStreamCallback onChunk) = 0;

		// A human-readable name for the provider, shown in the UI.
		virtual std::string GetName() const = 0;

		// The default model for this provider, used when the user has not
		// chosen one.
		virtual std::string GetDefaultModel() const = 0;

		// Return true if this provider currently has valid credentials
		// (API key or active OAuth token). Used for the status indicator.
		virtual bool IsAuthenticated() const = 0;

		// Attempt to refresh credentials (OAuth token refresh, etc.).
		// Returns true if credentials are valid after the call.
		virtual bool RefreshAuth() = 0;

		// Fetch the list of model IDs available from this provider.
		// Returns an empty vector if the provider does not support listing
		// models or the request fails.
		virtual std::vector<std::string> GetModels() const { return {}; }
	};
}
