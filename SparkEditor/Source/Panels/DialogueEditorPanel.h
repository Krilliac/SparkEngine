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

        /// A single player-selectable choice within a dialogue node.
        struct DialogueChoice
        {
            char text[256] = {};      ///< Choice display text.
            char nextNodeId[64] = {}; ///< Node ID to jump to when selected.
            char condition[128] = {}; ///< Condition expression (empty = always available).
        };

        /// A single node in the dialogue tree being edited.
        struct DialogueNode
        {
            char id[64] = {};               ///< Unique node identifier.
            NodeType type = NodeType::Text; ///< Node behavior type.
            char speakerName[128] = {};     ///< NPC name shown above the text.
            char text[512] = {};            ///< Dialogue text content.
            char nextNodeId[64] = {};       ///< Default next node (for Text nodes).
            float displayDuration = 3.0f;   ///< Auto-advance time (0 = wait for input).

            // Choice node
            std::vector<DialogueChoice> choices; ///< Available player choices.

            // Branch node
            char condition[128] = {};  ///< Branch condition expression.
            char trueNodeId[64] = {};  ///< Node if condition is true.
            char falseNodeId[64] = {}; ///< Node if condition is false.

            // Event node
            char eventName[128] = {}; ///< Event to fire (e.g. "GiveItem").
            char eventData[256] = {}; ///< Event payload data (JSON string).

            // Presentation
            char animation[128] = {}; ///< NPC animation to play during this node.
            char voiceClip[256] = {}; ///< Voice-over audio clip path.
        };

        void RenderNodeList();   ///< Draw the left-side node list.
        void RenderNodeEditor(); ///< Draw the right-side node property editor.
        void RenderToolbar();    ///< Draw the top toolbar (new, save, load).

        std::vector<DialogueNode> m_nodes;    ///< All nodes in the current dialogue tree.
        int m_selectedNode = -1;              ///< Index of the selected node (-1 = none).
        char m_treeName[128] = "NewDialogue"; ///< Name of the dialogue tree being edited.
        bool m_modified = false;              ///< True if unsaved changes exist.
    };

} // namespace SparkEditor
