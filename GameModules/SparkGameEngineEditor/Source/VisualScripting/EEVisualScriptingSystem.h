/**
 * @file EEVisualScriptingSystem.h
 * @brief Node-based visual scripting for no-code gameplay logic
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a complete visual scripting graph system: node definitions with
 * typed input/output pins, execution flow, variable management, graph
 * compilation to bytecode, and runtime execution within the ECS.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace EngineEditor
{

    /// @brief A single pin (input or output) on a scripting node
    struct ScriptPin
    {
        uint32_t pinId = 0;
        std::string name;
        PinType type = PinType::Exec;
        bool isInput = true;
        bool isConnected = false;
        float defaultFloat = 0.0f;
        int defaultInt = 0;
        std::string defaultString;
    };

    /// @brief A node in the visual scripting graph
    struct ScriptNode
    {
        uint32_t nodeId = 0;
        std::string name;
        std::string description;
        NodeCategory category = NodeCategory::Event;
        float posX = 0.0f;   ///< Canvas position X
        float posY = 0.0f;   ///< Canvas position Y
        bool isPure = false; ///< Pure nodes have no exec pins
        bool isCollapsed = false;
        std::vector<ScriptPin> inputs;
        std::vector<ScriptPin> outputs;
    };

    /// @brief A connection between two pins
    struct ScriptConnection
    {
        uint32_t connectionId = 0;
        uint32_t fromNodeId = 0;
        uint32_t fromPinId = 0;
        uint32_t toNodeId = 0;
        uint32_t toPinId = 0;
    };

    /// @brief A variable defined in a script graph
    struct ScriptVariable
    {
        uint32_t variableId = 0;
        std::string name;
        PinType type = PinType::Float;
        bool isPublic = false; ///< Exposed to editor inspector
        float valueFloat = 0.0f;
        int valueInt = 0;
        std::string valueString;
    };

    /// @brief A complete visual script graph
    struct ScriptGraph
    {
        uint32_t graphId = 0;
        std::string name;
        std::string description;
        std::vector<ScriptNode> nodes;
        std::vector<ScriptConnection> connections;
        std::vector<ScriptVariable> variables;
        bool isCompiled = false;
        bool hasErrors = false;
    };

    /**
     * @brief Visual scripting graph system for no-code gameplay
     *
     * Manages script graphs with typed node connections, variable scoping,
     * compile-time validation, and runtime execution per entity.
     */
    class EEVisualScriptingSystem
    {
      public:
        EEVisualScriptingSystem() = default;
        ~EEVisualScriptingSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Graph management
        uint32_t CreateGraph(const std::string& name);
        bool DeleteGraph(uint32_t graphId);
        bool CompileGraph(uint32_t graphId);
        bool CompileAll();

        // Node operations
        uint32_t AddNode(uint32_t graphId, NodeCategory category, const std::string& name);
        bool RemoveNode(uint32_t graphId, uint32_t nodeId);
        bool ConnectPins(uint32_t graphId, uint32_t fromNode, uint32_t fromPin, uint32_t toNode, uint32_t toPin);
        bool DisconnectPin(uint32_t graphId, uint32_t connectionId);

        // Variable management
        uint32_t AddVariable(uint32_t graphId, const std::string& name, PinType type);

        // Queries
        size_t GetGraphCount() const { return m_graphs.size(); }
        size_t GetNodeTemplateCount() const { return m_nodeTemplates.size(); }
        const ScriptGraph* GetGraph(uint32_t graphId) const;
        std::string GetGraphListString() const;
        std::string GetNodeCatalogString() const;
        std::string GetGraphDetailString(uint32_t graphId) const;

      private:
        void RegisterBuiltinNodes();
        void RegisterEventNodes();
        void RegisterFlowNodes();
        void RegisterMathNodes();
        void RegisterTransformNodes();
        void RegisterPhysicsNodes();
        bool ValidateConnection(const ScriptGraph& graph, uint32_t fromNode, uint32_t fromPin, uint32_t toNode,
                                uint32_t toPin) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<ScriptGraph> m_graphs;
        std::vector<ScriptNode> m_nodeTemplates; ///< Built-in node catalog
        uint32_t m_nextGraphId{1};
        uint32_t m_nextNodeId{1};
        uint32_t m_nextPinId{1};
        uint32_t m_nextConnectionId{1};
        uint32_t m_nextVariableId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
