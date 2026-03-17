/**
 * @file AIEditorPanel.h
 * @brief AI system editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for inspecting AI agents and editing behavior trees
     *
     * Provides a node-list view for creating behavior tree templates
     * (Selector, Sequence, Action, Condition nodes) and inspecting
     * active AI agent state at runtime.
     */
    class AIEditorPanel : public EditorPanel
    {
      public:
        AIEditorPanel();
        ~AIEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        enum class BTNodeType
        {
            Selector,
            Sequence,
            Action,
            Condition,
            Decorator,
            Parallel
        };

        struct BTNode
        {
            char name[128] = {};
            BTNodeType type = BTNodeType::Action;
            int parentIndex = -1;
            int depth = 0;
        };

        void RenderBehaviorTreeEditor();
        void RenderAgentInspector();
        void RenderBlackboardViewer();

        std::vector<BTNode> m_nodes;
        int m_selectedNode = -1;
        char m_treeName[128] = "NewBehaviorTree";
        int m_activeTab = 0; // 0=BT Editor, 1=Agent Inspector
    };

} // namespace SparkEditor
