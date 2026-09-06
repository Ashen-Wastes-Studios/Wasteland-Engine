#include "wlpch.h"
#include "AIChatPanel.h"
#include "ApiKeyStore.h"
#include "HttpClient.h"
#include "JsonMini.h"

#include <imgui/imgui.h>

#include <fstream>
#include <sstream>

namespace Wasteland
{

	AIChatPanel::AIChatPanel()
	{
	}

	AIChatPanel::~AIChatPanel()
	{
		if (m_Worker.joinable())
			m_Worker.join();
	}

	std::string AIChatPanel::BuildRequestBody(const std::string &model, float temperature,
		const std::string &systemPrompt, const std::vector<ChatMessage> &history, size_t maxHistory, bool stream)
	{
		std::string body = "{\"model\":\"" + JsonMini::Escape(model) + "\",\"stream\":" +
			(stream ? "true" : "false") + ",\"temperature\":" +
			std::to_string(temperature) + ",\"messages\":[";
		bool first = true;
		if (!systemPrompt.empty())
		{
			body += "{\"role\":\"system\",\"content\":\"" + JsonMini::Escape(systemPrompt) + "\"}";
			first = false;
		}
		// Bound request size: recent exchanges only (tighter when engine context is included).
		size_t start = history.size() > maxHistory ? history.size() - maxHistory : 0;
		for (size_t i = start; i < history.size(); i++)
		{
			const auto &m = history[i];
			std::string role = (m.Role == "user") ? "user" : "assistant";
			if (m.Role == "error")
				continue;
			if (!first)
				body += ",";
			body += "{\"role\":\"" + role + "\",\"content\":\"" + JsonMini::Escape(m.Text) + "\"}";
			first = false;
		}
		body += "]}";
		return body;
	}

	bool AIChatPanel::ParseResponseContent(const std::string &body, std::string &outContent, std::string &outError)
	{
		// Surface API errors first: {"error": {"message": "..."}}
		size_t errPos = body.find("\"error\"");
		if (errPos != std::string::npos)
		{
			std::string sub = body.substr(errPos);
			if (JsonMini::ExtractString(sub, "message", outError))
				return false;
		}
		size_t chPos = body.find("\"choices\"");
		if (chPos == std::string::npos)
		{
			outError = "Unexpected response (no choices).";
			return false;
		}
		std::string sub = body.substr(chPos);
		if (!JsonMini::ExtractString(sub, "content", outContent) || outContent.empty())
		{
			std::string refusal;
			if (JsonMini::ExtractString(sub, "refusal", refusal) && !refusal.empty())
				outContent = refusal;
			else
			{
				outError = "Empty response content.";
				return false;
			}
		}
		return true;
	}

	void AIChatPanel::LoadEngineContext()
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path path = fs::path("assets") / "ai" / "engine_reference.md";
		auto stamp = fs::last_write_time(path, ec);
		if (ec)
		{
			m_EngineContextCache.clear();
			m_EngineContextChars = 0;
			m_EngineContextHaveTime = false;
			m_EngineContextError = "assets/ai/engine_reference.md not found.";
			return;
		}
		if (m_EngineContextHaveTime && stamp == m_EngineContextTime && !m_EngineContextCache.empty())
			return; // Unchanged.
		std::ifstream file(path);
		if (!file.is_open())
		{
			m_EngineContextCache.clear();
			m_EngineContextChars = 0;
			m_EngineContextError = "Could not open assets/ai/engine_reference.md.";
			return;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		m_EngineContextCache = ss.str();
		m_EngineContextChars = (long)m_EngineContextCache.size();
		m_EngineContextError.clear();
		m_EngineContextTime = stamp;
		m_EngineContextHaveTime = true;
	}

