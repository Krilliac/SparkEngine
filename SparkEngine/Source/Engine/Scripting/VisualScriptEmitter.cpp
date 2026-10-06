/**
 * @file VisualScriptEmitter.cpp
 * @brief Execution-chain walking, point-of-use data evaluation and control-flow emission
 */

#include "VisualScriptEmitter.h"

#include <algorithm>
#include <format>

namespace Spark::Scripting::Detail
{
    namespace
    {
        /// Upper bound on emitted statements across one Compile() (the count
        /// lives in the shared EmitBudget). Diamond-shaped execution graphs
        /// re-emit shared chains; once the bound is hit, EmitStep, EmitChain and
        /// EmitPinChains all stop, so a hostile graph fails in bounded time and
        /// output instead of walking an exponential number of paths.
        constexpr size_t kMaxEmittedSteps = size_t{1} << 20;

        /// Upper bound on nested execution chains (Branch, ForLoop and Sequence
        /// bodies and exec fan-out). Emission recurses about four native frames
        /// per level and indents every nested line, so an unbounded chain of
        /// nested Branch nodes (the parser admits 16,384 nodes) would overflow
        /// the stack and grow the output quadratically. Real graphs nest a
        /// handful of levels.
        constexpr size_t kMaxChainDepth = 64;

        /// Upper bound on the length of a chain of pure data nodes feeding one
        /// statement; CollectPure recurses once per producer.
        constexpr size_t kMaxDataDepth = 256;
    } // namespace

    VisualScriptEmitter::VisualScriptEmitter(const VisualScriptGraph& graph, bool debugMode,
                                             std::vector<std::string>& errors, EmitBudget& budget)
        : m_debugMode(debugMode), m_errors(errors), m_budget(budget)
    {
        m_nodes.reserve(graph.nodes.size());
        m_topology.reserve(graph.nodes.size());
        m_activePath.reserve(graph.nodes.size());
        for (const auto& node : graph.nodes)
        {
            m_nodes.emplace(node.id, IndexedNode{&node}); // first definition wins
        }
        for (const auto& conn : graph.connections)
        {
            m_outgoing[conn.fromNode].push_back(&conn);
            m_incoming[conn.toNode].push_back(&conn);
        }
        for (const auto& node : graph.nodes)
        {
            const auto& incoming = Incoming(node.id);
            const bool hasDataInputs = std::any_of(incoming.begin(), incoming.end(), [&](const ScriptConnection* conn)
                                                   { return !IsExecConnection(*conn); });
            m_topology.emplace(&node, NodeTopology{ExecOutputPins(node), hasDataInputs});
        }
    }

    const ScriptNode* VisualScriptEmitter::FindNode(uint32_t id) const
    {
        const auto it = m_nodes.find(id);
        return it != m_nodes.end() ? it->second.node : nullptr;
    }

    /// Execution outputs of a node, in pin order. Without pin metadata the
    /// pins that carry execution wires are used.
    std::vector<uint32_t> VisualScriptEmitter::ExecOutputPins(const ScriptNode& node) const
    {
        if (const auto cached = m_topology.find(&node); cached != m_topology.end())
        {
            return cached->second.execPins;
        }
        std::vector<uint32_t> pins;
        if (!node.outputs.empty())
        {
            for (uint32_t i = 0; i < static_cast<uint32_t>(node.outputs.size()); ++i)
            {
                if (node.outputs[i].kind == PinKind::Execution)
                {
                    pins.push_back(i);
                }
            }
            return pins;
        }
        for (const auto* conn : Outgoing(node.id))
        {
            if (IsExecConnection(*conn) && std::find(pins.begin(), pins.end(), conn->fromPin) == pins.end())
            {
                pins.push_back(conn->fromPin);
            }
        }
        std::sort(pins.begin(), pins.end());
        return pins;
    }

    /// Emit every execution chain wired to one output pin of @p node.
    void VisualScriptEmitter::EmitPinChains(const ScriptNode& node, uint32_t pin, const std::string& indent,
                                            std::string& code)
    {
        for (const auto* conn : Outgoing(node.id))
        {
            if (StepLimitReached())
            {
                return;
            }
            if (conn->fromPin == pin && IsExecConnection(*conn))
            {
                EmitChain(conn->toNode, indent, code);
            }
        }
    }

