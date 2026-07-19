/**
 * @file VisualScriptGraphTypes.h
 * @brief Pin, node, connection, and graph data structures for visual scripting
 *
 * Part of the VisualScriptCompiler umbrella header — include
 * VisualScriptCompiler.h rather than this file directly.
 *
 * @see VisualScriptCompiler.h for the umbrella header
 */

#pragma once

#include "VisualScriptNodeTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

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
     * @brief A reusable function sub-graph (compiles to a separate method)
     */
    struct FunctionGraph
    {
        std::string name;                     ///< Function name (becomes method name)
        PinKind returnType = PinKind::Float;  ///< Return type (or Execution for void)
        std::vector<VariableDecl> parameters; ///< Function parameters
        std::vector<ScriptNode> nodes;
        std::vector<ScriptConnection> connections;
    };

    /**
     * @brief A custom event definition
     */
    struct CustomEventDef
    {
        std::string name;                     ///< Event name (becomes method name)
        std::vector<VariableDecl> parameters; ///< Event parameters
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
        std::vector<FunctionGraph> functions;     ///< Reusable function sub-graphs
        std::vector<CustomEventDef> customEvents; ///< Custom event definitions
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

    /**
     * @brief Node palette metadata for visual scripting authoring UI.
     */
    struct ScriptNodePaletteEntry
    {
        ScriptNodeType type{};
        const char* displayName = "";
        const char* category = "";
        const char* tooltip = "";
    };

} // namespace Spark::Scripting
