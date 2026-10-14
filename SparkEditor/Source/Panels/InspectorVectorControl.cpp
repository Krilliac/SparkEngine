/**
 * @file InspectorVectorControl.cpp
 * @brief Inspector XYZ drag control shared by the Transform and reflected Vector3 renderers.
 */

#include "InspectorPanel.h"
#include <imgui.h>
#include <imgui_internal.h> // ImGui::FindRenderedTextEnd: honour "##" hidden labels
#include <algorithm>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{
    void InspectorPanel::DrawVec3Control(const char* label, float* values, float resetValue, float speed)
    {
        // Each axis is identified by its letter and its Axis role, never by colour alone.
        const Spark::Ui::Role axisRoles[] = {Spark::Ui::Role::AxisX, Spark::Ui::Role::AxisY, Spark::Ui::Role::AxisZ};
        const char* axes[] = {"X", "Y", "Z"};
        const char* fields[] = {"##X", "##Y", "##Z"};

        ImGui::PushID(label);
        // A "##id" label (property-grid rows draw the name in their label column) shows no text.
        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        if (labelEnd != label)
        {
            ImGui::TextUnformatted(label, labelEnd);
        }

        // The row is three [letter button][drag field] pairs. The button is at least a frame high
        // and wide enough for its letter, so it never clips at large fonts or UI scales.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float available = ImGui::GetContentRegionAvail().x;
        const float letterWidth = ImGui::CalcTextSize("W").x + style.FramePadding.x * 2.0f;
        const float buttonSize = std::max(ImGui::GetFrameHeight(), letterWidth);
        const float gap = style.ItemSpacing.x;
        const float rowFieldWidth = (available - 3.0f * buttonSize - 5.0f * gap) / 3.0f;

        // Below about three characters per field a row would clip Z or squeeze each number past
        // readability; stack the axes instead. All axes keep stable IDs in both layouts.
        const bool stacked = rowFieldWidth < Spark::Ui::Em(3.0f);
        const float fieldWidth = std::max(1.0f, stacked ? available - buttonSize - gap : rowFieldWidth);

        for (int axis = 0; axis < 3; ++axis)
        {
            if (axis != 0 && !stacked)
            {
                ImGui::SameLine();
            }
            {
                const Spark::Ui::ScopedRoleColor fill(ImGuiCol_Button, axisRoles[axis]);
                const Spark::Ui::ScopedRoleColor hover(ImGuiCol_ButtonHovered, axisRoles[axis], 0.85f);
                const Spark::Ui::ScopedRoleColor active(ImGuiCol_ButtonActive, axisRoles[axis], 0.7f);
                const Spark::Ui::ScopedRoleColor letter(ImGuiCol_Text, Spark::Ui::Role::TextOnAccent);
                if (ImGui::Button(axes[axis], ImVec2(buttonSize, ImGui::GetFrameHeight())))
                {
                    values[axis] = resetValue;
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Reset %s to %.3g", axes[axis], resetValue);
            }
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetNextItemWidth(fieldWidth + gap);
            ImGui::DragFloat(fields[axis], &values[axis], speed);
        }
        ImGui::PopID();
    }
} // namespace SparkEditor