    void VisualScriptEmitter::EmitEntryChains(const std::vector<ScriptNode>& nodes, const std::string& indent,
                                              std::string& code)
    {
        bool hasStatements = false;
        bool hasEntry = false;
        for (const auto& node : nodes)
        {
            if (IsEventNode(node.type) || IsPureNode(node))
            {
                continue;
            }
            hasStatements = true;

            // Wires from event nodes do not count: events are not emitted in a function body.
            const auto& incoming = Incoming(node.id);
            const bool reachedByExec =
                std::any_of(incoming.begin(), incoming.end(),
                            [&](const ScriptConnection* conn)
                            {
                                const auto* from = FindNode(conn->fromNode);
                                return from && !IsEventNode(from->type) && IsExecConnection(*conn);
                            });
            if (reachedByExec)
            {
                continue;
            }

            hasEntry = true;
            EmitChain(node.id, indent, code);
            if (StepLimitReached())
            {
                return;
            }
        }
        if (hasStatements && !hasEntry)
        {
            m_errors.emplace_back("Function body has no entry statement: every statement is on an execution cycle");
        }
    }

    bool VisualScriptEmitter::StepLimitReached() const
    {
        return m_budget.halted || m_budget.steps > kMaxEmittedSteps;
    }

    void VisualScriptEmitter::Halt(const std::string& reason)
    {
        if (!m_budget.halted)
        {
            m_errors.push_back(reason);
        }
        m_budget.halted = true;
    }

    /// Emit one statement node with the pure data nodes it consumes.
    void VisualScriptEmitter::EmitStep(const ScriptNode& node, const std::string& indent, std::string& code)
    {
        if (m_budget.halted)
        {
            return;
        }
        // The limit covers the whole Compile(): source already committed by earlier
        // bodies (other events, other function emitters) plus this body so far.
        // Both terms are sizes of strings held in memory, so the sum cannot wrap.
        if (m_budget.committedBytes + code.size() > kMaxSourceBytes)
        {
            Halt("Graph generates more than " + std::to_string(kMaxSourceBytes) + " bytes of source");
            return;
        }
        if (++m_budget.steps > kMaxEmittedSteps)
        {
            if (m_budget.steps == kMaxEmittedSteps + 1)
            {
                m_errors.push_back("Graph expands to more than " + std::to_string(kMaxEmittedSteps) + " statements");
            }
            return;
        }

        if (m_debugMode)
        {
            code += indent + "debugTrace(" + std::to_string(node.id) + ", \"" +
                    std::to_string(static_cast<uint32_t>(node.type)) + "\", \"executing\");\n";
        }

        const auto dependencies = PureDependencies(node);
        if (node.type == ScriptNodeType::Sequence)
        {
            EmitControlFlow(node, {}, indent, code);
            return;
        }
        const std::string inner = indent + std::string(kIndent);

        if (node.type == ScriptNodeType::Branch || node.type == ScriptNodeType::ForLoop)
        {
            const bool isBranch = node.type == ScriptNodeType::Branch;
            const std::vector<uint32_t> dataInputs = isBranch ? std::vector<uint32_t>{1} : std::vector<uint32_t>{1, 2};
            std::vector<std::string> expressions;
            expressions.reserve(dataInputs.size());
            for (uint32_t input : dataInputs)
            {
                expressions.push_back(ResolveInput(node, input));
            }
            if (dependencies.empty())
            {
                EmitControlFlow(node, expressions, indent, code);
                return;
            }

            // Evaluate the inputs in a closed scope so the nested chains can
            // re-declare the same data nodes without shadowing them.
            const std::string id = std::to_string(node.id);
            const std::vector<std::string> temporaries = isBranch
                                                             ? std::vector<std::string>{"vsCond" + id}
                                                             : std::vector<std::string>{"vsStart" + id, "vsEnd" + id};
            code += indent + "{\n";
            for (const auto& temporary : temporaries)
            {
                code += std::format("{}{}{};\n", inner, isBranch ? "bool " : "int ", temporary);
            }
            code += inner + "{\n";
            for (const auto* dependency : dependencies)
            {
                EmitNode(*dependency, inner + std::string(kIndent), code);
            }
            for (size_t i = 0; i < temporaries.size(); ++i)
            {
                code += inner + std::string(kIndent) + temporaries[i] + " = " + expressions[i] + ";\n";
            }
            code += inner + "}\n";
            EmitControlFlow(node, temporaries, inner, code);
            code += indent + "}\n";
            return;
        }

        const bool selfPure = IsPureNode(node);
        if (dependencies.empty() && !selfPure)
        {
            EmitNode(node, indent, code);
            return;
        }

        // An impure call whose result later statements read declares the
        // result outside the scope that evaluates its arguments.
        const bool hoistResult = !selfPure && node.type == ScriptNodeType::CallFunction &&
                                 OutputKind(node, FirstDataOutput(node), PinKind::Execution) != PinKind::Execution;
        if (hoistResult)
        {
            const uint32_t result = FirstDataOutput(node);
            code += indent + PinTypeString(node.outputs[result].kind) + " " + VarName(node.id, result) + ";\n";
        }

        code += indent + "{\n";
        for (const auto* dependency : dependencies)
        {
            EmitNode(*dependency, inner, code);
        }
        EmitNode(node, inner, code, hoistResult);
        code += indent + "}\n";
    }

