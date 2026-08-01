#include "wlpch.h"
#include "ScriptEditorPanel.h"

#include <imgui/imgui.h>
#include <imgui/TextEditor.h>

#include <fstream>
#include <sstream>

namespace Wasteland
{

	static TextEditor::LanguageDefinition CreatePythonLanguageDef()
	{
		TextEditor::LanguageDefinition langDef;
		langDef.mName = "Python";
		langDef.mCaseSensitive = true;
		langDef.mSingleLineComment = "#";
		langDef.mCommentStart = "\"\"\"";
		langDef.mCommentEnd = "\"\"\"";
		langDef.mAutoIndentation = true;
		langDef.mPreprocChar = '\0';

		langDef.mKeywords = {
			"False", "None", "True", "and", "as", "assert", "async", "await",
			"break", "class", "continue", "def", "del", "elif", "else", "except",
			"finally", "for", "from", "global", "if", "import", "in", "is",
			"lambda", "nonlocal", "not", "or", "pass", "raise", "return",
			"try", "while", "with", "yield"
		};

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			R"(([a-zA-Z_][a-zA-Z0-9_]*))", TextEditor::PaletteIndex::Identifier));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			R"((\"([^\"\\]|\\.)*\"|\'([^\'\\]|\\.)*\'))", TextEditor::PaletteIndex::String));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			R"(([0-9]+\.?[0-9]*([eE][+-]?[0-9]+)?|0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+))", TextEditor::PaletteIndex::Number));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			R"((#[^\n]*)+)", TextEditor::PaletteIndex::Comment));

		langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
			R"(([\[\]\{\}\(\)\,\.\:\;\=\+\-\*\/\%\<\>\!\&\|\^~]))", TextEditor::PaletteIndex::Punctuation));

		return langDef;
	}

	ScriptEditorPanel::ScriptEditorPanel()
	{
		m_Editor = new TextEditor();
		SetupPythonLanguage();
	}

	ScriptEditorPanel::~ScriptEditorPanel()
	{
		delete m_Editor;
	}

	void ScriptEditorPanel::SetupPythonLanguage()
	{
		auto langDef = CreatePythonLanguageDef();
		m_Editor->SetLanguageDefinition(langDef);
	}

	void ScriptEditorPanel::OpenFile(const std::filesystem::path &path)
	{
		if (!std::filesystem::exists(path))
			return;

		std::ifstream file(path);
		if (!file.is_open())
			return;

		std::stringstream ss;
		ss << file.rdbuf();
		file.close();

		m_Editor->SetText(ss.str());
		m_CurrentFilePath = path;
		m_FileOpen = true;
		m_ShowPanel = true;
	}

	void ScriptEditorPanel::SaveFile()
	{
		if (!m_FileOpen)
			return;

		std::ofstream file(m_CurrentFilePath);
		if (!file.is_open())
			return;

		file << m_Editor->GetText();
		file.close();
	}

	void ScriptEditorPanel::CloseFile()
	{
		m_Editor->SetText("");
		m_FileOpen = false;
		m_CurrentFilePath.clear();
	}

	void ScriptEditorPanel::OnImGuiRender()
	{
		if (!m_ShowPanel)
			return;

		std::string windowTitle = "Script Editor";
		if (m_FileOpen)
			windowTitle += " - " + m_CurrentFilePath.filename().string();

		ImGui::Begin(windowTitle.c_str(), &m_ShowPanel);

		if (m_FileOpen)
		{
			if (ImGui::Button("Save"))
				SaveFile();

			ImGui::SameLine();
			if (ImGui::Button("Close"))
				CloseFile();

			ImGui::SameLine();
			if (m_Editor->IsTextChanged())
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(unsaved changes)");
		}
		else
		{
			ImGui::TextDisabled("No file open. Use Scripts > Open to edit a file.");
		}

		ImGui::Separator();

		if (m_FileOpen)
		{
			auto cursorPos = m_Editor->GetCursorPosition();
			ImGui::Text("Line %d, Col %d", cursorPos.mLine + 1, cursorPos.mColumn + 1);
			ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f);
			ImGui::Text("Lines: %d", m_Editor->GetTotalLines());

			ImGui::Separator();

			auto contentSize = ImGui::GetContentRegionAvail();
			m_Editor->Render("##TextEditor", contentSize);
		}

		ImGui::End();
	}

}
