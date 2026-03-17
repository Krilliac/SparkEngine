/**
 * @file DialogueEditorPanel.h
 * @brief Dialogue tree editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for authoring branching dialogue trees
     *
     * Provides a node-list view for creating and editing dialogue nodes,
     * choices, branches, and events. Dialogue data is stored in JSON files
     * and loaded by the engine's DialogueSystem at runtime.
     */
    class DialogueEditorPanel : public EditorPanel
    {
      public:
        DialogueEditorPanel();
        ~DialogueEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // Node types matching engine DialogueNodeType
        enum class NodeType
        {
            Text,
            Choice,
            Branch,
            Event,
            End
        };

        struct DialogueChoice
        {
            char text[256] = {};
            char nextNodeId[64] = {};
            char condition[128] = {};
        };

        struct DialogueNode
        {
            char id[64] = {};
            NodeType type = NodeType::Text;
            char speakerName[128] = {};
            char text[512] = {};
            char nextNodeId[64] = {};
            float displayDuration = 3.0f;

            // Choice node
            std::vector<DialogueChoice> choices;

            // Branch node
            char condition[128] = {};
            char trueNodeId[64] = {};
            char falseNodeId[64] = {};

            // Event node
            char eventName[128] = {};
            char eventData[256] = {};

            // Presentation
            char animation[128] = {};
            char voiceClip[256] = {};
        };

        void RenderNodeList();
        void RenderNodeEditor();
        void RenderToolbar();

        std::vector<DialogueNode> m_nodes;
        int m_selectedNode = -1;
        char m_treeName[128] = "NewDialogue";
        bool m_modified = false;
    };

} // namespace SparkEditor
