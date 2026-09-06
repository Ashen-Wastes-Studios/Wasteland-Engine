#include "wlpch.h"
#include "ContentBrowserPanel.h"

#include <imgui/imgui.h>

namespace Wasteland
{

    // Once we have projects, change this
    extern const std::filesystem::path g_AssetPath = "assets";

    ContentBrowserPanel::ContentBrowserPanel()
        : m_CurrentDirectory(g_AssetPath)
    {
        m_CurrentDirectory = g_AssetPath;
    }

    void ContentBrowserPanel::OnImGuiRender()
    {
        if (!m_Visible)
            return;

        ImGui::Begin("Content Browser", &m_Visible);

        if (m_CurrentDirectory != std::filesystem::path(g_AssetPath))
        {
            if (ImGui::Button("<-"))
            {
                m_CurrentDirectory = m_CurrentDirectory.parent_path();
            }
        }

        for (auto &directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory))
        {
            const auto &path = directoryEntry.path();
            auto relativePath = std::filesystem::relative(path, g_AssetPath);
            std::string filenameString = relativePath.filename().string();

            ImGui::PushID(filenameString.c_str());

            if (ImGui::Button(filenameString.c_str()))
            {
                // Single click logic here
            }

            if (ImGui::BeginDragDropSource())
            {
                const wchar_t *itemPath = relativePath.c_str();
                ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (wcslen(itemPath) + 1) * sizeof(wchar_t));
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (directoryEntry.is_directory())
                    m_CurrentDirectory /= path.filename();
            }

            ImGui::PopID();
        }

        ImGui::End();
    }

}