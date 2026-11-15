// OpenAICompatibleProvider.cpp -- provider for OpenAI-compatible chat APIs.
//
// Covers: OpenAI (api.openai.com), OpenRouter (openrouter.ai), Hermes/Nous,
// and any local server (LM Studio, Ollama, vLLM, llama.cpp) that exposes
// /chat/completions in OpenAI format.
//
// The wire format is the standard OpenAI chat-completions request/response,
// so the implementation is a thin wrapper over HttpClient::PostStream.
#include "wlpch.h"
#include "OpenAICompatibleProvider.h"
#include "../Panels/HttpClient.h"
#include "../Panels/JsonMini.h"

#include <string>
#include <vector>
#include "../Panels/AIChatPanel.h"

// <functional> for the streaming callback passed into PostStream.
#include <functional>

namespace Wasteland
{
	// Builds and sends an OpenAI-compatible /chat/completions request.
	//
	// Thread-safety: this object is immutable after construction, so Chat()
	// can be called from any number of worker threads concurrently. The only
	// shared state is the callback, which is captured by value in the lambda.
	OpenAICompatibleProvider::OpenAICompatibleProvider(std::string name, std::string baseUrl, std::string apiKey, std::string defaultModel)
		: m_Name(std::move(name)), m_BaseUrl(std::move(baseUrl)), m_ApiKey(std::move(apiKey)), m_DefaultModel(std::move(defaultModel))
	{
	}

	ProviderResult OpenAICompatibleProvider::Chat(const ProviderRequest &request, ProviderStreamCallback onChunk)
	{
		// Trailing slashes stripped before appending the path, so a base URL
		// ending in '/' does not produce a double slash.
		std::string endpoint = m_BaseUrl;
		while (!endpoint.empty() && endpoint.back() == '/')
			endpoint.pop_back();
		endpoint += "/chat/completions";

		// Bearer auth added only when a key exists, which is what lets an
		// unauthenticated local server work.
		HttpHeaders headers;
		if (!m_ApiKey.empty())
			headers.emplace_back("Authorization", "Bearer " + m_ApiKey);

		// Build the request body by string concatenation, matching the
		// existing pattern in AIChatPanel::BuildRequestBody. JsonMini::Escape
		// is used for every user-supplied string.
		std::string body = "{\"model\":\"" + JsonMini::Escape(request.Model) + "\",\"stream\":";
		body += request.Stream ? "true" : "false";
		body += ",\"temperature\":" + std::to_string(request.Temperature) + ",\"messages\":[";

		bool first = true;
		if (!request.SystemPrompt.empty())
		{
			body += "{\"role\":\"system\",\"content\":\"" + JsonMini::Escape(request.SystemPrompt) + "\"}";
			first = false;
		}

		// Trim history to the LAST maxHistory entries, not the first.
		size_t start = request.History.size() > request.MaxHistory ? request.History.size() - request.MaxHistory : 0;
		for (size_t i = start; i < request.History.size(); i++)
		{
			const auto &m = request.History[i];
			// Error messages are never sent to the model; re-sending a
			// failure would teach the model that it had failed.
			if (m.Role == "error")
				continue;
			// Tool results are folded into the transcript as assistant
			// messages, keeping the request compatible with servers that
			// do not implement the tool-calling API.
			if (m.Role == "tool")
			{
				if (!first)
					body += ",";
				body += "{\"role\":\"assistant\",\"content\":\"[Tool: " + JsonMini::Escape(m.ToolName) + " result]\n" + JsonMini::Escape(m.Text) + "\"}";
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

		// If the caller supplied tool definitions, attach them. OpenAI
		// function calling uses the "tools" / "tool_choice" keys.
		if (!request.ToolDefinitionsJson.empty())
		{
			// Insert before the closing brace. body currently ends with "]}"
			// from the messages array, so we back up two characters and
			// append the tools block.
			body.pop_back();
			body.pop_back();
			body += ",\"tools\":" + request.ToolDefinitionsJson + ",\"tool_choice\":\"auto\"}]";
		}

		long status = 0;
		std::string streamError;

		// Buffers partial SSE lines across chunk boundaries.
		std::string parseBuf;

		// The streaming callback mirrors the one in AIChatPanel::LaunchWorker.
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
				std::string delta;
				if (JsonMini::ExtractString(payload, "delta", delta))
				{
					// Extract the content field from the delta object.
					std::string content;
					if (JsonMini::ExtractString(delta, "content", content))
					{
						// Return false from onChunk to abort.
						if (!onChunk(content))
							return false;
					}
				}
				else if (payload.find("\"error\"") != std::string::npos)
				{
					std::string msg;
					if (JsonMini::ExtractString(payload, "message", msg))
						streamError = msg;
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

	std::string OpenAICompatibleProvider::GetName() const { return m_Name; }
	std::string OpenAICompatibleProvider::GetDefaultModel() const { return m_DefaultModel; }
	bool OpenAICompatibleProvider::IsAuthenticated() const { return !m_ApiKey.empty(); }
	bool OpenAICompatibleProvider::RefreshAuth() { return IsAuthenticated(); }

	std::vector<std::string> OpenAICompatibleProvider::GetModels() const
	{
		std::vector<std::string> models;
		if (m_BaseUrl.empty() || m_ApiKey.empty())
			return models;

		std::string endpoint = m_BaseUrl;
		while (!endpoint.empty() && endpoint.back() == '/')
			endpoint.pop_back();
		endpoint += "/v1/models";

		HttpHeaders headers;
		headers.emplace_back("Authorization", "Bearer " + m_ApiKey);

		HttpResult result = HttpClient::Get(endpoint, headers);
		if (!result.Succeeded || result.Body.empty())
			return models;

		std::string data;
		if (!JsonMini::ExtractString(result.Body, "data", data))
			return models;

		std::vector<std::string> objects = JsonMini::SplitArrayObjects(data);
		for (const auto &obj : objects)
		{
			std::string id;
			if (JsonMini::ExtractString(obj, "id", id) && !id.empty())
				models.push_back(id);
		}
		return models;
	}

	void OpenAICompatibleProvider::SetApiKey(std::string key) { m_ApiKey = std::move(key); }
	void OpenAICompatibleProvider::SetBaseUrl(std::string url) { m_BaseUrl = std::move(url); }
	void OpenAICompatibleProvider::SetDefaultModel(std::string model) { m_DefaultModel = std::move(model); }
	const std::string &OpenAICompatibleProvider::GetApiKey() const { return m_ApiKey; }
	const std::string &OpenAICompatibleProvider::GetBaseUrl() const { return m_BaseUrl; }
}
