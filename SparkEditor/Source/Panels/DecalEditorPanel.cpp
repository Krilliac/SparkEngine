/**
 * @file DecalEditorPanel.cpp
 * @brief Implementation of the decal material and surface mapping editor
 */

#include "DecalEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstring>
#include <cstdio>

namespace SparkEditor
{

    DecalEditorPanel::DecalEditorPanel() : EditorPanel("Decal Editor", "decal_editor_panel") {}

    bool DecalEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecalEditorPanel initialized");

        // Pre-populate default surface mappings
        const char* surfaceNames[] = {"Default", "Concrete", "Metal", "Wood",   "Glass",
                                      "Dirt",    "Flesh",    "Water", "Foliage"};
        for (int i = 0; i < 9; ++i)
        {
            SurfaceMappingEntry mapping;
            mapping.surfaceType = i;
            m_mappings.push_back(mapping);
        }
        return true;
    }

    void DecalEditorPanel::Update(float /*deltaTime*/) {}

    void DecalEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Stats bar
            ImGui::Text(ICON_FA_STAMP " Materials: %d | Active Decals: %d / %d", static_cast<int>(m_materials.size()),
                        m_activeDecalCount, m_maxDecals);

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Clear All Decals"))
                m_activeDecalCount = 0;

            ImGui::Separator();

            if (ImGui::BeginTabBar("DecalTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_PALETTE " Materials"))
                {
                    float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;

                    ImGui::BeginChild("MatList", ImVec2(listWidth, 0), true);
                    RenderMaterialList();
                    ImGui::EndChild();

                    ImGui::SameLine();

                    ImGui::BeginChild("MatDetails", ImVec2(0, 0), true);
                    RenderMaterialDetails();
                    ImGui::EndChild();

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MAP " Surface Mappings"))
                {
                    RenderSurfaceMappings();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void DecalEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecalEditorPanel shutting down");
    }

    void DecalEditorPanel::RenderMaterialList()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Material"))
        {
            DecalMaterialEntry mat;
            snprintf(mat.name, sizeof(mat.name), "Decal_%d", static_cast<int>(m_materials.size()));
            m_materials.push_back(mat);
            m_selectedMaterial = static_cast<int>(m_materials.size()) - 1;
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "DecalEditorPanel: created material '%s'", mat.name);
        }

        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            if (ImGui::Selectable(m_materials[i].name, m_selectedMaterial == i))
                m_selectedMaterial = i;
        }
    }

    void DecalEditorPanel::RenderMaterialDetails()
    {
        if (m_selectedMaterial < 0 || m_selectedMaterial >= static_cast<int>(m_materials.size()))
        {
            ImGui::TextDisabled("Select a decal material to edit");
            return;
        }

        auto& mat = m_materials[m_selectedMaterial];

        ImGui::InputText("Name", mat.name, sizeof(mat.name));
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::InputText("Albedo", mat.albedoPath, sizeof(mat.albedoPath));
            ImGui::InputText("Normal", mat.normalPath, sizeof(mat.normalPath));
            ImGui::InputText("Roughness", mat.roughnessPath, sizeof(mat.roughnessPath));
        }

        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Opacity", &mat.opacity, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Fade Time", &mat.fadeTime, 0.5f, 0.0f, 120.0f, "%.1f s");
            ImGui::Checkbox("Affects Normals", &mat.affectsNormals);
            ImGui::Checkbox("Affects Roughness", &mat.affectsRoughness);
        }

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Delete Material"))
        {
            // Remove any surface mappings pointing to this material
            for (auto& mapping : m_mappings)
            {
                if (mapping.decalMaterial == m_selectedMaterial)
                    mapping.decalMaterial = -1;
                else if (mapping.decalMaterial > m_selectedMaterial)
                    --mapping.decalMaterial;
            }

            m_materials.erase(m_materials.begin() + m_selectedMaterial);
            m_selectedMaterial = -1;
        }
    }

    void DecalEditorPanel::RenderSurfaceMappings()
    {
        ImGui::TextDisabled("Assign a decal material to each surface type.");
        ImGui::DragInt("Max Decals", &m_maxDecals, 1.0f, 16, 4096);
        ImGui::Separator();

        const char* surfaceNames[] = {"Default", "Concrete", "Metal", "Wood",   "Glass",
                                      "Dirt",    "Flesh",    "Water", "Foliage"};

        if (ImGui::BeginTable("SurfaceMapTable", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Surface Type", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Decal Material");
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_mappings.size()); ++i)
            {
                auto& mapping = m_mappings[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                int surfIdx = mapping.surfaceType;
                if (surfIdx >= 0 && surfIdx < 9)
                    ImGui::TextUnformatted(surfaceNames[surfIdx]);
                else
                    ImGui::Text("Surface %d", surfIdx);

                ImGui::TableNextColumn();
                // Build material name list for combo
                ImGui::SetNextItemWidth(-1);
                const char* currentName =
                    mapping.decalMaterial >= 0 && mapping.decalMaterial < static_cast<int>(m_materials.size())
                        ? m_materials[mapping.decalMaterial].name
                        : "(none)";

                if (ImGui::BeginCombo("##mat", currentName))
                {
                    if (ImGui::Selectable("(none)", mapping.decalMaterial < 0))
                        mapping.decalMaterial = -1;

                    for (int m = 0; m < static_cast<int>(m_materials.size()); ++m)
                    {
                        if (ImGui::Selectable(m_materials[m].name, mapping.decalMaterial == m))
                            mapping.decalMaterial = m;
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

} // namespace SparkEditor
