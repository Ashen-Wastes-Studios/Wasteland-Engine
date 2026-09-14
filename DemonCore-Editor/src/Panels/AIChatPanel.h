#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Wasteland
{

	struct ChatMessage
	{
		std::string Role; // "user" | "assistant" | "error" | "tool"
		std::string Text;
		std::string ToolName; // populated when Role == "tool"
	};

	// "AI Chatbot" viewport: chat UI over any OpenAI-compatible
	// /chat/completions endpoint. Key + settings via ApiKeyStore.
	// Agentic mode: when enabled, the AI can call tools (file ops, memory)
	// and iterate until the task is done.
	class AIChatPanel
	{
	public:
		AIChatPanel();
		~AIChatPanel();

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		// Bridge: called by EditorLayer when a file is written by the AI.
		void NotifyFileWritten(const std::string &path);

	private:
		void SendCurrentInput();
		void LaunchWorker();
		void LoadEngineContext();
		void AgentLoop();
		void ExecuteToolCall(const std::string &toolName, const std::string &argsJson);
		void RenderMessage(const ChatMessage &m);
		static std::string BuildRequestBody(const std::string &model, float temperature,
			const std::string &systemPrompt, const std::vector<ChatMessage> &history, size_t maxHistory, bool stream);
		static bool ParseResponseContent(const std::string &body, std::string &outContent, std::string &outError);
		static std::string ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs);
		static std::string StripToolCalls(const std::string &text);

	private:
		bool m_Visible = true;
		std::vector<ChatMessage> m_Messages;
		std::mutex m_Mutex;
		char m_InputBuf[4096] = {0};
		std::atomic<bool> m_Sending{false};
		std::thread m_Worker;
		std::string m_Status = "Idle. Set your API key below, then ask anything.";
		bool m_ScrollToBottom = false;

		// Agentic mode
		bool m_AgenticMode = true;
		int m_AgentIterations = 0;
		static const int kMaxAgentIterations = 10;

		// Local editable copies of settings (Save pushes to ApiKeyStore + disk).
		char m_KeyBuf[512] = {0};
		char m_BaseURLBuf[256] = {0};
		char m_ModelBuf[128] = {0};
		char m_SystemBuf[1024] = {0};
		float m_Temp = 0.7f;
		bool m_ShowKey = false;
		bool m_SettingsLoaded = false;

		// Engine scripting reference (assets/ai/engine_reference.md), UI thread only.
		bool m_UseEngineContext = true;
		std::string m_EngineContextCache;
		std::filesystem::file_time_type m_EngineContextTime{};
		bool m_EngineContextHaveTime = false;
		long m_EngineContextChars = 0;
		std::string m_EngineContextError;

		// Pending file opens from tool calls (drained on UI thread).
		std::vector<std::string> m_PendingFileOpens;
		std::mutex m_PendingMutex;
	};

}
