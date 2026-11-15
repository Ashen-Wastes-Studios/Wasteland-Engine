// PCH first -- it is what makes the ImGui and spdlog symbols below
// available without spelling out their includes here.
#include "wlpch.h"
// The panel class and the ChatMessage struct whose Role/Text/ToolName
// the whole file switches on.
#include "AIChatPanel.h"
// Persisted settings (key, base URL, model, temperature, system
// prompt) and the Save/Clear that write them back to disk.
#include "ApiKeyStore.h"
// The WinHTTP-backed transport. PostStream is the only entry point
// this panel uses -- the chat path is SSE-streamed.
#include "HttpClient.h"
// Hand-rolled JSON extraction, not a full parser: the panel only ever
// pulls a scalar out of a response it did not build, so a dependency
// would buy nothing.
#include "JsonMini.h"
// Tool catalogue and ExecuteTool. Only touched when agentic mode is
// -- on, which is why the definitions are fetched inside that branch.
#include "ToolServer.h"
// Only for GetAssetPath(), so the engine reference is read from the
// ACTIVE PROJECT rather than from a path relative to the editor.
#include "Wasteland/Project/Project.h"
// Provider factory for multi-provider support (OpenAI, Anthropic, Google, Hermes).
#include "../AI/ProviderFactory.h"
// IAIProvider types used by the provider-aware LaunchWorker.
#include "../AI/IAIProvider.h"
// Concrete providers so TriggerModelFetch can build one from the panel's
// current UI buffers instead of reading stale values from ApiKeyStore.
#include "../AI/OpenAICompatibleProvider.h"
#include "../AI/AnthropicProvider.h"
#include "../AI/GoogleProvider.h"
// Skill loader for Hermes-format SKILL.md files.
#include "../AI/SkillLoader.h"
// MemoryStore for persistent self-improvement memory.
#include "../AI/MemoryStore.h"

// ImGui, for the whole OnImGuiRender UI.
#include <imgui/imgui.h>

// DEFECT: <algorithm> is included but nothing in this file calls a
// std::algorithm. Unused; it suggests a helper that was moved
// elsewhere and never removed.
#include <fstream>
// std::stringstream, for slurping the engine reference file in one go.
#include <sstream>
#include <algorithm>

// Namespace Wasteland, matching the other editor panels.
namespace Wasteland
{

	// Defaulted: every member has an in-class initializer, so there is
	// nothing to do here. The body is kept rather than defaulted out of
	// line with the other panels.
	AIChatPanel::AIChatPanel()
	{
	}

	// Joins the worker before the members it captured by `this` are
	// destroyed. Without this the thread would outlive the panel and
	// write to freed memory.
	AIChatPanel::~AIChatPanel()
	{
		// joinable() guard, because joining a non-joinable thread calls
		// std::terminate.
		if (m_Worker.joinable())
			m_Worker.join();
		if (m_ModelsWorker.joinable())
			m_ModelsWorker.join();
	}

	// Builds the OpenAI-compatible request body by string concatenation.
	// Static and side-effect free, so it is testable without a panel.
	std::string AIChatPanel::BuildRequestBody(const std::string &model, float temperature,
		const std::string &systemPrompt, const std::vector<ChatMessage> &history, size_t maxHistory, bool stream)
	{
		// The three scalar fields are emitted by hand in OpenAI's key order.
		// JsonMini::Escape on the model name because it is user-editable and
		// could contain a quote.
		std::string body = "{\"model\":\"" + JsonMini::Escape(model) + "\",\"stream\":" +
			(stream ? "true" : "false") + "\",\"temperature\":" +
			// std::to_string on a float yields six decimal places ("0.700000"),
			// which is valid JSON and what the server expects for temperature.
			std::to_string(temperature) + ",\"messages\":[";
		// Tracks whether a separator is needed. A leading comma would produce
		// invalid JSON, and the system message below may be absent.
		bool first = true;
		// The system prompt is prepended outside the history loop, so it is
		// always the first message regardless of how the history is trimmed.
		if (!systemPrompt.empty())
		{
			body += "{\"role\":\"system\",\"content\":\"" + JsonMini::Escape(systemPrompt) + "\"}";
			first = false;
		}
		// Trim history to the LAST maxHistory entries, not the first.
		// Recency is what a model needs to stay coherent, and keeping the
		// oldest messages would drop the actual conversation.
		size_t start = history.size() > maxHistory ? history.size() - maxHistory : 0;
		// Walks the TRIMMED window only. `start` was computed from the
		// history size, so indices here are offsets into the original
		// vector.
		for (size_t i = start; i < history.size(); i++)
		{
			// By reference: these strings are large and are only read here.
			const auto &m = history[i];
			// DEFECT: `role` is computed BEFORE the error/tool checks below, so for
			// those two roles it is computed and then discarded. Harmless, but it
			// means the variable answers a question the code does not ask.
			std::string role = (m.Role == "user") ? "user" : "assistant";
			// "error" messages are UI-only. They are never sent to the model:
			// re-sending a failure would teach the model that it had failed.
			if (m.Role == "error")
				// Error messages exist only for display; sending them back would
				// teach the model it had failed.
				continue;
			// A tool result is folded into the transcript as an ASSISTANT
			// message rather than a real tool-role message. Deliberate: it keeps
			// the request compatible with servers that do not implement the
			// tool-calling API, at the cost of the model having to read the
			// bracketed text rather than a structured result.
			if (m.Role == "tool")
			{
				// Comma only when something was already emitted -- this is what
				// keeps the array well-formed when the system prompt is absent and
				// history is empty.
				if (!first)
					body += ",";
				// The literal "[Tool: name result]\n" prefix is the entire
				// convention. It is what StripToolCalls and the UI's tool branch
				// both key off, so the three must agree.
				body += "{\"role\":\"assistant\",\"content\":\"[Tool: " + JsonMini::Escape(m.ToolName) + " result]\\n" + JsonMini::Escape(m.Text) + "\"}";
				first = false;
				continue;
			}
			if (!first)
				body += ",";
			// The ordinary user/assistant case. `role` was computed above, and
			// the two special roles have already branched away.
			body += "{\"role\":\"" + role + "\",\"content\":\"" + JsonMini::Escape(m.Text) + "\"}";
			first = false;
		}
		// Closes the messages array and the object. Built by appending rather
		// than by a document model, which is why the opening brace on line 31
		// and this closer are far apart.
		body += "]}";
		// Returned by value; the caller owns the buffer.
		return body;
	}

	// Parses a non-streamed response body.
	//
	// DEFECT: this function is never called. It is the whole non-streaming
	// path -- the panel streams exclusively -- and the streaming parser
	// that replaced it was written separately below, so this is dead code
	// that can silently rot.
	bool AIChatPanel::ParseResponseContent(const std::string &body, std::string &outContent, std::string &outError)
	{
		// Error detection is a SUBSTRING search for "\"error\"" anywhere in
		// the body. A successful response whose assistant text happens to
		// contain the literal "error" would be misread as a failure.
		size_t errPos = body.find("\"error\"");
		// Presence check, then a scoped search below. Ordering matters: an
		// error is reported even if the body also contains a valid-looking
		// choices array.
		if (errPos != std::string::npos)
		{
			std::string sub = body.substr(errPos);
			// Searches only the substring from the error key onward, so a
			// "message" field inside the choices array cannot be mistaken for
			// the error message.
			if (JsonMini::ExtractString(sub, "message", outError))
				return false;
		}
		// "choices" is where a valid completion puts its content. Its absence
		// means the body is neither an error nor a completion.
		size_t chPos = body.find("\"choices\"");
		// Neither error nor choices: an unexpected body shape (a proxy error
		// page, a truncated response).
		if (chPos == std::string::npos)
		{
			outError = "Unexpected response (no choices).";
			return false;
		}
		// Search restricted to the choices region so an "error" key
		// elsewhere cannot be mistaken for the completion.
		std::string sub = body.substr(chPos);
		// A modern model may answer with a "refusal" field instead of
		// "content" when it declines, which is a valid response rather than
		// an empty one.
		if (!JsonMini::ExtractString(sub, "content", outContent) || outContent.empty())
		{
			std::string refusal;
			// A refusal is a legitimate answer, so it becomes the content
			// rather than an error -- the user sees why the model declined.
			if (JsonMini::ExtractString(sub, "refusal", refusal) && !refusal.empty())
				outContent = refusal;
			else
			{
				outError = "Empty response content.";
				return false;
			}
		}
		// The only success return. Both out-parameters are written on every
		// path that reaches here.
		return true;
	}

