#pragma once

#include <imgui.h>

namespace SparkEditor
{

    /// Draw a vertical separator line between SameLine() items.
    /// Usage: ImGui::SameLine(); VerticalSeparator(); ImGui::SameLine();
    inline void VerticalSeparator()
    {
        ImGui::SameLine();
        const float height = ImGui::GetFrameHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x, pos.y + height),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy(ImVec2(4.0f, height));
        ImGui::SameLine();
    }

} // namespace SparkEditor
