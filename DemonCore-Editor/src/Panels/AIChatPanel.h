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
		std::string Role; // "user" | "assistant" | "error"
		std::string Text;
	};

	// "AI Chatbot" viewport: chat UI over any OpenAI-compatible
	// /chat/completions endpoint. Key + settings via ApiKeyStore.
	class AIChatPanel
	{
	public:
		AIChatPanel();
		~AIChatPanel();

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		void SendCurrentInput();
		void LaunchWorker();
		void LoadEngineContext(); // UI thread: (re)load reference file if changed
		static std::string BuildRequestBody(const std::string &model, float temperature,
			const std::string &systemPrompt, const std::vector<ChatMessage> &history, size_t maxHistory, bool stream);
		static bool ParseResponseContent(const std::string &body, std::string &outContent, std::string &outError);

	private:
		bool m_Visible = true;
		std::vector<ChatMessage> m_Messages;
		std::mutex m_Mutex;
		char m_InputBuf[4096] = {0};
		std::atomic<bool> m_Sending{false};
		std::thread m_Worker;
		std::string m_Status = "Idle. Set your API key below, then ask anything.";
		bool m_ScrollToBottom = false;

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
	};

}
