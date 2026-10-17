/**
 * @file VisualScriptEmitter.h
 * @brief Internal code generation for VisualScriptCompiler (not part of the public scripting API)
 *
 * VisualScriptEmitter walks one graph's execution wires and writes AngelScript
 * statements: each statement node is preceded, in a scoped block, by the pure
 * data nodes it consumes, so data is evaluated at its point of use. The free
 * functions are the literal, identifier and node-classification rules shared by
 * the emitter and VisualScriptCompiler::Compile.
 *
 * Contract: an emitter borrows the graph, the error list and the compile-wide
 * EmitBudget for one Compile() call and is used on that call's thread only; it
 * allocates lookup tables proportional to the graph and the generated text.
 * Every emitter of one Compile() shares one EmitBudget, so the statement and
 * byte limits bound the whole generated source, not each method body.
 * Editor/tooling tier.
 *
 * @see VisualScriptCompiler.h for the public entry point
 */

#pragma once

#include "VisualScriptGraphTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Scripting::Detail
{
    /// Upper bound on the generated source of one whole Compile() call (every method body together).
    inline constexpr size_t kMaxSourceBytes = size_t{16} << 20;

    /**
     * @brief Emission limits shared by every VisualScriptEmitter of one Compile() call
     *
     * Compile() owns one budget and passes it to the event-graph emitter and to
     * each function sub-graph emitter, so a graph cannot multiply its output by
     * spreading it over many event nodes or functions.
     */
    struct EmitBudget
    {
        size_t committedBytes = 0; ///< Source already written outside the body being emitted
        size_t steps = 0;          ///< Statements emitted so far, across all emitters
        bool halted = false;       ///< A depth or size budget was exceeded; all emission stops
    };

    /// One indentation level of generated source.
    inline constexpr std::string_view kIndent = "    ";

    /// Escape text for a double-quoted AngelScript string literal (control characters are dropped).
    std::string EscapeAngelScriptString(const std::string& raw);

    /// Map text to an AngelScript identifier: [A-Za-z0-9_] kept, anything else '_', no leading digit.
    std::string SanitizeIdentifier(const std::string& raw, const char* fallback);

    /// Generated variable holding output @p pinIndex of node @p nodeID.
    std::string VarName(uint32_t nodeID, uint32_t pinIndex);

    /// AngelScript type of a data pin kind.
    std::string PinTypeString(PinKind kind);

    /// Literal for a pin's default value; floats parse back to exactly the stored value.
    std::string DefaultLiteral(const ScriptPin& pin);

    /// Whether @p text is empty or a single literal of @p kind (it is spliced after "=").
    bool IsVariableDefaultLiteral(PinKind kind, std::string_view text);

    /// Event nodes are entry points; each becomes (part of) a generated method.
    bool IsEventNode(ScriptNodeType type);

    /// Pure nodes have no execution pins and no side effects; they are evaluated where used.
    bool IsPureNode(const ScriptNode& node);

    /// Index of a node's first data output (its result pin), or 0 without pin metadata.
    uint32_t FirstDataOutput(const ScriptNode& node);

    /// Kind of output @p index, or @p fallback when the node declares no such pin.
    PinKind OutputKind(const ScriptNode& node, uint32_t index, PinKind fallback);

    /// Node property @p key, or @p fallback when missing or empty.
    std::string PropertyOr(const ScriptNode& node, const char* key, const char* fallback);

    /**
     * @brief Emits the statements of one graph (an event graph or a function sub-graph)
     */
    class VisualScriptEmitter
    {
      public:
        /// Borrow @p graph, @p errors and @p budget for the emitter's lifetime; all must outlive it.
        VisualScriptEmitter(const VisualScriptGraph& graph, bool debugMode, std::vector<std::string>& errors,
                            EmitBudget& budget);

        const ScriptNode* FindNode(uint32_t id) const;

        /// Execution outputs of a node, in pin order (without pin metadata, the pins carrying execution wires).
        std::vector<uint32_t> ExecOutputPins(const ScriptNode& node) const;

        /// Emit every execution chain wired to output @p pin of @p node.
        void EmitPinChains(const ScriptNode& node, uint32_t pin, const std::string& indent, std::string& code);

        /**
         * @brief Emit a function sub-graph body: one execution chain per entry statement
         *
         * An entry is a statement node (neither pure nor an event) with no incoming
         * execution wire; entries are taken in @p nodes order and each is walked with its
         * downstream chain, so Branch, Sequence and ForLoop targets appear only inside
         * their blocks. Statements that sit only on an execution cycle have no entry;
         * a body whose every statement is such is reported as an error.
         * @param nodes The sub-graph's nodes in listed order (the graph this emitter borrowed)
         */
        void EmitEntryChains(const std::vector<ScriptNode>& nodes, const std::string& indent, std::string& code);

        /// Emit one statement node together with the pure data nodes it consumes.
        void EmitStep(const ScriptNode& node, const std::string& indent, std::string& code);

        /// Emit a node's own statement(s); Branch, ForLoop and Sequence go through EmitStep.
        void EmitNode(const ScriptNode& node, const std::string& indent, std::string& code,
                      bool assignHoistedResult = false);

      private:
        static const std::vector<const ScriptConnection*>& Lookup(
            const std::unordered_map<uint32_t, std::vector<const ScriptConnection*>>& table, uint32_t id);

        const std::vector<const ScriptConnection*>& Outgoing(uint32_t id) const { return Lookup(m_outgoing, id); }
        const std::vector<const ScriptConnection*>& Incoming(uint32_t id) const { return Lookup(m_incoming, id); }

        bool IsExecConnection(const ScriptConnection& conn) const;
        const ScriptConnection* FindConnectionToInput(uint32_t nodeID, uint32_t pinIndex) const;
        std::string ResolveInput(const ScriptNode& node, uint32_t inputIndex) const;
        std::vector<const ScriptNode*> PureDependencies(const ScriptNode& node);
        void CollectPure(const ScriptNode& node, std::vector<const ScriptNode*>& order,
                         std::unordered_set<uint32_t>& done, std::unordered_set<uint32_t>& visiting, size_t depth);
        void EmitControlFlow(const ScriptNode& node, const std::vector<std::string>& inputs, const std::string& indent,
                             std::string& code);
        bool HasExecTarget(const ScriptNode& node, uint32_t pin) const;
        void EmitChain(uint32_t startNode, const std::string& indent, std::string& code);
        /// True once emission must stop: the step budget is spent or Halt() was called.
        bool StepLimitReached() const;
        /// Record @p reason (first call only) and stop all further emission.
        void Halt(const std::string& reason);

        bool m_debugMode;
        std::vector<std::string>& m_errors;
        struct IndexedNode
        {
            const ScriptNode* node;
            bool onPath = false;
        };
        struct NodeTopology
        {
            std::vector<uint32_t> execPins;
            bool hasDataInputs = false;
        };
        std::unordered_map<uint32_t, IndexedNode> m_nodes;
        /// Immutable per-node metadata; exact pointers preserve duplicate-ID pin semantics.
        std::unordered_map<const ScriptNode*, NodeTopology> m_topology;
        std::unordered_map<uint32_t, std::vector<const ScriptConnection*>> m_outgoing;
        std::unordered_map<uint32_t, std::vector<const ScriptConnection*>> m_incoming;
        EmitBudget& m_budget;
        /// One reusable active path, with a suffix owned by each nested chain.
        std::vector<uint32_t> m_activePath;
        size_t m_chainDepth = 0; ///< Execution chains currently being emitted (nesting depth)
    };

} // namespace Spark::Scripting::Detail
