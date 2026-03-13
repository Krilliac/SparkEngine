/**
 * @file DebugVisualizerPanel.cpp
 * @brief Debug visualization overlay panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include <iostream>

#include <imgui.h>

#include "DebugVisualizerPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    DebugVisualizerPanel::DebugVisualizerPanel() : EditorPanel("Debug Visualizer", "debug_visualizer_panel") {}

    DebugVisualizerPanel::~DebugVisualizerPanel() {}

    bool DebugVisualizerPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Debug Visualizer panel\n";
        return true;
    }

    void DebugVisualizerPanel::Update(float deltaTime)
    {
        // Update debug draw stats
        m_stats.gridLines = m_showGrid ? static_cast<int>((m_gridSize / m_gridSpacing) * 4) : 0;
    }

    void DebugVisualizerPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderQuickToggles();
            ImGui::Separator();

            if (ImGui::CollapsingHeader(ICON_FA_TH " Grid Settings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderGridSettings();
            }

            if (ImGui::CollapsingHeader(ICON_FA_EYE " Shading & Overlays"))
            {
                RenderOverlaySettings();
            }

            if (ImGui::CollapsingHeader(ICON_FA_CUBE " Physics Debug"))
            {
                RenderPhysicsDebug();
            }

            if (ImGui::CollapsingHeader(ICON_FA_MAP " Navigation Debug"))
            {
                RenderNavigationDebug();
            }

            if (ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " Rendering Debug"))
            {
                RenderRenderingDebug();
            }

            if (ImGui::CollapsingHeader(ICON_FA_VOLUME_UP " Audio Debug"))
            {
                RenderAudioDebug();
            }

            ImGui::Separator();
            RenderStats();
        }
        EndPanel();
    }

    void DebugVisualizerPanel::Shutdown()
    {
        std::cout << "Shutting down Debug Visualizer panel\n";
    }

    bool DebugVisualizerPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void DebugVisualizerPanel::RenderQuickToggles()
    {
        ImGui::Text(ICON_FA_BOLT " Quick Toggles");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        auto ToggleButton = [&](const char* label, bool& value)
        {
            if (value)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.95f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
                value = !value;
            if (value)
                ImGui::PopStyleColor(2);
        };

        ToggleButton(ICON_FA_TH " Grid", m_showGrid);
        ImGui::SameLine();
        ToggleButton(ICON_FA_BORDER_ALL " Wire", m_wireframeMode);
        ImGui::SameLine();
        ToggleButton(ICON_FA_CUBE " Collide", m_showColliders);

        ToggleButton(ICON_FA_EXPAND " BBox", m_showBoundingBoxes);
        ImGui::SameLine();
        ToggleButton(ICON_FA_MAP " Nav", m_showNavMesh);
        ImGui::SameLine();
        ToggleButton(ICON_FA_LIGHTBULB " Lights", m_showLightRadii);

        ToggleButton(ICON_FA_VOLUME_UP " Audio", m_showAudioRadii);
        ImGui::SameLine();
        ToggleButton(ICON_FA_CAMERA " Cam", m_showCameraFrusta);
        ImGui::SameLine();
        ToggleButton(ICON_FA_CROSSHAIRS " Origin", m_showObjectOrigins);
    }

    void DebugVisualizerPanel::RenderGridSettings()
    {
        ImGui::Indent(8.0f);
        ImGui::Checkbox("Show Grid", &m_showGrid);
        if (m_showGrid)
        {
            ImGui::DragFloat("Grid Size", &m_gridSize, 1.0f, 10.0f, 1000.0f, "%.0f m");
            ImGui::DragFloat("Grid Spacing", &m_gridSpacing, 0.1f, 0.1f, 50.0f, "%.1f m");
            ImGui::DragFloat("Major Line Every", &m_gridMajorLineEvery, 1.0f, 2.0f, 50.0f, "%.0f lines");
            ImGui::ColorEdit4("Grid Color", m_gridColor, ImGuiColorEditFlags_NoInputs);
            ImGui::ColorEdit4("Major Line Color", m_gridMajorColor, ImGuiColorEditFlags_NoInputs);
            ImGui::Checkbox("Snap to Origin", &m_gridSnapToOrigin);

            const char* planes[] = {"XZ (Floor)", "XY (Front)", "YZ (Side)"};
            ImGui::Combo("Grid Plane", &m_gridPlane, planes, 3);
        }
        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderOverlaySettings()
    {
        ImGui::Indent(8.0f);

        const char* shadingModes[] = {"Lit", "Unlit", "Wireframe", "Normals", "UVs", "Overdraw"};
        ImGui::Combo("Shading Mode", &m_shadingMode, shadingModes, 6);

        ImGui::Checkbox("Show Normals", &m_showNormals);
        if (m_showNormals)
        {
            ImGui::DragFloat("Normal Length", &m_normalLength, 0.01f, 0.01f, 5.0f, "%.2f m");
        }
        ImGui::Checkbox("Show Tangents", &m_showTangents);
        ImGui::Checkbox("Show UV Layout", &m_showUVs);
        ImGui::Checkbox("Show Selection Outline", &m_showSelectionOutline);
        ImGui::Checkbox("Show Object Origins", &m_showObjectOrigins);

        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderPhysicsDebug()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Show Colliders", &m_showColliders);
        if (m_showColliders)
        {
            ImGui::Checkbox("Filled Colliders", &m_showCollidersFilled);
            ImGui::DragFloat("Collider Alpha", &m_colliderAlpha, 0.01f, 0.05f, 1.0f, "%.2f");
        }
        ImGui::Checkbox("Show Bounding Boxes", &m_showBoundingBoxes);
        ImGui::Checkbox("Show Physics Contacts", &m_showPhysicsContacts);
        ImGui::Checkbox("Show Raycasts", &m_showRaycasts);
        ImGui::Checkbox("Show Velocity Vectors", &m_showVelocityVectors);
        if (m_showVelocityVectors)
        {
            ImGui::DragFloat("Velocity Scale", &m_velocityScale, 0.01f, 0.01f, 1.0f, "%.2f");
        }

        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderNavigationDebug()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Show NavMesh", &m_showNavMesh);
        if (m_showNavMesh)
        {
            ImGui::DragFloat("NavMesh Alpha", &m_navMeshAlpha, 0.01f, 0.05f, 1.0f, "%.2f");
        }
        ImGui::Checkbox("Show NavMesh Bounds", &m_showNavMeshBounds);
        ImGui::Checkbox("Show Agent Paths", &m_showNavPaths);
        ImGui::Checkbox("Show AI Agents", &m_showNavAgents);

        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderRenderingDebug()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Show Light Radii", &m_showLightRadii);
        ImGui::Checkbox("Show Shadow Cascades", &m_showShadowCascades);
        ImGui::Checkbox("Show Camera Frusta", &m_showCameraFrusta);
        ImGui::Separator();
        ImGui::Checkbox("Show FPS Overlay", &m_showFPS);
        ImGui::Checkbox("Show Draw Calls", &m_showDrawCalls);
        ImGui::Checkbox("Show Triangle Count", &m_showTriangleCount);

        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderAudioDebug()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Show Audio Source Radii", &m_showAudioRadii);
        ImGui::Checkbox("Show Audio Source Icons", &m_showAudioSourceIcons);

        ImGui::Unindent(8.0f);
    }

    void DebugVisualizerPanel::RenderStats()
    {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Debug Draw Stats:");
        ImGui::Text("  Grid Lines: %d", m_stats.gridLines);
        ImGui::Text("  Collider Shapes: %d", m_stats.colliderShapes);
        ImGui::Text("  Bounding Boxes: %d", m_stats.boundingBoxes);
        ImGui::Text("  NavMesh Polygons: %d", m_stats.navMeshPolygons);
        ImGui::Text("  Light Gizmos: %d", m_stats.lightGizmos);
        ImGui::Text("  Audio Sources: %d", m_stats.audioSources);
    }

} // namespace SparkEditor
