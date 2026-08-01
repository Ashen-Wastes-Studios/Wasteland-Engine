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

	private:
		void SetupPythonLanguage();

		TextEditor *m_Editor;
		std::filesystem::path m_CurrentFilePath;
		bool m_FileOpen = false;
		bool m_ShowPanel = true;
	};

}
