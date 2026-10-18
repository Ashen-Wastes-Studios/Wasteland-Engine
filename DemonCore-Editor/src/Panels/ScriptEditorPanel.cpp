// Precompiled header first, per project convention.
#include "wlpch.h"
// Own header. Note TextEditor itself is forward-declared there, so the
// real definition has to be pulled in below -- hence the include on
// line 5 rather than in the header.
#include "ScriptEditorPanel.h"

// The immediate-mode GUI API driving every widget below.
// Angle brackets: third-party dependency, resolved from the include
// path rather than this directory.
#include <imgui/imgui.h>
// The third-party text editor widget. Included HERE, in the .cpp,
// which is the whole point of the forward declaration in the header:
// every other translation unit that merely holds a panel skips this
// header entirely.
#include <imgui/TextEditor.h>

// Quoted form because it is a project header on the include path.
// Supplies the FileDialogs::OpenFile helper used on line 144 --
// the native open dialog, which is the only platform-specific code
// in this file.
#include "Wasteland/Utils/PlatformUtils.h"

// std::ifstream / std::ofstream for every file read and write below.
#include <fstream>
// std::stringstream, used as a whole-file slurp buffer in OpenFile
// (lines 78-79) and again in ReloadFile (lines 115-116).
#include <sstream>

namespace Wasteland
{

