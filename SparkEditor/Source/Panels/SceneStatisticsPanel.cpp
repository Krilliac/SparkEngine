/**
 * @file SceneStatisticsPanel.cpp
 * @brief Implementation of the scene statistics panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneStatisticsPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <imgui.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace SparkEditor
{

    namespace
    {
        /// Resident/working-set size of this process in megabytes; 0 when unavailable.
        float QueryProcessMemoryMB()
        {
#ifdef _WIN32
            PROCESS_MEMORY_COUNTERS counters = {};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            {
                return static_cast<float>(counters.WorkingSetSize) / (1024.0f * 1024.0f);
            }
            return 0.0f;
#else
            long residentPages = 0;
            if (FILE* statm = std::fopen("/proc/self/statm", "r"))
            {
                long totalPages = 0;
                const int read = std::fscanf(statm, "%ld %ld", &totalPages, &residentPages);
                std::fclose(statm);
                if (read != 2)
                    return 0.0f;
            }
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (residentPages <= 0 || pageSize <= 0)
                return 0.0f;
            return static_cast<float>(residentPages) * static_cast<float>(pageSize) / (1024.0f * 1024.0f);
#endif
        }
    } // namespace

    SceneStatisticsPanel::SceneStatisticsPanel() : EditorPanel("Scene Statistics", "SceneStats") {}

    bool SceneStatisticsPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Scene Statistics panel");
        m_fpsHistory.fill(0.0f);
        m_frameTimeHistory.fill(0.0f);
        m_drawCallHistory.fill(0.0f);

        m_isInitialized = true;
        return true;
    }

    void SceneStatisticsPanel::Update(float deltaTime)
    {
        m_frameCounter++;

        if (m_frameCounter >= m_updateInterval)
        {
            m_frameCounter = 0;
            CollectStats();
            UpdateHistoryBuffers(deltaTime);
        }
    }

    void SceneStatisticsPanel::CollectStats()
    {
        // --- Entities and components: measured from the document World ---
        m_totalEntities = 0;
        m_componentCounts.clear();
        if (m_world)
        {
            auto& registry = m_world->GetRegistry();
            const std::vector<std::string> typeNames = Spark::ComponentFactory::Get().GetRegisteredNames();

            for (auto&& [entity] : registry.storage<entt::entity>().each())
            {
                ++m_totalEntities;
                for (const std::string& typeName : typeNames)
                {
                    if (Spark::ComponentFactory::Get().HasComponent(typeName, m_world, static_cast<uint32_t>(entity)))
                    {
                        ++m_componentCounts[typeName];
                    }
                }
            }
        }

        // --- Physics: only from a running PhysicsSystem ---
        m_hasPhysicsStats = false;
        if (auto* context = ::EngineContext::Get())
        {
            if (auto* physics = context->GetPhysics())
            {
                const PhysicsSystem::PhysicsMetrics metrics = physics->GetMetrics();
                m_physicsStats.activeRigidBodies = static_cast<int>(metrics.activeRigidBodies);
                m_physicsStats.totalRigidBodies = static_cast<int>(metrics.totalRigidBodies);
                m_physicsStats.collisionPairs = static_cast<int>(metrics.collisionPairs);
                m_physicsStats.raycastsPerFrame = static_cast<int>(metrics.raycastCount);
                m_physicsStats.simulationTimeMs = metrics.simulationTime;
                m_hasPhysicsStats = true;
            }
        }

        // --- Memory: reported by the OS for this process ---
        m_processMemoryMB = QueryProcessMemoryMB();
        m_hasProcessMemory = m_processMemoryMB > 0.0f;
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

            if (!IsWorldConnected())
            {
                ImGui::TextDisabled("Preview - not connected: no World is wired to this panel.");
                ImGui::Spacing();
                return;
            }

            ImGui::Columns(2, "EntityStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Total Entities");
            ImGui::NextColumn();
            ImGui::Text("%d", m_totalEntities);
            ImGui::NextColumn();

            ImGui::Columns(1);

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

                for (const auto& [type, count] : m_componentCounts)
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

            if (!m_hasRenderStats)
            {
                ImGui::TextDisabled("Preview - not connected: the viewport has not reported render");
                ImGui::TextDisabled("statistics to this panel.");
                ImGui::Spacing();
                return;
            }

            ImGui::Columns(2, "RenderStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Draw Calls");
            ImGui::NextColumn();
            ImGui::Text("%u", m_renderStats.drawn);
            ImGui::NextColumn();

            ImGui::TextDisabled("Candidates");
            ImGui::NextColumn();
            ImGui::Text("%u", m_renderStats.candidates);
            ImGui::NextColumn();

            ImGui::TextDisabled("Visible");
            ImGui::NextColumn();
            ImGui::Text("%u", m_renderStats.visible);
            ImGui::NextColumn();

            ImGui::TextDisabled("Rejected");
            ImGui::NextColumn();
            ImGui::Text("%u", m_renderStats.rejected);
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

            if (!m_hasPhysicsStats)
            {
                ImGui::TextDisabled("Preview - not connected: no PhysicsSystem is running in this");
                ImGui::TextDisabled("process.");
                ImGui::Spacing();
                return;
            }

            ImGui::Columns(2, "PhysicsStatsColumns", false);
            ImGui::SetColumnWidth(0, 180.0f);

            ImGui::TextDisabled("Active Rigid Bodies");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.activeRigidBodies);
            ImGui::NextColumn();

            ImGui::TextDisabled("Total Rigid Bodies");
            ImGui::NextColumn();
            ImGui::Text("%d", m_physicsStats.totalRigidBodies);
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

            if (m_hasProcessMemory)
            {
                ImGui::Text("Editor process memory: %.1f MB", m_processMemoryMB);
                ImGui::TextDisabled("Reported by the OS for this process. Per-asset memory");
                ImGui::TextDisabled("breakdowns are not instrumented yet.");
            }
            else
            {
                ImGui::TextDisabled("Process memory is unavailable on this platform.");
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

            ImGui::Text("FPS: %.1f (avg: %.1f, min: %.1f, max: %.1f)", m_currentFps, m_averageFps, m_minFps, m_maxFps);

            ImGui::PlotLines("##FPSGraph", m_fpsHistory.data(), static_cast<int>(HISTORY_SIZE),
                             static_cast<int>(m_historyIndex), "FPS", 0.0f, 120.0f, ImVec2(0, 60));

            ImGui::Text("Frame Time: %.2f ms", m_currentFrameTime * 1000.0f);
            ImGui::PlotLines("##FrameTimeGraph", m_frameTimeHistory.data(), static_cast<int>(HISTORY_SIZE),
                             static_cast<int>(m_historyIndex), "Frame Time (ms)", 0.0f, 33.3f, ImVec2(0, 60));

            if (m_hasRenderStats)
            {
                ImGui::PlotHistogram("##DrawCallsGraph", m_drawCallHistory.data(), static_cast<int>(HISTORY_SIZE),
                                     static_cast<int>(m_historyIndex), "Draw Calls", 0.0f, 500.0f, ImVec2(0, 40));
            }

            ImGui::Spacing();
        }
        else
        {
            m_showPerformanceSection = false;
        }
    }

    void SceneStatisticsPanel::UpdateHistoryBuffers(float deltaTime)
    {
        // Measured frame delta only — a sample with no elapsed time is not recorded
        // rather than being replaced with an invented one.
        if (deltaTime <= 0.0f)
        {
            return;
        }

        m_currentFrameTime = deltaTime;
        m_currentFps = 1.0f / deltaTime;

        m_fpsHistory[m_historyIndex] = m_currentFps;
        m_frameTimeHistory[m_historyIndex] = m_currentFrameTime * 1000.0f;
        m_drawCallHistory[m_historyIndex] = static_cast<float>(m_renderStats.drawn);

        m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;

        float sum = 0.0f;
        float minFps = 0.0f;
        float maxFps = 0.0f;
        int validCount = 0;
        for (size_t i = 0; i < HISTORY_SIZE; ++i)
        {
            if (m_fpsHistory[i] <= 0.0f)
                continue;
            sum += m_fpsHistory[i];
            minFps = (validCount == 0) ? m_fpsHistory[i] : std::min(minFps, m_fpsHistory[i]);
            maxFps = std::max(maxFps, m_fpsHistory[i]);
            ++validCount;
        }
        m_averageFps = (validCount > 0) ? sum / static_cast<float>(validCount) : 0.0f;
        m_minFps = minFps;
        m_maxFps = maxFps;
    }

} // namespace SparkEditor
