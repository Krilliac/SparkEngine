/**
 * @file TerrainEditorUI.cpp
 * @brief ImGui UI rendering for the terrain editor panel
 */

#include "TerrainEditor.h"
#include "Utils/LogMacros.h"
#include "../Core/EditorIcons.h"

#include <imgui.h>
#include <cstring>
#include <string>

#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

using namespace DirectX;
namespace SparkEditor
{

    void TerrainEditor::RenderNoTerrainView()
    {
        Spark::Ui::EmptyState(ICON_FA_MOUNTAIN, "No terrain loaded", "Create a new terrain or load an existing one.");
        ImGui::Spacing();

        // --- Quick-create presets ---
        Spark::Ui::SubHeader(ICON_FA_PLUS " Create Terrain");

        static int presetSize = 1; // 0=Small, 1=Medium, 2=Large
        static int presetRes = 1;  // 0=Low, 1=Medium, 2=High
        static int presetType = 0; // 0=Flat, 1=Noise Hills, 2=Mountains

        Spark::Ui::SecondaryText("Size:");
        ImGui::SameLine();
        ImGui::RadioButton("512m", &presetSize, 0);
        ImGui::SameLine();
        ImGui::RadioButton("1024m", &presetSize, 1);
        ImGui::SameLine();
        ImGui::RadioButton("2048m", &presetSize, 2);

        Spark::Ui::SecondaryText("Resolution:");
        ImGui::SameLine();
        ImGui::RadioButton("257", &presetRes, 0);
        ImGui::SameLine();
        ImGui::RadioButton("513", &presetRes, 1);
        ImGui::SameLine();
        ImGui::RadioButton("1025", &presetRes, 2);

        Spark::Ui::SecondaryText("Preset:");
        ImGui::SameLine();
        ImGui::RadioButton("Flat", &presetType, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Hills", &presetType, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Mountains", &presetType, 2);

        ImGui::Spacing();

        float sizes[] = {512.0f, 1024.0f, 2048.0f};
        int resolutions[] = {257, 513, 1025};

        if (Spark::Ui::PrimaryButton(ICON_FA_PLUS " Create Terrain", ImVec2(-1, Spark::Ui::Px(36.0f))))
        {
            float selectedSize = sizes[presetSize];
            int selectedRes = resolutions[presetRes];

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating terrain: %.0fm, %dx%d, preset=%d", selectedSize,
                           selectedRes, selectedRes, presetType);

            CreateNewTerrain(selectedSize, selectedRes);

            // Apply procedural generation based on preset
            if (presetType == 1) // Hills
            {
                GenerateNoiseHeightmap(4, 0.005f, 30.0f, 2.0f, 0.5f);
                SmoothTerrain(2, 0.4f);
            }
            else if (presetType == 2) // Mountains
            {
                GenerateNoiseHeightmap(8, 0.003f, 80.0f, 2.2f, 0.45f);
                ApplyErosion(200, 0.15f, 0.02f, 0.3f);
                SmoothTerrain(1, 0.3f);
            }

            UpdateTerrainMesh();
        }

        ImGui::Spacing();

        // --- Load existing ---
        Spark::Ui::SubHeader(ICON_FA_FOLDER_OPEN " Load Existing");

