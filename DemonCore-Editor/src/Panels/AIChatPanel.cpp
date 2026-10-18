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
		// The key is copied into a local rather than captured by reference,
		// so a settings change mid-request cannot alter the in-flight call.
		std::string key = store.OpenAIKey;
		// Copied out of the store one field at a time, all on the UI thread,
		// so the worker never reads a settings object that Save() might be
		// mutating.
		std::string base = store.OpenAIBaseURL;
		std::string model = store.OpenAIModel;
		float temp = store.Temperature;
		std::string system = store.SystemPrompt;
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

		// Set before the thread starts, so a second Send is rejected by the
		// m_Sending check in SendCurrentInput.
		m_Sending.store(true);
		// Written without the lock. It is only read by the UI thread, which
		// is this thread, and the worker has not started yet -- but every
		// later m_Status write DOES take the lock, so the discipline is
		// inconsistent.
		m_Status = "Sending...";

		// The capture list is the contract with the UI thread: key, base,
		// model, temp, the assembled prompt, the history snapshot and the
		// limits are all COPIED. Only `this` is captured by pointer, and
		// the destructor joins before the panel dies, so that is safe.
		// `agentic` is captured too so the worker can skip the agent loop
		// without asking the UI thread.
		m_Worker = std::thread([this, key, base, model, temp, effectiveSystem, history, maxHistory, agentic]() {
			// The endpoint is assembled on the WORKER, not the UI thread, so a
			// second send cannot observe a half-built URL.
			std::string endpoint = base;
			// Trailing slashes stripped before appending, so a base URL ending
			// in '/' does not produce a double slash.
			while (!endpoint.empty() && endpoint.back() == '/')
				endpoint.pop_back();
			// The fixed path suffix. OpenAI-compatible servers expose the
			// completion endpoint here regardless of the base URL's shape.
			endpoint += "/chat/completions";

			// Bearer auth added only when a key exists, which is what lets an
			// unauthenticated local server (LM Studio, llama.cpp) work.
			HttpHeaders headers;
			// No key means no Authorization header at all, which is what allows
			// an unauthenticated local server to work.
			if (!key.empty())
				headers.emplace_back("Authorization", "Bearer " + key);

			// A placeholder assistant message is pushed BEFORE the request so
			// the streamed deltas have somewhere to land -- the callback below
			// appends to m_Messages.back().
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				// Empty text, filled in by the stream callback. DEFECT: it is pushed
				// under the lock here, but the request below can fail, in which case
				// the trailing error handling has to overwrite this same message --
				// the placeholder is doing double duty as both the streaming
				// target and the error slot.
				m_Messages.push_back({"assistant", ""});
				// Status changed under the lock because the UI reads it (
				// unsafely) from another thread.
				m_Status = "Receiving...";
				// Requested here and consumed by the UI's scroll logic below.
				m_ScrollToBottom = true;
			}

			// Buffers partial SSE lines across chunk boundaries. A TCP read can
			// split a line anywhere, so the tail must survive between calls.
			std::string parseBuf;
			// Three separate error channels, because three different things can
			// go wrong: the request itself (reqError), the SSE payload
			// (streamError), and the HTTP status (status).
			std::string streamError;
			long status = 0;
			// The one call whose RETURN VALUE is kept. The follow-up calls in
			// the agent loop discard theirs (see the note there).
			std::string reqError = HttpClient::PostStream(
				endpoint, BuildRequestBody(model, temp, effectiveSystem, history, maxHistory, true), headers,
				// The streaming callback, invoked on the WORKER thread as each chunk
				// arrives. Everything it touches that the UI also touches is under
				// the mutex -- except m_ScrollToBottom, which is a plain bool read
				// and written by both threads.
				[&parseBuf, this, &streamError](const char *data, size_t len) {
					parseBuf.append(data, len);
					// Declared outside the callback and captured by reference, so it
					// survives across chunk invocations -- which is what lets a line
					// split across two TCP reads still be parsed.
					size_t pos = 0;
					// Drain complete lines from the buffer. parseBuf keeps any trailing
					// partial line for the next chunk.
					while ((pos = parseBuf.find('\n')) != std::string::npos)
					{
						std::string line = parseBuf.substr(0, pos);
						// Erase from the front: O(n) per line, acceptable for a chat
						// transcript where lines are short and few.
						parseBuf.erase(0, pos + 1);
						// Tolerate CRLF as well as LF.
						if (!line.empty() && line.back() == '\r')
							line.pop_back();
						// SSE framing: every data line starts with "data:".
						if (line.rfind("data:", 0) != 0)
							continue;
						// Past the "data:" prefix. substr(5) is safe because rfind already
						// proved the prefix is there.
						std::string payload = line.substr(5);
						// Strip leading whitespace after the colon; the spec allows one
						// space and servers are inconsistent about how many.
						size_t b = payload.find_first_not_of(" \t");
						payload = (b == std::string::npos) ? "" : payload.substr(b);
						// "[DONE]" is the SSE terminator, and an empty payload is a keep-
						// alive. Neither carries content.
						if (payload.empty() || payload == "[DONE]")
							continue;
						std::string delta;
						// JsonMini::ExtractString is the return-as-condition idiom: true
						// means a content field was found AND the value was non-empty.
						if (JsonMini::ExtractString(payload, "content", delta))
						{
							std::lock_guard<std::mutex> lock(m_Mutex);
							// Guarded by role as well as emptiness: the placeholder is the last
							// message, but if a tool result were appended concurrently this would
							// otherwise write into the wrong one.
							if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
							{
								// Appended, not replaced -- deltas are fragments, so the transcript
								// grows token by token.
								m_Messages.back().Text += delta;
								m_ScrollToBottom = true;
							}
						}
						// An error can arrive mid-stream after content has already been
						// shown, so it is recorded separately and only surfaced below if
						// nothing was received.
						else if (payload.find("\"error\"") != std::string::npos)
						{
							std::string msg;
							if (JsonMini::ExtractString(payload, "message", msg))
								streamError = msg;
						}
					}
				},
				// The transport-level error string, kept alongside streamError so
				// the trailing block can pick the more informative of the two.
				status);

			// The agent loop. Only reached in agentic mode; otherwise the single
			// request above is the whole exchange.
			if (agentic)
			{
				// Copied OUT from under the lock into a local, so the tool-scan and
				// the HTTP call that follows can run without holding the mutex.
				std::string responseText;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// The assistant text assembled so far becomes the thing scanned for a
					// tool call. Read under the lock because the worker wrote it.
					if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
						responseText = m_Messages.back().Text;
				}

				// Bounded so a model that keeps requesting tools cannot loop forever
				// (or spend the user's API credit doing it).
				int iterations = 0;
				// kMaxAgentIterations is 10, per the header. The counter counts
				// ATTEMPTS, so a model gets at most ten tool round trips.
				while (iterations++ < kMaxAgentIterations)
				{
					// Both out-params default-constructed empty, so a tool call with no
					// <args> block yields an empty argsJson rather than garbage.
					std::string toolName, argsJson;
					// Scans the text for a tool call. An empty result means the model
					// answered in prose, which ends the loop.
					std::string toolCallText = ExtractToolCall(responseText, toolName, argsJson);
					if (toolCallText.empty())
						// The model answered in prose: the agent loop is finished.
						break;

					// The tool RUNS HERE, on the worker thread. ToolServer is therefore
					// reached from a non-UI thread for the whole session, which matters
					// for any tool that touches the filesystem.
					std::string result = ToolServer::Instance().ExecuteTool(toolName, argsJson);

					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						// Result recorded as a tool-role message, plus a fresh empty assistant
						// placeholder for the next round's stream to fill.
						m_Messages.push_back({"tool", result, toolName});
						// A new placeholder for the next round, appended right after the
						// tool result so the pair is adjacent in the transcript.
						m_Messages.push_back({"assistant", ""});
						m_Status = "Tool '" + toolName + "' executed. Thinking...";
						// The status line names the tool, so the user can see what the agent
						// is doing without reading the transcript.
						m_ScrollToBottom = true;
					}

					// A FRESH history snapshot for the follow-up request. This is what
					// makes the tool result visible to the model.
					std::vector<ChatMessage> updatedHistory;
					{
						// Snapshot including the tool result just pushed -- this is what
						// makes the model aware of what its tool did.
						std::lock_guard<std::mutex> lock(m_Mutex);
						updatedHistory = m_Messages;
					}

					// Explicit resets between rounds. parseBuf especially: leftover
					// bytes from the previous response would be prepended to the next
					// one and corrupt the first line parsed.
					parseBuf.clear();
					streamError.clear();
					status = 0;
					// DEFECT: this whole callback is a VERBATIM COPY of the lambda at
					// line 245 -- the SSE framing, the [DONE] check, the delta append,
					// the error capture, all of it, twice in one function. Any fix to
					// one copy (a new field, a framing change, a locking bug) will not
					// be applied to the other, and the two have already drifted in one
					// respect: neither resets parseBuf between the outer request and
					// this one except at the explicit clear() above.
					HttpClient::PostStream(
						endpoint, BuildRequestBody(model, temp, effectiveSystem, updatedHistory, maxHistory, true), headers,
						[&parseBuf, this, &streamError](const char *data, size_t len) {
							parseBuf.append(data, len);
							size_t pos = 0;
							// The duplicated loop -- identical to the one at line 457 in every
							// respect. See the DEFECT note at its declaration.
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
			// Note the idiom used throughout this file: a dozen separate
			// `lock_guard<std::mutex> lock(m_Mutex)` declarations (lines 190
			// through 588), each in its own short scope so the lock releases
			// early. They are siblings, not nested, so nothing shadows -- but
			// the repetition makes it easy to add one to the wrong scope.
			//
			// The lock above is held for the REST of the lambda, including every
			// error decision below, since all of them mutate m_Messages.
			//
			// haveText distinguishes "the stream produced something" from "it
			// did not", which decides whether an error REPLACES the message or
			// merely annotates the status line.
			//
			// This is also where the accumulated reply is re-read after each
			// agent round, which is what lets the next iteration's tool scan
			// see the model's newest output rather than the first response.
			bool haveText = !m_Messages.empty() && m_Messages.back().Role == "assistant" &&
				!m_Messages.back().Text.empty();
			// setErr overwrites the assistant placeholder when there is one, so a
			// failure does not leave an empty bubble in the transcript.
			auto setErr = [this](const std::string &err) {
				if (!m_Messages.empty() && m_Messages.back().Role == "assistant")
					m_Messages.back() = {"error", err};
				else
					m_Messages.push_back({"error", err});
				// setErr overwrites the assistant placeholder when there is one, so
				// a failure does not leave an empty bubble in the transcript;
				// otherwise it appends a new error message.
				//
				// The status line mirrors the error text, so a transport or stream
				// failure is visible without scrolling the transcript -- unlike
				// m_EngineContextError, which is computed and never shown at all.
				//
				// Everything here runs under the lock taken at the top of the
				// lambda, which is held to the very end.
				m_Status = err;
			};
			// Priority: a transport error with no content at all is the message;
			// a transport error AFTER content degrades to a status line, because
			// throwing away a partial answer the user can see would be worse.
			if (!reqError.empty() && !haveText)
				setErr(reqError);
			else if (!streamError.empty() && !haveText)
				setErr(streamError);
			// The partial-answer case: content kept, error demoted to the status
			// line so the truncation is visible but not destructive.
			else if (!reqError.empty())
				m_Status = "Idle (stream ended early: " + reqError + ").";
			else
				// The ordinary success path. Note it discards any earlier streamError
				// when content arrived.
				m_Status = "Idle.";
			// Cleared last, so the UI keeps showing "Receiving..." until the
			// worker is genuinely finished writing results.
			m_Sending.store(false);
			m_ScrollToBottom = true;
		// End of the worker lambda. m_Worker is joinable from here on, so
		// the next LaunchWorker() or the destructor joins it.
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
		size_t b = text.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			// Entirely whitespace: nothing to send. This is the guard that makes
			// the find_last_not_of below safe.
			return;
		// find_last_not_of gives the end index; +1 makes it a length. Safe
		// here only because the all-whitespace case already returned above.
		size_t e = text.find_last_not_of(" \t\r\n");
		// The actual trim. Substring rather than in-place erase because `text`
		// is a local copy and the original buffer is untouched.
		text = text.substr(b, e - b + 1);
		if (text.empty())
			return;

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
			// _TRUNCATE rather than an explicit length check: a key longer than
			// the buffer is silently cut rather than rejected. For the key
			// field that means a pasted 600-character key is accepted and then
			// fails authentication.
			strncpy_s(m_KeyBuf, sizeof(m_KeyBuf), store.OpenAIKey.c_str(), _TRUNCATE);
			strncpy_s(m_BaseURLBuf, sizeof(m_BaseURLBuf), store.OpenAIBaseURL.c_str(), _TRUNCATE);
			strncpy_s(m_ModelBuf, sizeof(m_ModelBuf), store.OpenAIModel.c_str(), _TRUNCATE);
			strncpy_s(m_SystemBuf, sizeof(m_SystemBuf), store.SystemPrompt.c_str(), _TRUNCATE);
			m_Temp = store.Temperature;
			// Set last, after every field is copied -- so a partial copy can
			// never mark the settings as loaded.
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
			if (ImGui::InputText("Model", m_ModelBuf, sizeof(m_ModelBuf)))
			{
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
				store.OpenAIKey = m_KeyBuf;
				// A blank base URL falls back to LM Studio's default port, which is
				// why a user who types nothing still gets a working local model.
				store.OpenAIBaseURL = m_BaseURLBuf[0] ? m_BaseURLBuf : "http://localhost:1234/v1";
				// Same fallback idea for the model name.
				store.OpenAIModel = m_ModelBuf[0] ? m_ModelBuf : "local-model";
				store.SystemPrompt = m_SystemBuf;
				store.Temperature = m_Temp;
				// Save writes to disk before the status line updates, so a failure
				// -- reported by Save() -- cannot be announced as success here.
				store.Save();
				m_Status = "Settings saved.";
			}
			// Clear key wipes both the buffer and the store, then saves -- so the
			// key is removed from disk too, not just from the UI.
			if (ImGui::SameLine(); ImGui::Button("Clear key"))
			{
				// Buffer cleared first, then the store, then a save. Three steps so
				// the key is gone from memory, from the store, and from disk.
				m_KeyBuf[0] = '\0';
				ApiKeyStore::Instance().OpenAIKey.clear();
				ApiKeyStore::Instance().Save();
				m_Status = "API key cleared.";
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
