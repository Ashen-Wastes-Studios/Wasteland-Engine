// Header guard. This panel is header-heavy: the implementation is in a .cpp
// that this header does not need to expose, but every private helper is
// DECLARED here rather than forward-declared, so the class layout is
// complete to any translation unit that includes it.
#pragma once

// m_Sending is the one flag shared between the UI thread and the worker.
// atomic rather than mutex-protected because it is a boolean polled
// every frame and does not need to be part of any larger invariant.
#include <atomic>
#include <filesystem>
// TWO mutexes guarding two unrelated collections -- m_Mutex for the message
// list, m_PendingMutex for the pending file-open queue. Separate locks
// mean the worker never blocks the UI while it appends a message.
#include <mutex>
#include <string>
// The worker thread handle. Joining it is the destructor's job -- see the
// note there.
#include <thread>
#include <vector>

namespace Wasteland
{

	// A POD-ish struct with three strings, holding one turn of the
	// conversation. Value semantics: the vector below reallocates and the
	// messages are copied, which is acceptable at chat-history sizes.
	struct ChatMessage
	{
		// A STRING, not an enum. DEFECT (untyped protocol, documented not fixed):
		// the four legal values are written in a comment rather than enforced.
		// A typo becomes a role the API rejects, discovered at request time as
		// an opaque HTTP 400 rather than at compile time. Every consumer --
		// BuildRequestBody, RenderMessage -- must also string-compare against
		// these literals.
		std::string Role; // "user" | "assistant" | "error" | "tool"
		std::string Text;
		// Discriminated by Role rather than by an optional, so a "tool" message
		// with an empty ToolName is representable and would render as a tool
		// message naming nothing.
		std::string ToolName; // populated when Role == "tool"
	};

	// "AI Chatbot" viewport: chat UI over any OpenAI-compatible
	// /chat/completions endpoint. Key + settings via ApiKeyStore.
	// Agentic mode: when enabled, the AI can call tools (file ops, memory)
	// and iterate until the task is done.
	// A viewport panel, not a service: it owns its own UI state, its own
	// worker thread and its own copy of the API settings. The settings are
	// duplicated locally rather than shared with ApiKeyStore so the text
	// fields can be edited in place without a store round-trip per keystroke.
	class AIChatPanel
	{
	public:
		AIChatPanel();
		// DEFECT (high risk, documented not fixed): the class owns a std::thread and
		// TWO mutexes plus vectors, and declares no copy or move operations. The
		        // implicit copy constructor would be deleted by the thread and the
		// mutexes, so it will not compile -- but the destructor being
		// non-virtual and user-declared means nothing at the declaration site
		// records the requirement to join m_Worker and clear the error state.
		// A panel held by pointer or moved would terminate. Worth an explicit
		// delete of the copy/move ctors so the failure is a compile error at
		// the misuse rather than a std::terminate at shutdown.
		~AIChatPanel();

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

		// Bridge: called by EditorLayer when a file is written by the AI.
		// Called from EditorLayer when the AI writes a file, so the panel can
		// reflect the change. The bridge between the worker thread's side
		// effects and the UI: the implementation must not touch m_Messages or
		// ImGui state directly from a file-watcher context.
		void NotifyFileWritten(const std::string &path);

	private:
		void SendCurrentInput();
		void LaunchWorker();
		void LoadEngineContext();
		void AgentLoop();
		void ExecuteToolCall(const std::string &toolName, const std::string &argsJson);
		void RenderMessage(const ChatMessage &m);
		// STATIC because it is pure -- it formats a request from its arguments
		// and touches no member. That is what makes the JSON assembly testable
		// without an instance, an API key or a network call.
		static std::string BuildRequestBody(const std::string &model, float temperature,
			const std::string &systemPrompt, const std::vector<ChatMessage> &history, size_t maxHistory, bool stream);
		// Two out-params rather than an optional struct: content and error are
		// mutually exclusive in practice, and the caller branches on the
		// bool before reading either, so one of the two is left default.
		static bool ParseResponseContent(const std::string &body, std::string &outContent, std::string &outError);
		// String parsing of tool calls out of model output, with the name and
		// arguments split into two out-params. DEFECT (fragile parsing,
		// documented not fixed): the signature returns a std::string AND
		// fills out-params, with no stated meaning for the return value, so a
		// caller cannot tell a successful parse from an empty one without
		// checking both.
		static std::string ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs);
		// The inverse: removes the tool-call markup so the prose around it can be
		// displayed. Returns the cleaned text, empty on nothing found.
		static std::string StripToolCalls(const std::string &text);

