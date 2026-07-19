/**
 * @file InspectorComponentRenderers_Reflected.cpp
 * @brief Inspector renderers using reflection-driven field rendering
 *
 * Contains: the RenderReflectedFields helper (with category, tooltip, and
 * enum dropdown support) and its file-local widget helpers.
 *
 * The per-component renderers built on it live in the sibling split files
 * (shared macros/builders in InspectorComponentRenderers_ReflectedInternal.h):
 *   InspectorComponentRenderers_ReflectedVolumes.cpp     — volumes, probes, placement, audio, misc
 *   InspectorComponentRenderers_ReflectedGameplay.cpp    — gameplay components
 *   InspectorComponentRenderers_Reflected2D.cpp          — 2D components
 *   InspectorComponentRenderers_ReflectedConditional.cpp — conditional field visibility
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    // ============================================================================
    // Reflection-driven field rendering
    // ============================================================================

    // Render a single field widget based on its FieldInfo metadata.
    static void RenderSingleField(const Spark::FieldInfo& field, char* dst)
    {
        // Enum fields with names → combo dropdown
        if (field.type == Spark::FieldType::Enum || (field.type == Spark::FieldType::Int && !field.enumNames.empty()))
        {
            int* val = reinterpret_cast<int*>(dst);
            if (!field.enumNames.empty())
            {
                // Build items string for ImGui::Combo (null-separated, double-null terminated)
                std::string items;
                for (const auto& name : field.enumNames)
                {
                    items += name;
                    items += '\0';
                }
                items += '\0';
                ImGui::Combo(field.name.c_str(), val, items.c_str());
            }
            else
            {
                ImGui::DragInt(field.name.c_str(), val);
            }
            return;
        }

        switch (field.type)
        {
        case Spark::FieldType::Bool:
            ImGui::Checkbox(field.name.c_str(), reinterpret_cast<bool*>(dst));
            break;

        case Spark::FieldType::Int:
            if (field.hasRange)
            {
                ImGui::SliderInt(field.name.c_str(), reinterpret_cast<int*>(dst), static_cast<int>(field.rangeMin),
                                 static_cast<int>(field.rangeMax));
            }
            else
            {
                ImGui::DragInt(field.name.c_str(), reinterpret_cast<int*>(dst));
            }
            break;

        case Spark::FieldType::Float:
            if (field.hasRange)
            {
                ImGui::SliderFloat(field.name.c_str(), reinterpret_cast<float*>(dst), field.rangeMin, field.rangeMax,
                                   "%.3f");
            }
            else
            {
                ImGui::DragFloat(field.name.c_str(), reinterpret_cast<float*>(dst), 0.1f);
            }
            break;

        case Spark::FieldType::Double:
        {
            auto val = static_cast<float>(*reinterpret_cast<double*>(dst));
            if (ImGui::DragFloat(field.name.c_str(), &val, 0.1f))
            {
                *reinterpret_cast<double*>(dst) = static_cast<double>(val);
            }
            break;
        }

        case Spark::FieldType::String:
            // Editor data types use char[N] arrays; handle as fixed buffer
            if (field.size > sizeof(std::string))
            {
                ImGui::InputText(field.name.c_str(), reinterpret_cast<char*>(dst), field.size);
            }
            else
            {
                auto* str = reinterpret_cast<std::string*>(dst);
                char buf[256];
                strncpy(buf, str->c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
                {
                    *str = buf;
                }
            }
            break;

        case Spark::FieldType::Vector2:
        {
            float* v = reinterpret_cast<float*>(dst);
            ImGui::DragFloat2(field.name.c_str(), v, 0.1f);
            break;
        }

        case Spark::FieldType::Vector3:
        {
            float* v = reinterpret_cast<float*>(dst);
            InspectorPanel::DrawVec3Control(field.name.c_str(), v, 0.0f, 0.1f);
            break;
        }

        case Spark::FieldType::Vector4:
        {
            float* v = reinterpret_cast<float*>(dst);
            ImGui::ColorEdit4(field.name.c_str(), v);
            break;
        }

        default:
            ImGui::TextDisabled("%s (unsupported type)", field.name.c_str());
            break;
        }
    }

    // Check if a field should be visible based on its visibleWhenField condition.
    static bool IsFieldVisible(const Spark::FieldInfo& field, const void* data,
                               const std::vector<Spark::FieldInfo>& allFields)
    {
        if (field.visibleWhenField.empty())
            return true; // No condition — always visible

        // Find the controlling field and read its int value
        for (const auto& ctrl : allFields)
        {
            if (ctrl.fieldName == field.visibleWhenField)
            {
                const auto* src = static_cast<const char*>(data) + ctrl.offset;
                int val = 0;
                if (ctrl.type == Spark::FieldType::Bool)
                {
                    bool b = false;
                    std::memcpy(&b, src, sizeof(bool));
                    val = b ? 1 : 0;
                }
                else
                {
                    std::memcpy(&val, src, sizeof(int));
                }
                return val == field.visibleWhenValue;
            }
        }
        return true; // Controlling field not found — show by default
    }

    void InspectorPanel::RenderReflectedFields(void* data, const std::vector<Spark::FieldInfo>& fields)
    {
        if (!data)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Inspector: RenderReflectedFields called with null data");
            ImGui::TextDisabled("(Component data unavailable)");
            return;
        }

        // Check if any field has a category assigned
        bool hasCategories = false;
        for (const auto& field : fields)
        {
            if (!field.category.empty())
            {
                hasCategories = true;
                break;
            }
        }

        if (hasCategories)
        {
            // Collect unique categories in order of first appearance
            std::vector<std::string> categories;
            for (const auto& field : fields)
            {
                const auto& cat = field.category;
                bool found = false;
                for (const auto& c : categories)
                {
                    if (c == cat)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    categories.push_back(cat);
            }

            for (const auto& cat : categories)
            {
                bool inSection = !cat.empty();
                if (inSection)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", cat.c_str());
                }

                for (const auto& field : fields)
                {
                    if (field.category != cat)
                        continue;
                    if (!IsFieldVisible(field, data, fields))
                        continue;

                    auto* dst = static_cast<char*>(data) + field.offset;
                    RenderSingleField(field, dst);

                    if (!field.tooltip.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", field.tooltip.c_str());

                    if (field.readOnly)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(read-only)");
                    }
                }
            }
        }
        else
        {
            // Flat rendering (no categories)
            for (const auto& field : fields)
            {
                if (!IsFieldVisible(field, data, fields))
                    continue;
                auto* dst = static_cast<char*>(data) + field.offset;
                RenderSingleField(field, dst);

                if (!field.tooltip.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", field.tooltip.c_str());

                if (field.readOnly)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(read-only)");
                }
            }
        }
    }

} // namespace SparkEditor
