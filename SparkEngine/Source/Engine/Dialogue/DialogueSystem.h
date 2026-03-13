/**
 * @file DialogueSystem.h
 * @brief Dialogue tree and conversation system for NPCs
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a branching dialogue system for story-driven FPS games. Supports
 * dialogue trees with conditions, player choices, NPC responses, and event
 * triggers. Integrates with the Localization system for translated text.
 *
 * ## Architecture
 * ```
 * DialogueSystem
 *   ├── m_dialogueTrees   (loaded from JSON, keyed by ID)
 *   ├── m_activeConversation (current dialogue state)
 *   └── Update(deltaTime)
 *         ├── DisplayCurrentNode()  → shows text/choices via UISystem
 *         ├── ProcessChoice()       → evaluates conditions, advances
 *         └── FireEvents()          → triggers gameplay events
 * ```
 *
 * ## Usage
 * @code
 *   DialogueSystem dialogue;
 *   dialogue.LoadTree("guard_talk", "Data/Dialogue/guard.json");
 *
 *   dialogue.StartConversation("guard_talk");
 *   // Each frame while conversation is active:
 *   dialogue.Update(deltaTime);
 *   auto node = dialogue.GetCurrentNode();
 *   // Display node.text and node.choices to UI
 *   dialogue.SelectChoice(0); // Player picks first option
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstdint>

#include "Utils/LogMacros.h"

namespace Spark
{

    // =============================================================================
    // DialogueNode
    // =============================================================================

    /**
 * @brief Types of dialogue nodes.
 */
    enum class DialogueNodeType
    {
        Text,   ///< NPC speaks text
        Choice, ///< Player makes a choice
        Branch, ///< Conditional branch (checks game state)
        Event,  ///< Fires a game event
        End     ///< End of conversation
    };

    /**
 * @brief A single player-selectable choice in a dialogue.
 */
    struct DialogueChoice
    {
        std::string text;            ///< Display text for the choice
        std::string localizationKey; ///< Localization key (if using L10N)
        std::string nextNodeId;      ///< Node to jump to if selected
        std::string condition;       ///< Condition expression (empty = always available)
        bool visited = false;        ///< Whether this choice was previously selected
    };

    /**
 * @brief A single node in a dialogue tree.
 */
    struct DialogueNode
    {
        std::string id; ///< Unique node identifier
        DialogueNodeType type = DialogueNodeType::Text;

        // Text content
        std::string speakerName;      ///< Who is speaking (NPC name)
        std::string text;             ///< Dialogue text to display
        std::string localizationKey;  ///< Localization key for text
        std::string voiceClip;        ///< Path to voice audio clip
        float displayDuration = 3.0f; ///< Auto-advance time (0 = wait for input)

        // Choices (for Choice nodes)
        std::vector<DialogueChoice> choices;

        // Branch condition (for Branch nodes)
        std::string condition;   ///< Condition expression to evaluate
        std::string trueNodeId;  ///< Node if condition is true
        std::string falseNodeId; ///< Node if condition is false

        // Event (for Event nodes)
        std::string eventName; ///< Event to fire
        std::string eventData; ///< Data payload for the event

        // Flow
        std::string nextNodeId; ///< Next node (for linear flow)

        // Presentation
        std::string animation;   ///< NPC animation to play during this node
        std::string cameraAngle; ///< Camera preset name
    };

    // =============================================================================
    // DialogueTree
    // =============================================================================

    /**
 * @brief A complete dialogue tree — a graph of DialogueNodes.
 */
    class DialogueTree
    {
      public:
        /** @brief Set the tree identifier. */
        void SetId(const std::string& id) { m_id = id; }
        const std::string& GetId() const { return m_id; }

        /** @brief Set the starting node ID. */
        void SetStartNodeId(const std::string& nodeId) { m_startNodeId = nodeId; }
        const std::string& GetStartNodeId() const { return m_startNodeId; }

        /**
     * @brief Add a node to the tree.
     * @param node The dialogue node.
     */
        void AddNode(const DialogueNode& node);

        /**
     * @brief Get a node by ID.
     * @param nodeId Node identifier.
     * @return Pointer to node, or nullptr if not found.
     */
        const DialogueNode* GetNode(const std::string& nodeId) const;

        /**
     * @brief Get a mutable node by ID.
     * @param nodeId Node identifier.
     * @return Pointer to node, or nullptr if not found.
     */
        DialogueNode* GetMutableNode(const std::string& nodeId);

        /** @brief Get all node IDs. */
        std::vector<std::string> GetNodeIds() const;

        /** @brief Get node count. */
        size_t GetNodeCount() const { return m_nodes.size(); }

        /**
     * @brief Load tree from a JSON file.
     * @param filePath Path to JSON dialogue file.
     * @return true if loading succeeded.
     */
        bool LoadFromFile(const std::string& filePath);

      private:
        std::string m_id;
        std::string m_startNodeId;
        std::unordered_map<std::string, DialogueNode> m_nodes;
    };

    // =============================================================================
    // ConversationState
    // =============================================================================

    /**
 * @brief Runtime state of an active conversation.
 */
    struct ConversationState
    {
        std::string treeId;                                     ///< Active dialogue tree
        std::string currentNodeId;                              ///< Current node being displayed
        float nodeTimer = 0.0f;                                 ///< Time spent on current node
        bool waitingForInput = false;                           ///< Waiting for player choice
        bool isActive = false;                                  ///< Whether conversation is active
        std::unordered_map<std::string, std::string> variables; ///< Conversation-local variables
    };

    // =============================================================================
    // DialogueSystem
    // =============================================================================

    /**
 * @class DialogueSystem
 * @brief Manages dialogue trees and active conversations.
 */
    class DialogueSystem
    {
      public:
        DialogueSystem();
        ~DialogueSystem() = default;

        /**
     * @brief Load a dialogue tree from a JSON file.
     * @param treeId    Unique identifier for this tree.
     * @param filePath  Path to JSON file.
     * @return true if loading succeeded.
     */
        bool LoadTree(const std::string& treeId, const std::string& filePath);

        /**
     * @brief Register a dialogue tree directly.
     * @param treeId Unique identifier.
     * @param tree   The dialogue tree.
     */
        void RegisterTree(const std::string& treeId, std::unique_ptr<DialogueTree> tree);

        /**
     * @brief Start a conversation with a dialogue tree.
     * @param treeId Tree to start.
     * @return true if the tree was found and conversation started.
     */
        bool StartConversation(const std::string& treeId);

        /**
     * @brief End the current conversation.
     */
        void EndConversation();

        /**
     * @brief Update the dialogue system (advance timers, auto-advance).
     * @param deltaTime Frame time in seconds.
     */
        void Update(float deltaTime);

        /**
     * @brief Select a choice in the current dialogue node.
     * @param choiceIndex Index of the choice (0-based).
     * @return true if the choice was valid and selected.
     */
        bool SelectChoice(size_t choiceIndex);

        /**
     * @brief Advance to the next node (for text nodes without choices).
     */
        void AdvanceNode();

        // --- Query ---

        /** @brief Check if a conversation is active. */
        bool IsConversationActive() const { return m_state.isActive; }

        /** @brief Get the current dialogue node. */
        const DialogueNode* GetCurrentNode() const;

        /** @brief Get the current conversation state. */
        const ConversationState& GetState() const { return m_state; }

        /** @brief Get available choices for the current node (filtered by conditions). */
        std::vector<DialogueChoice> GetAvailableChoices() const;

        // --- Condition evaluation ---

        /**
     * @brief Register a condition evaluator function.
     * @param name     Condition name (e.g. "hasItem", "questComplete").
     * @param evaluator Function that returns true/false given a parameter string.
     */
        void RegisterCondition(const std::string& name, std::function<bool(const std::string&)> evaluator);

        /**
     * @brief Set a conversation variable.
     * @param name  Variable name.
     * @param value Variable value.
     */
        void SetVariable(const std::string& name, const std::string& value);

        /**
     * @brief Get a conversation variable.
     * @param name Variable name.
     * @return Value, or empty string if not set.
     */
        std::string GetVariable(const std::string& name) const;

        // --- Callbacks ---

        /**
     * @brief Register callback for dialogue events.
     * @param callback Called with (eventName, eventData).
     */
        void OnDialogueEvent(std::function<void(const std::string&, const std::string&)> callback);

        /**
     * @brief Register callback when conversation ends.
     * @param callback Called with the tree ID.
     */
        void OnConversationEnded(std::function<void(const std::string&)> callback);

        // --- Console integration ---

        /** @brief Get dialogue system status (console integration). */
        std::string Console_GetStatus() const;

        /** @brief List loaded dialogue trees (console integration). */
        std::string Console_ListTrees() const;

      private:
        void ProcessNode(const DialogueNode& node);
        bool EvaluateCondition(const std::string& condition) const;

        std::unordered_map<std::string, std::unique_ptr<DialogueTree>> m_trees;
        ConversationState m_state;
        std::unordered_map<std::string, std::function<bool(const std::string&)>> m_conditionEvaluators;
        std::vector<std::function<void(const std::string&, const std::string&)>> m_eventCallbacks;
        std::vector<std::function<void(const std::string&)>> m_endCallbacks;

        int m_processDepth = 0; ///< Recursion depth counter for cycle detection in ProcessNode

        static constexpr int kMaxProcessDepth = 100; ///< Maximum recursion depth before aborting
    };

} // namespace Spark
