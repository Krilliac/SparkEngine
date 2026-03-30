/**
 * @file DialogueEditorPanel.cpp
 * @brief Implementation of the dialogue tree editor panel
 */

#include "DialogueEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    DialogueEditorPanel::DialogueEditorPanel() : EditorPanel("Dialogue Editor", "dialogue_editor_panel") {}

    bool DialogueEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "DialogueEditorPanel initialized");
        return true;
    }

    void DialogueEditorPanel::Update(float /*deltaTime*/) {}

    void DialogueEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;
            ImGui::BeginChild("NodeList", ImVec2(listWidth, 0), true);
            RenderNodeList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("NodeEditor", ImVec2(0, 0), true);
            RenderNodeEditor();
            ImGui::EndChild();
        }
        EndPanel();
    }

    void DialogueEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "DialogueEditorPanel shutting down");
    }

    void DialogueEditorPanel::RenderToolbar()
    {
        ImGui::InputText("Tree Name", m_treeName, sizeof(m_treeName));
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_PLUS " Add Node"))
        {
            DialogueNode node;
            snprintf(node.id, sizeof(node.id), "node_%d", static_cast<int>(m_nodes.size()));
            m_nodes.push_back(node);
            m_selectedNode = static_cast<int>(m_nodes.size()) - 1;
            m_modified = true;
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "DialogueEditorPanel: added node '%s'", node.id);
        }

        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(m_nodes.size()))
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Delete Node"))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "DialogueEditorPanel: deleted node at index %d",
                                m_selectedNode);
                m_nodes.erase(m_nodes.begin() + m_selectedNode);
                m_selectedNode = -1;
                m_modified = true;
            }
        }
    }

    void DialogueEditorPanel::RenderNodeList()
    {
        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        {
            const auto& node = m_nodes[i];
            const char* typeLabels[] = {"Text", "Choice", "Branch", "Event", "End"};
            const char* typeIcons[] = {ICON_FA_COMMENT, ICON_FA_LIST, ICON_FA_CODE_BRANCH, ICON_FA_BOLT, ICON_FA_STOP};

            int typeIdx = static_cast<int>(node.type);
            char label[256];
            snprintf(label, sizeof(label), "%s [%s] %s", typeIcons[typeIdx], typeLabels[typeIdx], node.id);

            bool selected = (m_selectedNode == i);
            if (ImGui::Selectable(label, selected))
                m_selectedNode = i;
        }
    }

    void DialogueEditorPanel::RenderNodeEditor()
    {
        if (m_selectedNode < 0 || m_selectedNode >= static_cast<int>(m_nodes.size()))
        {
            ImGui::TextDisabled("Select a node to edit");
            return;
        }

        auto& node = m_nodes[m_selectedNode];

        ImGui::InputText("Node ID", node.id, sizeof(node.id));

        const char* typeLabels[] = {"Text", "Choice", "Branch", "Event", "End"};
        int typeIdx = static_cast<int>(node.type);
        if (ImGui::Combo("Type", &typeIdx, typeLabels, IM_ARRAYSIZE(typeLabels)))
            node.type = static_cast<NodeType>(typeIdx);

        ImGui::Separator();

        switch (node.type)
        {
        case NodeType::Text:
            ImGui::InputText("Speaker", node.speakerName, sizeof(node.speakerName));
            ImGui::InputTextMultiline("Text", node.text, sizeof(node.text), ImVec2(-1, 80));
            ImGui::DragFloat("Duration", &node.displayDuration, 0.1f, 0.0f, 30.0f, "%.1f s (0=wait)");
            ImGui::InputText("Next Node", node.nextNodeId, sizeof(node.nextNodeId));
            break;

        case NodeType::Choice:
            ImGui::InputText("Speaker", node.speakerName, sizeof(node.speakerName));
            ImGui::InputTextMultiline("Prompt", node.text, sizeof(node.text), ImVec2(-1, 60));

            ImGui::Separator();
            ImGui::TextDisabled("Choices");
            for (int c = 0; c < static_cast<int>(node.choices.size()); ++c)
            {
                ImGui::PushID(c);
                auto& choice = node.choices[c];
                ImGui::InputText("Text", choice.text, sizeof(choice.text));
                ImGui::InputText("Next Node", choice.nextNodeId, sizeof(choice.nextNodeId));
                ImGui::InputText("Condition", choice.condition, sizeof(choice.condition));
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                {
                    node.choices.erase(node.choices.begin() + c);
                    --c;
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::Button(ICON_FA_PLUS " Add Choice"))
                node.choices.push_back(DialogueChoice{});
            break;

        case NodeType::Branch:
            ImGui::InputText("Condition", node.condition, sizeof(node.condition));
            ImGui::InputText("True -> Node", node.trueNodeId, sizeof(node.trueNodeId));
            ImGui::InputText("False -> Node", node.falseNodeId, sizeof(node.falseNodeId));
            break;

        case NodeType::Event:
            ImGui::InputText("Event Name", node.eventName, sizeof(node.eventName));
            ImGui::InputText("Event Data", node.eventData, sizeof(node.eventData));
            ImGui::InputText("Next Node", node.nextNodeId, sizeof(node.nextNodeId));
            break;

        case NodeType::End:
            ImGui::TextDisabled("End of conversation");
            break;
        }

        ImGui::Separator();
        ImGui::TextDisabled("Presentation");
        ImGui::InputText("Animation", node.animation, sizeof(node.animation));
        ImGui::InputText("Voice Clip", node.voiceClip, sizeof(node.voiceClip));
    }

} // namespace SparkEditor
