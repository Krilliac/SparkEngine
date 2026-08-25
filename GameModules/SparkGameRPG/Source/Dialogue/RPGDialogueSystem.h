/**
 * @file RPGDialogueSystem.h
 * @brief Branching dialogue trees with skill checks and consequences
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides dialogue trees with multiple node types (text, choice, skill check,
 * reward, end). Skill checks use character stats to determine pass/fail.
 * Dialogue outcomes can affect quest state and NPC disposition.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief A single choice option within a Choice dialogue node
    struct DialogueChoice
    {
        std::string text;
        uint32_t nextNodeId = 0;
    };

    /// @brief A skill check embedded in a Check dialogue node
    struct SkillCheck
    {
        std::string statName; ///< "strength", "charisma", etc.
        float requiredValue = 10.0f;
        uint32_t passNodeId = 0;
        uint32_t failNodeId = 0;
    };

    /// @brief Reward granted when reaching a Reward node
    struct DialogueReward
    {
        uint32_t xp = 0;
        uint32_t itemId = 0;
        int itemCount = 0;
        int dispositionChange = 0; ///< Positive = friendlier, negative = hostile
    };

    /// @brief A single node in a dialogue tree
    struct DialogueNode
    {
        uint32_t nodeId = 0;
        DialogueNodeType type = DialogueNodeType::Text;
        std::string speakerName;
        std::string text;

        // Type-specific data
        uint32_t nextNodeId = 0;             ///< For Text nodes: next node
        std::vector<DialogueChoice> choices; ///< For Choice nodes
        SkillCheck skillCheck;               ///< For Check nodes
        DialogueReward reward;               ///< For Reward nodes
    };

    /// @brief A complete dialogue tree with a root node
    struct DialogueTree
    {
        uint32_t treeId = 0;
        std::string name;
        uint32_t rootNodeId = 0;
        std::unordered_map<uint32_t, DialogueNode> nodes;
    };

    /// @brief Active dialogue session state
    struct DialogueSession
    {
        uint32_t treeId = 0;
        uint32_t currentNodeId = 0;
        uint32_t characterId = 0; ///< Player character in the conversation
        uint32_t npcId = 0;
        bool isActive = false;
    };

    /// @brief Callback for stat lookups during skill checks
    using StatLookupFn = std::function<float(uint32_t characterId, const std::string& statName)>;

    /**
     * @brief Branching dialogue with skill checks and consequence tracking
     */
    class RPGDialogueSystem
    {
      public:
        RPGDialogueSystem() = default;
        ~RPGDialogueSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Dialogue trees ===
        void RegisterTree(const DialogueTree& tree);
        const DialogueTree* GetTree(uint32_t treeId) const;
        size_t GetTreeCount() const { return m_trees.size(); }

        // === Session management ===
        bool StartDialogue(uint32_t treeId, uint32_t characterId, uint32_t npcId);
        void AdvanceDialogue(int choiceIndex = 0);
        void EndDialogue();
        const DialogueSession& GetCurrentSession() const { return m_session; }
        const DialogueNode* GetCurrentNode() const;
        bool IsInDialogue() const { return m_session.isActive; }

        // === Stat lookup for skill checks ===
        void SetStatLookup(StatLookupFn fn) { m_statLookup = std::move(fn); }

        // === Last results ===
        const DialogueReward& GetLastReward() const { return m_lastReward; }
        bool GetLastCheckPassed() const { return m_lastCheckPassed; }

      private:
        void RegisterDefaultTrees();
        void ProcessNode(const DialogueNode& node);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, DialogueTree> m_trees;
        DialogueSession m_session;
        StatLookupFn m_statLookup;

        DialogueReward m_lastReward;
        bool m_lastCheckPassed = false;
    };

} // namespace RPG
