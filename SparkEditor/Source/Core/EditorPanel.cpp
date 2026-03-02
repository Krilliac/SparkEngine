/**
 * @file EditorPanel.cpp
 * @brief Implementation of base editor panel class
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorPanel.h"
#include <imgui.h>

namespace SparkEditor {

EditorPanel::EditorPanel(const std::string& name, const std::string& id)
    : m_name(name)
    , m_id(id)
    , m_title(name)
{
}

bool EditorPanel::BeginPanel()
{
    if (!m_isVisible) {
        return false;
    }

    ImGuiWindowFlags flags = 0;
    if (!m_isClosable) {
        flags |= ImGuiWindowFlags_NoCollapse;
    }

    // Compose display title with icon, but use stable ###ID for docking
    std::string displayTitle;
    if (m_icon.empty()) {
        displayTitle = m_title + "###" + m_id;
    } else {
        displayTitle = m_icon + "  " + m_title + "###" + m_id;
    }

    bool visible = m_isVisible;
    bool result = ImGui::Begin(displayTitle.c_str(), m_isClosable ? &visible : nullptr, flags);
    
    if (visible != m_isVisible) {
        SetVisible(visible);
        NotifyStateChange();
    }

    if (result) {
        m_isFocused = ImGui::IsWindowFocused();
        
        // Update position and size
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        m_posX = pos.x;
        m_posY = pos.y;
        m_width = size.x;
        m_height = size.y;
    }

    return result;
}

void EditorPanel::EndPanel()
{
    ImGui::End();
}

void EditorPanel::NotifyStateChange()
{
    if (m_stateChangeCallback) {
        m_stateChangeCallback(this);
    }
}

} // namespace SparkEditor