    const std::vector<const ScriptConnection*>& VisualScriptEmitter::Lookup(
        const std::unordered_map<uint32_t, std::vector<const ScriptConnection*>>& table, uint32_t id)
    {
        static const std::vector<const ScriptConnection*> kNone;
        const auto it = table.find(id);
        return it != table.end() ? it->second : kNone;
    }

    /// Classify a connection as execution flow (white wire) vs data. When pin
    /// metadata is missing on both endpoints (hand-built graphs without pin
    /// declarations), assume execution so those graphs still compile.
    bool VisualScriptEmitter::IsExecConnection(const ScriptConnection& conn) const
    {
        const auto* from = FindNode(conn.fromNode);
        if (from && conn.fromPin < from->outputs.size())
        {
            return from->outputs[conn.fromPin].kind == PinKind::Execution;
        }
        const auto* to = FindNode(conn.toNode);
        if (to && conn.toPin < to->inputs.size())
        {
            return to->inputs[conn.toPin].kind == PinKind::Execution;
        }
        return true;
    }

    const ScriptConnection* VisualScriptEmitter::FindConnectionToInput(uint32_t nodeID, uint32_t pinIndex) const
    {
        for (const auto* conn : Incoming(nodeID))
        {
            if (conn->toPin == pinIndex)
            {
                return conn;
            }
        }
        return nullptr;
    }

    /// Resolve an input pin to an expression: the producer's variable, an
    /// event parameter, or the pin's default literal.
    std::string VisualScriptEmitter::ResolveInput(const ScriptNode& node, uint32_t inputIndex) const
    {
        if (const auto* conn = FindConnectionToInput(node.id, inputIndex))
        {
            const auto* producer = FindNode(conn->fromNode);
            if (producer && IsEventNode(producer->type) && conn->fromPin == 1)
            {
                // Event data outputs are the generated method's parameters.
                switch (producer->type)
                {
                case ScriptNodeType::OnUpdate:
                case ScriptNodeType::OnKeyPress:
                    return "dt";
                case ScriptNodeType::OnCollision:
                    return "other";
                case ScriptNodeType::OnTriggerEnter:
                case ScriptNodeType::OnTriggerExit:
                    return "triggerId";
                case ScriptNodeType::OnDamaged:
                    return "amount";
                default:
                    break;
                }
            }
            return VarName(conn->fromNode, conn->fromPin);
        }

        if (inputIndex < node.inputs.size())
        {
            return DefaultLiteral(node.inputs[inputIndex]);
        }
        return "0.0f";
    }

    /// Pure producers feeding @p node (transitively), producers first.
    std::vector<const ScriptNode*> VisualScriptEmitter::PureDependencies(const ScriptNode& node)
    {
        if (const auto cached = m_topology.find(&node); cached != m_topology.end() && !cached->second.hasDataInputs)
        {
            return {};
        }
        std::vector<const ScriptNode*> order;
        std::unordered_set<uint32_t> done;
        std::unordered_set<uint32_t> visiting{node.id};
        CollectPure(node, order, done, visiting, 0);
        return order;
    }

    void VisualScriptEmitter::CollectPure(const ScriptNode& node, std::vector<const ScriptNode*>& order,
                                          std::unordered_set<uint32_t>& done, std::unordered_set<uint32_t>& visiting,
                                          size_t depth)
    {
        if (depth >= kMaxDataDepth)
        {
            Halt("Data chain into node " + std::to_string(node.id) + " is deeper than " +
                 std::to_string(kMaxDataDepth) + " nodes");
            return;
        }

        // One producer per input pin (the first wire, as ResolveInput reads it), in pin order.
        std::vector<const ScriptConnection*> inputs;
        for (const auto* conn : Incoming(node.id))
        {
            if (IsExecConnection(*conn) || FindConnectionToInput(node.id, conn->toPin) != conn)
            {
                continue;
            }
            inputs.push_back(conn);
        }
        std::stable_sort(inputs.begin(), inputs.end(),
                         [](const ScriptConnection* a, const ScriptConnection* b) { return a->toPin < b->toPin; });

        for (const auto* conn : inputs)
        {
            if (m_budget.halted)
            {
                return;
            }
            const auto* producer = FindNode(conn->fromNode);
            if (!producer || !IsPureNode(*producer) || done.count(producer->id) != 0)
            {
                continue;
            }
            if (!visiting.insert(producer->id).second)
            {
                m_errors.push_back("Data cycle through node " + std::to_string(producer->id));
                continue;
            }
            CollectPure(*producer, order, done, visiting, depth + 1);
            visiting.erase(producer->id);
            done.insert(producer->id);
            order.push_back(producer);
        }
    }

