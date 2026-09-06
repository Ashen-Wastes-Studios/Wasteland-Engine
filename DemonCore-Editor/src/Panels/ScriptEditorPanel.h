#pragma once

#include <string>
#include <filesystem>

class TextEditor;

namespace Wasteland
{

	class ScriptEditorPanel
	{
	public:
		ScriptEditorPanel();
		~ScriptEditorPanel();

		void OnImGuiRender();

		void OpenFile(const std::filesystem::path &path);
		void SaveFile();
		void CloseFile();

		bool IsOpen() const { return m_FileOpen; }
		const std::filesystem::path &GetFilePath() const { return m_CurrentFilePath; }

		bool IsVisible() const { return m_Visible && m_ShowPanel; }
		void SetVisible(bool visible) { m_Visible = visible; m_ShowPanel = visible; }

	private:
		void SetupPythonLanguage();

		TextEditor *m_Editor;
		std::filesystem::path m_CurrentFilePath;
		bool m_FileOpen = false;
		bool m_ShowPanel = true;
		bool m_Visible = true;
	};

}