        if (m_loadPathBuffer[0] == '\0')
        {
            RefreshPathBuffers();
        }
        ImGui::InputText("##terrainpath", m_loadPathBuffer, sizeof(m_loadPathBuffer));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load Terrain File", ImVec2(-1, Spark::Ui::Px(32.0f))))
        {
            if (!LoadTerrain(m_loadPathBuffer))
            {
                m_terrainIoMessage = std::string("Failed to load terrain from: ") + m_loadPathBuffer;
            }
            else
            {
                m_terrainIoMessage.clear();
            }
        }
        if (!m_terrainIoMessage.empty())
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", m_terrainIoMessage.c_str());
        }
    }

    void TerrainEditor::RefreshPathBuffers()
    {
        const std::string terrainName = m_currentTerrain ? m_currentTerrain->name : std::string("terrain");
        const std::string suggested = m_terrainFilePath.empty() ? DefaultTerrainPath(terrainName) : m_terrainFilePath;

        std::strncpy(m_savePathBuffer, suggested.c_str(), sizeof(m_savePathBuffer) - 1);
        m_savePathBuffer[sizeof(m_savePathBuffer) - 1] = '\0';

        if (m_loadPathBuffer[0] == '\0')
        {
            const std::string defaultLoad = DefaultTerrainPath("terrain");
            std::strncpy(m_loadPathBuffer, defaultLoad.c_str(), sizeof(m_loadPathBuffer) - 1);
            m_loadPathBuffer[sizeof(m_loadPathBuffer) - 1] = '\0';
        }
    }

    void TerrainEditor::RenderToolPalette()
    {
        Spark::Ui::SubHeader(ICON_FA_MOUNTAIN " Terrain Tools");

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float btnWidth = (ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f;
        ImVec2 btnSize(btnWidth, Spark::Ui::Px(28.0f));

        auto ToolButton = [&](const char* label, TerrainTool tool)
        {
            bool clicked = false;
            if (m_currentTool == tool)
            {
                Spark::Ui::ScopedPrimaryButtonColors activeColours;
                clicked = ImGui::Button(label, btnSize);
            }
            else
            {
                clicked = ImGui::Button(label, btnSize);
            }
            if (clicked)
            {
                m_currentTool = tool;
                m_showHeightmapTools = (static_cast<int>(tool) < 10);
                m_showTexturePainting = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20);
                m_showDetailPlacement = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20 &&
                                         (tool == TerrainTool::PAINT_DETAIL || tool == TerrainTool::PAINT_TREES));
                m_showGenerationTools = false;
            }
            ImGui::SameLine();
        };

        // Sculpt row
        Spark::Ui::SecondaryText("Sculpt:");
        ToolButton(ICON_FA_LEVEL_UP_ALT " Raise", TerrainTool::SCULPT_RAISE);
        ToolButton(ICON_FA_ANGLE_DOWN " Lower", TerrainTool::SCULPT_LOWER);
        ToolButton(ICON_FA_MAGIC " Smooth", TerrainTool::SCULPT_SMOOTH);
        ToolButton(ICON_FA_COMPRESS " Flatten", TerrainTool::SCULPT_FLATTEN);
        ImGui::NewLine();

        // Paint row
        Spark::Ui::SecondaryText("Paint:");
        ToolButton(ICON_FA_PAINT_BRUSH " Texture", TerrainTool::PAINT_TEXTURE);
        ToolButton(ICON_FA_TREE " Trees", TerrainTool::PAINT_TREES);
        ToolButton(ICON_FA_LAYER_GROUP " Detail", TerrainTool::PAINT_DETAIL);
        ImGui::NewLine();

        // Utility row
        Spark::Ui::SecondaryText("Utility:");
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
        if (Spark::Ui::SectionHeader("Brush Settings", ICON_FA_PAINT_BRUSH))
        {
            ImGui::Indent(Spark::Ui::Px(8.0f));

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

            ImGui::Unindent(Spark::Ui::Px(8.0f));
        }
    }

    void TerrainEditor::RenderHeightmapTools()
    {
        if (Spark::Ui::SectionHeader("Heightmap", ICON_FA_MOUNTAIN))
        {
            ImGui::Indent(Spark::Ui::Px(8.0f));

            if (m_currentTerrain)
            {
                auto& hm = m_currentTerrain->heightmap;
                Spark::Ui::KeyValue("Resolution:", "%d x %d", hm.width, hm.height);
                ImGui::DragFloat("Height Scale", &hm.scale, 0.1f, 0.1f, 100.0f, "%.1f");
                ImGui::DragFloat("Min Height", &hm.minHeight, 0.5f, -1000.0f, hm.maxHeight, "%.1f");
                ImGui::DragFloat("Max Height", &hm.maxHeight, 0.5f, hm.minHeight, 1000.0f, "%.1f");

                Spark::Ui::SubHeader("Visualization");
                ImGui::Checkbox("Wireframe", &m_showWireframe);
                ImGui::SameLine();
                ImGui::Checkbox("Normals", &m_showNormals);
            }

            ImGui::Unindent(Spark::Ui::Px(8.0f));
        }
    }

    void TerrainEditor::RenderTexturePaintingTools()
    {
        if (Spark::Ui::SectionHeader("Texture Painting", ICON_FA_PAINT_BRUSH))
        {
            ImGui::Indent(Spark::Ui::Px(8.0f));
            RenderTextureLayersPanel();
            ImGui::Checkbox("Show Splatmaps", &m_showSplatmaps);
            ImGui::Unindent(Spark::Ui::Px(8.0f));
        }
    }

    void TerrainEditor::RenderDetailPlacementTools()
    {
        if (Spark::Ui::SectionHeader("Detail Placement", ICON_FA_TREE))
        {
            ImGui::Indent(Spark::Ui::Px(8.0f));
            RenderDetailMeshesPanel();
            ImGui::Unindent(Spark::Ui::Px(8.0f));
        }
    }


} // namespace SparkEditor