	void AIChatPanel::LaunchWorker()
	{
		if (m_Worker.joinable())
			m_Worker.join();

		ApiKeyStore &store = ApiKeyStore::Instance();
		std::string key = store.OpenAIKey;
		std::string base = store.OpenAIBaseURL;
		std::string model = store.OpenAIModel;
		float temp = store.Temperature;
		std::string system = store.SystemPrompt;
		std::vector<ChatMessage> history;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			history = m_Messages;
		}
		// Ground the model in the engine's scripting API when enabled.
		std::string effectiveSystem = system;
		size_t maxHistory = 30;
		if (m_UseEngineContext && !m_EngineContextCache.empty())
		{
			effectiveSystem += "\n\n--- Wasteland engine scripting reference ---\n" + m_EngineContextCache;
			maxHistory = 12; // Leave context-window room for local models.
		}
		// The new user message was already appended before launch.
		m_Sending.store(true);
		m_Status = "Sending...";

		m_Worker = std::thread([this, key, base, model, temp, effectiveSystem, history, maxHistory]() {
			std::string endpoint = base;
			while (!endpoint.empty() && endpoint.back() == '/')
				endpoint.pop_back();
			endpoint += "/chat/completions";

			HttpHeaders headers;
			if (!key.empty())
				headers.emplace_back("Authorization", "Bearer " + key);

			// Placeholder message, filled in as tokens stream in.
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.push_back({"assistant", ""});
				m_Status = "Receiving...";
				m_ScrollToBottom = true;
			}

			auto appendDelta = [this](const std::string &delta) {
				if (delta.empty())
					return;
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
				{
					m_Messages.back().Text += delta;
					m_ScrollToBottom = true;
				}
			};

			// SSE framing: "data: {...}\n" lines, "[DONE]" at the end.
			std::string parseBuf;
			std::string streamError;
			long status = 0;
			std::string reqError = HttpClient::PostStream(
				endpoint, BuildRequestBody(model, temp, effectiveSystem, history, maxHistory, true), headers,
				[&parseBuf, &appendDelta, &streamError](const char *data, size_t len) {
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
						size_t b = payload.find_first_not_of(" 	");
						payload = (b == std::string::npos) ? "" : payload.substr(b);
						if (payload.empty() || payload == "[DONE]")
							continue;
						std::string delta;
						if (JsonMini::ExtractString(payload, "content", delta))
							appendDelta(delta);
						else if (payload.find("\"error\"") != std::string::npos)
						{
							std::string msg;
							if (JsonMini::ExtractString(payload, "message", msg))
								streamError = msg;
						}
					}
				},
				status);