    void VisualScriptEmitter::EmitControlFlow(const ScriptNode& node, const std::vector<std::string>& inputs,
                                              const std::string& indent, std::string& code)
    {
        if (node.type == ScriptNodeType::Sequence)
        {
            // Borrow immutable pin order instead of allocating it for every shared-chain visit.
            // The fallback retains the internal EmitStep API's behavior for an unlisted node.
            const auto cached = m_topology.find(&node);
            std::vector<uint32_t> uncachedPins;
            const auto& pins =
                cached != m_topology.end() ? cached->second.execPins : (uncachedPins = ExecOutputPins(node));
            for (uint32_t pin : pins)
            {
                EmitPinChains(node, pin, indent, code);
            }
            return;
        }
        const std::string inner = indent + std::string(kIndent);
        switch (node.type)
        {
        case ScriptNodeType::Branch:
            if (!HasExecTarget(node, 0) && HasExecTarget(node, 1))
            {
                // Only the False output is wired: one negated block instead of an empty if.
                code += indent + "if (!(" + inputs[0] + "))\n" + indent + "{\n";
                EmitPinChains(node, 1, inner, code);
                code += indent + "}\n";
                break;
            }
            code += indent + "if (" + inputs[0] + ")\n" + indent + "{\n";
            EmitPinChains(node, 0, inner, code); // True
            code += indent + "}\n";
            if (HasExecTarget(node, 1))
            {
                code += indent + "else\n" + indent + "{\n";
                EmitPinChains(node, 1, inner, code); // False
                code += indent + "}\n";
            }
            break;
        case ScriptNodeType::ForLoop:
        {
            const std::string index = VarName(node.id, 1); // out[0] is LoopBody exec, out[1] is Index
            code += indent + "for (int " + index + " = " + inputs[0] + "; " + index + " < " + inputs[1] + "; " + index +
                    "++)\n" + indent + "{\n";
            EmitPinChains(node, 0, inner, code);
            code += indent + "}\n";
            break;
        }
        default:
            break;
        }
    }

    bool VisualScriptEmitter::HasExecTarget(const ScriptNode& node, uint32_t pin) const
    {
        const auto& outgoing = Outgoing(node.id);
        return std::any_of(outgoing.begin(), outgoing.end(), [&](const ScriptConnection* conn)
                           { return conn->fromPin == pin && IsExecConnection(*conn); });
    }

    /// Walk an execution chain: each node, then the node its execution output leads to.
    void VisualScriptEmitter::EmitChain(uint32_t startNode, const std::string& indent, std::string& code)
    {
        if (m_chainDepth >= kMaxChainDepth)
        {
            Halt("Graph nests execution flow deeper than " + std::to_string(kMaxChainDepth) + " levels");
            return;
        }
        // Balanced on every return path: the loop below only breaks, never returns.
        ++m_chainDepth;

        const size_t pathStart = m_activePath.size();
        uint32_t current = startNode;
        while (!StepLimitReached())
        {
            const auto found = m_nodes.find(current);
            if (found == m_nodes.end())
            {
                m_errors.push_back("Execution wire targets missing node " + std::to_string(current));
                break;
            }
            const auto* node = found->second.node;
            if (IsEventNode(node->type))
            {
                break;
            }
            if (found->second.onPath)
            {
                m_errors.push_back("Execution cycle through node " + std::to_string(current));
                break;
            }
            found->second.onPath = true;
            m_activePath.push_back(current);

            EmitStep(*node, indent, code);

            // Branch and Sequence route all of their outputs inside EmitStep.
            if (node->type == ScriptNodeType::Branch || node->type == ScriptNodeType::Sequence)
            {
                break;
            }

            std::vector<uint32_t> next;
            for (uint32_t pin : m_topology.at(node).execPins)
            {
                if (node->type == ScriptNodeType::ForLoop && pin == 0)
                {
                    continue; // loop body, emitted inside the loop
                }
                for (const auto* conn : Outgoing(node->id))
                {
                    if (conn->fromPin == pin && IsExecConnection(*conn))
                    {
                        next.push_back(conn->toNode);
                    }
                }
            }
            if (next.size() != 1)
            {
                for (uint32_t target : next)
                {
                    if (StepLimitReached())
                    {
                        break;
                    }
                    EmitChain(target, indent, code);
                }
                break;
            }
            current = next.front();
        }

        while (m_activePath.size() > pathStart)
        {
            m_nodes.at(m_activePath.back()).onPath = false;
            m_activePath.pop_back();
        }
        --m_chainDepth;
    }

} // namespace Spark::Scripting::Detail
