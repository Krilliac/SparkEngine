/**
 * @file AIDebugPanel.cpp
 * @brief Real-time AI debug visualization panel implementation
 */

#include "AIDebugPanel.h"

#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Utils/LogMacros.h"

namespace SparkEditor
{

    AIDebugPanel::AIDebugPanel() : EditorPanel("AI Debug", "ai_debug_panel") {}

    bool AIDebugPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AIDebugPanel initialized");
        return true;
    }

    void AIDebugPanel::Update(float deltaTime)
    {
        m_refreshTimer += deltaTime;
        if (m_refreshTimer < m_refreshInterval)
            return;
        m_refreshTimer = 0.0f;

        // Refresh agent data from AISystem
        // In a real integration, this would query AISystem::GetInstance() for live agent data.
        // The editor state preview does not tick AI or populate live agent data.
    }

    void AIDebugPanel::Render()
    {
        if (!BeginPanel())
            return;

        if (ImGui::BeginTabBar("AIDebugTabs"))
        {
            if (ImGui::BeginTabItem("Agents"))
            {
                RenderAgentList();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Blackboard"))
            {
                RenderBlackboardInspector();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("BT Trace"))
            {
                RenderBehaviorTreeTrace();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Overlays"))
            {
                RenderPerceptionOverlays();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Stats"))
            {
                RenderStatistics();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        EndPanel();
    }

    void AIDebugPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AIDebugPanel shutting down (%d agents tracked)",
                       static_cast<int>(m_agents.size()));
        m_agents.clear();
        m_blackboardEntries.clear();
    }

    // =========================================================================
    // Agent List
    // =========================================================================

    const char* AIDebugPanel::AgentStateToString(AgentState state)
    {
        switch (state)
        {
        case AgentState::Idle:
            return "Idle";
        case AgentState::Patrolling:
            return "Patrol";
        case AgentState::Alert:
            return "Alert";
        case AgentState::Combat:
            return "Combat";
        case AgentState::Fleeing:
            return "Flee";
        case AgentState::Dead:
            return "Dead";
        }
        return "Unknown";
    }

    void AIDebugPanel::RenderAgentList()
    {
        // State filter combo
        const char* filterOptions[] = {"All", "Idle", "Patrol", "Alert", "Combat", "Flee", "Dead"};
        ImGui::Combo("State Filter", &m_stateFilterIndex, filterOptions, 7);
        ImGui::Separator();

        if (m_agents.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_BRAIN, "No editor AI agents",
                                  "State preview does not tick AI. Use Play Control > Launch Game to test game AI.");
            return;
        }

        // Agent table
        if (Spark::Ui::BeginDataTable("AgentTable", 6))
        {
            ImGui::TableSetupColumn("Entity");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Behavior Tree");
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Target Dist");
            ImGui::TableSetupColumn("Path Nodes");
            ImGui::TableHeadersRow();

            for (const auto& agent : m_agents)
            {
                // Apply state filter
                if (m_stateFilterIndex > 0 && static_cast<int>(agent.state) != (m_stateFilterIndex - 1))
                    continue;

                ImGui::TableNextRow();

                bool isSelected = (agent.entityID == m_selectedAgentID);
                ImGui::TableSetColumnIndex(0);
                char label[32];
                std::snprintf(label, sizeof(label), "Entity %u", agent.entityID);
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "AIDebugPanel: selected agent %u", agent.entityID);
                    m_selectedAgentID = agent.entityID;
                }

                ImGui::TableSetColumnIndex(1);
                const char* stateStr = AgentStateToString(agent.state);
                // Threat states carry a status glyph as well as a colour; calm states stay muted.
                switch (agent.state)
                {
                case AgentState::Idle:
                    Spark::Ui::SecondaryText("%s", stateStr);
                    break;
                case AgentState::Combat:
                    Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", stateStr);
                    break;
                case AgentState::Alert:
                    Spark::Ui::StatusText(Spark::Ui::Status::Warning, "%s", stateStr);
                    break;
                case AgentState::Fleeing:
                    Spark::Ui::StatusText(Spark::Ui::Status::Info, "%s", stateStr);
                    break;
                case AgentState::Dead:
                    Spark::Ui::TextRole(Spark::Ui::Role::TextDisabled, "%s", stateStr);
                    break;
                default:
                    ImGui::TextUnformatted(stateStr);
                    break;
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(agent.behaviorTreeName.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(agent.currentAction.c_str());

                ImGui::TableSetColumnIndex(4);
                if (agent.hasTarget)
                    Spark::Ui::TableNumber("%.1f m", agent.distanceToTarget);
                else
                    ImGui::TextDisabled("--");

                ImGui::TableSetColumnIndex(5);
                Spark::Ui::TableNumber("%d", agent.pathNodeCount);
            }

            ImGui::EndTable();
        }
    }

    // =========================================================================
    // Blackboard Inspector
    // =========================================================================

    void AIDebugPanel::RenderBlackboardInspector()
    {
        if (m_selectedAgentID == 0)
        {
            Spark::Ui::EmptyState(ICON_FA_HAND_POINTER, "No agent selected",
                                  "Select an agent from the Agents tab to inspect its blackboard.");
            return;
        }

        char header[48];
        std::snprintf(header, sizeof(header), "Agent %u Blackboard", m_selectedAgentID);
        Spark::Ui::SubHeader(header);

        if (m_blackboardEntries.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_DATABASE, "No blackboard entries",
                                  "The agent may not be using a behavior tree.");
            return;
        }

        if (Spark::Ui::BeginDataTable("BlackboardTable", 3))
        {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();

            for (const auto& entry : m_blackboardEntries)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.key.c_str());
                ImGui::TableSetColumnIndex(1);
                Spark::Ui::SecondaryText("%s", entry.type.c_str());
                ImGui::TableSetColumnIndex(2);
                Spark::Ui::MonoText("%s", entry.value.c_str());
            }

            ImGui::EndTable();
        }
    }

    // =========================================================================
    // Behavior Tree Trace
    // =========================================================================

    void AIDebugPanel::RenderBehaviorTreeTrace()
    {
        if (m_selectedAgentID == 0)
        {
            Spark::Ui::EmptyState(ICON_FA_HAND_POINTER, "No agent selected",
                                  "Select an agent to view its behavior tree execution trace.");
            return;
        }

        ImGui::Checkbox("Highlight Active Node", &m_highlightActiveNode);
        ImGui::Separator();

        Spark::Ui::EmptyState(ICON_FA_SITEMAP, "No live trace in state preview",
                              "Behavior tree trace will display the call stack of the currently executing node, "
                              "with timing data per node.");
    }

    // =========================================================================
    // Perception Overlays
    // =========================================================================

    void AIDebugPanel::RenderPerceptionOverlays()
    {
        Spark::Ui::SubHeader("Debug Overlay Toggles");

        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "AIDebugPanel: rendering perception overlay toggles");
        ImGui::Checkbox("Detection Ranges", &m_showDetectionRanges);
        ImGui::SameLine();
        Spark::Ui::SecondaryText("(spheres around agents)");

        ImGui::Checkbox("Attack Ranges", &m_showAttackRanges);
        ImGui::SameLine();
        Spark::Ui::SecondaryText("(inner spheres)");

        ImGui::Checkbox("Nav Paths", &m_showNavPaths);
        ImGui::SameLine();
        Spark::Ui::SecondaryText("(line segments)");

        ImGui::Checkbox("Target Lines", &m_showTargetLines);
        ImGui::SameLine();
        Spark::Ui::SecondaryText("(lines to current target)");

        ImGui::Separator();
        ImGui::SliderFloat("Refresh Rate", &m_refreshInterval, 0.033f, 1.0f, "%.0f ms");
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    void AIDebugPanel::RenderStatistics()
    {
        Spark::Ui::SubHeader("AI System Statistics");

        int totalAgents = static_cast<int>(m_agents.size());
        int combatAgents = 0;
        int alertAgents = 0;
        int idleAgents = 0;
        int withTargets = 0;

        for (const auto& agent : m_agents)
        {
            if (agent.state == AgentState::Combat)
                combatAgents++;
            else if (agent.state == AgentState::Alert)
                alertAgents++;
            else if (agent.state == AgentState::Idle)
                idleAgents++;
            if (agent.hasTarget)
                withTargets++;
        }

        Spark::Ui::KeyValue("Total Agents:", "%d", totalAgents);
        Spark::Ui::KeyValue("In Combat:", "%d", combatAgents);
        Spark::Ui::KeyValue("Alert:", "%d", alertAgents);
        Spark::Ui::KeyValue("Idle:", "%d", idleAgents);
        Spark::Ui::KeyValue("With Targets:", "%d", withTargets);
    }

} // namespace SparkEditor