			std::lock_guard<std::mutex> lock(m_Mutex);
			bool haveText = !m_Messages.empty() && m_Messages.back().Role == "assistant" &&
				!m_Messages.back().Text.empty();
			auto setError = [this](const std::string &err) {
				if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
					m_Messages.back() = {"error", err};
				else
					m_Messages.push_back({"error", err});
				m_Status = err;
			};
			if (!reqError.empty() && !haveText)
				setError(reqError);
			else if (!streamError.empty() && !haveText)
				setError(streamError);
			else if (!reqError.empty())
				m_Status = "Idle (stream ended early: " + reqError + ").";
			else
				m_Status = "Idle.";
			m_Sending.store(false);
			m_ScrollToBottom = true;
		});
	}

	void AIChatPanel::SendCurrentInput()
	{
		if (m_Sending.load())
			return;
		std::string text = m_InputBuf;
		// trim
		size_t b = text.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			return;
		size_t e = text.find_last_not_of(" \t\r\n");
		text = text.substr(b, e - b + 1);
		if (text.empty())
			return;

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Messages.push_back({"user", text});
		}
		m_InputBuf[0] = '\0';
		m_ScrollToBottom = true;
		LaunchWorker();
	}

	void AIChatPanel::OnImGuiRender()
	{
		if (!m_Visible)
			return;

		LoadEngineContext();

		if (!m_SettingsLoaded)
		{
			ApiKeyStore &store = ApiKeyStore::Instance();
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.OpenAIKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.OpenAIBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.OpenAIModel.c_str(), _TRUNCATE);
			strncpy_s(m_SystemBuf, sizeof(m_SystemBuf), store.SystemPrompt.c_str(), _TRUNCATE);
			m_Temp = store.Temperature;
			m_SettingsLoaded = true;
		}

		ImGui::Begin("AI Chatbot", &m_Visible);

		if (ImGui::CollapsingHeader("Settings & API Key"))
		{
			ImGui::InputText("Base URL", m_BaseURLBuf, sizeof(m_BaseURLBuf));
			ImGui::InputText("Model", m_ModelBuf, sizeof(m_ModelBuf));
			ImGui::SliderFloat("Temperature", &m_Temp, 0.0f, 2.0f);
			ImGui::InputTextMultiline("System prompt", m_SystemBuf, sizeof(m_SystemBuf), ImVec2(-FLT_MIN, 60));

			ImGuiInputTextFlags keyFlags = m_ShowKey ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
			ImGui::InputText("API key", m_KeyBuf, sizeof(m_KeyBuf), keyFlags);
			ImGui::SameLine();
			ImGui::Checkbox("Show", &m_ShowKey);
			ImGui::TextWrapped("Defaults to a local LM Studio server (localhost:1234, key left empty). Point the base URL at OpenAI or any OpenAI-compatible API instead, add its key, and Save. Key file: %%APPDATA%%\\Wasteland\\ai_keys.cfg (never committed).");

			if (ImGui::Button("Save settings"))
			{
				ApiKeyStore &store = ApiKeyStore::Instance();
				store.OpenAIKey = m_KeyBuf;
				store.OpenAIBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "http://localhost:1234/v1";
				store.OpenAIModel = m_ModelBuf[0] ? m_ModelBuf : "local-model";
				store.SystemPrompt = m_SystemBuf;
				store.Temperature = m_Temp;
				store.Save();
				m_Status = "Settings saved.";
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear key"))
			{
				m_KeyBuf[0] = '\0';
				ApiKeyStore::Instance().OpenAIKey.clear();
				ApiKeyStore::Instance().Save();
				m_Status = "API key cleared.";
			}
		}

		if (ImGui::CollapsingHeader("Engine context"))
		{
			ImGui::Checkbox("Include Wasteland scripting reference", &m_UseEngineContext);
			if (m_EngineContextError.empty())
				ImGui::TextWrapped("Reference loaded: %ld chars from assets/ai/engine_reference.md — edit that file to teach it more. History trims while on, for small local context windows.", m_EngineContextChars);
			else
				ImGui::TextWrapped("Reference unavailable: %s", m_EngineContextError.c_str());
		}

		ImGui::Separator();

		// History (locked copy for rendering).
		std::vector<ChatMessage> messages;
		std::string status;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			messages = m_Messages;
			status = m_Status;
		}
		float footerH = ImGui::GetFrameHeightWithSpacing() * 2 + 8;
		ImGui::BeginChild("##chat_scroll", ImVec2(0, -footerH), true);
		for (auto &m : messages)
		{
			if (m.Role == "user")
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
				ImGui::TextUnformatted("You:");
				ImGui::PopStyleColor();
			}
			else if (m.Role == "error")
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
				ImGui::TextUnformatted("Error:");
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 1.0f, 0.65f, 1.0f));
				ImGui::TextUnformatted("Assistant:");
				ImGui::PopStyleColor();
			}
			ImGui::TextWrapped("%s", m.Text.c_str());
			ImGui::Separator();
		}
		if (m_ScrollToBottom)
		{
			ImGui::SetScrollHereY(1.0f);
			m_ScrollToBottom = false;
		}
		ImGui::EndChild();

		ImGui::TextWrapped("Status: %s", status.c_str());

		bool sending = m_Sending.load();
		ImGui::BeginDisabled(sending);
		bool submit = ImGui::InputText("##chat_input", m_InputBuf, sizeof(m_InputBuf),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Send") || submit)
			SendCurrentInput();
		ImGui::SameLine();
		if (ImGui::Button("Clear chat"))
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Messages.clear();
			m_Status = "Idle.";
		}
		ImGui::EndDisabled();

		ImGui::End();
	}

}
