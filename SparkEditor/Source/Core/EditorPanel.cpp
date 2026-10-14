/**
 * @file EditorPanel.cpp
 * @brief Implementation of base editor panel class
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorPanel.h"
#include "EditorIcons.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include <imgui.h>

namespace SparkEditor
{

    EditorPanel::EditorPanel(const std::string& name, const std::string& id) : m_name(name), m_id(id), m_title(name)
    {
        SPARK_WARN_IF(Spark::LogCategory::Editor, name.empty(), "EditorPanel created with empty name");
        SPARK_WARN_IF(Spark::LogCategory::Editor, id.empty(), "EditorPanel created with empty id");
    }

    const char* EditorPanel::CategoryIcon(PanelCategory category)
    {
        switch (category)
        {
        case PanelCategory::Viewport:
            return ICON_FA_DESKTOP;
        case PanelCategory::Inspector:
            return ICON_FA_SLIDERS;
        case PanelCategory::Tool:
            return ICON_FA_HAMMER;
        case PanelCategory::Config:
            return ICON_FA_COG;
        case PanelCategory::Debug:
            return ICON_FA_BUG;
        case PanelCategory::Other:
            break;
        }
        return ICON_FA_TH_LARGE;
    }

    bool EditorPanel::BeginPanel()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_isVisible)
        {
            return false;
        }

        ImGuiWindowFlags flags = 0;
        if (!m_isClosable)
        {
            flags |= ImGuiWindowFlags_NoCollapse;
        }

        // Compose display title with icon (the category glyph when the panel sets none), but use the stable
        // ###ID for docking so a title or icon change never moves the panel.
        const std::string displayTitle =
            (m_icon.empty() ? std::string(CategoryIcon(m_category)) : m_icon) + "  " + m_title + "###" + m_id;

        bool visible = m_isVisible;
        ImGui::SetNextWindowSize(ImVec2(m_width, m_height), ImGuiCond_FirstUseEver);
        bool result = ImGui::Begin(displayTitle.c_str(), m_isClosable ? &visible : nullptr, flags);

        if (visible != m_isVisible)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Panel '%s' %s", m_name.c_str(), visible ? "opened" : "closed");
            SetVisible(visible);
            NotifyStateChange();
        }

        if (result)
        {
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
        if (m_stateChangeCallback)
        {
            m_stateChangeCallback(this);
        }
    }

} // namespace SparkEditor
