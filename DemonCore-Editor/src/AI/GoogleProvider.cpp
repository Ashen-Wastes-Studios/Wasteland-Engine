// GoogleProvider.cpp -- provider for Google's Gemini generateContent API.
//
// Wire format: https://ai.google.dev/api/rest/v1beta/models/generateContent
// Uses the REST API (not the SDK) to avoid external dependencies.
#include "wlpch.h"
#include "GoogleProvider.h"
#include "../Panels/HttpClient.h"
#include "../Panels/JsonMini.h"
#include "../Panels/AIChatPanel.h"

#include <functional>

namespace Wasteland
{
	// Gemini generateContent API provider.
	//
	// Differences from OpenAI format:
	//   - API key is passed as ?key= query parameter (or via x-goog-api-key header)
	//   - Endpoint: /v1beta/models/{model}:generateContent?alt=sse
	//   - Request body uses "contents" array with "parts" array
	//   - System instruction is a separate "systemInstruction" field
	//   - Tool definitions use "functionDeclarations" with "parameters" schema
	//   - Streaming SSE uses "candidates" -> "content" -> "parts" -> "text"
	GoogleProvider::GoogleProvider(std::string baseUrl, std::string apiKey, std::string defaultModel)
		: m_BaseUrl(std::move(baseUrl)), m_ApiKey(std::move(apiKey)), m_DefaultModel(std::move(defaultModel))
	{
	}

	ProviderResult GoogleProvider::Chat(const ProviderRequest &request, ProviderStreamCallback onChunk)
	{
		// Build endpoint: {baseUrl}/v1beta/models/{model}:generateContent?alt=sse
		std::string endpoint = m_BaseUrl;
		while (!endpoint.empty() && endpoint.back() == '/')
			endpoint.pop_back();
		endpoint += "/v1beta/models/" + request.Model + ":generateContent?alt=sse";

		HttpHeaders headers;
		headers.emplace_back("content-type", "application/json");
		// Prefer x-goog-api-key header; fall back to query parameter below.
		if (!m_ApiKey.empty())
			headers.emplace_back("x-goog-api-key", m_ApiKey);

		// Build Gemini-format request body.
		std::string body = "{\"contents\":[";

		bool first = true;
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
				// Tool results become functionResponse parts.
				body += "{\"role\":\"user\",\"parts\":[{\"functionResponse\":{\"name\":\"" + JsonMini::Escape(m.ToolName) + "\",\"response\":{\"result\":\"" + JsonMini::Escape(m.Text) + "\"}}}]}";
				first = false;
				continue;
			}
			if (!first)
				body += ",";
			std::string role = (m.Role == "user") ? "user" : "model";
			body += "{\"role\":\"" + role + "\",\"parts\":[{\"text\":\"" + JsonMini::Escape(m.Text) + "\"}]}";
			first = false;
		}
		body += "]}";

		// System instruction as a separate field.
		if (!request.SystemPrompt.empty())
		{
			body.insert(body.rfind('}'), ",\"systemInstruction\":{\"parts\":[{\"text\":\"" + JsonMini::Escape(request.SystemPrompt) + "\"}]}");
		}

		// Generation config.
		body.insert(body.rfind('}'), ",\"generationConfig\":{\"temperature\":" + std::to_string(request.Temperature) + "}");

		// Attach tool definitions if provided.
		if (!request.ToolDefinitionsJson.empty())
		{
			// Gemini uses "functionDeclarations" inside a "tools" array.
			// For now we pass the OpenAI-format tools through; the schema
			// translation from "parameters" to the nested format would be
			// done here in a full implementation.
			size_t closeBrace = body.rfind('}');
			if (closeBrace != std::string::npos)
			{
				body.insert(closeBrace, ",\"tools\":[{\"functionDeclarations\":" + request.ToolDefinitionsJson + "}]");
			}
		}

		// If no API key header, append as query parameter.
		if (!m_ApiKey.empty())
		{
			std::string separator = (endpoint.find('?') != std::string::npos) ? "&" : "?";
			endpoint += separator + "key=" + m_ApiKey;
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

				// Navigate into candidates[0].content.parts[*].text
				std::string candidates;
				if (JsonMini::ExtractString(payload, "candidates", candidates))
				{
					// Find the first "parts" array and extract text from each part.
					size_t partsPos = candidates.find("\"parts\"");
					if (partsPos != std::string::npos)
					{
						size_t textPos = candidates.find("\"text\"", partsPos);
						while (textPos != std::string::npos)
						{
							size_t colon = textPos + 6;
							size_t quoteStart = candidates.find('"', colon + 1);
							if (quoteStart != std::string::npos)
							{
								size_t quoteEnd = candidates.find('"', quoteStart + 1);
								if (quoteEnd != std::string::npos)
								{
									std::string text = candidates.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
									if (!onChunk(text))
										return false;
								}
							}
							textPos = candidates.find("\"text\"", textPos + 1);
						}
					}
				}

				// Check for errors.
				if (payload.find("\"error\"") != std::string::npos)
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

	std::string GoogleProvider::GetName() const { return "Google"; }
	std::string GoogleProvider::GetDefaultModel() const { return m_DefaultModel; }
	bool GoogleProvider::IsAuthenticated() const { return !m_ApiKey.empty(); }
	bool GoogleProvider::RefreshAuth() { return IsAuthenticated(); }

	std::vector<std::string> GoogleProvider::GetModels() const
	{
		std::vector<std::string> models;
		if (m_BaseUrl.empty() || m_ApiKey.empty())
			return models;

		std::string endpoint = m_BaseUrl;
		while (!endpoint.empty() && endpoint.back() == '/')
			endpoint.pop_back();
		endpoint += "/v1beta/models?key=" + m_ApiKey;

		HttpResult result = HttpClient::Get(endpoint, {});
		if (!result.Succeeded || result.Body.empty())
			return models;

		std::string modelsJson;
		if (!JsonMini::ExtractString(result.Body, "models", modelsJson))
			return models;

		std::vector<std::string> objects = JsonMini::SplitArrayObjects(modelsJson);
		for (const auto &obj : objects)
		{
			std::string name;
			if (JsonMini::ExtractString(obj, "name", name) && !name.empty())
			{
				// Strip leading "models/" prefix.
				const std::string prefix = "models/";
				if (name.rfind(prefix, 0) == 0)
					name = name.substr(prefix.size());
				models.push_back(name);
			}
		}
		return models;
	}

	void GoogleProvider::SetApiKey(std::string key) { m_ApiKey = std::move(key); }
	void GoogleProvider::SetBaseUrl(std::string url) { m_BaseUrl = std::move(url); }
	void GoogleProvider::SetDefaultModel(std::string model) { m_DefaultModel = std::move(model); }
	const std::string &GoogleProvider::GetApiKey() const { return m_ApiKey; }
	const std::string &GoogleProvider::GetBaseUrl() const { return m_BaseUrl; }
}
