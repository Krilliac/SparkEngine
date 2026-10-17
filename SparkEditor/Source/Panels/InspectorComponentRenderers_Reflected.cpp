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
#include "SparkUI/Widgets.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <functional>

namespace SparkEditor
{

    // ============================================================================
    // Reflection-driven field rendering
    // ============================================================================

    // Render a single field widget based on its FieldInfo metadata. @p label is the widget's
    // ImGui label; the property grid draws the visible field name, so callers pass a hidden one.
    static bool RenderSingleField(const Spark::FieldInfo& field, char* dst, const char* label)
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
                return ImGui::Combo(label, val, items.c_str());
            }
            else
            {
                return ImGui::DragInt(label, val);
            }
        }

        switch (field.type)
        {
        case Spark::FieldType::Bool:
            return ImGui::Checkbox(label, reinterpret_cast<bool*>(dst));

        case Spark::FieldType::Int:
            if (field.hasRange)
            {
                return ImGui::SliderInt(label, reinterpret_cast<int*>(dst), static_cast<int>(field.rangeMin),
                                        static_cast<int>(field.rangeMax));
            }
            else
            {
                return ImGui::DragInt(label, reinterpret_cast<int*>(dst));
            }

        // Floating-point fields are copied in and out of the raw byte storage so the
        // widgets never alias a float through the char* view of the component.
        case Spark::FieldType::Float:
        {
            float value = 0.0f;
            std::memcpy(&value, dst, sizeof(value));
            const bool changed = field.hasRange
                                     ? ImGui::SliderFloat(label, &value, field.rangeMin, field.rangeMax, "%.3f")
                                     : ImGui::DragFloat(label, &value, 0.1f);
            if (changed)
                std::memcpy(dst, &value, sizeof(value));
            return changed;
        }

        case Spark::FieldType::Double:
        {
            double stored = 0.0;
            std::memcpy(&stored, dst, sizeof(stored));
            auto value = static_cast<float>(stored);
            if (ImGui::DragFloat(label, &value, 0.1f))
            {
                stored = static_cast<double>(value);
                std::memcpy(dst, &stored, sizeof(stored));
                return true;
            }
            return false;
        }

        case Spark::FieldType::String:
            // Editor data types use char[N] arrays; handle as fixed buffer
            if (field.size > sizeof(std::string))
            {
                return ImGui::InputText(label, reinterpret_cast<char*>(dst), field.size);
            }
            else
            {
                auto* str = reinterpret_cast<std::string*>(dst);
                char buf[256];
                strncpy(buf, str->c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText(label, buf, sizeof(buf)))
                {
                    *str = buf;
                    return true;
                }
                return false;
            }

        case Spark::FieldType::Vector2:
        {
            float v[2] = {};
            std::memcpy(v, dst, sizeof(v));
            if (ImGui::DragFloat2(label, v, 0.1f))
            {
                std::memcpy(dst, v, sizeof(v));
                return true;
            }
            return false;
        }

        case Spark::FieldType::Vector3:
        {
            float v[3] = {};
            std::memcpy(v, dst, sizeof(v));
            const DirectX::XMFLOAT3 before{v[0], v[1], v[2]};
            InspectorPanel::DrawVec3Control(label, v, 0.0f, 0.1f);
            const bool changed = before.x != v[0] || before.y != v[1] || before.z != v[2];
            if (changed)
                std::memcpy(dst, v, sizeof(v));
            return changed;
        }

        case Spark::FieldType::Vector4:
        {
            float v[4] = {};
            std::memcpy(v, dst, sizeof(v));
            if (ImGui::ColorEdit4(label, v))
            {
                std::memcpy(dst, v, sizeof(v));
                return true;
            }
            return false;
        }

        default:
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Unsupported field type");
            return false;
        }
        return false;
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

    // Render one visible field as a property-grid row: the field name in the label column, the
    // widget in the value column. The widget stays ImGui's last item when @p afterField runs.
    static bool RenderFieldRow(const Spark::FieldInfo& field, void* data,
                               const std::function<void(const Spark::FieldInfo&)>& afterField)
    {
        const char* tooltip = field.tooltip.empty() ? nullptr : field.tooltip.c_str();
        if (field.readOnly)
        {
            Spark::Ui::PropertyLabel((field.name + " (read-only)").c_str(), tooltip);
        }
        else
        {
            Spark::Ui::PropertyLabel(field.name.c_str(), tooltip);
        }

        auto* dst = static_cast<char*>(data) + field.offset;
        ImGui::PushID(field.fieldName.c_str());
        if (field.readOnly)
            ImGui::BeginDisabled();
        const bool changed = RenderSingleField(field, dst, "##value");
        if (field.readOnly)
            ImGui::EndDisabled();
        ImGui::PopID();

        if (afterField)
        {
            afterField(field);
        }
        if (tooltip != nullptr && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        return changed;
    }

    bool InspectorPanel::RenderReflectedFields(void* data, const std::vector<Spark::FieldInfo>& fields,
                                               const std::function<void(const Spark::FieldInfo&)>& afterField)
    {
        if (!data)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Inspector: RenderReflectedFields called with null data");
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Component data unavailable");
            return false;
        }

        // Collect categories in order of first appearance; uncategorised fields form the "" group.
        std::vector<const std::string*> categories;
        for (const auto& field : fields)
        {
            const bool known = std::any_of(categories.begin(), categories.end(),
                                           [&field](const std::string* c) { return *c == field.category; });
            if (!known)
                categories.push_back(&field.category);
        }

        // Each field list is a static or registry-owned vector, so its address scopes the grid IDs.
        ImGui::PushID(static_cast<const void*>(&fields));
        bool changed = false;
        for (const std::string* category : categories)
        {
            if (!category->empty())
            {
                Spark::Ui::SubHeader(category->c_str());
            }
            ImGui::PushID(category->c_str());
            if (Spark::Ui::BeginPropertyGrid("##fields"))
            {
                for (const auto& field : fields)
                {
                    if (field.category != *category || !IsFieldVisible(field, data, fields))
                        continue;
                    changed |= RenderFieldRow(field, data, afterField);
                }
                Spark::Ui::EndPropertyGrid();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }

} // namespace SparkEditor
