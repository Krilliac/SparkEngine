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

        /// A single node in the behavior tree template being edited.
        struct BTNode
        {
            char name[128] = {};                  ///< Node display name (editable in the panel).
            BTNodeType type = BTNodeType::Action; ///< Composite/leaf type of this node.
            int parentIndex = -1;                 ///< Index of parent node (-1 = root).
            int depth = 0;                        ///< Depth in the tree (for indentation).
        };

        void RenderBehaviorTreeEditor(); ///< Draw the behavior tree node editor view.
        void RenderAgentInspector();     ///< Draw the active AI agent inspector view.
        void RenderBlackboardViewer();   ///< Draw the blackboard key/value viewer.

        std::vector<BTNode> m_nodes;              ///< All nodes in the currently edited behavior tree.
        int m_selectedNode = -1;                  ///< Index of the selected node (-1 = none).
        char m_treeName[128] = "NewBehaviorTree"; ///< Name of the behavior tree being edited.
        int m_activeTab = 0;                      ///< Active tab: 0 = BT Editor, 1 = Agent Inspector.
    };

} // namespace SparkEditor
