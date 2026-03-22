/**
 * @file AIDebugPanel.cpp
 * @brief Real-time AI debug visualization panel implementation
 */

#include "AIDebugPanel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SparkEditor
{

    AIDebugPanel::AIDebugPanel() : EditorPanel("AI Debug", "ai_debug_panel") {}

    bool AIDebugPanel::Initialize()
    {
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
        // For now, the panel is wired and ready — agents appear when play mode is active.
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
            ImGui::TextDisabled("No AI agents active. Enter Play mode to see live agent data.");
            return;
        }

        // Agent table
        if (ImGui::BeginTable("AgentTable", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
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
                    m_selectedAgentID = agent.entityID;
                }

                ImGui::TableSetColumnIndex(1);
                const char* stateStr = AgentStateToString(agent.state);
                // Color-code state
                switch (agent.state)
                {
                case AgentState::Idle:
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", stateStr);
                    break;
                case AgentState::Combat:
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", stateStr);
                    break;
                case AgentState::Alert:
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", stateStr);
                    break;
                case AgentState::Fleeing:
                    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "%s", stateStr);
                    break;
                case AgentState::Dead:
                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "%s", stateStr);
                    break;
                default:
                    ImGui::Text("%s", stateStr);
                    break;
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", agent.behaviorTreeName.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", agent.currentAction.c_str());

                ImGui::TableSetColumnIndex(4);
                if (agent.hasTarget)
                    ImGui::Text("%.1f m", agent.distanceToTarget);
                else
                    ImGui::TextDisabled("--");

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d", agent.pathNodeCount);
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
            ImGui::TextDisabled("Select an agent from the Agents tab to inspect its blackboard.");
            return;
        }

        ImGui::Text("Agent %u Blackboard", m_selectedAgentID);
        ImGui::Separator();

        if (m_blackboardEntries.empty())
        {
            ImGui::TextDisabled("No blackboard entries. Agent may not be using a behavior tree.");
            return;
        }

        if (ImGui::BeginTable("BlackboardTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();

            for (const auto& entry : m_blackboardEntries)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", entry.key.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", entry.type.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", entry.value.c_str());
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
            ImGui::TextDisabled("Select an agent to view its behavior tree execution trace.");
            return;
        }

        ImGui::Checkbox("Highlight Active Node", &m_highlightActiveNode);
        ImGui::Separator();

        ImGui::TextDisabled("Behavior tree trace will display the call stack of the currently "
                            "executing node, with timing data per node. Requires play mode.");
    }

    // =========================================================================
    // Perception Overlays
    // =========================================================================

    void AIDebugPanel::RenderPerceptionOverlays()
    {
        ImGui::Text("Debug Overlay Toggles");
        ImGui::Separator();

        ImGui::Checkbox("Detection Ranges", &m_showDetectionRanges);
        ImGui::SameLine();
        ImGui::TextDisabled("(spheres around agents)");

        ImGui::Checkbox("Attack Ranges", &m_showAttackRanges);
        ImGui::SameLine();
        ImGui::TextDisabled("(inner spheres)");

        ImGui::Checkbox("Nav Paths", &m_showNavPaths);
        ImGui::SameLine();
        ImGui::TextDisabled("(line segments)");

        ImGui::Checkbox("Target Lines", &m_showTargetLines);
        ImGui::SameLine();
        ImGui::TextDisabled("(lines to current target)");

        ImGui::Separator();
        ImGui::SliderFloat("Refresh Rate", &m_refreshInterval, 0.033f, 1.0f, "%.0f ms");
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    void AIDebugPanel::RenderStatistics()
    {
        ImGui::Text("AI System Statistics");
        ImGui::Separator();

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

        ImGui::Text("Total Agents: %d", totalAgents);
        ImGui::Text("In Combat: %d", combatAgents);
        ImGui::Text("Alert: %d", alertAgents);
        ImGui::Text("Idle: %d", idleAgents);
        ImGui::Text("With Targets: %d", withTargets);
    }

} // namespace SparkEditor
