#include "wlpch.h"
#include "AIChatPanel.h"
#include "ApiKeyStore.h"
#include "HttpClient.h"
#include "JsonMini.h"

#include <imgui/imgui.h>

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
		const std::string &systemPrompt, const std::vector<ChatMessage> &history)
	{
		std::string body = "{\"model\":\"" + JsonMini::Escape(model) + "\",\"stream\":false,\"temperature\":" +
			std::to_string(temperature) + ",\"messages\":[";
		bool first = true;
		if (!systemPrompt.empty())
		{
			body += "{\"role\":\"system\",\"content\":\"" + JsonMini::Escape(systemPrompt) + "\"}";
			first = false;
		}
		// Bound request size: last 30 exchanges.
		size_t start = history.size() > 30 ? history.size() - 30 : 0;
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
		// The new user message was already appended before launch.
		m_Sending.store(true);
		m_Status = "Sending...";

		m_Worker = std::thread([this, key, base, model, temp, system, history]() {
			std::string endpoint = base;
			while (!endpoint.empty() && endpoint.back() == '/')
				endpoint.pop_back();
			endpoint += "/chat/completions";

			HttpHeaders headers;
			if (!key.empty())
				headers.emplace_back("Authorization", "Bearer " + key);
			HttpResult r = HttpClient::PostJson(endpoint, BuildRequestBody(model, temp, system, history), headers);

			std::lock_guard<std::mutex> lock(m_Mutex);
			if (r.Succeeded)
			{
				std::string content, err;
				if (ParseResponseContent(r.Body, content, err))
				{
					m_Messages.push_back({"assistant", content});
					m_Status = "Idle.";
				}
				else
				{
					m_Messages.push_back({"error", err});
					m_Status = err;
				}
			}
			else
			{
				std::string detail = r.Error;
				if (!r.Body.empty())
				{
					std::string apiErr;
					if (JsonMini::ExtractString(r.Body.substr(0, r.Body.find("\"choices\"") == std::string::npos ? r.Body.size() : r.Body.find("\"choices\"")), "message", apiErr))
						detail += ": " + apiErr;
					else if (detail.find("HTTP") == 0)
						detail += " — " + r.Body.substr(0, 200);
				}
				m_Messages.push_back({"error", detail});
				m_Status = detail;
			}
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
			ImGui::TextWrapped("Works with OpenAI or any OpenAI-compatible server (e.g. a local llama.cpp server — leave the key empty for local servers). Key file: %%APPDATA%%\\Wasteland\\ai_keys.cfg (never committed).");

			if (ImGui::Button("Save settings"))
			{
				ApiKeyStore &store = ApiKeyStore::Instance();
				store.OpenAIKey = m_KeyBuf;
				store.OpenAIBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://api.openai.com/v1";
				store.OpenAIModel = m_ModelBuf[0] ? m_ModelBuf : "gpt-4o-mini";
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
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Clear chat"))
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Messages.clear();
			m_Status = "Idle.";
		}

		ImGui::End();
	}

}
