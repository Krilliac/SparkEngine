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
#include "../UndoRedo/EditorCommand.h"
#include "../UndoRedo/UndoRedoManager.h"
#include "Engine/Scripting/VisualScriptCompiler.h"

#include <string>
#include <vector>

namespace SparkEditor
{

    class VisualScriptPanel; // forward declare for commands

    /// @brief Undo command for adding a node to the visual script graph
    class AddNodeCommand : public EditorCommand
    {
      public:
        AddNodeCommand(VisualScriptPanel* panel, Spark::Scripting::ScriptNodeType type, float x, float y)
            : m_panel(panel), m_type(type), m_x(x), m_y(y)
        {
        }
        void Execute() override;
        void Undo() override;
        std::string GetDescription() const override { return "Add Node"; }

      private:
        VisualScriptPanel* m_panel;
        Spark::Scripting::ScriptNodeType m_type;
        float m_x, m_y;
        uint32_t m_createdNodeId = 0;
    };

    /// @brief Undo command for removing a node from the visual script graph
    class RemoveNodeCommand : public EditorCommand
    {
      public:
        RemoveNodeCommand(VisualScriptPanel* panel, int nodeIndex) : m_panel(panel), m_nodeIndex(nodeIndex) {}
        void Execute() override;
        void Undo() override;
        std::string GetDescription() const override { return "Remove Node"; }

      private:
        VisualScriptPanel* m_panel;
        int m_nodeIndex;
        // Snapshot for undo
        Spark::Scripting::ScriptNode m_savedNode;
        float m_savedX = 0, m_savedY = 0;
        std::vector<Spark::Scripting::ScriptConnection> m_savedConnections;
    };

    /// @brief Undo command for adding a connection
    class AddConnectionCommand : public EditorCommand
    {
      public:
        AddConnectionCommand(VisualScriptPanel* panel, Spark::Scripting::ScriptConnection conn)
            : m_panel(panel), m_conn(conn)
        {
        }
        void Execute() override;
        void Undo() override;
        std::string GetDescription() const override { return "Add Connection"; }

      private:
        VisualScriptPanel* m_panel;
        Spark::Scripting::ScriptConnection m_conn;
    };

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

        // -- Data types (public for undo/redo command access) --
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

        // -- Connection management --
        void TryStartConnection(int nodeIndex, int pinIndex, bool isOutput);
        void TryCompleteConnection(int nodeIndex, int pinIndex, bool isOutput);
        bool AreTypesCompatible(Spark::Scripting::PinKind a, Spark::Scripting::PinKind b) const;
        void GetPinScreenPos(int nodeIndex, int pinIndex, bool isOutput, float& outX, float& outY) const;
        int HitTestPin(float mouseX, float mouseY, int& outPinIndex, bool& outIsOutput) const;
        void RenderPendingConnection();

        // -- Compilation --
        void CompileGraph();
        void SaveGraph(const std::string& path);
        void LoadGraph(const std::string& path);

        // -- Internal graph state --
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
        float m_canvasOriginX = 0.0f; ///< Canvas top-left screen X
        float m_canvasOriginY = 0.0f; ///< Canvas top-left screen Y

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

        // Debug
        bool m_debugCompile = false;

        // Undo/Redo
        UndoRedoManager* m_undoRedo = nullptr;

      public:
        // Accessed by undo commands
        void AddNodeAtPositionDirect(Spark::Scripting::ScriptNodeType type, float x, float y);
        void RemoveNodeDirect(int nodeIndex);
        void AddConnectionDirect(const Spark::Scripting::ScriptConnection& conn);
        void RemoveConnectionDirect(const Spark::Scripting::ScriptConnection& conn);
        std::vector<NodeUI>& GetNodes() { return m_nodes; }
        std::vector<ConnectionUI>& GetConnections() { return m_connections; }
        uint32_t GetNextNodeId() const { return m_nextNodeId; }
        void SetUndoRedoManager(UndoRedoManager* mgr) { m_undoRedo = mgr; }
    };

} // namespace SparkEditor