	// file-static: internal linkage, so this builder is invisible outside
	// this translation unit and there is no ODR clash if another panel
	// ever grows the same helper.
	// Returns by value; the ~30 regex/keyword strings inside are copied
	// or moved into the caller's local on line 65.
	static TextEditor::LanguageDefinition CreatePythonLanguageDef()
	{
		// Default-constructed, then filled field by field below. Anything
		// not assigned here keeps the widget's default, which is why the
		// block-comment and line-continuation configs are left alone.
		TextEditor::LanguageDefinition langDef;
		// Display name only. It is what a language dropdown would show; this
		// panel hardcodes Python so nothing reads it.
		langDef.mName = "Python";
		// Python IS case sensitive, unlike SQL or Pascal which the same
		// widget also supports.
		langDef.mCaseSensitive = true;
		// Python has exactly one line-comment form. The widget supports C/C++
		// style "//" as an alternative, so this is a real choice.
		langDef.mSingleLineComment = "#";
		// Python triple-quoted strings. This is a genuine mis-mapping: the
		// widget treats a block-comment delimiter as a region and will happily
		// restyle a multi-line string as a comment. The mTokenRegexStrings
		// entry on line 38 handles triple-quotes as a STRING (so single-line
		// """docstrings""" colour correctly), but the block-comment rule is
		// still active underneath it. Worth confirming against the widget's
		// precedence rules.
		langDef.mCommentStart = "\"\"\"";
		// Same delimiter, so the block-comment region is non-greedy: only an
		// immediately-following """ closes it.
		langDef.mCommentEnd = "\"\"\"";
		// Carry the previous line's indentation on Enter, which is what
		// makes Python tolerable to type in this widget.
		langDef.mAutoIndentation = true;
		// The NUL character disables preprocessor-directive highlighting.
		// Python has no preprocessor, so this is the correct "off" value --
		// the C++ default in the widget is presumably '#', which would
		// wrongly highlight any line starting with # as a directive.
		langDef.mPreprocChar = '\0';

		// Braced assignment, so this REPLACES any default keyword list rather
		// than appending to it.
		// Python 3 keywords, sorted alphabetically apart from the three
		// capitalised ones which are listed first (False, None, True).
		langDef.mKeywords = {
			// Python 3.7+ also has "async"/"await" as soft keywords; listing them
			// as hard keywords is a cosmetic over-highlight in a few contexts
			// (they are legal identifiers in 3.6 and, since 3.7, only reserved
			// inside async def).
			// "match"/"case" from 3.10 structural pattern matching are MISSING
			// here, so they will not be highlighted. Not a correctness bug --
			// just inconsistent with the rest of the 3.x list.
			"False", "None", "True", "and", "as", "assert", "async", "await",
			"break", "class", "continue", "def", "del", "elif", "else", "except",
			"finally", "for", "from", "global", "if", "import", "in", "is",
			"lambda", "nonlocal", "not", "or", "pass", "raise", "return",
			"try", "while", "with", "yield"
		};

		// Token regexes, applied IN ORDER. Each entry pairs a regex with a
		// palette index; the widget colours the leftmost match at each
		// position, so earlier entries win ties. That ordering is why the
		// identifier rule (line 35) is registered before numbers and
		// punctuation below.
		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			// Raw string literal so the backslashes reach the regex engine intact.
			// Character class is explicit about the leading underscore, which
			// Python allows and most C-like languages do not.
			// Deliberately does NOT exclude keywords: keyword colouring is a
			// separate lookup against mKeywords, so a name that matches the
			// identifier regex is then checked against the keyword list.
			R"(([a-zA-Z_][a-zA-Z0-9_]*))", TextEditor::PaletteIndex::Identifier));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			// Single- AND double-quoted strings, in one alternation, with the
			// two capture groups matching the doubled quote characters.
			// DEFECT: this handles only SINGLE-LINE strings. A multi-line
			// triple-quoted string is not matched by either alternative, because
			// there is no DOTALL behaviour here and no allowance for a newline
			// inside the quotes. The mCommentStart config on line 21 is
			// presumably what colours triple-quoted regions instead -- which is
			// why they read as comments rather than strings.
			R"((\"([^\"\\]|\\.)*\"|'([^'\\]|\\.)*'))", TextEditor::PaletteIndex::String));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			// Covers decimal (with optional fraction and exponent), hex, binary
			// and octal. Alternation order matters: the plain decimal branch comes
			// first, but it cannot match a leading 0 followed by x/b/o, so the
			// prefixed forms still get a chance.
			// DEFECT (minor): a number written as .5 (no leading digit) does not
			// match -- the leading [0-9]+ is mandatory. Also no underscore digit
			// separators (1_000_000), which Python allows.
			R"(([0-9]+\.?[0-9]*([eE][+-]?[0-9]+)?|0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+))", TextEditor::PaletteIndex::Number));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			// Line comments. The negated class excludes the newline, so this
			// cannot run past the end of its line.
			// The outer + is redundant here given [^\n]*, but harmless.
			// Registered AFTER the identifier and number rules, which matters
			// only if a line begins with something those already matched.
			R"((#[^\n]*)+)", TextEditor::PaletteIndex::Comment));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			// Single-character punctuation class. Note what is NOT here:
			// brackets [ ] and the dot in an ellipsis are absent, and neither is
			// the backtick. Python list literals and subscripts will therefore
			// leave those characters uncoloured.
			R"(([\{\}\(\)\,\.\:\;\=\+\-\*\/\%\<\>\!\&\|\^~]))", TextEditor::PaletteIndex::Punctuation));

		// Returned by value; NRVO or a move applies, so this does not copy
		// the vectors of regex strings a second time.
		return langDef;
	}

	// Out-of-line in the .cpp because the body touches TextEditor, whose
	// definition is deliberately not visible in the header.
	ScriptEditorPanel::ScriptEditorPanel()
	{
		// Raw new to match the raw-pointer member (see the header). DEFECT:
		// the allocation is not exception-safe in the sense that if
		// SetupPythonLanguage() on line 55 throws, m_Editor leaks -- there
		// is no RAII wrapper. Low risk in practice.
		// This is the only place m_Editor is assigned.
		m_Editor = new TextEditor();
		// Called once here and never again: the language definition does not
		// depend on which file is opened.
		SetupPythonLanguage();
	}

	// Out-of-line, and this is the REASON it cannot be defaulted inline:
	// `delete m_Editor` needs TextEditor's complete type, which the
	// header forward-declares.
	ScriptEditorPanel::~ScriptEditorPanel()
	{
		// DEFECT: m_Editor is NOT set to nullptr afterwards. A second delete,
		// or a use-after-destroy through any surviving pointer, would be a
		// double free. The class is non-copyable only by convention here --
		// there is no deleted copy constructor or assignment operator, so
		// copying a ScriptEditorPanel would copy the raw pointer and produce
		// exactly that double free. That is a real hazard in this header.
		delete m_Editor;
	}

	// Private, called only from the constructor.
	void ScriptEditorPanel::SetupPythonLanguage()
	{
		// Materialises the definition built by the static helper above.
		auto langDef = CreatePythonLanguageDef();
		// Passes it BY REFERENCE to the widget (the signature takes a const
		// LanguageDefinition&), which is why langDef must outlive the call.
		// Whether the widget copies or keeps the reference is not visible
		// here; if it kept it, langDef going out of scope at this function's
		// end would be a dangling reference. The local is destroyed
		// immediately after, so this hinges on the widget's implementation.
		m_Editor->SetLanguageDefinition(langDef);
	}

	// Loads a file into the editor buffer and marks it as the current one.
	void ScriptEditorPanel::OpenFile(const std::filesystem::path &path)
	{
		// Two existence checks in a row (this and line 75) because exists()
		// only answers about the filesystem, not about whether the file can
		// actually be opened for reading.
		if (!std::filesystem::exists(path))
			return;

		// Constructs and opens in one step. `path` converts to the
		// platform-appropriate string type implicitly.
		std::ifstream file(path);
		if (!file.is_open())
			return;

		// Slurp buffer. Reading via a stringstream and then ss.str() avoids
		// seeking to the end and calling seekg(0) to learn the length, which
		// is the usual reason to write it this way.
		std::stringstream ss;
		// streambuf insertion copies the whole stream in one go.
		// DEFECT: the resulting failbit from the extraction is not checked, so
		// a partially-read file loads silently -- there is no distinction
		// between a full read and a truncated one.
		ss << file.rdbuf();
		// Explicit close; the destructor would do it anyway. It flushes any
		// read-ahead state.
		file.close();

		// Replaces the entire buffer. DEFECT: TextEditor tracks a modified
		// flag internally, and SetText resets it, so opening a file with
		// unsaved edits discards them with no prompt -- the panel has no
		// unsaved-changes check anywhere, despite the header comment on
		// ReloadFile mentioning one.
		m_Editor->SetText(ss.str());
		// Stored by value, which the header notes is required because path is
		// a const reference that a caller may pass a temporary for.
		m_CurrentFilePath = path;
		// The FILE-open flag. Distinct from m_ShowPanel/m_Visible: this one
		// decides whether Save/Reload/Close appear, the others decide whether
		// the panel is drawn at all.
		m_FileOpen = true;
		// Force-shows the panel when a file opens. DEFECT: this is an
		// independent write to m_ShowPanel, which is exactly what the header
		// comment (lines 85-89) speculates about. Consequence: a user who
		// closed the panel with its X button and then opens a file from
		// elsewhere gets it back.
		m_ShowPanel = true;
	}

	// No path parameter: saves whatever m_CurrentFilePath holds. There is
	// no Save As, so a buffer whose path was never set would write to an
	// empty path -- though m_FileOpen guards that case.
	void ScriptEditorPanel::SaveFile()
	{
		// Guard against saving with no path. Every one of OpenFile,
		// SaveFile and ReloadFile opens with this same check.
		if (!m_FileOpen)
			return;

		// Truncating open: an existing file is overwritten from byte 0. There
		// is no temp-file-then-rename, so a failure partway through a large
		// save leaves the original truncated on disk.
		std::ofstream file(m_CurrentFilePath);
		// Read-only files, missing directories and permission errors all land
		// here.
		// DEFECT: a silent return. The caller cannot tell a successful save
		// from a failed one, and neither can the user -- there is no status
		// message, no log, and no return value.
		if (!file.is_open())
			return;

		// Serialises the whole buffer. DEFECT: the stream state is not
		// checked, so a full disk or a write error after the header goes
		// unnoticed and the file is left partial.
		file << m_Editor->GetText();
		file.close();
	}

	// Drops the file but leaves the panel visible -- a deliberate
	// distinction from OpenFile, which force-shows it.
	void ScriptEditorPanel::CloseFile()
	{
		// Empties the widget so the buffer content cannot be read back after
		// close. GetCurrentText() would otherwise keep returning the old
		// text.
		m_Editor->SetText("");
		m_FileOpen = false;
		m_CurrentFilePath.clear();
	}

	// Discards the buffer and re-reads from disk.
	void ScriptEditorPanel::ReloadFile()
	{
		// Same guard as SaveFile, immediately followed by the open attempt.
		if (!m_FileOpen)
			return;
		// No exists() check here, unlike OpenFile line 71. The is_open() check
		// below covers it, so the difference is redundant rather than wrong.
		std::ifstream file(m_CurrentFilePath);
		if (!file.is_open())
			return;
		std::stringstream ss;
		ss << file.rdbuf();
		file.close();
		// DEFECT: unlike OpenFile, this does NOT touch m_CurrentFilePath,
		// m_FileOpen or m_ShowPanel -- which is correct, since all three
		// already describe this file. But it also does not warn first:
		// reloading silently throws away any edits in the buffer.
		m_Editor->SetText(ss.str());
	}

	// const method, but it calls m_Editor->GetText() through a raw pointer,
	// which is fine only because that pointee is not const-propagated.
	std::string ScriptEditorPanel::GetCurrentText() const
	{
		// Copies the entire buffer out on every call. The header documents
		// this; for a large script polled per frame it would be expensive.
		return m_Editor->GetText();
	}

	// Overwrites the buffer without touching m_CurrentFilePath. A later
	// SaveFile() therefore writes this externally-supplied text to
	// whatever file was last opened.
	void ScriptEditorPanel::SetText(const std::string &text)
	{
		m_Editor->SetText(text);
	}

	// The draw entry. Called every frame while the editor is up; it is
	// the caller's job to decide when that is.
	void ScriptEditorPanel::OnImGuiRender()
	{
		// Early-out before any ImGui call. Correct: ImGui::Begin/End must be
		// balanced, and returning here keeps them balanced trivially.
		// Duplicates the header's IsVisible(), which is the same conjunction.
		if (!m_Visible || !m_ShowPanel)
			return;

		// Built as a std::string rather than an ImGui title, because the
		// filename gets concatenated on conditionally.
		std::string windowTitle = "Script Editor";
		if (m_FileOpen)
			// filename() strips the directories, so only the base name appears in
			// the title bar. Two files with the same name in different directories
			// are therefore indistinguishable here.
			windowTitle += " - " + m_CurrentFilePath.filename().string();

		// The second argument is the close-button flag: ImGui writes false
		// into m_ShowPanel when the user clicks X. That is the independent
		// write the header speculated about -- and it means SetVisible(false)
		// followed by an unrelated SetVisible(true) is not the only way
		// m_ShowPanel can drift from m_Visible.
		ImGui::Begin(windowTitle.c_str(), &m_ShowPanel);

		// Immediate mode: the return value is "was clicked this frame",
		// evaluated fresh every frame. There is no action queue.
		if (ImGui::Button("Open"))
		{
			// The native OS file dialog, reached through the platform utils
			// header. DEFECT: this is a MODAL, BLOCKING call -- it pumps its own
			// event loop and will stall the application until the user picks a
			// file or cancels.
			// The filter string uses embedded NULs (\0) as the Win32-style
			// multi-filter convention, so on a non-Windows backend the whole
			// remainder is one opaque filter string unless FileDialogs
			// translates it.
			std::string filepath = FileDialogs::OpenFile("Python Script (*.py)\0*.py\0");
			if (!filepath.empty())
				// filepath is a std::string; OpenFile takes a std::filesystem::path,
				// so this is an implicit conversion at the call.
				OpenFile(filepath);
		}

		// Save/Reload/Close and the editor widget only exist while a file is
		// open; otherwise the else branch shows a placeholder.
		if (m_FileOpen)
		{
			// Lays the next button out beside the previous one instead of on a
			// new row. Repeated four times below to build one toolbar row.
			ImGui::SameLine();
			if (ImGui::Button("Save"))
				SaveFile();
			ImGui::SameLine();
			if (ImGui::Button("Reload"))
				ReloadFile();
			ImGui::SameLine();
			if (ImGui::Button("Close"))
				CloseFile();

			// The space left after the button row, i.e. the editor gets everything
			// below it. Standard ImGui idiom for a fill-the-remaining-space
			// widget.
			ImVec2 size = ImGui::GetContentRegionAvail();
			// The third argument is TextEditor's "with parent" flag, making the
			// widget size itself to the given rectangle rather than to its own
			// preferred size.
			// "ScriptEditor" is the ImGui window name, which gives the widget a
			// stable ID for ImGui state. It is a bare string because ImGui takes
			// a const char* here.
			m_Editor->Render("ScriptEditor", size, true);
		}
		else
		{
			// Dimmed text, the conventional way to show a non-interactive hint.
			ImGui::TextDisabled("No script open. Open a .py file to edit.");
		}

		// Closes the Begin on line 140. Every path from line 140 to here must
		// reach it or ImGui asserts -- there is no early return inside that
		// region, which is why this function has no guard after Begin.
		ImGui::End();
	}

}
