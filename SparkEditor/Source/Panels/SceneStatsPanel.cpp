/**
 * @file SceneStatsPanel.cpp
 * @brief Scene statistics and performance information panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>

#include <imgui.h>

#include "SceneStatsPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    SceneStatsPanel::SceneStatsPanel() : EditorPanel("Scene Stats", "scene_stats_panel")
    {
        m_lastUpdate = std::chrono::steady_clock::now();
        std::memset(m_frameHistory, 0, sizeof(m_frameHistory));
    }

    SceneStatsPanel::~SceneStatsPanel() {}

    bool SceneStatsPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Scene Stats panel\n";
        return true;
    }

    void SceneStatsPanel::Update(float deltaTime)
    {
        if (deltaTime <= 0.0f)
            return;

        m_currentFrameTime = deltaTime * 1000.0f; // to ms
        float instantFPS = 1.0f / deltaTime;

        // Rolling frame history
        m_frameHistory[m_frameHistoryIndex] = m_currentFrameTime;
        m_frameHistoryIndex = (m_frameHistoryIndex + 1) % FRAME_HISTORY_SIZE;

        // Update stats periodically
        m_fpsUpdateTimer += deltaTime;
        if (m_fpsUpdateTimer >= 0.25f)
        {
            m_fpsUpdateTimer = 0.0f;
            m_currentFPS = instantFPS;

            // Compute average from history
            float sum = 0.0f;
            float minT = 9999.0f;
            float maxT = 0.0f;
            for (size_t i = 0; i < FRAME_HISTORY_SIZE; ++i)
            {
                sum += m_frameHistory[i];
                if (m_frameHistory[i] > 0.0f && m_frameHistory[i] < minT)
                    minT = m_frameHistory[i];
                if (m_frameHistory[i] > maxT)
                    maxT = m_frameHistory[i];
            }
            float avgMs = sum / static_cast<float>(FRAME_HISTORY_SIZE);
            m_avgFPS = avgMs > 0.001f ? 1000.0f / avgMs : 0.0f;
            m_minFPS = maxT > 0.001f ? 1000.0f / maxT : 0.0f; // max time = min FPS
            m_maxFPS = minT > 0.001f ? 1000.0f / minT : 0.0f;
        }
    }

    void SceneStatsPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::CollapsingHeader(ICON_FA_CHART_BAR " Performance", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderPerformanceSection();
            }

            if (ImGui::CollapsingHeader(ICON_FA_SITEMAP " Scene", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderSceneSection();
            }

            if (ImGui::CollapsingHeader(ICON_FA_CAMERA " Rendering"))
            {
                RenderRenderingSection();
            }

            if (ImGui::CollapsingHeader(ICON_FA_CUBE " Physics"))
            {
                RenderPhysicsSection();
            }

            if (ImGui::CollapsingHeader(ICON_FA_VOLUME_UP " Audio"))
            {
                RenderAudioSection();
            }

            if (ImGui::CollapsingHeader(ICON_FA_COG " Memory"))
            {
                RenderMemorySection();
            }
        }
        EndPanel();
    }

    void SceneStatsPanel::Shutdown()
    {
        std::cout << "Shutting down Scene Stats panel\n";
    }

    bool SceneStatsPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void SceneStatsPanel::RenderPerformanceSection()
    {
        ImGui::Indent(8.0f);

        // FPS with color coding
        ImVec4 fpsColor = m_currentFPS >= 60.0f   ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                          : m_currentFPS >= 30.0f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                                                  : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

        ImGui::TextColored(fpsColor, "FPS: %.1f", m_currentFPS);
        ImGui::SameLine(150);
        ImGui::Text("Frame: %.2f ms", m_currentFrameTime);

        ImGui::Text("Avg: %.1f FPS", m_avgFPS);
        ImGui::SameLine(150);
        ImGui::Text("Min: %.1f / Max: %.1f", m_minFPS, m_maxFPS);

        // Frame time graph
        RenderFrameGraph();

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderSceneSection()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Objects: %d", m_totalObjects);
        ImGui::SameLine(150);
        ImGui::Text("Selected: %d", m_selectedObjects);

        int totalLights = m_directionalLights + m_pointLights + m_spotLights;
        ImGui::Text("Lights: %d total", totalLights);
        if (totalLights > 0)
        {
            ImGui::Text("  Directional: %d | Point: %d | Spot: %d", m_directionalLights, m_pointLights, m_spotLights);
        }

        ImGui::Text("AI Agents: %d", m_aiAgentCount);

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderRenderingSection()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Draw Calls: %d", m_drawCalls);
        ImGui::Text("Triangles: %d", m_triangleCount);
        ImGui::Text("Vertices: %d", m_vertexCount);

        // Draw call bar
        float drawCallRatio = std::min(static_cast<float>(m_drawCalls) / 1000.0f, 1.0f);
        ImVec4 barColor = drawCallRatio < 0.5f    ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                          : drawCallRatio < 0.75f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                                                  : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        ImGui::ProgressBar(drawCallRatio, ImVec2(-1, 14), "");
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Draw call budget: %d / 1000", m_drawCalls);

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderPhysicsSection()
    {
        ImGui::Indent(8.0f);

        int totalBodies = m_activePhysicsBodies + m_sleepingPhysicsBodies;
        ImGui::Text("Physics Bodies: %d total", totalBodies);
        ImGui::Text("  Active: %d | Sleeping: %d", m_activePhysicsBodies, m_sleepingPhysicsBodies);

        if (totalBodies > 0)
        {
            float activeRatio = static_cast<float>(m_activePhysicsBodies) / static_cast<float>(totalBodies);
            ImGui::ProgressBar(activeRatio, ImVec2(-1, 14), "");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%.0f%% active", activeRatio * 100.0f);
        }

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderAudioSection()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Audio Sources: %d / %d playing", m_playingAudioSources, m_totalAudioSources);

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderMemorySection()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Texture Memory: %s", FormatBytes(m_textureMemoryBytes).c_str());
        ImGui::Text("Mesh Memory: %s", FormatBytes(m_meshMemoryBytes).c_str());

        size_t totalGPU = m_textureMemoryBytes + m_meshMemoryBytes;
        ImGui::Separator();
        ImGui::Text("Total GPU: %s", FormatBytes(totalGPU).c_str());

        // Memory budget bar
        float memRatio = std::min(static_cast<float>(totalGPU) / (512.0f * 1024.0f * 1024.0f), 1.0f);
        ImVec4 memColor = memRatio < 0.5f    ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                          : memRatio < 0.75f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                                             : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, memColor);
        ImGui::ProgressBar(memRatio, ImVec2(-1, 14), "");
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "GPU budget: %s / 512 MB", FormatBytes(totalGPU).c_str());

        ImGui::Unindent(8.0f);
    }

    void SceneStatsPanel::RenderFrameGraph()
    {
        // Build array starting from oldest data
        float plotData[FRAME_HISTORY_SIZE];
        for (size_t i = 0; i < FRAME_HISTORY_SIZE; ++i)
        {
            size_t idx = (m_frameHistoryIndex + i) % FRAME_HISTORY_SIZE;
            plotData[i] = m_frameHistory[idx];
        }

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.2f ms", m_currentFrameTime);
        ImGui::PlotLines("##FrameTime", plotData, static_cast<int>(FRAME_HISTORY_SIZE), 0, overlay, 0.0f, 50.0f,
                         ImVec2(-1, 50));
    }

    std::string SceneStatsPanel::FormatBytes(size_t bytes)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        if (bytes >= 1024ULL * 1024 * 1024)
            oss << static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) << " GB";
        else if (bytes >= 1024ULL * 1024)
            oss << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
        else if (bytes >= 1024ULL)
            oss << static_cast<double>(bytes) / 1024.0 << " KB";
        else
            oss << bytes << " B";
        return oss.str();
    }

} // namespace SparkEditor
