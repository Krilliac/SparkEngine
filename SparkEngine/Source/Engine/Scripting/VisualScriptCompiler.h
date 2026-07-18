/**
 * @file VisualScriptCompiler.h
 * @brief Compiles a visual script node graph into AngelScript source code
 *
 * Follows the ShaderGraphCompiler pattern: topological sort from entry-point
 * nodes, then emit code in dependency order. The generated .as file feeds
 * directly into AngelScriptEngine + ScriptHotReload — no new runtime needed.
 *
 * Umbrella header: node type enum lives in VisualScriptNodeTypes.h and the
 * graph data structures live in VisualScriptGraphTypes.h; both are included
 * here so existing includers need no changes.
 *
 * @see ShaderGraphCompiler.h for the HLSL equivalent
 * @see AngelScriptEngine.h for script compilation and execution
 */

#pragma once

#include "VisualScriptNodeTypes.h"
#include "VisualScriptGraphTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

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
         * @param debugMode When true, inserts debugTrace() calls at each node for tracing
         * @return Compilation result with generated source or errors
         */
        static ScriptCompileResult Compile(const VisualScriptGraph& graph, bool debugMode = false);

        /// Blueprint-style authoring metadata used by editor palettes and search.
        static const std::vector<ScriptNodePaletteEntry>& GetNodePalette();

        /// Display name for a node type (fallback: "Unknown").
        static const char* GetNodeDisplayName(ScriptNodeType type);

        /// Category name for a node type (fallback: "Misc").
        static const char* GetNodeCategory(ScriptNodeType type);

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

        /// Emit every node in the execution chain hanging off an output pin (used
        /// by Branch/ForLoop/Sequence so multi-node chains stay inside the block)
        static void EmitExecChain(const VisualScriptGraph& graph, uint32_t fromNodeID, uint32_t fromPinIndex,
                                  std::string& code);

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