	private:
		// Starts VISIBLE. Every other flag in this class defaults to false or to a
		// safe value; this one does not, which is a UI choice (the chat panel
		// greets the user on first open) rather than a safety one.
		bool m_Visible = true;
		std::vector<ChatMessage> m_Messages;
		// Guards m_Messages. The worker appends the assistant reply; the UI thread
		// reads and renders. Anything reading the vector during render must
		// hold this lock, since a reallocation during iteration would be
		// undefined -- the lock is the only thing preventing that.
		std::mutex m_Mutex;
		// A FIXED C array, not a std::string: this is an ImGui input buffer, and
		// ImGui's InputText wants a char* with a length. The zero initialiser
		// is what makes it a valid empty string rather than garbage, and
		// ImGui additionally writes a terminator within the bound.
		char m_InputBuf[4096] = {0};
		// The send-in-flight flag, read by the UI thread every frame to disable
		// the send button and by the worker when it finishes. atomic because
		// it is the one piece of state deliberately shared WITHOUT the mutex
		// -- it is a standalone flag, not part of the message-list invariant.
		std::atomic<bool> m_Sending{false};
		// A single worker, not a pool. LaunchWorker must therefore refuse to
		// start while one is running -- and m_Sending, not m_Worker's
		// joinability, is the flag that guards it. A std::thread that is
		// joinable and gets destroyed terminates.
		std::thread m_Worker;
		// A DEFAULT status string, so the panel never opens with an empty status
		// line. It names the required first step, which is the panel's
		// onboarding.
		std::string m_Status = "Idle. Set your API key below, then ask anything.";
		// A one-shot request, not a mode: set when a reply arrives and consumed by
		// the next render. Resetting it must happen in the render path, which
		// is why it is a member rather than a local in the append site.
		bool m_ScrollToBottom = false;

		// Agentic mode
		// DEFECT (permissive default, documented not fixed): agentic mode is ON by
		// default, which per the class comment lets the model call file
		// operations and memory tools without the user opting in. For a panel
		// that can write files into the user's project, the safer default is
		// false and opt-in. Nothing in the header marks this as deliberate.
		bool m_AgenticMode = true;
		// The iteration COUNTER, separate from the cap below. Reset per user turn
		// and compared against the cap -- the counter is what AgentLoop
		// increments, and the cap is what stops it.
		int m_AgentIterations = 0;
		// static const class member rather than a file-scope constant or an enum,
		// so it is namespaced with the class that uses it.
		// DEFECT (OdrUse, documented not fixed): declared without an out-of-
		// class definition. Passing it BY REFERENCE or binding a const
		// reference to it -- which std::min/max or a stream insertion would
		// do -- requires a definition at link time and fails with an
		// undefined-reference error. A plain comparison or arithmetic use is
		// fine. constexpr would remove the hazard entirely.
		static const int kMaxAgentIterations = 10;

		// Local editable copies of settings (Save pushes to ApiKeyStore + disk).
		// DEFECT (secret in plaintext memory, documented not fixed): the API key is
		// held in a heap-adjacent fixed buffer for the whole session, and
		// m_KeyBuf is not cleared on destruction -- the destructor is declared
		// in the .cpp, so whether it zeroes the buffer cannot be seen here.
		// The buffer's length is a fixed cap with no user-facing message when
		// a pasted key exceeds it, and ImGui will simply truncate.
		char m_KeyBuf[512] = {0};
		char m_BaseURLBuf[256] = {0};
		char m_ModelBuf[128] = {0};
		char m_SystemBuf[1024] = {0};
		float m_Temp = 0.7f;
		// Toggles the key between masked and plaintext. A local bool rather than
		// saved state, so the key is masked again on every panel open.
		bool m_ShowKey = false;
		// Distinguishes "not loaded yet" from "loaded, and empty". Without it the
		// panel cannot tell whether an empty key buffer means "no key saved"
		// or "load has not run", which is exactly the state in which a user
		// would be told their key is missing when it is merely unread.
		bool m_SettingsLoaded = false;

		// Engine scripting reference (assets/ai/engine_reference.md), UI thread only.
		// The comment above scopes this to the UI thread, which is the right
		// constraint: the engine reference is loaded, cached and read during
		// render, never from the worker.
		bool m_UseEngineContext = true;
		std::string m_EngineContextCache;
		// The mtime used to decide whether the cache is stale. Value-initialised
		// to the epoch, which is meaningless as a real timestamp -- hence the
		// separate m_EngineContextHaveTime flag below rather than comparing
		// against a magic minimum.
		std::filesystem::file_time_type m_EngineContextTime{};
		// Explicit validity flag instead of a sentinel timestamp, and it also
		// doubles as "have we ever stat'd the file". Without it a successful
		// cache write would be indistinguishable from a first run.
		bool m_EngineContextHaveTime = false;
		// Character count of the cached context, for display -- how much engine
		// documentation is being sent with every request. DEFECT (width,
		// documented not fixed): `long` is 4 bytes on Windows and 8 on Linux,
		// so this member has a different size on different platforms. It is
		// only ever a count, so the impact is theoretical, but size_t would
		// be the correct type.
		long m_EngineContextChars = 0;
		std::string m_EngineContextError;

		// Pending file opens from tool calls (drained on UI thread).
		// A QUEUE rather than a flag: several tool calls may write files in one
		// turn, and each needs a separate open. Drained on the UI thread, so
		// the worker never touches the editor.
		std::vector<std::string> m_PendingFileOpens;
		// A second mutex, separate from m_Mutex on purpose: the two collections
		// are independent, and one lock covering both would mean a long
		// render of the message list blocks the worker's file-write
		// notification and vice versa.
		// DEFECT (lock ordering, documented not fixed): nothing in this header
		// states the rule, but the .cpp must never hold both at once, or a
		// future change could introduce a deadlock. The correct discipline is
		// one lock per scope.
		std::mutex m_PendingMutex;
	};

}
