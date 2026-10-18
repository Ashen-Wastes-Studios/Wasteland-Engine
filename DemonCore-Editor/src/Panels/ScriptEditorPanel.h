#pragma once

#include <string>
// std::filesystem rather than a hand-rolled string path type. The
// editor uses this for file dialogs and for the unsaved-changes
// prompt, so it needs to interoperate with the rest of the editor's
// path handling.
#include <filesystem>

// Forward declaration: the third-party TextEditor is referenced only
// through a pointer here, so its header -- which pulls in the entire
// editor widget set -- stays out of every translation unit that merely
// holds a panel.
class TextEditor;

namespace Wasteland
{

	// A plain class, not a Window/UI base subclass: the editor's panels
	// are composed by the owning window rather than registered with a
	// panel manager, which is why there is no virtual destructor and no
	// base interface.
	class ScriptEditorPanel
	{
	public:
		// Allocates m_Editor and calls SetupPythonLanguage. Written out of
		// line in the header only as a declaration; the definition is in the
		// editor's .cpp.
		ScriptEditorPanel();
		// Non-virtual and declared here: it must destroy m_Editor. Since the
		// class has no virtual functions, destroying through a base pointer
		// would leak -- so m_Editor must never be owned polymorphically.
		~ScriptEditorPanel();

		// The ImGui draw entry. Naming it explicitly rather than overriding
		// a base hook is the convention across the editor panels, and it
		// makes the call site an ordinary member call.
		void OnImGuiRender();

		// Takes a const reference and must COPY it internally: m_CurrentFilePath
		// outlives this call, and the caller is free to pass a temporary.
		void OpenFile(const std::filesystem::path &path);
		// No path parameter: it saves whatever is open. There is no
		// SaveFileAs, so the path comes from OpenFile alone -- a file opened
		// by any other route has no path and this has to handle that.
		void SaveFile();
		void CloseFile();
		// Distinguishes reloading the same path from reopening it, so an
		// unsaved-changes prompt can be skipped when nothing was modified.
		void ReloadFile();

		// FILE open, not panel visibility -- two different questions, which
		// is why there are two separate flags (m_FileOpen and the pair below).
		bool IsOpen() const { return m_FileOpen; }
		const std::filesystem::path &GetFilePath() const { return m_CurrentFilePath; }
		// Returns a copy: the editor's buffer is internal, so a caller
		// wanting the text pays a full string copy every call.
		std::string GetCurrentText() const;
		void SetText(const std::string &text);

		// AND of two flags -- see the note on the members. m_Visible is
		// driven by a menu toggle; m_ShowPanel is the panel's own state.
		bool IsVisible() const { return m_Visible && m_ShowPanel; }
		// Sets BOTH flags from one parameter, so SetVisible(false) hides the
		// panel and SetVisible(true) shows it -- m_ShowPanel can never be
		// true while m_Visible is false unless something writes it directly.
		// That makes m_Visible redundant: IsVisible() could return m_ShowPanel
		// alone. Unless something else in the .cpp writes m_ShowPanel
		// independently, in which case this setter is not the only path and
		// the redundancy is load-bearing.
		void SetVisible(bool visible) { m_Visible = visible; m_ShowPanel = visible; }

	private:
		// One-time syntax-colouring setup, called from the ctor. Not per-file:
		// the language definition does not vary by path.
		void SetupPythonLanguage();

		// Raw pointer, not unique_ptr: TextEditor is forward-declared, so a
		// smart pointer would need a complete type at the destructor. Storing
		// it raw is what allows the destructor to live in the .cpp where the
		// definition is visible.
		TextEditor *m_Editor;
		std::filesystem::path m_CurrentFilePath;
		bool m_FileOpen = false;
		// Both default-initialised true, and SetVisible above writes both in
		// lockstep -- so as written m_ShowPanel is redundant with m_Visible.
		// Suspicious enough to be worth confirming against the .cpp, which is
		// where either a separate panel toggle or an independent write must
		// live.
		bool m_ShowPanel = true;
		bool m_Visible = true;
	};

}
