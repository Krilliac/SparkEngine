/**
 * @file AIEditorPanel.cpp
 * @brief Implementation of the AI system editor panel
 */

#include "AIEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    AIEditorPanel::AIEditorPanel() : EditorPanel("AI Editor", "ai_editor_panel") {}

    bool AIEditorPanel::Initialize()
    {
        std::cout << "Initializing AI Editor panel\n";
        return true;
    }

    void AIEditorPanel::Update(float /*deltaTime*/) {}

    void AIEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("AITabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_PROJECT_DIAGRAM " Behavior Tree"))
                {
                    m_activeTab = 0;
                    RenderBehaviorTreeEditor();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_USER " Agent Inspector"))
                {
                    m_activeTab = 1;
                    RenderAgentInspector();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CHALKBOARD " Blackboard"))
                {
                    m_activeTab = 2;
                    RenderBlackboardViewer();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void AIEditorPanel::Shutdown()
    {
        std::cout << "Shutting down AI Editor panel\n";
    }

    void AIEditorPanel::RenderBehaviorTreeEditor()
    {
        ImGui::InputText("Tree Name", m_treeName, sizeof(m_treeName));

        if (ImGui::Button(ICON_FA_PLUS " Add Node"))
        {
            BTNode node;
            snprintf(node.name, sizeof(node.name), "Node_%d", static_cast<int>(m_nodes.size()));
            if (m_selectedNode >= 0)
            {
                node.parentIndex = m_selectedNode;
                node.depth = m_nodes[m_selectedNode].depth + 1;
            }
            m_nodes.push_back(node);
            m_selectedNode = static_cast<int>(m_nodes.size()) - 1;
        }

        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(m_nodes.size()))
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Delete"))
            {
                m_nodes.erase(m_nodes.begin() + m_selectedNode);
                // Fix parent references
                for (auto& n : m_nodes)
                {
                    if (n.parentIndex == m_selectedNode)
                        n.parentIndex = -1;
                    else if (n.parentIndex > m_selectedNode)
                        --n.parentIndex;
                }
                m_selectedNode = -1;
            }
        }

        ImGui::Separator();

        // Node tree view
        float listWidth = ImGui::GetContentRegionAvail().x * 0.4f;
        ImGui::BeginChild("BTNodeList", ImVec2(listWidth, 0), true);

        const char* typeIcons[] = {ICON_FA_QUESTION, ICON_FA_ARROW_RIGHT, ICON_FA_BOLT,
                                   ICON_FA_CHECK,    ICON_FA_FILTER,      ICON_FA_COLUMNS};
        const char* typeNames[] = {"Selector", "Sequence", "Action", "Condition", "Decorator", "Parallel"};

        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        {
            auto& node = m_nodes[i];
            int typeIdx = static_cast<int>(node.type);

            // Indent by depth
            if (node.depth > 0)
                ImGui::Indent(static_cast<float>(node.depth) * 16.0f);

            char label[256];
            snprintf(label, sizeof(label), "%s %s [%s]##%d", typeIcons[typeIdx], node.name, typeNames[typeIdx], i);

            if (ImGui::Selectable(label, m_selectedNode == i))
                m_selectedNode = i;

            if (node.depth > 0)
                ImGui::Unindent(static_cast<float>(node.depth) * 16.0f);
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Node properties
        ImGui::BeginChild("BTNodeProps", ImVec2(0, 0), true);
        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(m_nodes.size()))
        {
            auto& node = m_nodes[m_selectedNode];
            ImGui::InputText("Name", node.name, sizeof(node.name));

            int typeIdx = static_cast<int>(node.type);
            if (ImGui::Combo("Type", &typeIdx, typeNames, IM_ARRAYSIZE(typeNames)))
                node.type = static_cast<BTNodeType>(typeIdx);

            // Parent selection
            if (ImGui::BeginCombo("Parent", node.parentIndex >= 0 ? m_nodes[node.parentIndex].name : "(Root)"))
            {
                if (ImGui::Selectable("(Root)", node.parentIndex == -1))
                {
                    node.parentIndex = -1;
                    node.depth = 0;
                }
                for (int p = 0; p < static_cast<int>(m_nodes.size()); ++p)
                {
                    if (p == m_selectedNode)
                        continue;
                    if (ImGui::Selectable(m_nodes[p].name, node.parentIndex == p))
                    {
                        node.parentIndex = p;
                        node.depth = m_nodes[p].depth + 1;
                    }
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("Select a node to edit properties");
        }
        ImGui::EndChild();
    }

    void AIEditorPanel::RenderAgentInspector()
    {
        ImGui::TextDisabled("Runtime AI agent states will appear here during Play mode.");
        ImGui::Separator();

        // Placeholder columns for runtime agent display
        if (ImGui::BeginTable("AgentTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Entity");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Behavior Tree");
            ImGui::TableSetupColumn("Target");
            ImGui::TableHeadersRow();
            ImGui::EndTable();
        }
    }

    void AIEditorPanel::RenderBlackboardViewer()
    {
        ImGui::TextDisabled("AI Blackboard variables will appear here during Play mode.");
        ImGui::Separator();

        if (ImGui::BeginTable("BlackboardTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            ImGui::EndTable();
        }
    }

} // namespace SparkEditor
