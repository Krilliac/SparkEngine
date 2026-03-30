/**
 * @file SceneStatisticsPanel.cpp
 * @brief Implementation of the scene statistics panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneStatisticsPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    SceneStatisticsPanel::SceneStatisticsPanel() : EditorPanel("Scene Statistics", "SceneStats") {}

    bool SceneStatisticsPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Scene Statistics panel");
        m_fpsHistory.fill(0.0f);
        m_frameTimeHistory.fill(0.0f);
        m_drawCallHistory.fill(0.0f);
        m_memoryHistory.fill(0.0f);

        // Initialize with sample data for demonstration
        m_entityStats.totalEntities = 142;
        m_entityStats.activeEntities = 128;
        m_entityStats.inactiveEntities = 14;
        m_entityStats.componentCounts = {{"Transform", 142}, {"MeshRenderer", 87},   {"Light", 12},
                                         {"Camera", 3},      {"RigidBody", 45},      {"Collider", 52},
                                         {"AudioSource", 8}, {"SpriteRenderer", 15}, {"ParticleSystem", 6}};

        m_renderStats.drawCalls = 256;
        m_renderStats.triangleCount = 145000;
        m_renderStats.vertexCount = 87000;
        m_renderStats.shaderCount = 24;
        m_renderStats.textureMemoryMB = 384.5f;
        m_renderStats.renderTargetCount = 6;
        m_renderStats.batchCount = 48;

        m_physicsStats.activeRigidBodies = 45;
        m_physicsStats.staticBodies = 87;
        m_physicsStats.collisionPairs = 23;
        m_physicsStats.raycastsPerFrame = 12;
        m_physicsStats.simulationTimeMs = 2.3f;

        m_memoryStats.totalSceneMemoryMB = 512.0f;
        m_memoryStats.meshMemoryMB = 128.0f;
        m_memoryStats.textureMemoryMB = 256.0f;
        m_memoryStats.audioMemoryMB = 64.0f;
        m_memoryStats.assetCacheMemoryMB = 48.0f;
        m_memoryStats.scriptMemoryMB = 16.0f;

        m_isInitialized = true;
        return true;
    }

    void SceneStatisticsPanel::Update(float deltaTime)
    {
        m_simulationTime += deltaTime;
        m_frameCounter++;

        if (m_frameCounter >= m_updateInterval)
        {
            m_frameCounter = 0;
            UpdateHistoryBuffers(deltaTime);
        }
    }

    void SceneStatisticsPanel::Render()
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

        // Update interval control
        ImGui::SliderInt("Update Interval", &m_updateInterval, 1, 60, "%d frames");
        ImGui::Separator();

        RenderPerformanceGraphs();
        RenderEntityStats();
        RenderRenderStats();
        RenderPhysicsStats();
        RenderMemoryStats();

        EndPanel();
    }

    void SceneStatisticsPanel::Shutdown() {}

    void SceneStatisticsPanel::RenderEntityStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CUBE " Entity Statistics",
                                    m_showEntitySection ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            m_showEntitySection = true;

            ImGui::Columns(2, "EntityStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Total Entities");
            ImGui::NextColumn();
            ImGui::Text("%d", m_entityStats.totalEntities);
            ImGui::NextColumn();

            ImGui::TextDisabled("Active");
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "%d", m_entityStats.activeEntities);
            ImGui::NextColumn();

            ImGui::TextDisabled("Inactive");
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.3f, 1.0f), "%d", m_entityStats.inactiveEntities);
            ImGui::NextColumn();

            ImGui::Columns(1);

            // Component breakdown table
            if (ImGui::TreeNode("Component Breakdown"))
            {
                ImGui::BeginChild("ComponentTable", ImVec2(0, 200), true);

                ImGui::Columns(2, "ComponentColumns");
                ImGui::SetColumnWidth(0, 180.0f);
                ImGui::TextDisabled("Component Type");
                ImGui::NextColumn();
                ImGui::TextDisabled("Count");
                ImGui::NextColumn();
                ImGui::Separator();

                for (const auto& [type, count] : m_entityStats.componentCounts)
                {
                    ImGui::Text("%s", type.c_str());
                    ImGui::NextColumn();
                    ImGui::Text("%d", count);
                    ImGui::NextColumn();
                }

                ImGui::Columns(1);
                ImGui::EndChild();
                ImGui::TreePop();
            }

            ImGui::Spacing();
        }
        else
        {
            m_showEntitySection = false;
        }
    }

    void SceneStatisticsPanel::RenderRenderStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CAMERA " Render Statistics",
                                    m_showRenderSection ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            m_showRenderSection = true;

            ImGui::Columns(2, "RenderStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Draw Calls");
            ImGui::NextColumn();
            ImGui::Text("%d", m_renderStats.drawCalls);
            ImGui::NextColumn();

            ImGui::TextDisabled("Triangles");
            ImGui::NextColumn();
            ImGui::Text("%s", (m_renderStats.triangleCount > 1000)
                                  ? (std::to_string(m_renderStats.triangleCount / 1000) + "K").c_str()
                                  : std::to_string(m_renderStats.triangleCount).c_str());
            ImGui::NextColumn();

            ImGui::TextDisabled("Vertices");
            ImGui::NextColumn();
            ImGui::Text("%s", (m_renderStats.vertexCount > 1000)
                                  ? (std::to_string(m_renderStats.vertexCount / 1000) + "K").c_str()
                                  : std::to_string(m_renderStats.vertexCount).c_str());
            ImGui::NextColumn();

            ImGui::TextDisabled("Shaders");
            ImGui::NextColumn();
            ImGui::Text("%d", m_renderStats.shaderCount);
            ImGui::NextColumn();

            ImGui::TextDisabled("Texture Memory");
            ImGui::NextColumn();
            ImGui::Text("%.1f MB", m_renderStats.textureMemoryMB);
            ImGui::NextColumn();

            ImGui::TextDisabled("Render Targets");
            ImGui::NextColumn();
            ImGui::Text("%d", m_renderStats.renderTargetCount);
            ImGui::NextColumn();

            ImGui::TextDisabled("Batches");
            ImGui::NextColumn();
            ImGui::Text("%d", m_renderStats.batchCount);
            ImGui::NextColumn();

            ImGui::Columns(1);
            ImGui::Spacing();
        }
        else
        {
            m_showRenderSection = false;
        }
    }

    void SceneStatisticsPanel::RenderPhysicsStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_BOLT " Physics Statistics",
                                    m_showPhysicsSection ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            m_showPhysicsSection = true;

            ImGui::Columns(2, "PhysicsStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Active Rigid Bodies");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.activeRigidBodies);
            ImGui::NextColumn();

            ImGui::TextDisabled("Static Bodies");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.staticBodies);
            ImGui::NextColumn();

            ImGui::TextDisabled("Collision Pairs");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.collisionPairs);
            ImGui::NextColumn();

            ImGui::TextDisabled("Raycasts/Frame");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.raycastsPerFrame);
            ImGui::NextColumn();

            ImGui::TextDisabled("Simulation Time");
            ImGui::NextColumn();
            ImGui::Text("%.2f ms", m_physicsStats.simulationTimeMs);
            ImGui::NextColumn();

            ImGui::Columns(1);
            ImGui::Spacing();
        }
        else
        {
            m_showPhysicsSection = false;
        }
    }

    void SceneStatisticsPanel::RenderMemoryStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CHART_BAR " Memory Usage",
                                    m_showMemorySection ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            m_showMemorySection = true;

            // Total memory bar
            ImGui::Text("Total Scene Memory: %.1f MB", m_memoryStats.totalSceneMemoryMB);

            // Breakdown bars
            float total = m_memoryStats.totalSceneMemoryMB;
            if (total > 0.0f)
            {
                ImGui::TextDisabled("Meshes");
                ImGui::SameLine(120);
                ImGui::ProgressBar(m_memoryStats.meshMemoryMB / total, ImVec2(-1, 0),
                                   (std::to_string(static_cast<int>(m_memoryStats.meshMemoryMB)) + " MB").c_str());

                ImGui::TextDisabled("Textures");
                ImGui::SameLine(120);
                ImGui::ProgressBar(m_memoryStats.textureMemoryMB / total, ImVec2(-1, 0),
                                   (std::to_string(static_cast<int>(m_memoryStats.textureMemoryMB)) + " MB").c_str());

                ImGui::TextDisabled("Audio");
                ImGui::SameLine(120);
                ImGui::ProgressBar(m_memoryStats.audioMemoryMB / total, ImVec2(-1, 0),
                                   (std::to_string(static_cast<int>(m_memoryStats.audioMemoryMB)) + " MB").c_str());

                ImGui::TextDisabled("Asset Cache");
                ImGui::SameLine(120);
                ImGui::ProgressBar(
                    m_memoryStats.assetCacheMemoryMB / total, ImVec2(-1, 0),
                    (std::to_string(static_cast<int>(m_memoryStats.assetCacheMemoryMB)) + " MB").c_str());

                ImGui::TextDisabled("Scripts");
                ImGui::SameLine(120);
                ImGui::ProgressBar(m_memoryStats.scriptMemoryMB / total, ImVec2(-1, 0),
                                   (std::to_string(static_cast<int>(m_memoryStats.scriptMemoryMB)) + " MB").c_str());
            }

            ImGui::Spacing();
        }
        else
        {
            m_showMemorySection = false;
        }
    }

    void SceneStatisticsPanel::RenderPerformanceGraphs()
    {
        if (ImGui::CollapsingHeader(ICON_FA_BOLT " Performance",
                                    m_showPerformanceSection ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            m_showPerformanceSection = true;

            // FPS display
            ImGui::Text("FPS: %.1f (avg: %.1f, min: %.1f, max: %.1f)", m_currentFps, m_averageFps, m_minFps, m_maxFps);

            // FPS graph
            ImGui::PlotLines("##FPSGraph", m_fpsHistory.data(), static_cast<int>(HISTORY_SIZE),
                             static_cast<int>(m_historyIndex), "FPS", 0.0f, 120.0f, ImVec2(0, 60));

            // Frame time graph
            ImGui::Text("Frame Time: %.2f ms", m_currentFrameTime * 1000.0f);
            ImGui::PlotLines("##FrameTimeGraph", m_frameTimeHistory.data(), static_cast<int>(HISTORY_SIZE),
                             static_cast<int>(m_historyIndex), "Frame Time (ms)", 0.0f, 33.3f, ImVec2(0, 60));

            // Draw calls graph
            ImGui::PlotHistogram("##DrawCallsGraph", m_drawCallHistory.data(), static_cast<int>(HISTORY_SIZE),
                                 static_cast<int>(m_historyIndex), "Draw Calls", 0.0f, 500.0f, ImVec2(0, 40));

            ImGui::Spacing();
        }
        else
        {
            m_showPerformanceSection = false;
        }
    }

    void SceneStatisticsPanel::UpdateHistoryBuffers(float deltaTime)
    {
        if (deltaTime > 0.0f)
        {
            m_currentFrameTime = deltaTime;
            m_currentFps = 1.0f / deltaTime;
        }
        else
        {
            m_currentFrameTime = 0.016f;
            m_currentFps = 60.0f;
        }

        // Add some realistic variation for demonstration
        float variation = std::sin(m_simulationTime * 0.5f) * 5.0f;
        float demoFps = 60.0f + variation + std::sin(m_simulationTime * 2.3f) * 3.0f;

        m_fpsHistory[m_historyIndex] = demoFps;
        m_frameTimeHistory[m_historyIndex] = 1000.0f / demoFps;
        m_drawCallHistory[m_historyIndex] = static_cast<float>(m_renderStats.drawCalls) + variation * 5.0f;
        m_memoryHistory[m_historyIndex] = m_memoryStats.totalSceneMemoryMB + std::sin(m_simulationTime) * 2.0f;

        m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;

        // Update min/max/avg FPS
        float sum = 0.0f;
        m_minFps = 999.0f;
        m_maxFps = 0.0f;
        int validCount = 0;
        for (size_t i = 0; i < HISTORY_SIZE; ++i)
        {
            if (m_fpsHistory[i] > 0.0f)
            {
                sum += m_fpsHistory[i];
                m_minFps = std::min(m_minFps, m_fpsHistory[i]);
                m_maxFps = std::max(m_maxFps, m_fpsHistory[i]);
                ++validCount;
            }
        }
        m_averageFps = (validCount > 0) ? sum / static_cast<float>(validCount) : 0.0f;
        if (m_minFps > 900.0f)
        {
            m_minFps = 0.0f;
        }
    }

} // namespace SparkEditor
