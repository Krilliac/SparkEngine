/**
 * @file VisualScriptCompiler.h
 * @brief Compiles a visual script node graph into AngelScript source code
 *
 * Follows the ShaderGraphCompiler pattern: topological sort from entry-point
 * nodes, then emit code in dependency order. The generated .as file feeds
 * directly into AngelScriptEngine + ScriptHotReload — no new runtime needed.
 *
 * @see ShaderGraphCompiler.h for the HLSL equivalent
 * @see AngelScriptEngine.h for script compilation and execution
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

    // ========================================================================
    // Node Type Enum
    // ========================================================================

    /**
     * @brief Types of nodes in a visual script graph
     */
    enum class ScriptNodeType : uint32_t
    {
        // Events (entry points — each generates a method in the class)
        OnStart = 0,
        OnUpdate = 1,
        OnTriggerEnter = 2,
        OnTriggerExit = 3,
        OnDamaged = 4,
        OnKeyPress = 5,
        OnCollision = 6,
        OnCustomEvent = 7,

        // Flow control
        Branch = 50,
        ForLoop = 51,
        Sequence = 52,
        DoNothing = 53,

        // Getters (produce data values)
        GetPosition = 100,
        GetRotation = 101,
        GetHealth = 102,
        GetSpeed = 103,
        GetEntityByName = 104,
        GetSelf = 105,
        GetKeyDown = 106,
        GetKey = 107,
        GetDeltaTime = 108,

        // Setters / Actions (execute side effects)
        SetPosition = 150,
        SetRotation = 151,
        SetHealth = 152,
        ApplyForce = 153,
        PlaySound = 154,
        PlayAnimation = 155,
        SpawnEntity = 156,
        DestroyEntity = 157,
        PrintMessage = 158,
        FireEvent = 159,

        // Math
        Add = 200,
        Subtract = 201,
        Multiply = 202,
        Divide = 203,
        Normalize = 204,
        DotProduct = 205,
        Distance = 206,
        Lerp = 207,
        Clamp = 208,
        Random = 209,
        RandomRange = 210,
        Abs = 211,
        Negate = 212,

        // Logic
        And = 250,
        Or = 251,
        Not = 252,
        Equal = 253,
        NotEqual = 254,
        Greater = 255,
        Less = 256,
        GreaterEqual = 257,
        LessEqual = 258,

        // Variables
        GetVariable = 300,
        SetVariable = 301,

        // Constants
        ConstFloat = 350,
        ConstInt = 351,
        ConstBool = 352,
        ConstString = 353,
        ConstVector3 = 354,
    };

    // ========================================================================
    // Pin and Node Structures
    // ========================================================================

    /**
     * @brief Data type of a visual script pin
     */
    enum class PinKind : uint8_t
    {
        Execution, ///< Flow control (white wire)
        Bool,
        Int,
        Float,
        String,
        Vector3,
        Entity, ///< EntityID (uint32)
        Any,    ///< Untyped (resolved at compile time)
    };

    /**
     * @brief A single input or output pin on a script node
     */
    struct ScriptPin
    {
        PinKind kind = PinKind::Float;
        float defaultValue[4] = {};
        std::string defaultString;
        bool isConnected = false;
    };

    /**
     * @brief A node in the visual script graph
     */
    struct ScriptNode
    {
        uint32_t id = 0;
        ScriptNodeType type{};
        std::vector<ScriptPin> inputs;
        std::vector<ScriptPin> outputs;
        std::unordered_map<std::string, std::string> properties; ///< Node-specific properties
    };

    /**
     * @brief A connection between two pins
     */
    struct ScriptConnection
    {
        uint32_t fromNode = 0;
        uint32_t fromPin = 0;
        uint32_t toNode = 0;
        uint32_t toPin = 0;
    };

    /**
     * @brief A user-defined variable in the script
     */
    struct VariableDecl
    {
        std::string name;
        PinKind type = PinKind::Float;
        std::string defaultValue;
    };

    /**
     * @brief Complete visual script graph
     */
    struct VisualScriptGraph
    {
        std::string className = "MyScript";
        std::vector<ScriptNode> nodes;
        std::vector<ScriptConnection> connections;
        std::vector<VariableDecl> variables;
    };

    /**
     * @brief Result of compiling a visual script graph
     */
    struct ScriptCompileResult
    {
        std::string angelScriptSource; ///< Generated AngelScript code
        std::vector<std::string> errors;
        bool success = false;
    };

    // ========================================================================
    // Compiler
    // ========================================================================

    /**
     * @brief Compiles a visual script node graph to AngelScript source code
     *
     * Mirrors the ShaderGraphCompiler pattern:
     * 1. Find event entry-point nodes
     * 2. Topological sort: walk execution + data connections
     * 3. Emit AngelScript class with member variables, lifecycle methods
     */
    class VisualScriptCompiler
    {
      public:
        /**
         * @brief Compile a visual script graph to AngelScript source
         * @param graph Input graph with nodes and connections
         * @return Compilation result with generated source or errors
         */
        static ScriptCompileResult Compile(const VisualScriptGraph& graph);

      private:
        /// Generate a unique variable name for a node's output
        static std::string VarName(uint32_t nodeID, uint32_t pinIndex);

        /// Find the AngelScript type string for a pin kind
        static std::string PinTypeString(PinKind kind);

        /// Get the default value literal for a pin
        static std::string DefaultLiteral(const ScriptPin& pin);

        /// Topological sort of data-dependency nodes reachable from an execution chain
        static std::vector<uint32_t> TopologicalSortData(const VisualScriptGraph& graph, uint32_t startNode);

        /// Emit AngelScript code for a single node
        static void EmitNode(const ScriptNode& node, const VisualScriptGraph& graph, std::string& code);

        /// Resolve an input pin to its expression (connected var or default)
        static std::string ResolveInput(const ScriptNode& node, uint32_t inputIndex, const VisualScriptGraph& graph);

        /// Find the connection feeding into a specific input pin
        static const ScriptConnection* FindConnectionToInput(const VisualScriptGraph& graph, uint32_t nodeID,
                                                             uint32_t pinIndex);

        /// Find a node by ID
        static const ScriptNode* FindNode(const VisualScriptGraph& graph, uint32_t nodeID);

        /// Check if a node type is an event entry point
        static bool IsEventNode(ScriptNodeType type);

        /// Check if a node type is an action (has execution pins)
        static bool IsActionNode(ScriptNodeType type);
    };

} // namespace Spark::Scripting
