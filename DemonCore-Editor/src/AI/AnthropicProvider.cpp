// AnthropicProvider.cpp -- provider for Anthropic's Claude Messages API.
//
// Wire format: https://docs.anthropic.com/claude/reference/messages-post
// Uses the /v1/messages endpoint with Anthropic-specific header names
// (x-api-key, anthropic-version, etc.).
#include "wlpch.h"
#include "AnthropicProvider.h"
#include "../Panels/HttpClient.h"
#include "../Panels/JsonMini.h"
#include "../Panels/AIChatPanel.h"

#include <functional>

namespace Wasteland
{
	// Anthropic Messages API provider.
	//
	// Differences from OpenAI format:
	//   - x-api-key header instead of Authorization: Bearer
	//   - anthropic-version header required
	//   - "messages" array does not include system role; system is a top-level field
	//   - "max_tokens" is required
	//   - tool use uses "tools" array with "input_schema" instead of "parameters"
	//   - streaming uses SSE events: message_start, content_block_start, content_block_delta, message_stop
	AnthropicProvider::AnthropicProvider(std::string baseUrl, std::string apiKey, std::string defaultModel)
		: m_BaseUrl(std::move(baseUrl)), m_ApiKey(std::move(apiKey)), m_DefaultModel(std::move(defaultModel))
	{
	}

	ProviderResult AnthropicProvider::Chat(const ProviderRequest &request, ProviderStreamCallback onChunk)
	{
		std::string endpoint = m_BaseUrl;
		while (!endpoint.empty() && endpoint.back() == '/')
			endpoint.pop_back();
		endpoint += "/v1/messages";

		HttpHeaders headers;
		// Anthropic requires x-api-key header.
		if (!m_ApiKey.empty())
			headers.emplace_back("x-api-key", m_ApiKey);
		headers.emplace_back("anthropic-version", "2023-06-01");
		headers.emplace_back("content-type", "application/json");

		// Build Anthropic-format request body.
		// Top-level fields: model, max_tokens, system (optional), messages, tools (optional), stream
		std::string body = "{\"model\":\"" + JsonMini::Escape(request.Model) + "\",\"max_tokens\":4096,\"stream\":";
		body += request.Stream ? "true" : "false";
		body += ",\"messages\":[";

		bool first = true;
		// Anthropic does not put system in the messages array; it is a top-level field.
		if (!request.SystemPrompt.empty())
		{
			// We will insert system after the opening brace, before messages.
			// Build messages first, then patch.
		}

		size_t start = request.History.size() > request.MaxHistory ? request.History.size() - request.MaxHistory : 0;
		for (size_t i = start; i < request.History.size(); i++)
		{
			const auto &m = request.History[i];
			if (m.Role == "error")
				continue;
			if (m.Role == "tool")
			{
				if (!first)
					body += ",";
				// Anthropic tool_result role.
				body += "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"tool_use_id_placeholder\",\"content\":\"" + JsonMini::Escape(m.Text) + "\"}]}";
				first = false;
				continue;
			}
			if (!first)
				body += ",";
			std::string role = (m.Role == "user") ? "user" : "assistant";
			body += "{\"role\":\"" + role + "\",\"content\":\"" + JsonMini::Escape(m.Text) + "\"}";
			first = false;
		}
		body += "]}";

		// Inject system prompt before messages if present.
		if (!request.SystemPrompt.empty())
		{
			// Find the position of ",\"messages\"" and insert system before it.
			std::string sysField = ",\"system\":\"" + JsonMini::Escape(request.SystemPrompt) + "\"";
			size_t msgPos = body.find(",\"messages\"");
			if (msgPos != std::string::npos)
				body.insert(msgPos, sysField);
		}

		// Attach tool definitions if provided.
		if (!request.ToolDefinitionsJson.empty())
		{
			// Anthropic uses "tools" array with input_schema.
			// For now we pass the OpenAI-format tools through; the model
			// will need to handle the schema difference. A full translation
			// layer would map "parameters" -> "input_schema".
			size_t closeBrace = body.rfind('}');
			if (closeBrace != std::string::npos)
			{
				body.insert(closeBrace, ",\"tools\":" + request.ToolDefinitionsJson);
			}
		}

		long status = 0;
		std::string streamError;
		std::string parseBuf;

		auto callback = [&parseBuf, &streamError, onChunk](const char *data, size_t len)
		{
			parseBuf.append(data, len);
			size_t pos = 0;
			while ((pos = parseBuf.find('\n')) != std::string::npos)
			{
				std::string line = parseBuf.substr(0, pos);
				parseBuf.erase(0, pos + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.rfind("data:", 0) != 0)
					continue;
				std::string payload = line.substr(5);
				size_t b = payload.find_first_not_of(" \t");
				payload = (b == std::string::npos) ? "" : payload.substr(b);
				if (payload.empty() || payload == "[DONE]")
					continue;

				// Anthropic streaming events have a "type" field.
				std::string eventType;
				if (JsonMini::ExtractString(payload, "type", eventType))
				{
					if (eventType == "content_block_delta")
					{
						std::string delta;
						if (JsonMini::ExtractString(payload, "delta", delta))
						{
							std::string text;
							if (JsonMini::ExtractString(delta, "text", text))
							{
								if (!onChunk(text))
									return false;
							}
						}
					}
					else if (eventType == "error")
					{
						std::string msg;
						if (JsonMini::ExtractString(payload, "message", msg))
							streamError = msg;
					}
				}
			}
			return true;
		};

		std::string reqError = HttpClient::PostStream(endpoint, body, headers, callback, status);

		ProviderResult result;
		result.StatusCode = status;
		if (!reqError.empty())
		{
			result.Succeeded = false;
			result.Error = reqError;
		}
		else if (!streamError.empty())
		{
			result.Succeeded = false;
			result.Error = streamError;
		}
		else
		{
			result.Succeeded = true;
		}
		return result;
	}

	std::string AnthropicProvider::GetName() const { return "Anthropic"; }
	std::string AnthropicProvider::GetDefaultModel() const { return m_DefaultModel; }
	bool AnthropicProvider::IsAuthenticated() const { return !m_ApiKey.empty(); }
	bool AnthropicProvider::RefreshAuth() { return IsAuthenticated(); }

	std::vector<std::string> AnthropicProvider::GetModels() const
	{
		// Anthropic does not expose a public model-listing endpoint.
		// Return a static list only when the user has actually configured
		// an API key; otherwise return empty so the dropdown does not
		// advertise models that may not be available to this account.
		if (m_ApiKey.empty())
			return {};

		return {
			"claude-opus-4-20250514",
			"claude-sonnet-4-20250514",
			"claude-3-5-sonnet-20241022",
			"claude-3-5-haiku-20241022"};
	}

	void AnthropicProvider::SetApiKey(std::string key) { m_ApiKey = std::move(key); }
	void AnthropicProvider::SetBaseUrl(std::string url) { m_BaseUrl = std::move(url); }
	void AnthropicProvider::SetDefaultModel(std::string model) { m_DefaultModel = std::move(model); }
	const std::string &AnthropicProvider::GetApiKey() const { return m_ApiKey; }
	const std::string &AnthropicProvider::GetBaseUrl() const { return m_BaseUrl; }
}
