#include "wlpch.h"
#include "AIChatPanel.h"
#include "ApiKeyStore.h"
#include "HttpClient.h"
#include "JsonMini.h"
#include "ToolServer.h"

#include <imgui/imgui.h>

#include <fstream>
#include <sstream>
#include <algorithm>

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
			(stream ? "true" : "false") + "\",\"temperature\":" +
			std::to_string(temperature) + ",\"messages\":[";
		bool first = true;
		if (!systemPrompt.empty())
		{
			body += "{\"role\":\"system\",\"content\":\"" + JsonMini::Escape(systemPrompt) + "\"}";
			first = false;
		}
		size_t start = history.size() > maxHistory ? history.size() - maxHistory : 0;
		for (size_t i = start; i < history.size(); i++)
		{
			const auto &m = history[i];
			std::string role = (m.Role == "user") ? "user" : "assistant";
			if (m.Role == "error")
				continue;
			if (m.Role == "tool")
			{
				if (!first)
					body += ",";
				body += "{\"role\":\"assistant\",\"content\":\"[Tool: " + JsonMini::Escape(m.ToolName) + " result]\\n" + JsonMini::Escape(m.Text) + "\"}";
				first = false;
				continue;
			}
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

	std::string AIChatPanel::ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs)
	{
		size_t start = text.find("<tool_call");
		if (start == std::string::npos)
			return "";
		size_t nameStart = text.find("name=\"", start);
		if (nameStart == std::string::npos)
			return "";
		nameStart += 6;
		size_t nameEnd = text.find("\"", nameStart);
		if (nameEnd == std::string::npos)
			return "";
		outToolName = text.substr(nameStart, nameEnd - nameStart);

		size_t tagEnd = text.find(">", nameEnd);
		if (tagEnd == std::string::npos)
			return "";
		size_t argsStart = text.find("<args>", tagEnd);
		if (argsStart != std::string::npos)
		{
			argsStart += 6;
			size_t argsEnd = text.find("</args>", argsStart);
			if (argsEnd != std::string::npos)
				outArgs = text.substr(argsStart, argsEnd - argsStart);
		}

		size_t callEnd = text.find("</tool_call>", tagEnd);
		if (callEnd == std::string::npos)
			return "";
		return text.substr(start, callEnd + 12 - start);
	}

	std::string AIChatPanel::StripToolCalls(const std::string &text)
	{
		std::string result = text;
		while (true)
		{
			size_t start = result.find("<tool_call");
			if (start == std::string::npos)
				break;
			size_t end = result.find("</tool_call>", start);
			if (end == std::string::npos)
				break;
			end += 12;
			result.erase(start, end - start);
		}
		return result;
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
			return;
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
		bool agentic = m_AgenticMode;
		std::string toolDefs = ToolServer::Instance().GetToolDefinitionsJson();
		std::string memoryContent;
		if (agentic)
		{
			memoryContent = ToolServer::Instance().ExecuteTool("memory_read", "{}");
		}

		std::string effectiveSystem = system;
		size_t maxHistory = 30;
		if (m_UseEngineContext && !m_EngineContextCache.empty())
		{
			effectiveSystem += "\n\n--- Wasteland engine scripting reference ---\n" + m_EngineContextCache;
			maxHistory = 12;
		}
		if (agentic)
		{
			effectiveSystem += "\n\n## Available Tools\nYou have access to tools for file operations and memory. When you need to use a tool, output a tool call in this format:\n<tool_call name=\"TOOL_NAME\"><args>JSON_ARGS</args></tool_call>\n\nAvailable tools:\n" + toolDefs + "\n\n";
			effectiveSystem += "After each tool result, decide if you need more tools or can give a final answer.\n";
			effectiveSystem += "Always explain what you are about to do before calling a tool.";
			maxHistory = 20;
			if (!memoryContent.empty() && memoryContent.find("(memory") != 0)
			{
				effectiveSystem += "\n\n## Your Memory\n" + memoryContent + "\n";
			}
		}

		m_Sending.store(true);
		m_Status = "Sending...";

		m_Worker = std::thread([this, key, base, model, temp, effectiveSystem, history, maxHistory, agentic]() {
			std::string endpoint = base;
			while (!endpoint.empty() && endpoint.back() == '/')
				endpoint.pop_back();
			endpoint += "/chat/completions";

			HttpHeaders headers;
			if (!key.empty())
				headers.emplace_back("Authorization", "Bearer " + key);

			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.push_back({"assistant", ""});
				m_Status = "Receiving...";
				m_ScrollToBottom = true;
			}

			std::string parseBuf;
			std::string streamError;
			long status = 0;
			std::string reqError = HttpClient::PostStream(
				endpoint, BuildRequestBody(model, temp, effectiveSystem, history, maxHistory, true), headers,
				[&parseBuf, this, &streamError](const char *data, size_t len) {
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
						if (JsonMini::ExtractString(payload, "content", delta))
						{
							std::lock_guard<std::mutex> lock(m_Mutex);
							if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
							{
								m_Messages.back().Text += delta;
								m_ScrollToBottom = true;
							}
						}
						else if (payload.find("\"error\"") != std::string::npos)
						{
							std::string msg;
							if (JsonMini::ExtractString(payload, "message", msg))
								streamError = msg;
						}
					}
				},
				status);

			if (agentic)
			{
				std::string responseText;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
						responseText = m_Messages.back().Text;
				}

				int iterations = 0;
				while (iterations++ < kMaxAgentIterations)
				{
					std::string toolName, argsJson;
					std::string toolCallText = ExtractToolCall(responseText, toolName, argsJson);
					if (toolCallText.empty())
						break;

					std::string result = ToolServer::Instance().ExecuteTool(toolName, argsJson);

					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						m_Messages.push_back({"tool", result, toolName});
						m_Messages.push_back({"assistant", ""});
						m_Status = "Tool '" + toolName + "' executed. Thinking...";
						m_ScrollToBottom = true;
					}

					std::vector<ChatMessage> updatedHistory;
					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						updatedHistory = m_Messages;
					}

					parseBuf.clear();
					streamError.clear();
					status = 0;
					HttpClient::PostStream(
						endpoint, BuildRequestBody(model, temp, effectiveSystem, updatedHistory, maxHistory, true), headers,
						[&parseBuf, this, &streamError](const char *data, size_t len) {
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
								if (JsonMini::ExtractString(payload, "content", delta))
								{
									std::lock_guard<std::mutex> lock(m_Mutex);
									if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
									{
										m_Messages.back().Text += delta;
										m_ScrollToBottom = true;
									}
								}
								else if (payload.find("\"error\"") != std::string::npos)
								{
									std::string msg;
									if (JsonMini::ExtractString(payload, "message", msg))
										streamError = msg;
								}
							}
						},
						status);

					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
							responseText = m_Messages.back().Text;
					}
				}
			}

			std::lock_guard<std::mutex> lock(m_Mutex);
			bool haveText = !m_Messages.empty() && m_Messages.back().Role == "assistant" &&
				!m_Messages.back().Text.empty();
			auto setErr = [this](const std::string &err) {
				if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
					m_Messages.back() = {"error", err};
				else
					m_Messages.push_back({"error", err});
				m_Status = err;
			};
			if (!reqError.empty() && !haveText)
				setErr(reqError);
			else if (!streamError.empty() && !haveText)
				setErr(streamError);
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

	void AIChatPanel::NotifyFileWritten(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(m_PendingMutex);
		m_PendingFileOpens.push_back(path);
	}

	void AIChatPanel::RenderMessage(const ChatMessage &m)
	{
		if (m.Role == "user")
		{
			ImVec4 color(0.55f, 0.85f, 1.0f, 1.0f);
			(void)color;
			(void)ImGui::TextColored(color, "You:");
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else if (m.Role == "tool")
		{
			ImVec4 color(0.6f, 0.8f, 1.0f, 1.0f);
			char buf[512];
			_snprintf_s(buf, sizeof(buf), "[Tool: %s]", m.ToolName.c_str());
			(void)color;
			(void)ImGui::TextColored(color, "%s", buf);
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else if (m.Role == "error")
		{
			(void)ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Error:");
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else
		{
			std::string displayText = StripToolCalls(m.Text);
			if (!displayText.empty())
			{
				ImVec4 color(0.65f, 1.0f, 0.65f, 1.0f);
				(void)color;
				(void)ImGui::TextColored(color, "AI:");
				(void)ImGui::TextWrapped("%s", displayText.c_str());
			}
		}
	}

	void AIChatPanel::OnImGuiRender()
	{
		if (!m_Visible)
			return;

		LoadEngineContext();

		{
			std::lock_guard<std::mutex> lock(m_PendingMutex);
			if (!m_PendingFileOpens.empty())
			{
				for (const auto &p : m_PendingFileOpens)
				{
					WL_CORE_INFO("Tool wrote file: {0}", p);
				}
				m_PendingFileOpens.clear();
			}
		}

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

		if (!ImGui::Begin("AI Chatbot", &m_Visible))
		{
			ImGui::End();
			return;
		}

		if (ImGui::CollapsingHeader("Settings & API Key"))
		{
			if (ImGui::Checkbox("Agent Mode (tools + memory)", &m_AgenticMode))
			{
				if (m_AgenticMode)
					m_Status = "Agent mode ON. AI can edit files and remember things.";
				else
					m_Status = "Agent mode OFF. Normal chat.";
			}

			if (ImGui::CollapsingHeader("Engine context"))
			{
				if (ImGui::Checkbox("Include Wasteland scripting reference", &m_UseEngineContext))
				{
				}
				if (!m_EngineContextError.empty())
				{
				}
			}

			if (ImGui::InputText("Base URL", m_BaseURLBuf, sizeof(m_BaseURLBuf)))
			{
			}
			if (ImGui::InputText("Model", m_ModelBuf, sizeof(m_ModelBuf)))
			{
			}
			if (ImGui::SliderFloat("Temperature", &m_Temp, 0.0f, 2.0f))
			{
			}
			if (ImGui::InputTextMultiline("System prompt", m_SystemBuf, sizeof(m_SystemBuf), ImVec2(-FLT_MIN, 60)))
			{
			}

			if (ImGui::InputText("API key", m_KeyBuf, sizeof(m_KeyBuf), m_ShowKey ? 0 : ImGuiInputTextFlags_Password))
			{
			}
			if (ImGui::SameLine(); ImGui::Checkbox("Show", &m_ShowKey))
			{
			}
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
			if (ImGui::SameLine(); ImGui::Button("Clear key"))
			{
				m_KeyBuf[0] = '\0';
				ApiKeyStore::Instance().OpenAIKey.clear();
				ApiKeyStore::Instance().Save();
				m_Status = "API key cleared.";
			}
		}
		else
		{
			if (ImGui::Button("Agent ON"))
				m_AgenticMode = true;
			if (ImGui::SameLine(); ImGui::Button("Agent OFF"))
				m_AgenticMode = false;
		}

		ImGui::BeginChild("##chat_scroll", ImVec2(0, -60), true);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			for (const auto &m : m_Messages)
			{
				RenderMessage(m);
				(void)ImGui::Separator();
			}
			if (m_ScrollToBottom)
			{
				ImGui::SetScrollHereY(1.0f);
				m_ScrollToBottom = false;
			}
		}
		ImGui::EndChild();
		{
			const char *statusFmt = "Status: %s";
			(void)ImGui::TextWrapped(statusFmt, m_Status.c_str());
		}

		bool sending = m_Sending.load();
		if (sending)
		{
			if (ImGui::Button("Cancel"))
			{
				m_Sending = false;
				m_Status = "Canceled.";
			}
		}
		else
		{
			bool submit = ImGui::InputText("##chat_input", m_InputBuf, sizeof(m_InputBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("Send") || submit)
				SendCurrentInput();
			if (ImGui::Button("Clear chat"))
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.clear();
				m_Status = "Idle.";
			}
		}

	 ImGui::End();
	}

}
