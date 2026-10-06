/**
 * @file VisualScriptCompiler.h
 * @brief Compiles a visual script node graph into AngelScript source code
 *
 * Each event node becomes a method of the generated class. Execution wires
 * are walked in order from the event; Branch, Sequence and ForLoop emit their
 * output chains inside their own blocks, so control flow nests to any depth.
 * Pure data nodes (no execution pins) are evaluated at the point of use: every
 * statement re-evaluates the data nodes feeding it in a scoped block, so a
 * Get Variable read after a Set Variable in the same chain sees the new value.
 * The generated .as file feeds directly into AngelScriptEngine and
 * ScriptHotReload; no separate runtime exists.
 *
 * Contract: stateless and re-entrant (any thread); allocates only the output
 * strings; editor/tooling tier, never called per frame.
 *
 * Umbrella header: node type enum lives in VisualScriptNodeTypes.h and the
 * graph data structures live in VisualScriptGraphTypes.h; both are included
 * here so existing includers need no changes.
 *
 * @see VisualScriptGraphIO.h for the .vscript graph file format
 * @see AngelScriptEngine.h for script compilation and execution
 */

#pragma once

#include "VisualScriptNodeTypes.h"
#include "VisualScriptGraphTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Scripting
{

    // ========================================================================
    // Compiler
    // ========================================================================

    /**
     * @brief Compiles a visual script node graph to AngelScript source code
     *
     * Emits one AngelScript class: member variables, one method per event
     * signature, reusable function graphs and custom event handlers. Fails
     * (success == false, errors filled) on an empty graph, a graph without
     * events, an execution or data cycle, or a variable default that is not a
     * literal of the variable's type.
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
    };

} // namespace Spark::Scripting
