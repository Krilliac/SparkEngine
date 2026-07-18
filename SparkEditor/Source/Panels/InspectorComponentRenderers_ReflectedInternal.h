/**
 * @file InspectorComponentRenderers_ReflectedInternal.h
 * @brief Shared macros and FieldInfo builders for the reflection-driven inspector renderer split
 * @author Spark Engine Team
 * @date 2026
 *
 * Internal to the InspectorComponentRenderers_Reflected* .cpp split
 * (InspectorComponentRenderers_Reflected.cpp,
 * InspectorComponentRenderers_ReflectedVolumes.cpp,
 * InspectorComponentRenderers_ReflectedGameplay.cpp,
 * InspectorComponentRenderers_Reflected2D.cpp,
 * InspectorComponentRenderers_ReflectedConditional.cpp) — do not include
 * from other panels.
 *
 * Contains: RENDER_REFLECTED_COMPONENT macro (header + context menu +
 * undo/redo snapshot around RenderReflectedFields), the MakeField /
 * MakeEnumField FieldInfo builders, and the FIELD_* shorthand macros.
 */

#pragma once

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../CommandHistory.h"
#include <imgui.h>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace SparkEditor
{

    // ============================================================================
    // Helper: render a component header + reflected fields from editor data
    // ============================================================================

#define RENDER_REFLECTED_COMPONENT(compType, icon, displayName, dataType, ...)                                         \
    {                                                                                                                  \
        bool headerOpen = ImGui::CollapsingHeader(icon " " displayName);                                               \
        if (ImGui::BeginPopupContextItem("##" displayName "Ctx"))                                                      \
        {                                                                                                              \
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))                                                    \
                RemoveComponent(compType);                                                                             \
            ImGui::EndPopup();                                                                                         \
        }                                                                                                              \
        if (headerOpen)                                                                                                \
        {                                                                                                              \
            ImGui::Indent(4);                                                                                          \
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, compType);                                   \
            auto* d = comp ? comp->GetData<dataType>() : nullptr;                                                      \
            if (d)                                                                                                     \
            {                                                                                                          \
                static const std::vector<Spark::FieldInfo> fields = {__VA_ARGS__};                                     \
                dataType oldSnapshot = *d;                                                                             \
                RenderReflectedFields(d, fields);                                                                      \
                if (std::memcmp(&oldSnapshot, d, sizeof(dataType)) != 0)                                               \
                {                                                                                                      \
                    dataType newSnapshot = *d;                                                                         \
                    SceneFile* cs = m_scene;                                                                           \
                    ObjectID ci = m_inspectedObjectID;                                                                 \
                    ComponentType ct = compType;                                                                       \
                    auto& history = Spark::Editor::CommandHistory::GetInstance();                                      \
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(                                    \
                        [cs, ci, ct, newSnapshot]()                                                                    \
                        {                                                                                              \
                            Component* c = FindComponent(cs, ci, ct);                                                  \
                            if (auto* p = c ? c->GetData<dataType>() : nullptr)                                        \
                                *p = newSnapshot;                                                                      \
                        },                                                                                             \
                        [cs, ci, ct, oldSnapshot]()                                                                    \
                        {                                                                                              \
                            Component* c = FindComponent(cs, ci, ct);                                                  \
                            if (auto* p = c ? c->GetData<dataType>() : nullptr)                                        \
                                *p = oldSnapshot;                                                                      \
                        },                                                                                             \
                        displayName " Change"));                                                                       \
                }                                                                                                      \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                ImGui::TextDisabled("(Component data unavailable)");                                                   \
            }                                                                                                          \
            ImGui::Unindent(4);                                                                                        \
        }                                                                                                              \
    }

    // Helper to build FieldInfo inline for editor data types
    inline Spark::FieldInfo MakeField(const char* name, const char* fieldName, Spark::FieldType type, size_t offset,
                                      size_t size, float rangeMin = 0, float rangeMax = 0, bool hasRange = false)
    {
        Spark::FieldInfo f;
        f.name = name;
        f.fieldName = fieldName;
        f.type = type;
        f.offset = offset;
        f.size = size;
        f.rangeMin = rangeMin;
        f.rangeMax = rangeMax;
        f.hasRange = hasRange;
        return f;
    }

    // Helper to build an enum FieldInfo with named values for combo dropdown
    inline Spark::FieldInfo MakeEnumField(const char* name, const char* fieldName, size_t offset, size_t size,
                                          std::vector<std::string> names)
    {
        Spark::FieldInfo f;
        f.name = name;
        f.fieldName = fieldName;
        f.type = Spark::FieldType::Int;
        f.offset = offset;
        f.size = size;
        f.enumNames = std::move(names);
        return f;
    }

    // Shorthand macros for concise field descriptors
#define FIELD_FLOAT(DataType, member, displayName)                                                                     \
    MakeField(displayName, #member, Spark::FieldType::Float, offsetof(DataType, member), sizeof(float))
#define FIELD_FLOAT_RANGE(DataType, member, displayName, lo, hi)                                                       \
    MakeField(displayName, #member, Spark::FieldType::Float, offsetof(DataType, member), sizeof(float), lo, hi, true)
#define FIELD_INT(DataType, member, displayName)                                                                       \
    MakeField(displayName, #member, Spark::FieldType::Int, offsetof(DataType, member), sizeof(int))
#define FIELD_BOOL(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Bool, offsetof(DataType, member), sizeof(bool))
#define FIELD_VEC2(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector2, offsetof(DataType, member), sizeof(XMFLOAT2))
#define FIELD_VEC3(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector3, offsetof(DataType, member), sizeof(XMFLOAT3))
#define FIELD_VEC4(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector4, offsetof(DataType, member), sizeof(XMFLOAT4))
#define FIELD_STRING(DataType, member, displayName)                                                                    \
    MakeField(displayName, #member, Spark::FieldType::String, offsetof(DataType, member), sizeof(DataType::member))
#define FIELD_ENUM(DataType, member, displayName, ...)                                                                 \
    MakeEnumField(displayName, #member, offsetof(DataType, member), sizeof(DataType::member),                          \
                  std::vector<std::string>{__VA_ARGS__})

} // namespace SparkEditor
