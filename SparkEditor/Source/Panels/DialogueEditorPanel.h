/**
 * @file DialogueEditorPanel.h
 * @brief Node-based dialogue tree editor for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a visual editor for creating branching dialogue trees used
 * in NPC conversations, cutscenes, and quest interactions. Supports
 * conditional branching, variable tracking, and event triggers.
 */

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>
#include <algorithm>

#include <imgui.h>

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    // =========================================================================
    // Dialogue tree data model
    // =========================================================================

    /**
     * @brief Unique identifier for dialogue nodes
     */
    using DialogueNodeId = uint32_t;
    static constexpr DialogueNodeId INVALID_DIALOGUE_NODE = 0;

    /**
     * @brief Type of dialogue node
     */
    enum class DialogueNodeType
    {
        Entry,        ///< Starting node of a conversation
        NPCLine,      ///< NPC speaks a line of dialogue
        PlayerChoice, ///< Player selects from multiple responses
        Condition,    ///< Branch based on game state condition
        Event,        ///< Fire a game event (quest update, item give, etc.)
        SetVariable,  ///< Set a dialogue variable
        Random,       ///< Randomly choose an output branch
        Delay,        ///< Wait before continuing (for pacing)
        Exit          ///< End the conversation
    };

    /**
     * @brief Comparison operator for conditions
     */
    enum class ConditionOperator
    {
        Equals,
        NotEquals,
        GreaterThan,
        LessThan,
        GreaterOrEqual,
        LessOrEqual,
        Contains,
        HasFlag
    };

    /**
     * @brief A condition that gates dialogue flow
     */
    struct DialogueCondition
    {
        std::string variableName; ///< Variable to check
        ConditionOperator op = ConditionOperator::Equals;
        std::string compareValue; ///< Value to compare against
        bool negate = false;      ///< Invert the condition result
    };

    /**
     * @brief A single player response choice
     */
    struct DialogueChoice
    {
        std::string text; ///< Choice text displayed to the player
        DialogueNodeId targetNodeId = INVALID_DIALOGUE_NODE;
        std::vector<DialogueCondition> conditions; ///< Conditions to show this choice
        bool isDefault = false;                    ///< Fallback if no other choice is available
        std::string tooltip;                       ///< Optional hover hint
        int priority = 0;                          ///< Display order (higher = first)
    };

    /**
     * @brief A game event triggered by a dialogue node
     */
    struct DialogueEvent
    {
        std::string eventName; ///< Event identifier
        std::string eventData; ///< Serialized event payload
    };

    /**
     * @brief A single node in the dialogue tree
     */
    struct DialogueNode
    {
        DialogueNodeId id = INVALID_DIALOGUE_NODE;
        DialogueNodeType type = DialogueNodeType::NPCLine;
        std::string title; ///< Editor-only label

        /// NPC line data
        std::string speakerName;      ///< Name of the speaking character
        std::string dialogueText;     ///< The line of dialogue
        std::string voiceClipPath;    ///< Path to voice audio file
        std::string animationName;    ///< Animation to play during line
        float displayDuration = 3.0f; ///< How long the line is shown (seconds)

        /// Player choice data
        std::vector<DialogueChoice> choices;

        /// Condition data
        DialogueCondition condition;
        DialogueNodeId trueTargetId = INVALID_DIALOGUE_NODE;
        DialogueNodeId falseTargetId = INVALID_DIALOGUE_NODE;

        /// Event data
        std::vector<DialogueEvent> events;

        /// Variable assignment
        std::string setVariableName;
        std::string setVariableValue;

        /// Random branch data
        std::vector<DialogueNodeId> randomTargets;

        /// Delay
        float delayDuration = 1.0f;

        /// Default next node (for linear flow)
        DialogueNodeId nextNodeId = INVALID_DIALOGUE_NODE;

        /// Editor layout position
        ImVec2 editorPosition{0, 0};
        bool isSelected = false;

        /// Comment/notes for designers
        std::string designerNotes;
    };

    /**
     * @brief Connection between two dialogue nodes (for rendering)
     */
    struct DialogueConnection
    {
        DialogueNodeId fromNodeId = INVALID_DIALOGUE_NODE;
        DialogueNodeId toNodeId = INVALID_DIALOGUE_NODE;
        int outputIndex = 0; ///< Which output slot (for choices/conditions)
        ImVec4 color{0.8f, 0.8f, 0.8f, 1.0f};
    };

    /**
     * @brief A complete dialogue tree asset
     */
    struct DialogueTreeAsset
    {
        std::string name = "Untitled Dialogue";
        std::string filePath;
        std::vector<DialogueNode> nodes;
        std::vector<DialogueConnection> connections;
        DialogueNodeId entryNodeId = INVALID_DIALOGUE_NODE;

        /// Variables tracked within this dialogue
        std::unordered_map<std::string, std::string> variables;

        /// Character definitions used in this dialogue
        std::vector<std::string> characters;

        bool isModified = false;

        DialogueNodeId nextNodeId = 1; ///< Auto-incrementing ID counter

        /**
         * @brief Generate the next unique node ID
         * @return A new unique DialogueNodeId
         */
        DialogueNodeId GenerateNodeId() { return nextNodeId++; }

        /**
         * @brief Find a node by ID
         * @param id Node ID to search for
         * @return Pointer to the node, or nullptr if not found
         */
        DialogueNode* FindNode(DialogueNodeId id)
        {
            for (auto& node : nodes)
            {
                if (node.id == id)
                    return &node;
            }
            return nullptr;
        }

        const DialogueNode* FindNode(DialogueNodeId id) const
        {
            for (const auto& node : nodes)
            {
                if (node.id == id)
                    return &node;
            }
            return nullptr;
        }
    };

    // =========================================================================
    // DialogueEditorPanel
    // =========================================================================

    /**
     * @brief Node-based dialogue tree editor
     *
     * Provides a visual graph editor for creating branching conversations:
     * - Drag-and-drop node placement on a 2D canvas
     * - Connection drawing between nodes (click-drag from output to input)
     * - Node types: NPC lines, player choices, conditions, events, variables
     * - Condition editor with variable comparison and flag checks
     * - Character management for speaker assignments
     * - Variable tracking and manipulation nodes
     * - Dialogue preview/playback mode
     * - Export to runtime format (.sparkdlg)
     * - Localization support with string table references
     */
    class DialogueEditorPanel : public EditorPanel
    {
      public:
        DialogueEditorPanel();
        ~DialogueEditorPanel() override;

        // EditorPanel interface
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        std::string GetTypeName() const override { return "DialogueEditorPanel"; }

        /**
         * @brief Create a new empty dialogue tree
         */
        void NewDialogue();

        /**
         * @brief Load a dialogue tree from file
         * @param filePath Path to the .sparkdlg file
         * @return true if load succeeded
         */
        bool LoadDialogue(const std::string& filePath);

        /**
         * @brief Save the current dialogue tree
         * @param filePath Path to save to (empty = use current path)
         * @return true if save succeeded
         */
        bool SaveDialogue(const std::string& filePath = "");

        /**
         * @brief Add a new node to the dialogue tree
         * @param type Type of node to create
         * @param position Position in editor canvas space
         * @return ID of the newly created node
         */
        DialogueNodeId AddNode(DialogueNodeType type, ImVec2 position);

        /**
         * @brief Delete a node and its connections
         * @param nodeId ID of the node to remove
         */
        void DeleteNode(DialogueNodeId nodeId);

        /**
         * @brief Create a connection between two nodes
         * @param fromId Source node ID
         * @param toId Target node ID
         * @param outputIndex Which output slot on the source node
         */
        void ConnectNodes(DialogueNodeId fromId, DialogueNodeId toId, int outputIndex = 0);

        /**
         * @brief Get the current dialogue tree
         */
        const DialogueTreeAsset* GetCurrentDialogue() const { return m_currentDialogue.get(); }

      private:
        // Render sub-sections
        void RenderToolbar();
        void RenderCanvas();
        void RenderNode(DialogueNode& node);
        void RenderConnections();
        void RenderNodeInspector();
        void RenderCharacterList();
        void RenderVariableList();
        void RenderContextMenu();
        void RenderPreviewMode();
        void RenderMinimap();

        // Node rendering by type
        void RenderNPCLineNode(DialogueNode& node);
        void RenderPlayerChoiceNode(DialogueNode& node);
        void RenderConditionNode(DialogueNode& node);
        void RenderEventNode(DialogueNode& node);

        // Canvas helpers
        ImVec2 ScreenToCanvas(ImVec2 screenPos) const;
        ImVec2 CanvasToScreen(ImVec2 canvasPos) const;
        void HandleCanvasInput();
        void DrawConnectionCurve(ImVec2 start, ImVec2 end, ImVec4 color, float thickness = 2.0f);

        // Node colors by type
        ImVec4 GetNodeColor(DialogueNodeType type) const;
        const char* GetNodeTypeName(DialogueNodeType type) const;

      private:
        std::unique_ptr<DialogueTreeAsset> m_currentDialogue;

        // Canvas state
        ImVec2 m_canvasOffset{0, 0}; ///< Scroll offset of the canvas
        float m_canvasZoom = 1.0f;   ///< Zoom level
        bool m_isDraggingCanvas = false;
        ImVec2 m_dragStart{0, 0};

        // Node interaction state
        DialogueNodeId m_selectedNodeId = INVALID_DIALOGUE_NODE;
        DialogueNodeId m_hoveredNodeId = INVALID_DIALOGUE_NODE;
        bool m_isDraggingNode = false;
        ImVec2 m_nodeDragOffset{0, 0};

        // Connection drawing state
        bool m_isDrawingConnection = false;
        DialogueNodeId m_connectionSourceId = INVALID_DIALOGUE_NODE;
        int m_connectionOutputIndex = 0;
        ImVec2 m_connectionEndPos{0, 0};

        // Context menu
        bool m_showContextMenu = false;
        ImVec2 m_contextMenuPos{0, 0};

        // Preview mode
        bool m_previewMode = false;
        DialogueNodeId m_previewCurrentNode = INVALID_DIALOGUE_NODE;

        // UI state
        bool m_showMinimap = true;
        bool m_showGrid = true;
        bool m_snapToGrid = true;
        float m_gridSize = 20.0f;
    };

} // namespace SparkEditor
