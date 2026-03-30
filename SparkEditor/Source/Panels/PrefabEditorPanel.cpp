/**
 * @file PrefabEditorPanel.cpp
 * @brief Implementation of the prefab editor panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "PrefabEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <algorithm>

namespace SparkEditor
{

    PrefabEditorPanel::PrefabEditorPanel(PrefabManager* prefabManager)
        : EditorPanel("Prefab Editor", "PrefabEditor"), m_prefabManager(prefabManager)
    {
    }

    bool PrefabEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PrefabEditorPanel initialized");
        m_isInitialized = true;
        return true;
    }

    void PrefabEditorPanel::Update(float /*deltaTime*/) {}

    void PrefabEditorPanel::Render()
    {
        if (!m_isVisible)
        {
            return;
        }

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();
        ImGui::Separator();

        // Split view: prefab list on left, inspector on right
        float availWidth = ImGui::GetContentRegionAvail().x;
        float listWidth = availWidth * 0.35f;

        ImGui::BeginChild("PrefabList", ImVec2(listWidth, 0), true);
        RenderPrefabList();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("PrefabInspector", ImVec2(0, 0), true);
        RenderPrefabInspector();
        ImGui::EndChild();

        // Create dialog popup
        if (m_showCreateDialog)
        {
            ImGui::OpenPopup("Create Prefab");
            m_showCreateDialog = false;
        }

        if (ImGui::BeginPopupModal("Create Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter a name for the new prefab:");
            ImGui::InputText("##PrefabName", m_newPrefabName, sizeof(m_newPrefabName));

            if (ImGui::Button("Create", ImVec2(120, 0)))
            {
                if (m_prefabManager && m_newPrefabName[0] != '\0')
                {
                    m_prefabManager->CreateEmptyPrefab(m_newPrefabName);
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "PrefabEditorPanel: created prefab '%s'",
                                   m_newPrefabName);
                    m_selectedPrefab = m_newPrefabName;
                    m_newPrefabName[0] = '\0';
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                m_newPrefabName[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        EndPanel();
    }

    void PrefabEditorPanel::Shutdown() {}

    void PrefabEditorPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_PLUS " New"))
        {
            m_showCreateDialog = true;
        }
        ImGui::SameLine();

        bool hasSelection =
            !m_selectedPrefab.empty() && m_prefabManager && m_prefabManager->GetPrefab(m_selectedPrefab);

        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            if (m_prefabManager)
            {
                m_prefabManager->SavePrefab(m_selectedPrefab);
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_COPY " Duplicate"))
        {
            if (m_prefabManager)
            {
                auto* source = m_prefabManager->GetPrefab(m_selectedPrefab);
                if (source)
                {
                    std::string newName = m_selectedPrefab + " (Copy)";
                    auto* newPrefab = m_prefabManager->CreateEmptyPrefab(newName);
                    if (newPrefab)
                    {
                        for (const auto& comp : source->GetComponents())
                        {
                            newPrefab->AddComponent(comp);
                        }
                        m_selectedPrefab = newName;
                    }
                }
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_CUBE " Instantiate"))
        {
            if (m_prefabManager)
            {
                m_prefabManager->InstantiatePrefab(m_selectedPrefab);
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_TRASH " Delete"))
        {
            if (m_prefabManager)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "PrefabEditorPanel: deleting prefab '%s'",
                               m_selectedPrefab.c_str());
                m_prefabManager->DeletePrefab(m_selectedPrefab);
                m_selectedPrefab.clear();
            }
        }
        ImGui::EndDisabled();

        // Search filter
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        char filterBuf[128] = {};
        if (m_searchFilter.size() < sizeof(filterBuf))
        {
            std::copy(m_searchFilter.begin(), m_searchFilter.end(), filterBuf);
        }
        if (ImGui::InputTextWithHint("##PrefabFilter", ICON_FA_SEARCH " Filter...", filterBuf, sizeof(filterBuf)))
        {
            m_searchFilter = filterBuf;
        }
    }

    void PrefabEditorPanel::RenderPrefabList()
    {
        if (!m_prefabManager)
        {
            ImGui::TextDisabled("No prefab manager available");
            return;
        }

        ImGui::Text(ICON_FA_CUBE " Prefabs (%zu)", m_prefabManager->GetPrefabCount());
        ImGui::Separator();

        auto names = m_prefabManager->GetPrefabNames();
        for (const auto& name : names)
        {
            // Apply search filter
            if (!m_searchFilter.empty())
            {
                std::string lowerName = name;
                std::string lowerFilter = m_searchFilter;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                if (!lowerName.contains(lowerFilter))
                {
                    continue;
                }
            }

            bool isSelected = (name == m_selectedPrefab);
            const auto* prefab = m_prefabManager->GetPrefab(name);

            std::string label = ICON_FA_CUBE " " + name;
            if (prefab && prefab->IsModified())
            {
                label += " *";
            }

            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_selectedPrefab = name;
            }

            // Tooltip with component list
            if (ImGui::IsItemHovered() && prefab)
            {
                ImGui::BeginTooltip();
                ImGui::Text("Components: %zu", prefab->GetComponents().size());
                for (const auto& comp : prefab->GetComponents())
                {
                    ImGui::BulletText("%s", comp.typeName.c_str());
                }
                ImGui::EndTooltip();
            }
        }
    }

    void PrefabEditorPanel::RenderPrefabInspector()
    {
        if (m_selectedPrefab.empty() || !m_prefabManager)
        {
            ImGui::TextDisabled("Select a prefab to inspect");
            return;
        }

        auto* prefab = m_prefabManager->GetPrefab(m_selectedPrefab);
        if (!prefab)
        {
            ImGui::TextDisabled("Prefab not found");
            return;
        }

        ImGui::Text(ICON_FA_SLIDERS " %s", prefab->GetName().c_str());
        if (prefab->IsModified())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(modified)");
        }
        ImGui::Separator();

        // Prefab info
        ImGui::TextDisabled("ID: %llu", static_cast<unsigned long long>(prefab->GetId()));
        if (!prefab->GetFilePath().empty())
        {
            ImGui::TextDisabled("Path: %s", prefab->GetFilePath().c_str());
        }
        ImGui::Spacing();

        // Component editors
        auto& components = prefab->GetComponents();
        for (size_t i = 0; i < components.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            RenderComponentEditor(components[i]);
            ImGui::PopID();
        }

        // Add component button
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_PLUS " Add Component", ImVec2(-1, 0)))
        {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (ImGui::BeginPopup("AddComponentPopup"))
        {
            const char* componentTypes[] = {"MeshRenderer", "Light",       "Camera",         "RigidBody",
                                            "Collider",     "AudioSource", "SpriteRenderer", "ParticleSystem"};
            for (const auto* type : componentTypes)
            {
                if (!prefab->HasComponent(type) && ImGui::MenuItem(type))
                {
                    SerializedComponent newComp;
                    newComp.typeName = type;
                    prefab->AddComponent(newComp);
                }
            }
            ImGui::EndPopup();
        }
    }

    void PrefabEditorPanel::RenderComponentEditor(SerializedComponent& component)
    {
        bool isOpen = ImGui::CollapsingHeader(component.typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

        // Right-click context menu
        if (ImGui::BeginPopupContextItem())
        {
            if (component.typeName != "Transform" && ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                // Mark for removal (handled after iteration)
            }
            if (ImGui::MenuItem(ICON_FA_COPY " Copy Component"))
            {
                // Copy component data to clipboard
            }
            ImGui::EndPopup();
        }

        if (isOpen)
        {
            ImGui::Indent(10.0f);
            for (auto& [name, value] : component.properties)
            {
                RenderPropertyEditor(name, value);
            }

            if (component.properties.empty())
            {
                ImGui::TextDisabled("(no properties)");
            }
            ImGui::Unindent(10.0f);
        }
    }

    void PrefabEditorPanel::RenderPropertyEditor(const std::string& name, PrefabPropertyValue& value)
    {
        ImGui::PushID(name.c_str());
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 120.0f);
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::NextColumn();

        std::visit(
            [&](auto&& val)
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    ImGui::Checkbox(("##" + name).c_str(), &val);
                }
                else if constexpr (std::is_same_v<T, int>)
                {
                    ImGui::DragInt(("##" + name).c_str(), &val);
                }
                else if constexpr (std::is_same_v<T, float>)
                {
                    ImGui::DragFloat(("##" + name).c_str(), &val, 0.1f);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    float fval = static_cast<float>(val);
                    if (ImGui::DragFloat(("##" + name).c_str(), &fval, 0.1f))
                    {
                        val = static_cast<double>(fval);
                    }
                }
                else if constexpr (std::is_same_v<T, std::string>)
                {
                    char buf[256] = {};
                    if (val.size() < sizeof(buf))
                    {
                        std::copy(val.begin(), val.end(), buf);
                    }
                    if (ImGui::InputText(("##" + name).c_str(), buf, sizeof(buf)))
                    {
                        val = buf;
                    }
                }
                else if constexpr (std::is_same_v<T, XMFLOAT3>)
                {
                    float v[3] = {val.x, val.y, val.z};
                    if (ImGui::DragFloat3(("##" + name).c_str(), v, 0.1f))
                    {
                        val = XMFLOAT3{v[0], v[1], v[2]};
                    }
                }
                else if constexpr (std::is_same_v<T, XMFLOAT4>)
                {
                    float v[4] = {val.x, val.y, val.z, val.w};
                    if (ImGui::DragFloat4(("##" + name).c_str(), v, 0.1f))
                    {
                        val = XMFLOAT4{v[0], v[1], v[2], v[3]};
                    }
                }
            },
            value);

        ImGui::Columns(1);
        ImGui::PopID();
    }

} // namespace SparkEditor
