/**
 * @file VisualScriptSystem.h
 * @brief Node-based visual scripting system with Blueprint-to-AngelScript compilation
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a graph-based visual scripting system where nodes represent operations
 * (events, functions, flow control, math, variables) and edges represent data/execution
 * flow. Graphs can be compiled to AngelScript for runtime execution.
 *
 * Key features:
 *   - Type-safe pins with automatic coercion
 *   - Execution flow (white wires) separate from data flow (coloured wires)
 *   - Built-in node library: events, math, logic, string, entity, physics, input
 *   - Hot-reload: re-compile graph while the game is running
 *   - Serialisation to/from JSON for editor persistence
 *   - Blueprint-to-AngelScript compilation pipeline
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Spark::Scripting
{

    // ============================================================================
    // PIN TYPES
    // ============================================================================

    /**
     * @brief Data types that a visual script pin can carry.
     */
    enum class PinType : uint8_t
    {
        Execution, ///< White wire — controls which node fires next
        Bool,
        Int,
        Float,
        String,
        Vector2,
        Vector3,
        Vector4,
        Entity, ///< Entity ID reference
        Any     ///< Wildcard — resolved at connection time
    };

    /**
     * @brief Direction of a pin on a node.
     */
    enum class PinDirection : uint8_t
    {
        Input,
        Output
    };

    /**
     * @brief A typed value carried along a data wire.
     */
    using PinValue = std::variant<std::monostate, bool, int32_t, float, std::string, std::array<float, 2>, // Vector2
                                  std::array<float, 3>,                                                    // Vector3
                                  std::array<float, 4>,                                                    // Vector4
                                  uint64_t                                                                 // Entity ID
                                  >;

    // ============================================================================
    // PIN & LINK DESCRIPTORS
    // ============================================================================

    using NodeID = uint32_t;
    using PinID = uint32_t;
    using LinkID = uint32_t;

    constexpr NodeID INVALID_NODE_ID = 0;
    constexpr PinID INVALID_PIN_ID = 0;
    constexpr LinkID INVALID_LINK_ID = 0;

    struct PinDesc
    {
        PinID id = INVALID_PIN_ID;
        std::string name;
        PinType type = PinType::Any;
        PinDirection direction = PinDirection::Input;
        PinValue defaultValue;
    };

    /**
     * @brief A wire connecting an output pin to an input pin.
     */
    struct LinkDesc
    {
        LinkID id = INVALID_LINK_ID;
        NodeID sourceNode = INVALID_NODE_ID;
        PinID sourcePin = INVALID_PIN_ID;
        NodeID targetNode = INVALID_NODE_ID;
        PinID targetPin = INVALID_PIN_ID;
    };

    // ============================================================================
    // NODE CATEGORIES
    // ============================================================================

    enum class NodeCategory : uint8_t
    {
        Event,       ///< BeginPlay, Tick, OnCollision, etc.
        FlowControl, ///< Branch, ForLoop, Sequence, etc.
        Math,        ///< Add, Multiply, Clamp, Lerp, etc.
        Logic,       ///< AND, OR, NOT, Compare, etc.
        String,      ///< Concat, Format, Length, etc.
        Variable,    ///< Get/Set local or graph variables
        Entity,      ///< GetComponent, SpawnEntity, Destroy, etc.
        Physics,     ///< AddForce, Raycast, SetVelocity, etc.
        Input,       ///< IsKeyDown, GetAxis, GetMousePos, etc.
        Audio,       ///< PlaySound, StopSound, SetVolume, etc.
        Debug,       ///< Print, DrawLine, Log, etc.
        Custom       ///< User-registered nodes
    };

    // ============================================================================
    // NODE
    // ============================================================================

    /**
     * @brief A single node in the visual script graph.
     *
     * Nodes have typed input/output pins and an execute callback.
     * During compilation the callback is translated to AngelScript source.
     */
    class ScriptNode
    {
      public:
        ScriptNode() = default;
        ScriptNode(NodeID id, const std::string& name, NodeCategory category);
        ~ScriptNode() = default;

        NodeID GetID() const { return m_id; }
        const std::string& GetName() const { return m_name; }
        NodeCategory GetCategory() const { return m_category; }

        /// Pin management
        PinID AddInputPin(const std::string& name, PinType type, const PinValue& defaultVal = {});
        PinID AddOutputPin(const std::string& name, PinType type);

        const std::vector<PinDesc>& GetInputPins() const { return m_inputPins; }
        const std::vector<PinDesc>& GetOutputPins() const { return m_outputPins; }

        const PinDesc* FindPin(PinID pinId) const;

        /// Runtime execution callback: receives resolved input values, returns output values
        using ExecuteCallback = std::function<std::vector<PinValue>(const std::vector<PinValue>&)>;
        void SetExecuteCallback(ExecuteCallback cb) { m_executeCallback = std::move(cb); }
        const ExecuteCallback& GetExecuteCallback() const { return m_executeCallback; }

        /// AngelScript code fragment emitted during compilation
        void SetCodeTemplate(const std::string& code) { m_codeTemplate = code; }
        const std::string& GetCodeTemplate() const { return m_codeTemplate; }

        /// Editor position (for serialisation only)
        void SetPosition(float x, float y)
        {
            m_posX = x;
            m_posY = y;
        }
        float GetPosX() const { return m_posX; }
        float GetPosY() const { return m_posY; }

      private:
        NodeID m_id = INVALID_NODE_ID;
        std::string m_name;
        NodeCategory m_category = NodeCategory::Custom;
        std::vector<PinDesc> m_inputPins;
        std::vector<PinDesc> m_outputPins;
        ExecuteCallback m_executeCallback;
        std::string m_codeTemplate;
        float m_posX = 0.0f;
        float m_posY = 0.0f;
        PinID m_nextPinId = 1;
    };

    // ============================================================================
    // GRAPH VARIABLE
    // ============================================================================

    struct GraphVariable
    {
        std::string name;
        PinType type = PinType::Float;
        PinValue value;
        bool isPublic = false; ///< Exposed to the editor / inspector
    };

    // ============================================================================
    // VISUAL SCRIPT GRAPH
    // ============================================================================

    /**
     * @brief A complete visual script graph — a directed graph of ScriptNodes.
     *
     * Supports graph editing (add/remove nodes/links), validation,
     * interpretation (stepping through execution flow), and compilation
     * to AngelScript source code.
     */
    class VisualScriptGraph
    {
      public:
        VisualScriptGraph() = default;
        explicit VisualScriptGraph(const std::string& name);
        ~VisualScriptGraph() = default;

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        // -- Node management ------------------------------------------------

        NodeID AddNode(const std::string& typeName, float posX = 0, float posY = 0);
        bool RemoveNode(NodeID id);
        ScriptNode* GetNode(NodeID id);
        const ScriptNode* GetNode(NodeID id) const;
        const std::unordered_map<NodeID, ScriptNode>& GetNodes() const { return m_nodes; }

        // -- Link management ------------------------------------------------

        LinkID AddLink(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin);
        bool RemoveLink(LinkID id);
        const std::vector<LinkDesc>& GetLinks() const { return m_links; }

        /// Check if connecting two pins is valid (type compatibility, no cycles)
        bool CanConnect(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin) const;

        // -- Variables ------------------------------------------------------

        void AddVariable(const std::string& name, PinType type, const PinValue& defaultVal = {}, bool isPublic = false);
        bool RemoveVariable(const std::string& name);
        GraphVariable* GetVariable(const std::string& name);
        const std::vector<GraphVariable>& GetVariables() const { return m_variables; }

        // -- Validation -----------------------------------------------------

        struct ValidationResult
        {
            bool isValid = true;
            std::vector<std::string> errors;
            std::vector<std::string> warnings;
        };

        ValidationResult Validate() const;

        // -- Compilation to AngelScript ------------------------------------

        /**
         * @brief Compile the graph to AngelScript source code.
         * @return The generated AngelScript source, or empty string on failure.
         */
        std::string CompileToAngelScript() const;

        // -- Serialisation --------------------------------------------------

        std::string SerializeToJSON() const;
        bool DeserializeFromJSON(const std::string& json);

        // -- Execution (interpreter mode) -----------------------------------

        /**
         * @brief Execute the graph starting from the specified event node.
         * @param eventNodeId The event node to start execution from.
         */
        void Execute(NodeID eventNodeId);

      private:
        bool HasCycle(NodeID fromNode, NodeID toNode) const;
        bool HasCycleHelper(NodeID current, NodeID target, std::unordered_map<NodeID, bool>& visited) const;
        std::vector<NodeID> TopologicalSort() const;
        std::string GenerateNodeCode(const ScriptNode& node,
                                     const std::unordered_map<PinID, std::string>& pinVarNames) const;
        std::string PinTypeToASType(PinType type) const;
        std::string PinValueToASLiteral(const PinValue& value, PinType type) const;

        std::string m_name = "Untitled";
        std::unordered_map<NodeID, ScriptNode> m_nodes;
        std::vector<LinkDesc> m_links;
        std::vector<GraphVariable> m_variables;
        NodeID m_nextNodeId = 1;
        LinkID m_nextLinkId = 1;
    };

    // ============================================================================
    // NODE LIBRARY (built-in node templates)
    // ============================================================================

    /**
     * @brief Registry of node templates that can be instantiated in a graph.
     */
    class NodeLibrary
    {
      public:
        static NodeLibrary& GetInstance();

        struct NodeTemplate
        {
            std::string typeName;
            std::string displayName;
            NodeCategory category;
            std::vector<PinDesc> inputPins;
            std::vector<PinDesc> outputPins;
            std::string codeTemplate;
            ScriptNode::ExecuteCallback executeCallback;
        };

        void RegisterTemplate(const NodeTemplate& tmpl);
        const NodeTemplate* FindTemplate(const std::string& typeName) const;
        std::vector<const NodeTemplate*> GetTemplatesByCategory(NodeCategory category) const;
        const std::unordered_map<std::string, NodeTemplate>& GetAllTemplates() const { return m_templates; }

        /// Register all built-in nodes (events, math, logic, flow, etc.)
        void RegisterBuiltinNodes();

      private:
        NodeLibrary() = default;
        std::unordered_map<std::string, NodeTemplate> m_templates;
        bool m_builtinsRegistered = false;
    };

    // ============================================================================
    // VISUAL SCRIPT SYSTEM (top-level manager)
    // ============================================================================

    /**
     * @brief Top-level manager for visual scripts in the engine.
     *
     * Manages multiple graphs, handles hot-reload, and bridges
     * between the visual editor and the AngelScript runtime.
     */
    class VisualScriptSystem
    {
      public:
        static VisualScriptSystem& GetInstance();

        bool Initialize();
        void Shutdown();

        /// Graph management
        VisualScriptGraph* CreateGraph(const std::string& name);
        VisualScriptGraph* GetGraph(const std::string& name);
        bool RemoveGraph(const std::string& name);
        const std::unordered_map<std::string, std::unique_ptr<VisualScriptGraph>>& GetGraphs() const
        {
            return m_graphs;
        }

        /// Compile a graph and register it with the AngelScript engine
        bool CompileAndRegister(const std::string& graphName);

        /// Hot-reload: re-compile a graph while the game is running
        bool HotReload(const std::string& graphName);

        /// Save/load graphs to/from disk
        bool SaveGraph(const std::string& graphName, const std::string& filePath) const;
        bool LoadGraph(const std::string& filePath);

        /// Console integration
        std::string Console_GetStatus() const;
        std::string Console_ListGraphs() const;
        std::string Console_ListNodes() const;

      private:
        VisualScriptSystem() = default;

        std::unordered_map<std::string, std::unique_ptr<VisualScriptGraph>> m_graphs;
        bool m_isInitialized = false;
    };

} // namespace Spark::Scripting
