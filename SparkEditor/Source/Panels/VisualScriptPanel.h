/**
 * @file VisualScriptPanel.h
 * @brief Node-based visual scripting editor panel
 *
 * Provides a visual node graph editor that compiles to AngelScript.
 * Non-coders can create gameplay logic by connecting nodes instead of writing code.
 *
 * @see VisualScriptCompiler.h for the compilation backend
 * @see MaterialEditorPanel.h for a similar node graph UI pattern
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/Scripting/VisualScriptCompiler.h"

#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Visual scripting editor with node graph canvas
     *
     * Layout:
     * - Left sidebar: Node palette (categorized)
     * - Center: Canvas with pan/zoom, node rendering, connection drawing
     * - Right sidebar: Variables panel + selected node properties
     * - Bottom bar: Compile button, error list, generated script info
     */
    class VisualScriptPanel : public EditorPanel
    {
      public:
        VisualScriptPanel();
        ~VisualScriptPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // -- UI Sections --
        void RenderNodePalette();
        void RenderCanvas();
        void RenderVariablesPanel();
        void RenderCompileBar();
        void RenderNodeProperties();

        // -- Canvas operations --
        void RenderNode(int nodeIndex);
        void RenderConnections();
        void HandleCanvasInput();
        void AddNodeAtPosition(Spark::Scripting::ScriptNodeType type, float x, float y);
        void AddContextMenuNode();

        // -- Compilation --
        void CompileGraph();
        void SaveGraph(const std::string& path);
        void LoadGraph(const std::string& path);

        // -- Internal graph state --
        struct NodeUI
        {
            Spark::Scripting::ScriptNode node;
            float posX = 0.0f;
            float posY = 0.0f;
            float width = 160.0f;
            float height = 80.0f;
            bool selected = false;
        };

        struct ConnectionUI
        {
            Spark::Scripting::ScriptConnection connection;
        };

        struct VariableUI
        {
            char name[64] = {};
            int typeIndex = 2; // Default: Float
            char defaultValue[64] = {};
        };

        std::vector<NodeUI> m_nodes;
        std::vector<ConnectionUI> m_connections;
        std::vector<VariableUI> m_variables;

        // Canvas state
        float m_canvasOffsetX = 0.0f;
        float m_canvasOffsetY = 0.0f;
        float m_canvasZoom = 1.0f;
        bool m_isDraggingCanvas = false;
        int m_draggedNode = -1;
        int m_selectedNode = -1;

        // Connection drawing state
        bool m_isDrawingConnection = false;
        int m_connectionSourceNode = -1;
        int m_connectionSourcePin = -1;
        bool m_connectionSourceIsOutput = false;

        // Context menu
        bool m_showContextMenu = false;
        float m_contextMenuX = 0.0f;
        float m_contextMenuY = 0.0f;

        // Compilation
        std::string m_lastCompiledSource;
        std::vector<std::string> m_compileErrors;
        bool m_compileSuccess = false;
        char m_scriptName[128] = "MyScript";
        char m_savePath[256] = "Assets/Scripts/Generated/";

        // Node ID counter
        uint32_t m_nextNodeId = 1;
    };

} // namespace SparkEditor
