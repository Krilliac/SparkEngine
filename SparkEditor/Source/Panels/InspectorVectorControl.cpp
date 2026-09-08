#include "InspectorPanel.h"
#include <imgui.h>
#include <algorithm>

namespace SparkEditor
{
    void InspectorPanel::DrawVec3Control(const char* label, float* values, float resetValue, float speed)
    {
        const ImVec4 colors[] = {ImVec4(0.9f, 0.2f, 0.2f, 1.0f), ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
                                 ImVec4(0.2f, 0.4f, 0.9f, 1.0f)};
        const ImVec4 hovered[] = {ImVec4(1.0f, 0.3f, 0.3f, 1.0f), ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                                  ImVec4(0.3f, 0.5f, 1.0f, 1.0f)};
        const char* axes[] = {"X", "Y", "Z"};
        const char* fields[] = {"##X", "##Y", "##Z"};

        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        const float available = ImGui::GetContentRegionAvail().x;
        const float buttonSize = ImGui::GetFrameHeight();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float rowFieldWidth = (available - 3.0f * buttonSize - 5.0f * gap) / 3.0f;
        // Keep numbers editable at narrow dock widths instead of clipping Z or
        // squeezing each field below a readable width. All axes keep stable IDs.
        const bool stacked = rowFieldWidth < 48.0f;
        const float fieldWidth = std::max(1.0f, stacked ? available - buttonSize - gap : rowFieldWidth);

        for (int axis = 0; axis < 3; ++axis)
        {
            if (axis != 0 && !stacked)
                ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, colors[axis]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered[axis]);
            if (ImGui::Button(axes[axis], ImVec2(buttonSize, buttonSize)))
                values[axis] = resetValue;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset %s to %.3g", axes[axis], resetValue);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fieldWidth);
            ImGui::DragFloat(fields[axis], &values[axis], speed);
        }
        ImGui::PopID();
    }
}
