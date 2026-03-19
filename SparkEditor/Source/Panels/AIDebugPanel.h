/**
 * @file AIDebugPanel.h
 * @brief Real-time AI debug visualization panel for inspecting live agent state
 *
 * Displays per-agent runtime data during play mode: current AI state, behavior
 * tree execution path, blackboard variables, perception ranges, and nav paths.
 * Complements AIEditorPanel (which is for authoring BT templates).
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Live AI agent inspector for play-mode debugging.
     *
     * Features:
     * - Agent list with state, target, and behavior tree name
     * - Per-agent blackboard variable viewer
     * - Behavior tree execution trace (active node highlighted)
     * - Perception range visualization toggles
     * - Nav path display
     */
    class AIDebugPanel : public EditorPanel
    {
      public:
        AIDebugPanel();
        ~AIDebugPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // --- Agent data cache (refreshed each frame from AISystem) ---

        enum class AgentState
        {
            Idle,
            Patrolling,
            Alert,
            Combat,
            Fleeing,
            Dead
        };

        struct AgentSnapshot
        {
            uint32_t entityID = 0;
            AgentState state = AgentState::Idle;
            std::string behaviorTreeName;
            std::string currentAction;
            float detectionRange = 0.0f;
            float attackRange = 0.0f;
            float distanceToTarget = 0.0f;
            bool hasTarget = false;
            int pathNodeCount = 0;
        };

        struct BlackboardEntry
        {
            std::string key;
            std::string type;
            std::string value;
        };

        // UI rendering
        void RenderAgentList();
        void RenderBlackboardInspector();
        void RenderBehaviorTreeTrace();
        void RenderPerceptionOverlays();
        void RenderStatistics();

        // Data
        std::vector<AgentSnapshot> m_agents;
        std::vector<BlackboardEntry> m_blackboardEntries;
        uint32_t m_selectedAgentID = 0;
        float m_refreshTimer = 0.0f;
        float m_refreshInterval = 0.1f; ///< 10 Hz refresh rate

        // Visualization toggles
        bool m_showDetectionRanges = true;
        bool m_showAttackRanges = false;
        bool m_showNavPaths = true;
        bool m_showTargetLines = true;
        bool m_highlightActiveNode = true;

        // Filter
        char m_stateFilter[64] = {};
        int m_stateFilterIndex = 0; ///< 0 = All, 1-6 = specific state
    };

} // namespace SparkEditor