	// Pulls the first tool call out of assistant text.
	//
	// Tag-based rather than JSON-based: the model is INSTRUCTED (see
	// LaunchWorker) to emit <tool_call name="..."> itself, and this
	// finds it. It returns the matched tag text so the caller can tell
	// "no tool call" (empty) from "tool call found".
	std::string AIChatPanel::ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs)
	{
		// Opening tag searched for. No closing-tag check yet -- a truncated
		// stream is handled by each subsequent search returning npos.
		size_t start = text.find("<tool_call");
		if (start == std::string::npos)
			// Empty string means "no tool call in this text" -- the signal the
			// agent loop uses to stop iterating.
			return "";
		// The name attribute, quoted in the documented format.
		size_t nameStart = text.find("name=\"", start);
		if (nameStart == std::string::npos)
			return "";
		// Skip past `name="`. Hardcoded 6 must match the literal searched
		// for above; changing one without the other silently shifts the
		// extracted name.
		nameStart += 6;
		// The closing quote of the name attribute. Its absence means a
		// malformed tag, so nothing is written to the out-parameters.
		size_t nameEnd = text.find("\"", nameStart);
		if (nameEnd == std::string::npos)
			return "";
		// outToolName is written BEFORE the remaining searches below, so a
		// later early return leaves it populated while outArgs stays empty.
		// Callers only use the return value's emptiness to decide, so this is
		// currently harmless -- but it means the out-params are not all-or-
		// nothing.
		outToolName = text.substr(nameStart, nameEnd - nameStart);

		// Search for the tag's own '>' so the args search starts after the
		// attribute list rather than inside the quoted name.
		size_t tagEnd = text.find(">", nameEnd);
		if (tagEnd == std::string::npos)
			return "";
		// <args> is optional: a tool needing no arguments can omit the block
		// and outArgs is simply left empty.
		size_t argsStart = text.find("<args>", tagEnd);
		if (argsStart != std::string::npos)
		{
			// +6 skips the literal "<args>".
			argsStart += 6;
			size_t argsEnd = text.find("</args>", argsStart);
			// An unterminated <args> leaves outArgs empty rather than running to
			// the end of the message -- which would otherwise execute the whole
			// remaining text as arguments.
			if (argsEnd != std::string::npos)
				// The arguments are taken VERBATIM -- no JSON validation. A malformed
				// args blob is passed to ExecuteTool as-is, so ToolServer has to be
				// the thing that rejects it.
				outArgs = text.substr(argsStart, argsEnd - argsStart);
		}

		// The closing tag. Note the literal is "</tool_call>" and the
		// extraction below hardcodes 12 as its length.
		size_t callEnd = text.find("</tool_call>", tagEnd);
		if (callEnd == std::string::npos)
			// Empty means no complete closing tag, so the model emitted a
			// partial call (truncated stream) and nothing is executed.
			return "";
		// DEFECT: the return slices [start, callEnd + 12), assuming the
		// closing tag is exactly 12 characters. If the model emits
		// </tool_call >" with a space, or the tag text is ever changed,
		// the slice is off by one and the returned string no longer ends at
		// the tag. The caller only tests emptiness, so it goes unnoticed.
		return text.substr(start, callEnd + 12 - start);
	}

	// Removes every tool-call tag from assistant text so the UI shows the
	// model the prose without the machine-readable block.
	std::string AIChatPanel::StripToolCalls(const std::string &text)
	{
		std::string result = text;
		// Loop rather than a single find/replace, because a model can emit
		// several tool calls in one message and each must go.
		while (true)
		{
			// Restart the search after each erase, since the remaining text
			// shifted left.
			size_t start = result.find("<tool_call");
			// No opening tag left in the text: the strip is complete.
			if (start == std::string::npos)
				break;
			// An unterminated tool_call is left in place: breaking out is safer
			// than erasing a partial tag that might be legitimate text.
			size_t end = result.find("</tool_call>", start);
			if (end == std::string::npos)
				break;
			// Same hardcoded 12 as ExtractToolCall above -- the two must agree,
			// and nothing enforces that.
			end += 12;
			// Erase start..end inclusive. The remainder shifts left, which is why
			// the loop searches again from scratch rather than resuming at an
			// offset.
			result.erase(start, end - start);
		}
		return result;
	}

	// Loads the engine scripting reference, re-reading only when the
	// file's mtime changes. Called every frame from OnImGuiRender, so
	// the cache check below is what keeps it off the disk.
	void AIChatPanel::LoadEngineContext()
	{
		// Local alias to keep the filesystem calls below short.
		namespace fs = std::filesystem;
		// error_code overload throughout, so a missing file is a value to
		// branch on rather than an exception.
		std::error_code ec;
		// Per-project: each project carries its own engine reference notes,
		// resolved through the active project rather than a fixed path.
		// Per-project: each project carries its own engine reference notes.
		fs::path path = Project::Get().GetAssetPath("ai/engine_reference.md");
		// mtime is the cache key. A missing file sets ec, which is how the
		// "not found" case below is detected -- there is no exists() call.
		auto stamp = fs::last_write_time(path, ec);
		if (ec)
		{
			// Every cache field is cleared together, because leaving a stale
			// cache behind would let a deleted reference keep being sent.
			m_EngineContextCache.clear();
			// The char count is zeroed alongside the cache so a later read of
			// it cannot describe the file that just disappeared.
			m_EngineContextChars = 0;
			m_EngineContextHaveTime = false;
			// Surfaced in the UI, where it is currently rendered by an empty
			// if-body (see OnImGuiRender) -- the message is computed and then
			// never shown.
			m_EngineContextError = "ai/engine_reference.md not found in this project.";
			return;
		}
		// Cache hit. The !empty() term matters: if the reference file exists
		// but is empty, the read below is retried every frame rather than
		// being cached as an empty result.
		if (m_EngineContextHaveTime && stamp == m_EngineContextTime && !m_EngineContextCache.empty())
			return;
		std::ifstream file(path);
		if (!file.is_open())
		{
			m_EngineContextCache.clear();
			m_EngineContextChars = 0;
			m_EngineContextError = "Could not open ai/engine_reference.md.";
			return;
		}
		// Slurp the whole file through a stringstream rather than a line loop,
		// because the reference is prose with no line structure to exploit.
		std::stringstream ss;
		// rdbuf() streams the whole file; ss.str() then copies it. Two
		// buffers, but the reference is read at most once per mtime change.
		ss << file.rdbuf();
		m_EngineContextCache = ss.str();
		// Stored as a long because it is later used as a token-budget
		// estimate. DEFECT: m_EngineContextChars is written here and in the
		// two failure paths but never read anywhere -- the char count is
		// computed and then unused.
		m_EngineContextChars = (long)m_EngineContextCache.size();
		// Cleared only on a SUCCESSFUL read, so a failure leaves the error
		// visible for the UI (where it is currently never shown).
		m_EngineContextError.clear();
		// Stamp and flag recorded together, so the next call's cache check
		// has both.
		m_EngineContextTime = stamp;
		m_EngineContextHaveTime = true;
	}

	void AIChatPanel::LoadProviderSettings(const char *provider)
	{
		ApiKeyStore &store = ApiKeyStore::Instance();
		if (strcmp(provider, "anthropic") == 0)
		{
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.AnthropicKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.AnthropicBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.AnthropicModel.c_str(), _TRUNCATE);
		}
		else if (strcmp(provider, "google") == 0)
		{
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.GoogleKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.GoogleBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.GoogleModel.c_str(), _TRUNCATE);
		}
		else if (strcmp(provider, "hermes") == 0)
		{
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.HermesKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.HermesBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.HermesModel.c_str(), _TRUNCATE);
		}
		else
		{
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.OpenAIKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.OpenAIBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.OpenAIModel.c_str(), _TRUNCATE);
		}
		strncpy_s(m_SystemBuf, sizeof(m_SystemBuf), store.SystemPrompt.c_str(), _TRUNCATE);
		m_Temp = store.Temperature;
		// Subagent settings are engine-wide rather than per-provider, so they
		// are loaded here alongside the other persisted values.
		m_AutoDelegate = store.AutoDelegate;
		m_MaxSubagents = store.MaxSubagents;
	}

	void AIChatPanel::RefreshSkills()
	{
		SkillLoader::Refresh();
		m_SkillNames.clear();
		m_SelectedSkillIndex = 0;
		for (const auto &skill : SkillLoader::GetAll())
			m_SkillNames.push_back(skill.Name);
		m_SkillsLoaded = true;
		m_SkillsStatus = m_SkillNames.empty() ? "No skills found" : "";
	}

	// Runs a parsed <delegate> block on the worker thread. The aggregate is
	// appended as a "tool" message so the main agent's next request sees it in
	// the same shape as any other tool result, and the per-task results are
	// stored for the UI's expandable list.
	bool AIChatPanel::RunSubagentDelegation(const std::string &text, const std::string &providerName,
											const std::string &model, float temp,
											const std::string &systemPrompt, const std::string &toolDefs,
											bool autoDelegate, int maxSubagents)
	{
		// The opt-in gate. A model that emits <delegate> while automatic
		// delegation is off is told so rather than silently ignored, so it can
		// fall back to doing the work itself or asking the user.
		if (!autoDelegate)
			return false;

		std::vector<SubagentTask> tasks;
		if (!SubagentSystem::ParseDelegateTag(text, tasks))
			return false;

		SubagentRunContext ctx;
		ctx.ProviderName = providerName;
		ctx.Model = model;
		ctx.Temperature = temp;
		ctx.SystemPrompt = systemPrompt;
		ctx.ToolDefinitionsJson = toolDefs;
		ctx.MaxIterations = 6;
		ctx.MaxConcurrent = std::max(1, maxSubagents);

		auto results = SubagentSystem::RunParallel(tasks, ctx);
		std::string aggregate = SubagentSystem::Aggregate(results);

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LastSubagentResults = std::move(results);
			m_ShowSubagentResults = true;
			// Same shape as a tool result, so the loop can continue unchanged.
			m_Messages.push_back({"tool", aggregate, "delegate"});
			m_Messages.push_back({"assistant", ""});
			m_Status = "Delegated " + std::to_string(tasks.size()) + " subagent task(s).";
			m_ScrollToBottom = true;
		}
		return true;
	}

	// Manual /delegate: the user's goal becomes a single subagent task. Unlike
	// the automatic path this bypasses m_AutoDelegate -- the user asked for it
	// explicitly.
	bool AIChatPanel::DelegateSingleGoal(const std::string &goal)
	{
		if (goal.empty())
			return false;

		SubagentTask task;
		task.Id = "manual";
		task.Goal = goal;
		task.TimeoutSeconds = 120;

		std::vector<SubagentTask> tasks;
		tasks.push_back(task);

		// Settings are read on the UI thread here, then copied into the run
		// context by value -- the worker threads must not touch ApiKeyStore.
		ApiKeyStore &store = ApiKeyStore::Instance();
		std::string providerName = m_ProviderBuf;
		std::string model = m_ModelBuf[0] ? m_ModelBuf : store.OpenAIModel;

		SubagentRunContext ctx;
		ctx.ProviderName = providerName;
		ctx.Model = model;
		ctx.Temperature = store.Temperature;
		ctx.SystemPrompt = store.SystemPrompt;
		ctx.ToolDefinitionsJson = ToolServer::Instance().GetToolDefinitionsJson();
		ctx.MaxIterations = 6;
		ctx.MaxConcurrent = std::max(1, m_MaxSubagents);

		auto results = SubagentSystem::RunParallel(tasks, ctx);
		std::string aggregate = SubagentSystem::Aggregate(results);

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LastSubagentResults = std::move(results);
			m_ShowSubagentResults = true;
			m_Messages.push_back({"tool", aggregate, "delegate"});
			m_Status = "Delegated goal: " + goal;
			m_ScrollToBottom = true;
		}
		return true;
	}

	void AIChatPanel::TriggerModelFetch()
	{
		if (m_ModelsWorker.joinable())
			m_ModelsWorker.join();

		std::string provider = m_ProviderBuf;
		std::string key = m_KeyBuf;
		std::string base = m_BaseURLBuf[0] ? m_BaseURLBuf : "";
		std::string model = m_ModelBuf[0] ? m_ModelBuf : "";
		m_ModelsStatus = "Loading models...";
		m_AvailableModels.clear();
		m_SelectedModelIndex = 0;

		m_ModelsWorker = std::thread([this, provider, key, base, model]() {
			std::vector<std::string> models;
			IAIProvider *p = nullptr;
			std::string lower = provider;
			for (char &c : lower)
				c = static_cast<char>(tolower(c));

			if (lower == "anthropic" || lower == "claude")
			{
				std::string baseUrl = base.empty() ? "https://api.anthropic.com" : base;
				std::string modelId = model.empty() ? "opus-5.5" : model;
				p = new AnthropicProvider(baseUrl, key, modelId);
			}
			else if (lower == "google" || lower == "gemini")
			{
				std::string baseUrl = base.empty() ? "https://generativelanguage.googleapis.com/v1beta" : base;
				std::string modelId = model.empty() ? "gemini-3.8-flash" : model;
				p = new GoogleProvider(baseUrl, key, modelId);
			}
			else
			{
				std::string name = lower;
				if (name == "custom") name = "Custom";
				else if (name == "local") name = "Local";
				else if (name == "openrouter") name = "OpenRouter";
				else name = "OpenAI";

				std::string baseUrl = base.empty() ? "https://api.openai.com/v1" : base;
				std::string modelId = model.empty() ? "gpt-6.1-sol" : model;
				p = new OpenAICompatibleProvider(name, baseUrl, key, modelId);
			}

			if (p)
			{
				models = p->GetModels();
				delete p;
			}
			{
				std::lock_guard<std::mutex> lock(m_ModelsMutex);
				m_AvailableModels = std::move(models);
				m_ModelsStatus = m_AvailableModels.empty() ? "No models found" : "";
			}
		});
	}

	// Sets up and launches the worker thread that performs the request.
	// Everything the thread needs is copied out on THIS thread first --
	// see the capture list at the std::thread below.
	void AIChatPanel::LaunchWorker()
	{
		// Join any previous worker. In practice a no-op, because m_Sending
		// gates entry to SendCurrentInput, but it keeps the assignment below
		// from destroying a joinable thread.
		if (m_Worker.joinable())
			// Second guard on the same join, in case this path is reached while
			// a previous worker is somehow still alive.
			m_Worker.join();

		// Settings are read HERE, on the UI thread, and copied into the
		// thread by value. The worker never touches ApiKeyStore.
		ApiKeyStore &store = ApiKeyStore::Instance();
		std::string providerName = m_ProviderBuf;
		std::string key;
		std::string base;
		std::string model;
		if (strcmp(providerName.c_str(), "anthropic") == 0)
		{
			key = store.AnthropicKey;
			base = store.AnthropicBaseURL;
			model = store.AnthropicModel;
		}
		else if (strcmp(providerName.c_str(), "google") == 0)
		{
			key = store.GoogleKey;
			base = store.GoogleBaseURL;
			model = store.GoogleModel;
		}
		else if (strcmp(providerName.c_str(), "hermes") == 0)
		{
			key = store.HermesKey;
			base = store.HermesBaseURL;
			model = store.HermesModel;
		}
		else
		{
			key = store.OpenAIKey;
			base = store.OpenAIBaseURL;
			model = store.OpenAIModel;
		}
		if (model.empty())
			model = m_ModelBuf;
		float temp = store.Temperature;
		std::string system = store.SystemPrompt;
		std::string effort = m_EffortPresetBuf;
		std::string modelPreset = m_ModelPresetBuf;
		// The transcript snapshot. Copied under the lock because the worker
		// will keep appending to m_Messages while this runs.
		std::vector<ChatMessage> history;
		{
			// The lock guards the copy only. It is released before the thread
			// starts -- holding it across the request would freeze the UI.
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The copy that the worker will send. This snapshot predates the
			// user's new message being appended? No -- SendCurrentInput appends
			// first, so the question is included.
			history = m_Messages;
		}
		// Read on the UI thread for the same reason as history.
		bool agentic = m_AgenticMode;
		// Subagent settings are snapshotted on the UI thread with the rest of
		// the request state. The worker reads the copies, never the members,
		// so a checkbox edit mid-run is a plain stale read rather than a data
		// race.
		bool autoDelegate = m_AutoDelegate;
		int maxSubagents = m_MaxSubagents;
		// Tool schemas are fetched on the UI thread, so a slow ToolServer
		// cannot stall the worker.
		std::string toolDefs = ToolServer::Instance().GetToolDefinitionsJson();
		std::string memoryContent;
		// Memory is only read when agentic -- the non-agentic path has no
		// tools and no memory to speak of.
		if (agentic)
		{
			// Synchronous tool call on the UI thread. A slow memory backend
			// therefore stalls the frame -- noted because it is the only tool
			// invoked outside the worker.
			memoryContent = ToolServer::Instance().ExecuteTool("memory_read", "{}");
		}

		// The system prompt is ASSEMBLED here rather than sent as a separate
		// parameter, because everything the engine reference, tool list and
		// memory contribute has to end up in one message.
		std::string effectiveSystem = system;
		// 30 messages by default; the two branches below lower it, because
		// a longer system prompt eats the context budget the history needs.
		size_t maxHistory = 30;
		// The engine reference is appended, not substituted, so the user's
		// own prompt survives alongside it.
		if (m_UseEngineContext && !m_EngineContextCache.empty())
		{
			effectiveSystem += "\n\n--- Wasteland engine scripting reference ---\n" + m_EngineContextCache;
			// Cut to 12 messages: the reference is large and older turns
			// contribute least.
			maxHistory = 12;
		}
		// Skills are injected after engine context but before tool definitions,
		// so the model sees available skills before tool conventions.
		if (!m_SkillNames.empty())
		{
			effectiveSystem += "\n\n" + SkillLoader::GetDescriptionsMarkdown();
			// Skills add context, so reduce history slightly.
			maxHistory = std::max(size_t(8), maxHistory - 2);
		}
		// Second pass over agentic, after the engine-context branch above
		// has already lowered maxHistory. Order is significant: agentic
		// wins.
		if (agentic)
		{
			// The tool-calling convention is DEFINED HERE, in prose, and
			// ExtractToolCall/StripToolCalls parse exactly this format. That
			// coupling is implicit -- nothing checks the two agree.
			effectiveSystem += "\n\n## Available Tools\nYou have access to tools for file operations and memory. When you need to use a tool, output a tool call in this format:\n<tool_call name=\"TOOL_NAME\"><args>JSON_ARGS</args></tool_call>\n\nAvailable tools:\n" + toolDefs + "\n\n";
			effectiveSystem += "After each tool result, decide if you need more tools or can give a final answer.\n";
			// Two separate appends rather than one combined string, so each
			// instruction reads as its own paragraph in the prompt.
			effectiveSystem += "Always explain what you are about to do before calling a tool.";
			// Overrides the 12 above: an agent needs more turns of context to
			// keep track of what it has already done.
			maxHistory = 20;
			// DEFECT: the guard is `find("(memory") != 0`, i.e. a tool result
			// that does not START with "(memory" is included. That reads as a
			// half-implemented sentinel: the intent was presumably to skip a
			// placeholder like "(memory empty)", but any memory content
			// beginning with other text is appended anyway.
			if (!memoryContent.empty() && memoryContent.find("(memory") != 0)
			{
				effectiveSystem += "\n\n## Your Memory\n" + memoryContent + "\n";
			}
		}

		// Subagent delegation is advertised only when the user opted in. The
		// tag format is documented here because SubagentSystem parses exactly
		// this shape; the two are coupled by convention, not by a shared
		// constant.
		if (agentic && m_AutoDelegate)
		{
			effectiveSystem += "\n\n## Subagents\n";
			effectiveSystem += "When a request contains multiple INDEPENDENT parts, you may delegate them to parallel subagents instead of doing them one at a time. ";
			effectiveSystem += "Emit a <delegate> block listing one <task> per part. Each task runs on its own thread with its own tool access and returns a summary to you.\n";
			effectiveSystem += "Format:\n<delegate>\n<task id=\"1\" goal=\"What the subagent should accomplish\" tools=\"tool_a,tool_b\" />\n<task id=\"2\" goal=\"Another independent goal\" />\n</delegate>\n";
			effectiveSystem += "Rules: only delegate independent work; do not delegate when a later step depends on an earlier result; keep the number of tasks reasonable (2-6). ";
			effectiveSystem += "After the subagent results come back, synthesize them into your final answer.";
		}

		// Apply effort preset to temperature if not custom.
		if (effort == "low")
			temp = 0.3f;
		else if (effort == "high")
			temp = 1.2f;

		// Set before the thread starts, so a second Send is rejected by the
		// m_Sending check in SendCurrentInput.
		m_Sending.store(true);
		m_Status = "Sending...";

		// Create the provider on the UI thread so factory errors surface immediately.
		IAIProvider *provider = ProviderFactory_Create(providerName);
		if (!provider)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Messages.push_back({"error", "Unknown provider: " + providerName + ". Choose OpenAI, Anthropic, Google, Hermes, Local, or Custom."});
			m_Status = "Idle.";
			m_Sending.store(false);
			m_ScrollToBottom = true;
			return;
		}

		// The capture list is the contract with the UI thread: provider pointer,
		// settings, the assembled prompt, the history snapshot and the limits
		// are all COPIED. Only `this` is captured by pointer.
		// providerName and toolDefs are captured for the subagent path, which
		// rebuilds the run context per delegation rather than reusing the main
		// thread's provider instance.
		m_Worker = std::thread([this, provider, providerName, model, temp, effectiveSystem, history, maxHistory, agentic, toolDefs, autoDelegate, maxSubagents]() {
			// The streaming callback: appends text deltas to the current
			// assistant message under the lock.
			auto streamCallback = [this](const std::string &delta) -> bool {
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
				{
					m_Messages.back().Text += delta;
					m_ScrollToBottom = true;
				}
				return true;
			};

			// Helper: perform one provider call with the given history.
			auto doCall = [this, provider, streamCallback](const std::string &model, float temp,
				const std::string &system, const std::vector<ChatMessage> &hist, size_t maxHist) -> ProviderResult {
				ProviderRequest request;
				request.Model = model;
				request.Temperature = temp;
				request.SystemPrompt = system;
				request.History = hist;
				request.MaxHistory = maxHist;
				request.Stream = true;
				request.ToolDefinitionsJson = ToolServer::Instance().GetToolDefinitionsJson();

				ProviderResult result = provider->Chat(request, streamCallback);
				return result;
			};

			// First request.
			ProviderResult result = doCall(model, temp, effectiveSystem, history, maxHistory);

			// The agent loop. Only reached in agentic mode; otherwise the single
			// request above is the whole exchange.
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
					// Subagent delegation is checked BEFORE the tool-call path:
					// a <delegate> block is not a tool call, and treating it as
					// one would hand it to ExecuteTool as an unknown name.
					// RunSubagentDelegation is a no-op (returns false) when
					// auto_delegate is off or the text holds no delegate block,
					// so the normal path below is untouched in that case.
					bool didDelegate = RunSubagentDelegation(responseText, providerName, model, temp,
															 effectiveSystem, toolDefs,
															 autoDelegate, maxSubagents);

					if (!didDelegate)
					{
						std::string toolName, argsJson;
						std::string toolCallText = ExtractToolCall(responseText, toolName, argsJson);
						if (toolCallText.empty())
							break;

						std::string toolResult = ToolServer::Instance().ExecuteTool(toolName, argsJson);
						{
							std::lock_guard<std::mutex> lock(m_Mutex);
							m_Messages.push_back({"tool", toolResult, toolName});
							m_Messages.push_back({"assistant", ""});
							m_Status = "Tool '" + toolName + "' executed. Thinking...";
							m_ScrollToBottom = true;
						}
					}

					std::vector<ChatMessage> updatedHistory;
					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						updatedHistory = m_Messages;
					}

					result = doCall(model, temp, effectiveSystem, updatedHistory, maxHistory);
					if (!result.Succeeded)
						break;

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
			if (!result.Succeeded && !haveText)
				setErr(result.Error.empty() ? "Request failed." : result.Error);
			else if (!result.Succeeded && haveText)
				m_Status = "Idle (stream ended early: " + result.Error + ").";
			else
				m_Status = "Idle.";

			m_Sending.store(false);
			m_ScrollToBottom = true;

			// Clean up the provider after the call.
			delete provider;
		});
	}

	// Entry point for the Send button and Enter in the input box.
	void AIChatPanel::SendCurrentInput()
	{
		// Atomic load, and the single gate against a second concurrent send.
		if (m_Sending.load())
			return;
		// Copied out of the ImGui buffer, so later edits to the widget cannot
		// change what is being sent.
		std::string text = m_InputBuf;
		// Trimmed before use, so a message that is only whitespace is
		// discarded rather than sent as an empty turn.
		size_t b = text.find_first_not_of(" 	\r\n");
		if (b == std::string::npos)
			// Entirely whitespace: nothing to send. This is the guard that makes
			// the find_last_not_of below safe.
			return;
		// find_last_not_of gives the end index; +1 makes it a length. Safe
		// here only because the all-whitespace case already returned above.
		size_t e = text.find_last_not_of(" 	\r\n");
		// The actual trim. Substring rather than in-place erase because `text`
		// is a local copy and the original buffer is untouched.
		text = text.substr(b, e - b + 1);
		if (text.empty())
			return;

		// Slash command parser: handles commands before the normal chat send.
		if (text.front() == '/')
		{
			std::string response;
			std::istringstream iss(text);
			std::string cmd;
			iss >> cmd;

			if (cmd == "/help")
			{
				response = "Available commands:\n"
					"/help - show this list\n"
					"/clear - clear transcript\n"
					"/save - save settings\n"
					"/provider <name> - switch provider (openai, anthropic, google, hermes, local, custom)\n"
					"/model <name> - switch model\n"
					"/effort <low|medium|high|custom> - set effort preset\n"
					"/python <code> - execute Python code\n"
					"/scene - list active scene entities\n"
					"/entity <id> - show entity info\n"
					"/memory - read persistent memory\n"
					"/delegate <goal> - run a subagent for the given goal\n"
					"/doctor - run diagnostics";
			}
			else if (cmd == "/clear")
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.clear();
				response = "Transcript cleared.";
			}
			else if (cmd == "/save")
			{
				ApiKeyStore &store = ApiKeyStore::Instance();
				store.OpenAIKey = m_KeyBuf;
				store.OpenAIBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://api.openai.com/v1";
				store.OpenAIModel = m_ModelBuf[0] ? m_ModelBuf : "gpt-6.1-sol";
				store.SystemPrompt = m_SystemBuf;
				store.Temperature = m_Temp;
				store.EffortPreset = m_EffortPresetBuf;
				store.ModelPreset = m_ModelPresetBuf;
				store.Provider = m_ProviderBuf;
				store.AutoDelegate = m_AutoDelegate;
				store.MaxSubagents = m_MaxSubagents;
				store.Save();
				response = "Settings saved.";
			}
			else if (cmd == "/provider")
			{
				std::string provider;
				iss >> provider;
				if (provider.empty())
					response = "Usage: /provider <openai|anthropic|google|hermes|local|custom>";
				else
				{
					if (provider == "openai") m_ProviderIndex = 0;
					else if (provider == "anthropic") m_ProviderIndex = 1;
					else if (provider == "google") m_ProviderIndex = 2;
					else if (provider == "hermes") m_ProviderIndex = 3;
					else if (provider == "local") m_ProviderIndex = 4;
					else if (provider == "custom") m_ProviderIndex = 5;
					else
					{
						response = "Unknown provider: " + provider + ". Use openai, anthropic, google, hermes, local, or custom.";
						provider.clear();
					}
					if (!provider.empty())
					{
						strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), provider.c_str(), _TRUNCATE);
						LoadProviderSettings(m_ProviderBuf);
						TriggerModelFetch();
						response = "Provider switched to " + provider + ".";
					}
				}
			}
			else if (cmd == "/model")
			{
				std::string model;
				std::getline(iss, model);
				b = model.find_first_not_of(" 	");
				if (b != std::string::npos)
					model = model.substr(b);
				if (model.empty())
					response = "Usage: /model <model_id>";
				else
				{
					strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), model.c_str(), _TRUNCATE);
					response = "Model switched to " + model + ".";
				}
			}
			else if (cmd == "/effort")
			{
				std::string effort;
				iss >> effort;
				if (effort.empty())
					response = "Usage: /effort <low|medium|high|custom>";
				else if (effort == "low") m_EffortPresetIndex = 0;
				else if (effort == "medium") m_EffortPresetIndex = 1;
				else if (effort == "high") m_EffortPresetIndex = 2;
				else if (effort == "custom") m_EffortPresetIndex = 3;
				else
					response = "Unknown effort: " + effort + ". Use low, medium, high, or custom.";
				if (effort == "low" || effort == "medium" || effort == "high" || effort == "custom")
				{
					strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), effort.c_str(), _TRUNCATE);
					response = "Effort set to " + effort + ".";
				}
			}
			else if (cmd == "/python")
			{
				std::string code;
				std::getline(iss, code);
				b = code.find_first_not_of(" 	");
				if (b != std::string::npos)
					code = code.substr(b);
				if (code.empty())
					response = "Usage: /python <code>";
				else
					response = ToolServer::Instance().ExecuteTool("python_exec", "{\"code\":\"" + code + "\"}");
			}
			else if (cmd == "/scene")
			{
				response = ToolServer::Instance().ExecuteTool("list_entities", "{}");
			}
			else if (cmd == "/entity")
			{
				std::string id;
				iss >> id;
				if (id.empty())
					response = "Usage: /entity <uuid>";
				else
					response = ToolServer::Instance().ExecuteTool("get_entity_info", "{\"id\":\"" + id + "\"}");
			}
			else if (cmd == "/memory")
			{
				response = ToolServer::Instance().ExecuteTool("memory_read", "{}");
			}
			else if (cmd == "/delegate")
			{
				std::string goal;
				std::getline(iss, goal);
				b = goal.find_first_not_of(" 	");
				if (b != std::string::npos)
					goal = goal.substr(b);
				if (goal.empty())
					response = "Usage: /delegate <goal>";
				else if (DelegateSingleGoal(goal))
					response = "Subagent dispatched for: " + goal;
				else
					response = "Subagent dispatch failed (provider unavailable?).";
			}
			else if (cmd == "/doctor")
			{
				response = "Diagnostics:\n";
				response += "Provider: " + std::string(m_ProviderBuf) + "\n";
				response += "Model: " + std::string(m_ModelBuf) + "\n";
				response += "Base URL: " + std::string(m_BaseURLBuf) + "\n";
				response += "Key: " + std::string(m_KeyBuf[0] ? "set" : "not set") + "\n";
				response += "Temperature: " + std::to_string(m_Temp) + "\n";
				response += "Effort: " + std::string(m_EffortPresetBuf) + "\n";
				response += "Model preset: " + std::string(m_ModelPresetBuf) + "\n";
				response += "Subagents: " + std::string(m_AutoDelegate ? "auto-delegate ON" : "auto-delegate OFF") + "\n";
				response += "Max subagents: " + std::to_string(m_MaxSubagents) + "\n";
				// The advertised tool count, counted out of the schema rather than
				// from a second list: the schema is what the model is told it can
				// call, so its size is the number that matters for a mismatch
				// between what is advertised and what ExecuteTool dispatches.
				const std::string toolDefs = ToolServer::Instance().GetToolDefinitionsJson();
				int toolCount = 0;
				for (size_t p = toolDefs.find("\"name\":"); p != std::string::npos;
					 p = toolDefs.find("\"name\":", p + 7))
					++toolCount;
				response += "Tools advertised: " + std::to_string(toolCount) + "\n";
			}
			else
			{
				response = "Unknown command: " + cmd + ". Type /help for available commands.";
			}

			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.push_back({"assistant", response});
			}
			m_ScrollToBottom = true;
			return;
		}

		{
			// Locked because the worker may be appending deltas while this
			// push_back happens.
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The user's turn is appended BEFORE the worker starts, so the
			// transcript shows the question immediately rather than after the
			// model has finished answering.
			m_Messages.push_back({"user", text});
		}
		// Buffer cleared by writing a NUL at index 0 -- InputText treats that
		// as an empty string. Cheaper and less error-prone than std::fill.
		m_InputBuf[0] = '\0';
		m_ScrollToBottom = true;
		// The actual send. Called after the input is cleared and the user
		// turn is recorded, so the worker sees a consistent transcript.
		LaunchWorker();
	}

	// Bridge from the editor: a tool wrote a file, and the panel is told
	// so it can surface the path on the UI thread.
	void AIChatPanel::NotifyFileWritten(const std::string &path)
	{
		// A SEPARATE mutex from m_Mutex. Deliberate: NotifyFileWritten can be
		// called from a tool running on the worker thread, and using the
		// transcript lock there would contend with the streaming appends.
		std::lock_guard<std::mutex> lock(m_PendingMutex);
		// push_back only -- no dedupe and no cap, so a tool that writes
		// thousands of files grows this vector without bound within one
		// frame.
		m_PendingFileOpens.push_back(path);
	}

	// One message rendered. The role decides both the label and the
	// colour, which is the only per-role styling in the panel.
	void AIChatPanel::RenderMessage(const ChatMessage &m)
	{
		// Four mutually exclusive branches. The final else is "assistant",
		// so an unrecognized role would be rendered as a normal reply
		// rather than rejected.
		if (m.Role == "user")
		{
			ImVec4 color(0.55f, 0.85f, 1.0f, 1.0f);
			// DEFECT: `(void)color;` is a no-op -- color is used on the very next
			// line. The same dead cast appears in the tool and assistant branches.
			// It looks like a leftover from when the colours were disabled.
			(void)color;
			(void)ImGui::TextColored(color, "You:");
			// TextWrapped, not Text: long messages must wrap, and the %s form
			// keeps a '%' in the content from being read as a format specifier.
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else if (m.Role == "tool")
		{
			// A paler blue than the user's, so tool output is visually
			// distinguishable from either party.
			ImVec4 color(0.6f, 0.8f, 1.0f, 1.0f);
			// Fixed 512-byte buffer with the secure CRT call. Truncation is safe
			// (that is what _snprintf_s guarantees) but a very long tool name is
			// silently cut.
			char buf[512];
			// The same "[Tool: name]" label convention the request builder uses
			// -- the two are separate implementations of one format.
			_snprintf_s(buf, sizeof(buf), "[Tool: %s]", m.ToolName.c_str());
			(void)color;
			(void)ImGui::TextColored(color, "%s", buf);
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else if (m.Role == "error")
		{
			// Red, and the only branch that inlines its colour rather than
			// naming a local.
			(void)ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Error:");
			(void)ImGui::TextWrapped("%s", m.Text.c_str());
		}
		else
		{
			// The assistant branch strips tool-call tags before display, so the
			// machine-readable block never reaches the user.
			std::string displayText = StripToolCalls(m.Text);
			// An assistant message consisting only of tool-call tags renders as
			// nothing at all -- which is correct: there is no prose to show.
			if (!displayText.empty())
			{
				ImVec4 color(0.65f, 1.0f, 0.65f, 1.0f);
				(void)color;
				(void)ImGui::TextColored(color, "AI:");
				(void)ImGui::TextWrapped("%s", displayText.c_str());
			}
		}
	}

	// The whole UI. Called every frame while the panel is visible.
	void AIChatPanel::OnImGuiRender()
	{
		// Visibility gate, so an invisible panel costs nothing.
		if (!m_Visible)
			// Early out before any ImGui call, so a hidden panel leaves no
			// layout behind.
			return;

		// Refreshed here so a newly-opened project gets its engine reference
		// without the user reopening the panel.
		LoadEngineContext();

		{
			// Pending file opens are DRAINED and logged -- but nothing opens them.
			// DEFECT: the queue exists and is emptied every frame, yet the loop
			// body only logs each path. Whatever was meant to surface or open
			// these files is missing, and because the vector is cleared the
			// evidence is gone by the next frame.
			std::lock_guard<std::mutex> lock(m_PendingMutex);
			if (!m_PendingFileOpens.empty())
			{
				for (const auto &p : m_PendingFileOpens)
				{
					WL_CORE_INFO("Tool wrote file: {0}", p);
				}
				// Cleared unconditionally once drained. See the DEFECT note above:
				// the paths are logged and then discarded.
				m_PendingFileOpens.clear();
			}
		}

		// Settings are pulled from the store ONCE. The m_SettingsLoaded flag
		// means a change made elsewhere (or by the Save button below) does
		// not refresh these buffers on reopen -- the edit fields show the
		// last-loaded values, not the store's.
		if (!m_SettingsLoaded)
		{
			ApiKeyStore &store = ApiKeyStore::Instance();
			// Restore the last selected provider.
			strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), store.Provider.c_str(), _TRUNCATE);
			if (strcmp(m_ProviderBuf, "anthropic") == 0) m_ProviderIndex = 1;
			else if (strcmp(m_ProviderBuf, "google") == 0) m_ProviderIndex = 2;
			else if (strcmp(m_ProviderBuf, "hermes") == 0) m_ProviderIndex = 3;
			else if (strcmp(m_ProviderBuf, "local") == 0) m_ProviderIndex = 4;
			else if (strcmp(m_ProviderBuf, "custom") == 0) m_ProviderIndex = 5;
			else { strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "openai", _TRUNCATE); m_ProviderIndex = 0; }

			LoadProviderSettings(m_ProviderBuf);

			RefreshSkills();
			TriggerModelFetch();

			strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), store.EffortPreset.c_str(), _TRUNCATE);
			if (strcmp(m_EffortPresetBuf, "low") == 0) m_EffortPresetIndex = 0;
			else if (strcmp(m_EffortPresetBuf, "medium") == 0) m_EffortPresetIndex = 1;
			else if (strcmp(m_EffortPresetBuf, "high") == 0) m_EffortPresetIndex = 2;
			else m_EffortPresetIndex = 3;
			strncpy_s(m_ModelPresetBuf, sizeof(m_ModelPresetBuf), store.ModelPreset.c_str(), _TRUNCATE);
			m_ModelPresetIndex = 0;
			m_SettingsLoaded = true;
		}

		// Begin returns false when the window is collapsed/closed, but End
		// still has to be called -- hence the End-then-return below.
		if (!ImGui::Begin("AI Chatbot", &m_Visible))
		{
			// End before returning on a collapsed Begin, or ImGui's internal
			// stack stays unbalanced and the NEXT panel's Begin fails.
			ImGui::End();
			return;
		}

		// The settings section is a COLLAPSING header: the panel is mostly
		// chat, and the key should not dominate it.
		if (ImGui::CollapsingHeader("Settings & API Key"))
		{
			// Agent mode is the panel's headline feature and defaults ON in the
			// header, so it is the first control shown.
			if (ImGui::Checkbox("Agent Mode (tools + memory)", &m_AgenticMode))
			{
				if (m_AgenticMode)
					m_Status = "Agent mode ON. AI can edit files and remember things.";
				else
					m_Status = "Agent mode OFF. Normal chat.";
			}

			// The engine reference toggle is nested one level deeper -- it only
			// matters in combination with the settings above.
			if (ImGui::CollapsingHeader("Engine context"))
			{
				if (ImGui::Checkbox("Include Wasteland scripting reference", &m_UseEngineContext))
				{
				}
				// DEFECT: this if-body is EMPTY. m_EngineContextError is populated by
				// LoadEngineContext with a user-facing message, and this is where it
				// was meant to be displayed. The result is that a missing or
				// unreadable engine reference is silently ignored: the checkbox
				// appears to work, the cache is empty, and no text is ever shown.
				if (!m_EngineContextError.empty())
				{
				}
			}

			// Skills selection dropdown.
			if (ImGui::CollapsingHeader("Skills"))
			{
				if (m_SkillsStatus.empty())
					ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "%zu skills loaded", m_SkillNames.size());
				else
					ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.6f, 1.0f), "%s", m_SkillsStatus.c_str());

				if (!m_SkillNames.empty())
				{
					if (ImGui::Combo("Active Skill", &m_SelectedSkillIndex, [](void *vec, int idx, const char **out_text) -> bool
						{
							const std::vector<std::string> *skills = (const std::vector<std::string> *)vec;
							if (idx < 0 || idx >= (int)skills->size())
								return false;
							*out_text = (*skills)[idx].c_str();
							return true;
						}, &m_SkillNames, (int)m_SkillNames.size()))
					{
						m_SkillsStatus.clear();
					}

					if (m_SelectedSkillIndex >= 0 && m_SelectedSkillIndex < (int)m_SkillNames.size())
					{
						const SkillInfo *skill = SkillLoader::FindByName(m_SkillNames[m_SelectedSkillIndex]);
						if (skill)
						{
							ImGui::TextWrapped("Description: %s", skill->Description.c_str());
							if (!skill->Version.empty())
								ImGui::Text("Version: %s", skill->Version.c_str());
							if (!skill->Platforms.empty())
							{
								ImGui::Text("Platforms: %s", skill->Platforms[0].c_str());
								for (size_t i = 1; i < skill->Platforms.size(); ++i)
									ImGui::SameLine(), ImGui::Text(", %s", skill->Platforms[i].c_str());
							}
						}
					}
				}
			}

			// Subagent delegation settings. Off by default: the model cannot
			// spawn threads unless the user opts in, and /delegate remains
			// available as an explicit per-request override.
			if (ImGui::CollapsingHeader("Subagents"))
			{
				if (ImGui::Checkbox("Auto-delegate parallel work", &m_AutoDelegate))
				{
					if (m_AutoDelegate)
						m_Status = "Auto-delegate ON. The AI can spawn subagents for independent tasks.";
					else
						m_Status = "Auto-delegate OFF. /delegate still works on request.";
				}
				// Clamped on change so a zero or negative value cannot
				// produce an empty worker pool (which RunParallel would
				// silently treat as "run everything at once").
				if (ImGui::InputInt("Max concurrent subagents", &m_MaxSubagents))
					m_MaxSubagents = std::max(1, std::min(16, m_MaxSubagents));
				// The setting is persisted by /save, like the other
				// ApiKeyStore fields; nothing writes it here.
				if (!m_AutoDelegate)
				ImGui::TextWrapped("Off: the AI will not spawn subagents on its own. Use /delegate <goal> to run one explicitly.");
			}

			// Provider selection dropdown.
			if (ImGui::Combo("Provider", &m_ProviderIndex, "OpenAI\0Anthropic\0Google\0Hermes\0Local\0Custom\0"))
			{
				if (m_ProviderIndex == 0) strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "openai", _TRUNCATE);
				else if (m_ProviderIndex == 1) strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "anthropic", _TRUNCATE);
				else if (m_ProviderIndex == 2) strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "google", _TRUNCATE);
				else if (m_ProviderIndex == 3) strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "hermes", _TRUNCATE);
				else if (m_ProviderIndex == 4) strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "local", _TRUNCATE);
				else strncpy_s(m_ProviderBuf, sizeof(m_ProviderBuf), "custom", _TRUNCATE);
				LoadProviderSettings(m_ProviderBuf);
				TriggerModelFetch();
			}

			// Effort + model preset dropdowns.
			if (ImGui::Combo("Effort", &m_EffortPresetIndex, "Low\0Medium\0High\0Custom\0"))
			{
				if (m_EffortPresetIndex == 0) strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), "low", _TRUNCATE);
				else if (m_EffortPresetIndex == 1) strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), "medium", _TRUNCATE);
				else if (m_EffortPresetIndex == 2) strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), "high", _TRUNCATE);
				else strncpy_s(m_EffortPresetBuf, sizeof(m_EffortPresetBuf), "custom", _TRUNCATE);
			}
			if (ImGui::Combo("Model preset", &m_ModelPresetIndex, "Default\0Fast\0Smart\0Custom\0"))
			{
				if (m_ModelPresetIndex == 0) strncpy_s(m_ModelPresetBuf, sizeof(m_ModelPresetBuf), "default", _TRUNCATE);
				else if (m_ModelPresetIndex == 1) strncpy_s(m_ModelPresetBuf, sizeof(m_ModelPresetBuf), "fast", _TRUNCATE);
				else if (m_ModelPresetIndex == 2) strncpy_s(m_ModelPresetBuf, sizeof(m_ModelPresetBuf), "smart", _TRUNCATE);
				else strncpy_s(m_ModelPresetBuf, sizeof(m_ModelPresetBuf), "custom", _TRUNCATE);
			}

			// DEFECT: this and the five InputText/SliderFloat blocks below all
			// have empty if-bodies. The widgets still work -- ImGui writes
			// through the pointers passed to it, so no return-value handling is
			// needed -- but the empty braces read as unfinished code and will
			// swallow any future dirty-check logic silently.
			if (ImGui::InputText("Base URL", m_BaseURLBuf, sizeof(m_BaseURLBuf)))
			{
			}
			// Empty bodies throughout this block: ImGui writes through the
			// pointers, so the widgets work without any return-value handling.
			// The braces are still misleading.
			{
				std::vector<std::string> models;
				{
					std::lock_guard<std::mutex> lock(m_ModelsMutex);
					models = m_AvailableModels;
				}
				if (models.empty())
					models.push_back(m_ModelBuf[0] ? m_ModelBuf : "(enter model)");

				auto itemsGetter = [](void *data, int idx, const char **out_text) -> bool {
					const auto *vec = static_cast<const std::vector<std::string> *>(data);
					if (idx < 0 || idx >= (int)vec->size())
						return false;
					*out_text = vec->at(idx).c_str();
					return true;
				};

				int currentIdx = m_SelectedModelIndex;
				if (currentIdx < 0 || currentIdx >= (int)models.size())
					currentIdx = 0;
				if (ImGui::Combo("Model", &currentIdx, itemsGetter, (void *)&models, (int)models.size()))
				{
					if (currentIdx >= 0 && currentIdx < (int)models.size())
					{
						strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), models[currentIdx].c_str(), _TRUNCATE);
						m_SelectedModelIndex = currentIdx;
					}
				}
			}
			if (ImGui::SliderFloat("Temperature", &m_Temp, 0.0f, 2.0f))
			{
			}
			// -FLT_MIN is the ImGui idiom for "fill the available width"; the
			// 60 is a fixed pixel height for the multi-line box.
			if (ImGui::InputTextMultiline("System prompt", m_SystemBuf, sizeof(m_SystemBuf), ImVec2(-FLT_MIN, 60)))
			{
			}

			// The key field is masked unless Show is ticked.
			if (ImGui::InputText("API key", m_KeyBuf, sizeof(m_KeyBuf), m_ShowKey ? 0 : ImGuiInputTextFlags_Password))
			{
			}
			// if-with-initializer (C++17): SameLine is the init-statement, so the
			// checkbox lands beside the key field. Same pattern at 536 and 548.
			if (ImGui::SameLine(); ImGui::Checkbox("Show", &m_ShowKey))
			{
			}
			// Save pushes the buffers into the store and then to disk.
			if (ImGui::Button("Save settings"))
			{
				ApiKeyStore &store = ApiKeyStore::Instance();
				if (strcmp(m_ProviderBuf, "anthropic") == 0)
				{
					store.AnthropicKey = m_KeyBuf;
					store.AnthropicBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://api.anthropic.com";
					store.AnthropicModel = m_ModelBuf[0] ? m_ModelBuf : "opus-5.5";
				}
				else if (strcmp(m_ProviderBuf, "google") == 0)
				{
					store.GoogleKey = m_KeyBuf;
					store.GoogleBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://generativelanguage.googleapis.com/v1beta";
					store.GoogleModel = m_ModelBuf[0] ? m_ModelBuf : "gemini-3.8-flash";
				}
				else if (strcmp(m_ProviderBuf, "hermes") == 0)
				{
					store.HermesKey = m_KeyBuf;
					store.HermesBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://inference-api.nousresearch.com/v1";
					store.HermesModel = m_ModelBuf[0] ? m_ModelBuf : "step-3.7-flash";
				}
				else
				{
					store.OpenAIKey = m_KeyBuf;
					store.OpenAIBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "https://api.openai.com/v1";
					store.OpenAIModel = m_ModelBuf[0] ? m_ModelBuf : "gpt-6.1-sol";
				}
				store.SystemPrompt = m_SystemBuf;
				store.Temperature = m_Temp;
				store.EffortPreset = m_EffortPresetBuf;
				store.ModelPreset = m_ModelPresetBuf;
				store.Provider = m_ProviderBuf;
				store.AutoDelegate = m_AutoDelegate;
				store.MaxSubagents = m_MaxSubagents;
				store.Save();
				m_Status = "Settings saved.";
				}
				// Clear key wipes both the buffer and the store, then saves -- so the
				// key is removed from disk too, not just from the UI.
				if (ImGui::SameLine(); ImGui::Button("Clear key"))
				{
				m_KeyBuf[0] = '\0';
				ApiKeyStore &store = ApiKeyStore::Instance();
				if (strcmp(m_ProviderBuf, "anthropic") == 0)
					store.AnthropicKey.clear();
				else if (strcmp(m_ProviderBuf, "google") == 0)
					store.GoogleKey.clear();
				else if (strcmp(m_ProviderBuf, "hermes") == 0)
					store.HermesKey.clear();
				else
					store.OpenAIKey.clear();
				store.Save();
				m_Status = "API key cleared.";
				}

				// Memory inspector: shows project memory entries and allows forgetting.
			if (ImGui::CollapsingHeader("Memory Inspector"))
			{
				MemoryStore::Initialize();
				if (!MemoryStore::IsAvailable())
				{
					ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.6f, 1.0f), "No project open for memory");
				}
				else
				{
					auto stats = MemoryStore::ProjectStats();
					ImGui::Text("Total entries: %zu", stats.TotalEntries);
					ImGui::Text("Lessons: %zu | Preferences: %zu | Corrections: %zu | Snippets: %zu | Tool telemetry: %zu",
						stats.Lessons, stats.Preferences, stats.Corrections, stats.Snippets, stats.ToolTelemetry);

					static char memQuery[128] = {};
					if (ImGui::InputText("Search memory", memQuery, sizeof(memQuery)))
					{
						m_MemorySearchResults.clear();
						if (memQuery[0])
							MemoryStore::ProjectSearch(memQuery, m_MemorySearchResults);
					}

					std::vector<MemoryEntry> displayEntries;
					if (memQuery[0])
						displayEntries = m_MemorySearchResults;
					else
						displayEntries = MemoryStore::ProjectRead("");

					for (size_t i = 0; i < displayEntries.size(); ++i)
					{
						const auto &entry = displayEntries[i];
						char timeBuf[64] = {};
						tm timeInfo = {};
						localtime_s(&timeInfo, &entry.Timestamp);
						std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &timeInfo);

						if (ImGui::TreeNode(("##mem_" + std::to_string(i)).c_str()))
						{
							ImGui::Text("[%s] [%s]", timeBuf, entry.Category.c_str());
							ImGui::TextWrapped("%s", entry.Text.c_str());
							ImGui::TreePop();
						}
						if (ImGui::SameLine(); ImGui::SmallButton(("Forget##" + std::to_string(i)).c_str()))
						{
							MemoryStore::ProjectForget(entry.Key);
							m_MemorySearchResults.clear();
						}
					}
				}
			}

			// Subagent results: shown only after a delegation has run, so the
			// section is absent from a fresh panel rather than empty.
			if (m_ShowSubagentResults)
			{
				if (ImGui::CollapsingHeader("Subagent Results"))
				{
					std::vector<SubagentResult> results;
					{
						// Copied under the lock because the worker fills the list
						// while the UI renders.
						std::lock_guard<std::mutex> lock(m_Mutex);
						results = m_LastSubagentResults;
					}

					if (results.empty())
					{
					 ImGui::Text("No subagent results yet.");
					}

					for (size_t i = 0; i < results.size(); ++i)
					{
						const auto &r = results[i];
						// Success/failure is colour-coded, because a failed
						// subagent is easy to miss in a wall of text.
						ImVec4 colour = r.Success ? ImVec4(0.6f, 0.8f, 0.6f, 1.0f)
												 : ImVec4(0.8f, 0.6f, 0.6f, 1.0f);
						std::string label = "Task " + r.TaskId + (r.Success ? " (done)" : " (failed)");
						if (ImGui::TreeNode(("##sub_" + std::to_string(i)).c_str()))
						{
							if (!r.Success)
							 ImGui::TextColored(colour, "Error: %s", r.Error.c_str());
							// The summary is the part the main agent consumed; it
							// is shown in full because it is usually a paragraph.
							if (!r.Summary.empty())
							 ImGui::TextWrapped("%s", r.Summary.c_str());
							if (!r.FullOutput.empty() && r.FullOutput != r.Summary)
							{
								if (ImGui::TreeNode("Full output"))
								{
								 ImGui::TextWrapped("%s", r.FullOutput.c_str());
									// TreePop() returns void; a stray `== false` here was a
									// no-op comparison that did not compile.
									(void)ImGui::TreePop();
								}
							}
						}
						else
						{
							// One-line preview so a collapsed task is still
							// identifiable without opening it.
							std::string preview = r.Success ? r.Summary : r.Error;
							if (preview.size() > 80)
								preview = preview.substr(0, 77) + "...";
							if (!preview.empty())
							ImGui::SameLine(), ImGui::TextColored(colour, "%s", preview.c_str());
						}
					}

					if (ImGui::Button("Clear results"))
					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						m_LastSubagentResults.clear();
						m_ShowSubagentResults = false;
					}
				}
			}
		}
		// Collapsed state: two one-click agent toggles instead of the whole
		// settings block, so the common case stays one line tall.
		else
		{
			if (ImGui::Button("Agent ON"))
				m_AgenticMode = true;
			if (ImGui::SameLine(); ImGui::Button("Agent OFF"))
				m_AgenticMode = false;
		}

		// The scrolling transcript. The -60 height leaves room for the input
		// row below; the 1.0f width means "fill the panel".
		ImGui::BeginChild("##chat_scroll", ImVec2(0, -60), true);
		{
			// The transcript lock is held across the whole render loop, which
			// means a slow frame here BLOCKS the worker's streaming appends.
			// Cheap to draw, so acceptable -- but it is why the worker takes the
			// same lock per delta rather than batching.
			std::lock_guard<std::mutex> lock(m_Mutex);
			for (const auto &m : m_Messages)
			{
				RenderMessage(m);
				(void)ImGui::Separator();
			}
			// Auto-scroll: requested by the worker when a delta arrives, consumed
			// here once. The flag is cleared so it does not fight the user's own
			// scrolling on subsequent frames.
			if (m_ScrollToBottom)
			{
				ImGui::SetScrollHereY(1.0f);
				m_ScrollToBottom = false;
			}
		}
		ImGui::EndChild();
		{
			// The status line. DEFECT: m_Status is read here WITHOUT the mutex
			// while the worker writes it under the lock -- a data race on a
			// std::string, which can be read while it is being reassigned.
			// The statusFmt local is pointless indirection; it exists only so
			// the format string is not a bare literal at the call site.
			const char *statusFmt = "Status: %s";
			(void)ImGui::TextWrapped(statusFmt, m_Status.c_str());
		}

		// Atomic load, because the worker sets it.
		bool sending = m_Sending.load();
		if (sending)
		{
			// DEFECT: Cancel only sets m_Sending = false. The worker thread is
			// NOT signalled, does not check the flag mid-stream, and keeps
			// streaming and appending deltas; it will also set m_Sending and
			// m_Status again on its way out, overwriting "Canceled.". The button
			// restores the input box but does not cancel anything.
			if (ImGui::Button("Cancel"))
			{
				m_Sending = false;
				m_Status = "Canceled.";
			}
		}
		else
		{
			// EnterReturnsTrue makes the input field itself submit, so the user
			// can send without reaching for the Send button.
			bool submit = ImGui::InputText("##chat_input", m_InputBuf, sizeof(m_InputBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("Send") || submit)
				SendCurrentInput();
			// Clear chat wipes the transcript and resets the status. The worker
			// is not running here -- this button only exists in the non-sending
			// branch.
			if (ImGui::Button("Clear chat"))
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Messages.clear();
				m_Status = "Idle.";
			}
		}

	 // DEFECT (cosmetic): this ImGui::End() is indented with a single
	 // SPACE while every other line in the file uses tabs. Harmless to the
	 // preprocessor, but it is a formatting outlier in a tab-indented file.
	 ImGui::End();
	}

}
