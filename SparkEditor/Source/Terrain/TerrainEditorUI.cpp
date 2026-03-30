/**
 * @file TerrainEditorUI.cpp
 * @brief ImGui UI rendering for the terrain editor panel
 */

#include "TerrainEditor.h"
#include "Utils/LogMacros.h"
#include "../Core/EditorIcons.h"

#include <imgui.h>
#include <cstring>

using namespace DirectX;
namespace SparkEditor
{

    void TerrainEditor::RenderNoTerrainView()
    {
        ImGui::Spacing();
        ImGui::TextWrapped("No terrain loaded. Create a new terrain or load an existing one.");
        ImGui::Spacing();

        if (ImGui::Button(ICON_FA_PLUS " Create New Terrain", ImVec2(-1, 32)))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating new terrain via UI");
            CreateNewTerrain();
        }
    }

    void TerrainEditor::RenderToolPalette()
    {
        ImGui::Text(ICON_FA_MOUNTAIN " Terrain Tools");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 12.0f) / 4.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        auto ToolButton = [&](const char* label, TerrainTool tool)
        {
            bool active = (m_currentTool == tool);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.95f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
            {
                m_currentTool = tool;
                m_showHeightmapTools = (static_cast<int>(tool) < 10);
                m_showTexturePainting = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20);
                m_showDetailPlacement = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20 &&
                                         (tool == TerrainTool::PAINT_DETAIL || tool == TerrainTool::PAINT_TREES));
                m_showGenerationTools = false;
            }
            if (active)
                ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };

        // Sculpt row
        ImGui::TextDisabled("Sculpt:");
        ToolButton(ICON_FA_LEVEL_UP_ALT " Raise", TerrainTool::SCULPT_RAISE);
        ToolButton(ICON_FA_ANGLE_DOWN " Lower", TerrainTool::SCULPT_LOWER);
        ToolButton(ICON_FA_MAGIC " Smooth", TerrainTool::SCULPT_SMOOTH);
        ToolButton(ICON_FA_COMPRESS " Flatten", TerrainTool::SCULPT_FLATTEN);
        ImGui::NewLine();

        // Paint row
        ImGui::TextDisabled("Paint:");
        ToolButton(ICON_FA_PAINT_BRUSH " Texture", TerrainTool::PAINT_TEXTURE);
        ToolButton(ICON_FA_TREE " Trees", TerrainTool::PAINT_TREES);
        ToolButton(ICON_FA_LAYER_GROUP " Detail", TerrainTool::PAINT_DETAIL);
        ImGui::NewLine();

        // Utility row
        ImGui::TextDisabled("Utility:");
        if (ImGui::Button(ICON_FA_COGS " Generate", btnSize))
        {
            m_showGenerationTools = !m_showGenerationTools;
            m_showHeightmapTools = false;
            m_showTexturePainting = false;
            m_showDetailPlacement = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO " Undo", btnSize))
            UndoOperation();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REDO " Redo", btnSize))
            RedoOperation();
        ImGui::NewLine();
    }

    void TerrainEditor::RenderBrushSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " Brush Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            ImGui::DragFloat("Radius", &m_brushSettings.radius, 0.5f, 1.0f, 500.0f, "%.1f");
            ImGui::DragFloat("Strength", &m_brushSettings.strength, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Falloff", &m_brushSettings.falloff, 0.01f, 0.0f, 1.0f, "%.2f");

            const char* falloffTypes[] = {"Linear", "Smooth", "Sphere", "Sharp", "Custom"};
            int falloffIdx = static_cast<int>(m_brushSettings.falloffType);
            if (ImGui::Combo("Falloff Type", &falloffIdx, falloffTypes, 5))
            {
                m_brushSettings.falloffType = static_cast<TerrainBrush::FalloffType>(falloffIdx);
            }

            ImGui::Checkbox("Show Preview", &m_brushSettings.showPreview);
            ImGui::Checkbox("Pen Pressure", &m_brushSettings.enablePressure);

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderHeightmapTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_MOUNTAIN " Heightmap", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            if (m_currentTerrain)
            {
                auto& hm = m_currentTerrain->heightmap;
                ImGui::Text("Resolution: %d x %d", hm.width, hm.height);
                ImGui::DragFloat("Height Scale", &hm.scale, 0.1f, 0.1f, 100.0f, "%.1f");
                ImGui::DragFloat("Min Height", &hm.minHeight, 0.5f, -1000.0f, hm.maxHeight, "%.1f");
                ImGui::DragFloat("Max Height", &hm.maxHeight, 0.5f, hm.minHeight, 1000.0f, "%.1f");

                ImGui::Separator();
                ImGui::Text("Visualization:");
                ImGui::Checkbox("Wireframe", &m_showWireframe);
                ImGui::SameLine();
                ImGui::Checkbox("Normals", &m_showNormals);
            }

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTexturePaintingTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " Texture Painting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);
            RenderTextureLayersPanel();
            ImGui::Checkbox("Show Splatmaps", &m_showSplatmaps);
            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderDetailPlacementTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TREE " Detail Placement", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);
            RenderDetailMeshesPanel();
            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTerrainProperties()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COG " Terrain Properties"))
        {
            if (!m_currentTerrain)
                return;

            ImGui::Indent(8.0f);

            char nameBuffer[256] = {};
            std::strncpy(nameBuffer, m_currentTerrain->name.c_str(), sizeof(nameBuffer) - 1);
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                m_currentTerrain->name = nameBuffer;
                SetModified(true);
            }

            ImGui::DragFloat("Size", &m_currentTerrain->size, 10.0f, 100.0f, 10000.0f, "%.0f m");
            ImGui::DragFloat3("Position", &m_currentTerrain->position.x, 1.0f);

            ImGui::Separator();
            ImGui::Text("LOD:");
            ImGui::DragInt("LOD Levels", &m_currentTerrain->lodLevels, 0.1f, 1, 8);
            ImGui::DragFloat("LOD Bias", &m_currentTerrain->lodBias, 0.1f, 0.1f, 4.0f, "%.1f");

            ImGui::Separator();
            ImGui::Text("Physics:");
            ImGui::Checkbox("Generate Collider", &m_currentTerrain->generateCollider);

            ImGui::Separator();
            if (ImGui::Button(ICON_FA_SAVE " Save Terrain", ImVec2(-1, 28)))
            {
                SaveTerrain("terrain.sparkterrain");
            }

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTextureLayersPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Texture Layers (%d)", static_cast<int>(m_currentTerrain->textureLayers.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Layer"))
        {
            m_currentTerrain->AddTextureLayer("New Layer");
            SetModified(true);
        }

        ImGui::BeginChild("##TextureLayers", ImVec2(0, 150), true);
        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(m_currentTerrain->textureLayers.size()); ++i)
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(i)];
            ImGui::PushID(i);

            bool selected = (m_selectedTextureLayer == i);
            if (ImGui::Selectable(layer->name.c_str(), selected))
            {
                m_selectedTextureLayer = i;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Tiling: %.1f x %.1f\nOpacity: %.0f%%", layer->tiling.x, layer->tiling.y,
                                  layer->opacity * 100.0f);
            }

            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Remove"))
                    removeIdx = i;
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        if (removeIdx >= 0)
        {
            m_currentTerrain->RemoveTextureLayer(removeIdx);
            SetModified(true);
        }

        if (m_selectedTextureLayer >= 0 &&
            m_selectedTextureLayer < static_cast<int>(m_currentTerrain->textureLayers.size()))
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(m_selectedTextureLayer)];
            ImGui::Separator();
            ImGui::DragFloat2("Tiling", &layer->tiling.x, 0.1f, 0.01f, 100.0f);
            ImGui::DragFloat("Opacity", &layer->opacity, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Metallic", &layer->metallic, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Roughness", &layer->roughness, 0.01f, 0.0f, 1.0f, "%.2f");

            ImGui::Separator();
            ImGui::Checkbox("Auto Placement", &layer->useAutoPlacement);
            if (layer->useAutoPlacement)
            {
                ImGui::DragFloatRange2("Height", &layer->minHeight, &layer->maxHeight, 0.5f, 0.0f, 1000.0f);
                ImGui::DragFloatRange2("Slope", &layer->minSlope, &layer->maxSlope, 0.5f, 0.0f, 90.0f);
                if (ImGui::Button("Apply Auto-Placement"))
                {
                    AutoGenerateTexturePlacement(m_selectedTextureLayer);
                }
            }
        }
    }

    void TerrainEditor::RenderDetailMeshesPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Detail Meshes (%d)", static_cast<int>(m_currentTerrain->detailMeshes.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Detail"))
        {
            auto detail = std::make_unique<TerrainDetailMesh>();
            detail->name = "New Detail";
            m_currentTerrain->detailMeshes.push_back(std::move(detail));
            SetModified(true);
        }

        ImGui::BeginChild("##DetailMeshes", ImVec2(0, 120), true);
        for (int i = 0; i < static_cast<int>(m_currentTerrain->detailMeshes.size()); ++i)
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(i)];
            ImGui::PushID(i);
            bool selected = (m_selectedDetailMesh == i);
            if (ImGui::Selectable(detail->name.c_str(), selected))
            {
                m_selectedDetailMesh = i;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (m_selectedDetailMesh >= 0 && m_selectedDetailMesh < static_cast<int>(m_currentTerrain->detailMeshes.size()))
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(m_selectedDetailMesh)];
            ImGui::DragFloat("Density", &detail->density, 0.1f, 0.1f, 10.0f);
            ImGui::DragFloat("View Distance", &detail->viewDistance, 1.0f, 10.0f, 500.0f);
            ImGui::DragFloat2("Scale Range", &detail->scaleRange.x, 0.05f, 0.1f, 5.0f);
            ImGui::Checkbox("Cast Shadows", &detail->castShadows);
        }
    }

    void TerrainEditor::RenderGenerationTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COGS " Procedural Generation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            ImGui::TextDisabled("Noise");
            ImGui::DragInt("Octaves", &m_generationParams.noiseOctaves, 0.1f, 1, 12);
            ImGui::DragFloat("Frequency", &m_generationParams.noiseFrequency, 0.001f, 0.001f, 1.0f, "%.3f");
            ImGui::DragFloat("Amplitude", &m_generationParams.noiseAmplitude, 1.0f, 1.0f, 500.0f, "%.0f");
            ImGui::DragFloat("Lacunarity", &m_generationParams.noiseLacunarity, 0.1f, 1.0f, 4.0f, "%.1f");
            ImGui::DragFloat("Persistence", &m_generationParams.noisePersistence, 0.01f, 0.0f, 1.0f, "%.2f");

            if (ImGui::Button(ICON_FA_BOLT " Generate Heightmap", ImVec2(-1, 28)))
            {
                GenerateNoiseHeightmap(m_generationParams.noiseOctaves, m_generationParams.noiseFrequency,
                                       m_generationParams.noiseAmplitude, m_generationParams.noiseLacunarity,
                                       m_generationParams.noisePersistence);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Post-Process");

            ImGui::DragInt("Smooth Iterations", &m_generationParams.smoothIterations, 0.1f, 1, 20);
            ImGui::DragFloat("Smooth Strength", &m_generationParams.smoothStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_MAGIC " Smooth", ImVec2(-1, 28)))
            {
                SmoothTerrain(m_generationParams.smoothIterations, m_generationParams.smoothStrength);
            }

            ImGui::Separator();
            ImGui::DragInt("Erosion Iterations", &m_generationParams.erosionIterations, 1.0f, 10, 10000);
            ImGui::DragFloat("Erosion Strength", &m_generationParams.erosionStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_FIRE " Apply Erosion", ImVec2(-1, 28)))
            {
                ApplyErosion(m_generationParams.erosionIterations, m_generationParams.erosionStrength,
                             m_generationParams.evaporationRate, m_generationParams.depositionRate);
            }

            ImGui::Unindent(8.0f);
        }
    }

} // namespace SparkEditor
