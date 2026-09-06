#pragma once

#include <filesystem>

namespace Wasteland {

	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }
	private:
		std::filesystem::path m_CurrentDirectory;
		bool m_Visible = true;
	};

}